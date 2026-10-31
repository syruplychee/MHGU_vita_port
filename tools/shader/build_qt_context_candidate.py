"""Build an isolated Qt context-ownership fix over the pinned B30 backend.

Upstream source, existing objects/libraries, and baseline executable are read
only. Only game_window.cpp is copied/patched/compiled; all B30 overrides stay
in the link. Vita3K GPL-2.0-or-later provenance is preserved in the source copy.
"""
import argparse
import difflib
import json
from pathlib import Path
import re
import subprocess

from build_depth_preservation_emulator import copy_runtime, sha

SOURCE = "vita3k/gui-qt/src/game_window.cpp"
PIN = "5b55e6f6e0b680830902454092f35444b30e7ef17059adf3180fa7aa6e5358d5"


def patch(text):
    old = "if (QThread::currentThread() != QCoreApplication::instance()->thread()) {"
    new = """// A temporary drawable loss releases currentness while the render
        // loop is still alive. Keep QObject ownership on that render thread;
        // only final render-loop teardown hands the context back to the UI.
        if (m_emuenv.renderer && m_emuenv.renderer->render_abort.load(std::memory_order_relaxed)
            && QThread::currentThread() != QCoreApplication::instance()->thread()) {
            LOG_INFO("VP_GL_CONTEXT return-to-ui owner={} caller={}",
                static_cast<void *>(m_gl_context->thread()), static_cast<void *>(QThread::currentThread()));"""
    if text.count(old) != 1:
        raise ValueError("nonunique context ownership patch")
    text = text.replace(old, new)
    old = "    if (!m_gl_context->makeCurrent(this)) {"
    new = """    if (QThread::currentThread() != m_gl_context->thread()) {
        LOG_ERROR("VP_GL_CONTEXT mismatch owner={} caller={} migration_pending={}",
            static_cast<void *>(m_gl_context->thread()), static_cast<void *>(QThread::currentThread()),
            m_gl_migration_pending);
    }
    if (!m_gl_context->makeCurrent(this)) {"""
    if text.count(old) != 1:
        raise ValueError("nonunique makeCurrent logging patch")
    return text.replace(old, new)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--upstream", type=Path, required=True)
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--baseline", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--vcvars", type=Path, required=True)
    ap.add_argument("--ninja", type=Path, required=True)
    ap.add_argument("--runtime-config", type=Path, required=True,
                    help="Explicit base config; the host refuses startup without it")
    args = ap.parse_args()
    upstream, build, base, out = [p.resolve() for p in
                                  (args.upstream, args.build, args.baseline, args.output)]
    if out.exists() or out.is_relative_to(upstream) or out.is_relative_to(base):
        raise ValueError("output must be fresh and independent")
    source = upstream / SOURCE
    if sha(source) != PIN:
        raise ValueError("source identity mismatch")
    baseline = json.loads((base / "build.json").read_text(encoding="utf-8"))
    executable = base / "bin/Vita3K.exe"
    if sha(executable) != baseline["executable_sha256"]:
        raise ValueError("baseline executable identity mismatch")
    pins = dict(baseline["libraries"])
    for record in baseline["sources"]:
        pins[str(base / (record["name"] + ".obj"))] = record["object_sha256"]
    for path, digest in pins.items():
        if sha(path) != digest:
            raise ValueError("baseline link input drift: " + path)
    for name in ("src", "bin", "tmp", "link-obj"):
        (out / name).mkdir(parents=True, exist_ok=True)
    original = source.read_text(encoding="utf-8")
    modified = patch(original)
    copied = out / "src/game_window.cpp"
    copied.write_text(modified, encoding="utf-8", newline="\n")
    (out / "context-ownership.patch").write_text("".join(difflib.unified_diff(
        original.splitlines(True), modified.splitlines(True), fromfile=SOURCE, tofile=SOURCE)),
        encoding="utf-8", newline="\n")
    database = json.loads((build / "compile_commands.json").read_text())
    entry = next(e for e in database if e["file"].replace("\\", "/").endswith("/game_window.cpp"))
    command = re.sub(r'^\s*(?:"[^"]*cl\.exe"|\S*cl\.exe)\s+', '', entry["command"], flags=re.I)
    obj = out / "game_window.obj"
    command = re.sub(r'/Fo\S+', lambda _: '/Fo"' + str(obj) + '"', command)
    command = re.sub(r'/Fd\S+', lambda _: '/Fd"' + str(out / "compile.pdb") + '"', command)
    command = re.sub(r'-c\s+\S+$', lambda _: '-c "' + str(copied) + '"', command)
    rsp = out / "compile.rsp"
    rsp.write_text(command + "\n", encoding="utf-8")
    vcvars = str(args.vcvars.resolve())
    bat = out / "compile.bat"
    bat.write_text('@echo off\ncall "' + vcvars + '" >nul 2>nul\ncl.exe @"' + str(rsp) + '"\n')
    with (out / "compile.log").open("wb") as log:
        subprocess.run(["cmd.exe", "/c", str(bat)], cwd=build,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    def np(path):
        return str(path).replace("\\", "/").replace(":", "$:").replace(" ", "$ ")
    link = (base / "link.ninja").read_text()
    # B30 override object paths stay at baseline; only link outputs change.
    link = link.replace(np(base / "bin/Vita3K.exe"), np(out / "bin/Vita3K.exe"))
    for name in ("link-obj", "Vita3K.lib", "Vita3K.pdb", "link.rsp"):
        link = link.replace(np(base / name), np(out / name))
    link = link.replace("builddir = " + np(base), "builddir = " + np(out))
    link = link.replace("CXX_EXECUTABLE_LINKER__vita3k_RelWithDebInfo ",
                        "CXX_EXECUTABLE_LINKER__vita3k_RelWithDebInfo " + np(obj) + " ", 1)
    (out / "link.ninja").write_text(link, encoding="utf-8")
    bat = out / "link.bat"
    bat.write_text('@echo off\ncall "' + vcvars + '" >nul 2>nul\n'
                   'set "TEMP=' + str(out / "tmp") + '"\nset "TMP=' + str(out / "tmp") + '"\n'
                   '"' + str(args.ninja.resolve()) + '" -f "' + str(out / "link.ninja") + '"\n')
    with (out / "link.log").open("wb") as log:
        subprocess.run(["cmd.exe", "/c", str(bat)], cwd=build,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    runtime = copy_runtime(base / "bin", out / "bin", upstream, base / "bin")
    config_bytes = args.runtime_config.read_bytes()
    (out / "bin/config.yml").write_bytes(config_bytes)
    if sha(source) != PIN or sha(executable) != baseline["executable_sha256"]:
        raise ValueError("read-only baseline drift")
    for path, digest in pins.items():
        if sha(path) != digest:
            raise ValueError("link input changed during build: " + path)
    report = dict(source=str(source), source_sha256=PIN, patched_sha256=sha(copied),
                  object_sha256=sha(obj), baseline_sha256=sha(executable),
                  executable_sha256=sha(out / "bin/Vita3K.exe"), runtime=runtime,
                  link_inputs_sha256=pins,
                  runtime_config_sha256=sha(args.runtime_config),
                  scope="isolated-host-diagnostic-backend; no Vita hardware claim")
    (out / "build.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items()
                      if k not in ("runtime", "link_inputs_sha256")}, indent=2))


if __name__ == "__main__":
    main()

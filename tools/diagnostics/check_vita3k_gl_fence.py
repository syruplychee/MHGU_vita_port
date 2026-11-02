"""Exercise an external Vita3K Fence implementation with scripted GL results.

No third-party source is vendored. Generated sources and executables belong in
an ignored build directory. This checks CPU state transitions, not GPU timing.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


GL_HEADER = r"""
#pragma once
using GLenum = unsigned; using GLint = int; using GLsizei = int;
using GLbitfield = unsigned; using GLuint64 = unsigned long long;
using GLsync = void *;
constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE=0x9117, GL_SYNC_STATUS=0x9114;
constexpr GLenum GL_UNSIGNALED=0x9118, GL_SIGNALED=0x9119;
constexpr GLenum GL_ALREADY_SIGNALED=0x911a, GL_TIMEOUT_EXPIRED=0x911b;
constexpr GLenum GL_CONDITION_SATISFIED=0x911c, GL_WAIT_FAILED=0x911d;
constexpr GLbitfield GL_SYNC_FLUSH_COMMANDS_BIT=1;
GLsync glFenceSync(GLenum, GLbitfield);
GLenum glClientWaitSync(GLsync, GLbitfield, GLuint64);
void glGetSynciv(GLsync, GLenum, GLsizei, GLint *, GLint *);
void glDeleteSync(GLsync);
"""

HARNESS = r"""
#include <renderer/gl/fence.h>
#include <cstdio>
#include <cstdint>
static unsigned next_id, waits[16], queries, deletes;
static GLenum wait_result=GL_ALREADY_SIGNALED;
GLsync glFenceSync(GLenum, GLbitfield) {
    return reinterpret_cast<GLsync>(static_cast<uintptr_t>(++next_id));
}
GLenum glClientWaitSync(GLsync p, GLbitfield, GLuint64) {
    ++waits[reinterpret_cast<uintptr_t>(p)]; return wait_result;
}
void glGetSynciv(GLsync, GLenum, GLsizei, GLint *length, GLint *status) {
    ++queries; if(length) *length=1; *status=GL_SIGNALED;
}
void glDeleteSync(GLsync) { ++deletes; }
static void result(const char *name, bool ok) {
    std::printf("%s %s\n", name, ok ? "PASS" : "FAIL");
}
int main() {
    {
        renderer::gl::Fence f;
        f.insert(); bool first=f.wait_for_signal();
        f.insert(); bool second=f.wait_for_signal();
        result("two_generations", first && second && waits[1]==1 && waits[2]==1);
    }
    {
        renderer::gl::Fence f; wait_result=GL_TIMEOUT_EXPIRED;
        f.insert(); bool ok=f.wait_for_signal();
        result("polled_completion", ok && queries==1 && f.empty());
    }
    {
        renderer::gl::Fence f; wait_result=GL_ALREADY_SIGNALED;
        f.insert(); bool ok=f.wait_for_signal();
        result("immediate_completion", ok && waits[next_id]==1 && f.empty());
    }
    {
        renderer::gl::Fence f; wait_result=GL_WAIT_FAILED;
        unsigned before=deletes;
        f.insert(); bool ok=f.wait_for_signal();
        result("failed_wait", !ok && deletes==before+1 && f.empty());
    }
    {
        renderer::gl::Fence f; wait_result=GL_CONDITION_SATISFIED;
        f.insert(); unsigned old=next_id;
        f.insert(); unsigned fresh=next_id;
        bool ok=f.wait_for_signal();
        result("insert_pending_generation", ok && waits[old]==1 && waits[fresh]==1);
    }
}
"""


def minimal_fix(source):
    anchors = [
        ("    sync_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);",
         "    signaled_ = false;\n"
         "    sync_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);"),
        ("            if (status == GL_SIGNALED) {\n                break;",
         "            if (status == GL_SIGNALED) {\n"
         "                error = GL_CONDITION_SATISFIED;\n                break;"),
    ]
    for old, new in anchors:
        if source.count(old) != 1:
            raise ValueError("External implementation changed; review before testing")
        source = source.replace(old, new)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_root", type=Path, help="External Vita3K checkout")
    parser.add_argument("--out", required=True, type=Path, help="Fresh build directory")
    parser.add_argument("--vcvars", required=True, type=Path)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    original = source_root / "vita3k/renderer/src/gl/fence.cpp"
    include_root = source_root / "vita3k/renderer/include"
    source = original.read_text(encoding="utf-8-sig")
    variants = {"baseline": source, "fixed": minimal_fix(source)}
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    for name in ("glutil", "util"):
        (out / name).mkdir()
    (out / "glutil/gl.h").write_text(GL_HEADER, encoding="utf-8")
    (out / "util/log.h").write_text("#define LOG_ERROR(...) ((void)0)\n", encoding="utf-8")
    harness = out / "harness.cpp"
    harness.write_text(HARNESS, encoding="utf-8")
    receipt = {"source": str(original), "source_sha256": hashlib.sha256(
        original.read_bytes()).hexdigest(), "evidence": "L1", "variants": {}}
    for name, text in variants.items():
        directory = out / name
        directory.mkdir()
        cpp = directory / "fence.cpp"
        exe = directory / "check.exe"
        cpp.write_text(text, encoding="utf-8")
        batch = directory / "build.cmd"
        batch.write_text(
            f'@echo off\ncall "{args.vcvars.resolve()}" >nul 2>nul\n'
            f'cl /nologo /EHsc /std:c++17 /I"{out}" /I"{include_root}" '
            f'"{cpp}" "{harness}" /Fe:"{exe}"\nexit /b %ERRORLEVEL%\n',
            encoding="utf-8")
        with (directory / "compile.log").open("w", encoding="utf-8") as log:
            subprocess.run(["cmd", "/c", str(batch)], cwd=directory,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        output = subprocess.check_output([str(exe)], text=True)
        receipt["variants"][name] = dict(line.split() for line in output.splitlines())
    expected_failures = {"two_generations", "polled_completion", "insert_pending_generation"}
    baseline = receipt["variants"]["baseline"]
    fixed = receipt["variants"]["fixed"]
    reproduced = {key for key, value in baseline.items() if value == "FAIL"}
    receipt["expected_result"] = (baseline.keys() == fixed.keys() and
                                  reproduced == expected_failures and
                                  len(fixed) == 5 and all(v == "PASS" for v in fixed.values()))
    (out / "result.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    print(json.dumps(receipt, indent=2))
    return 0 if receipt["expected_result"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

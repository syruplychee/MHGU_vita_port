"""Build an isolated Vita3K diagnostic backend for B44.

Only patched translation units are recompiled with the pinned MSVC command
lines from the read-only build-debug compilation database; the resulting
executable is linked in a new artifact directory and the runtime allowlist is
copied from the preserved B30 backend. The upstream source/build trees, the
B30 backend and all prior evidence remain untouched.

The B44 surface-cache patch only adds logging around cache erasure and
framebuffer creation. The draw patch reports framebuffer attachments and texture levels after a GL
error; the texture patch records selection and compressed storage bindings.
These patches query state without reattaching or repairing objects. None
changes rendering behaviour; this tool does not produce a rendering fix.

With --fix-target-reuse, additionally recreate a texture-cache slot's owned GL
object when its immutable target changes. This is a host backend candidate;
the native Vita renderer is not patched.
"""
import argparse
import difflib
import hashlib
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH_SRC = ROOT / 'tools/shader/patches'
TURNS = (
    ('spirv_recompiler', 'vita3k/shader/src/spirv_recompiler.cpp',
     'e96d630524a7cab3057bc60d97e859c0e52db3e9f8dceb9f56a78254289db842',
     ('p5-temp-array.patch',)),
    ('compile_program', 'vita3k/renderer/src/gl/compile_program.cpp',
     '5faf3c3682e61a392ef634342f620b38c5bd07ee55c22dbcc94556d4b7406d31',
     ('p5-backend-evidence.patch',)),
    ('usse_program_analyzer', 'vita3k/shader/src/usse_program_analyzer.cpp',
     '1041c3d4fdb36ed1cecbc4ad8f82b590288c452b0d9c28a19e0a784f7d1731a8',
     ('p5-multicall-analyzer.patch',)),
    ('SceLibKernel', 'vita3k/modules/SceLibKernel/SceLibKernel.cpp',
     '197d82971d1a1525bf7e2f35b9a80b6ed9c7eba6fc5e5f1254fc6bf4f9987caf',
     ('p5-app-exit-evidence.patch',)),
    ('SceIofilemgr', 'vita3k/modules/SceIofilemgr/SceIofilemgr.cpp',
     '44826afb692fa55dd4ff2d12a7fe350e36b30089bf01bc2af6fcdfde2360633d',
     ('p5-probe-completion-evidence.patch',)),
    ('surface_cache', 'vita3k/renderer/src/gl/surface_cache.cpp',
     'b2e73b4ae6f5601d9356fca4ba34b1f74dde90ca2fa3fed563e66dc29a87db48',
     ('m6-depth-preservation.patch', 'b44-surface-cache-trace.patch')),
    ('sync_state', 'vita3k/renderer/src/gl/sync_state.cpp',
     'a714bfe4634df5c074944f67aa6e65d976f9cf6f85b4e29928e75793ad6ad668',
     ('m6b30-safe-texture-trace.patch',)),
    ('draw', 'vita3k/renderer/src/gl/draw.cpp',
     '5b1356ea9cfee45f6cfad18158fadc65885d11d53b4a6ea924022fa0dad22fd3',
     ('b44-draw-fbo-trace-v3pure.patch',)),
    ('texture', 'vita3k/renderer/src/gl/texture.cpp', '6709474f8760cc0d3e35ecd1b3f76dd0800541582370e0298cabfa5365bc114a', ('b44-texture-bind-trace.patch',)),
)
RUNTIME_DIRS = ('data', 'icons', 'imageformats', 'platforms', 'styles',
                'translations', 'gui-configs', 'tls', 'plugins',
                'shaders-builtin')
SKIP_DIRS = {'cache', 'logs', 'patch', 'shaderlog', 'texturelog'}
HEADER = 'vita3k/glutil/include/glutil/object_array.h'
HEADER_PIN = '8fa08ee8c6d6d46beed1521dba519d584782ae89d0978633e9f5b84b6dfcd20d'


def sha(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def apply_patch(text, patch):
    hunks = re.split(r'^@@[^\n]*\n', patch, flags=re.M)[1:]
    if not hunks:
        raise ValueError('patch contains no hunks')
    for hunk in hunks:
        old, new = [], []
        for line in hunk.splitlines(keepends=True):
            if line in ('\n', '\r\n'):
                line = ' ' + line  # blank context line lost its marker
            if line[:1] in (' ', '-'):
                old.append(line[1:])
            if line[:1] in (' ', '+'):
                new.append(line[1:])
        before, after = ''.join(old), ''.join(new)
        if not before or text.count(before) != 1:
            raise ValueError('patch context does not match uniquely')
        text = text.replace(before, after, 1)
    return text


def np(path):
    return str(path).replace('\\', '/').replace(':', '$:').replace(' ', '$ ')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--upstream', type=Path, required=True)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--ninja', type=Path, required=True)
    ap.add_argument('--vcvars', type=Path, required=True)
    ap.add_argument('--runtime-template', type=Path, required=True)
    ap.add_argument('--fix-target-reuse', action='store_true')
    args = ap.parse_args()
    upstream, build, out = (p.resolve() for p in
                            (args.upstream, args.build, args.output))
    if out.exists():
        raise ValueError('output must be a new directory; preserve evidence')
    db = json.loads((build / 'compile_commands.json').read_text())
    (out / 'src').mkdir(parents=True)
    (out / 'bin').mkdir()
    (out / 'tmp').mkdir()
    header_record = None
    if args.fix_target_reuse:
        header = upstream / HEADER
        if sha(header) != HEADER_PIN:
            raise ValueError('header identity mismatch: ' + HEADER)
        patched_header = apply_patch(header.read_text(encoding='utf-8'),
            (PATCH_SRC / 'b44-object-recreate.patch').read_text(encoding='utf-8'))
        header_copy = out / 'include/glutil/object_array.h'
        header_copy.parent.mkdir(parents=True)
        header_copy.write_text(patched_header, encoding='utf-8', newline='\n')
        header_record = dict(source=HEADER, original_sha256=HEADER_PIN,
                             patched_sha256=sha(header_copy))
    records = []
    for name, rel, digest, patches in TURNS:
        if name == 'texture' and args.fix_target_reuse:
            patches = (*patches, 'b44-texture-target-reuse.patch')
        path = upstream / rel
        if sha(path) != digest:
            raise ValueError('source identity mismatch: ' + rel)
        original = path.read_text(encoding='utf-8')
        patched = original
        for patch_name in patches:
            candidate = Path(__file__).parent / 'patches' / patch_name
            if not candidate.is_file():
                candidate = PATCH_SRC / patch_name
            patched = apply_patch(patched, candidate.read_text(encoding='utf-8'))
        source, obj = out / 'src' / (name + '.cpp'), out / (name + '.obj')
        source.write_text(patched, encoding='utf-8', newline='\n')
        (out / (name + '.patch')).write_text(''.join(difflib.unified_diff(
            original.splitlines(True), patched.splitlines(True),
            fromfile=rel, tofile=rel)), encoding='utf-8', newline='\n')
        entry = next(e for e in db
                     if e['file'].replace('\\', '/').endswith('/' + name + '.cpp'))
        command = re.sub(r'^\s*(?:"[^"]*cl\.exe"|\S*cl\.exe)\s+', '',
                         entry['command'], flags=re.I)
        command = '/I"' + str(path.parent) + '" ' + command
        if args.fix_target_reuse:
            command = '/I"' + str(out / 'include') + '" ' + command
        command = re.sub(r'/Fo\S+', lambda m: '/Fo"' + str(obj) + '"', command)
        command = re.sub(r'/Fd\S+', lambda m: '/Fd"' + str(obj.with_suffix('.pdb')) + '"',
                         command)
        command = re.sub(r'-c\s+\S+$', lambda m: '-c "' + str(source) + '"', command)
        rsp = out / (name + '.rsp')
        rsp.write_text(command + '\n', encoding='utf-8')
        bat = out / (name + '.bat')
        bat.write_text('@echo off\ncall "' + str(args.vcvars.resolve()) +
                       '" >nul 2>nul\ncl.exe @"' + str(rsp) + '"\n')
        with (out / (name + '.log')).open('wb') as log:
            subprocess.run(['cmd.exe', '/c', str(bat)], cwd=build,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        records.append(dict(name=name, source=rel, original_sha256=digest,
                            patched_sha256=sha(source), object_sha256=sha(obj),
                            rsp_sha256=sha(rsp), patches=list(patches)))
    lines = (build / 'build.ninja').read_text().splitlines()
    start = next(i for i, line in enumerate(lines)
                 if line.startswith('build bin\\Vita3K.exe:'))
    rule, *inputs = lines[start].split(':', 1)[1].split('|', 1)[0].split()
    variables = {}
    for line in lines[start + 1:]:
        if not line.startswith('  '):
            break
        key, _, value = line.strip().partition(' = ')
        variables[key] = value
    executable = out / 'bin/Vita3K.exe'
    objdir = out / 'link-obj'
    objdir.mkdir()
    nl = ['include CMakeFiles/rules.ninja', 'builddir = ' + np(out), '',
          'build %s: %s %s %s' % (np(executable), rule,
              ' '.join(np(out / (n + '.obj')) for n, *_ in TURNS),
              ' '.join(inputs))]
    values = {k: variables[k] for k in
              ('CONFIG', 'FLAGS', 'LINK_FLAGS', 'LINK_LIBRARIES')}
    values.update(
        MANIFESTS=np(upstream / 'vita3k/Windows.manifest'),
        OBJECT_DIR=np(objdir), PRE_LINK='cd .', POST_BUILD='cd .',
        TARGET_FILE=np(executable), TARGET_IMPLIB=np(out / 'Vita3K.lib'),
        TARGET_PDB=np(out / 'Vita3K.pdb'), RSP_FILE=np(out / 'link.rsp'))
    nl.extend('  %s = %s' % item for item in values.items())
    ninja = out / 'link.ninja'
    ninja.write_text('\n'.join(nl) + '\n', encoding='utf-8')
    bat = out / 'link.bat'
    bat.write_text('@echo off\ncall "' + str(args.vcvars.resolve()) + '" >nul 2>nul\n'
                   'set "TEMP=' + str(out / 'tmp') + '"\nset "TMP=' + str(out / 'tmp') +
                   '"\n"' + str(args.ninja.resolve()) + '" -f "' + str(ninja) +
                   '" "' + str(executable) + '"\n')
    with (out / 'link.log').open('wb') as log:
        subprocess.run(['cmd.exe', '/c', str(bat)], cwd=build,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    template = args.runtime_template.resolve()
    copied = {}
    for source in sorted(template.iterdir()):
        if not source.is_file():
            continue
        if source.suffix.lower() not in ('.dll', '.yml', '.txt'):
            continue
        if source.name.lower().endswith('.ilk'):
            continue
        shutil.copyfile(source, out / 'bin' / source.name)
        copied[source.name] = sha(source)
    for root, dirs, files in os.walk(template):
        root_path = Path(root)
        relative = root_path.relative_to(template)
        if relative == Path('.'):
            # Top-level files were copied above; never overwrite the linked
            # executable with the preserved template's Vita3K.exe.
            dirs[:] = [d for d in dirs if d in RUNTIME_DIRS]
            continue
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for filename in files:
            if filename.lower().endswith(('.log', '.ilk', '.pdb')):
                continue
            source = root_path / filename
            target = out / 'bin' / source.relative_to(template)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
    for name, rel, digest, _ in TURNS:
        if sha(upstream / rel) != digest:
            raise ValueError('upstream source changed during build: ' + rel)
    if args.fix_target_reuse and sha(upstream / HEADER) != HEADER_PIN:
        raise ValueError('upstream header changed during build')
    summary = dict(schema=1, sources=records, runtime_inputs=copied,
                   target_reuse_fix=args.fix_target_reuse, header=header_record,
                   executable=str(executable), executable_sha256=sha(executable),
                   builder_sha256=sha(Path(__file__)))
    (out / 'build.json').write_text(json.dumps(summary, indent=2) + '\n',
                                    encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items()
                      if k != 'runtime_inputs'}, indent=2))


if __name__ == '__main__':
    main()

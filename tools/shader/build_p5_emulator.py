"""Build an independent P5 diagnostic emulator from pinned local source inputs.

The original source/build and locked executable remain read-only. Only patched
translation units are recompiled; a separate executable is linked against the
existing build's libraries. Source/patch/object/library hashes are recorded.
This is a host validation tool, never part of the Vita production package.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_depth_preservation_emulator import copy_runtime as copy_runtime_allowlisted

PINS = {
    'spirv_recompiler': ('vita3k/shader/src/spirv_recompiler.cpp',
                       'e96d630524a7cab3057bc60d97e859c0e52db3e9f8dceb9f56a78254289db842',
                       'p5-temp-array.patch'),
    'compile_program': ('vita3k/renderer/src/gl/compile_program.cpp',
                       '5faf3c3682e61a392ef634342f620b38c5bd07ee55c22dbcc94556d4b7406d31',
                       'p5-backend-evidence.patch'),
    'usse_program_analyzer': ('vita3k/shader/src/usse_program_analyzer.cpp',
                            '1041c3d4fdb36ed1cecbc4ad8f82b590288c452b0d9c28a19e0a784f7d1731a8',
                            'p5-multicall-analyzer.patch'),
    'SceLibKernel': ('vita3k/modules/SceLibKernel/SceLibKernel.cpp',
                     '197d82971d1a1525bf7e2f35b9a80b6ed9c7eba6fc5e5f1254fc6bf4f9987caf',
                     'p5-app-exit-evidence.patch'),
    'SceIofilemgr': ('vita3k/modules/SceIofilemgr/SceIofilemgr.cpp',
                     '44826afb692fa55dd4ff2d12a7fe350e36b30089bf01bc2af6fcdfde2360633d',
                     'p5-probe-completion-evidence.patch'),
    'surface_cache': ('vita3k/renderer/src/gl/surface_cache.cpp',
                      'b2e73b4ae6f5601d9356fca4ba34b1f74dde90ca2fa3fed563e66dc29a87db48',
                      'm6-depth-preservation.patch'),
    'sync_state': ('vita3k/renderer/src/gl/sync_state.cpp',
                   'a714bfe4634df5c074944f67aa6e65d976f9cf6f85b4e29928e75793ad6ad668',
                   'm6b30-safe-texture-trace.patch'),
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def patch_text(text, patch):
    """Apply our small pinned hunks, requiring exact unique context for each."""
    hunks = re.split(r'^@@[^\n]*\n', patch, flags=re.M)[1:]
    for hunk in hunks:
        old, new = [], []
        for line in hunk.splitlines(keepends=True):
            if line[:1] in (' ', '-'):
                old.append(line[1:])
            if line[:1] in (' ', '+'):
                new.append(line[1:])
        before, after = ''.join(old), ''.join(new)
        if not before or text.count(before) != 1:
            raise ValueError('patch context does not match uniquely')
        text = text.replace(before, after, 1)
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--upstream', type=Path, required=True)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--ninja', type=Path, required=True)
    ap.add_argument('--vcvars', type=Path, required=True)
    ap.add_argument('--runtime-template', type=Path,
                    help='read-only template for DLLs, plugins and UI assets; caches are excluded')
    ap.add_argument('--multicall', action='store_true',
                    help='also handle multiple preceding Shacc helpers by their BL target/return')
    ap.add_argument('--depth-preservation', action='store_true',
                    help='recompile the pinned surface_cache.cpp with the '
                         'B16 depth-preservation patch; no other source changes')
    ap.add_argument('--safe-texture-trace', action='store_true',
                    help='recompile the pinned sync_state.cpp with the M6-B30 '
                         'patch that removes unsafe CPU texel reads while '
                         'retaining metadata logging; no sampler behavior changes')
    args = ap.parse_args()
    upstream, build, out = (p.resolve() for p in (args.upstream, args.build, args.output))
    if out.is_relative_to(upstream) or upstream.is_relative_to(out):
        raise ValueError('output and upstream must be independent')
    if out.exists():
        raise ValueError('output must be a new directory; preserve previous evidence')
    db_path = build / 'compile_commands.json'
    db = json.loads(db_path.read_text())
    sources = {}
    names = ['spirv_recompiler', 'compile_program', 'SceLibKernel', 'SceIofilemgr']
    if args.multicall:
        names.append('usse_program_analyzer')
    if args.depth_preservation:
        names.append('surface_cache')
    if args.safe_texture_trace:
        names.append('sync_state')
    # Validate all source inputs before writing anything.
    for name in names:
        rel, digest, patch = PINS[name]
        path = upstream / rel
        if sha(path) != digest:
            raise ValueError('source identity mismatch: ' + rel)
        original = path.read_text(encoding='utf-8')
        if patch:
            patched = patch_text(original, (Path(__file__).parent / 'patches' / patch).read_text())
        sources[name] = (original, patched, digest, rel)
    (out / 'src').mkdir(parents=True)
    (out / 'bin').mkdir()
    (out / 'tmp').mkdir()
    records = []
    for name, (original, patched, digest, rel) in sources.items():
        source, obj = out / 'src' / (name + '.cpp'), out / (name + '.obj')
        source.write_text(patched, encoding='utf-8', newline='\n')
        (out / (name + '.patch')).write_text(''.join(difflib.unified_diff(
            original.splitlines(True), patched.splitlines(True), fromfile=rel, tofile=rel)),
            encoding='utf-8', newline='\n')
        entry = next(e for e in db if e['file'].replace('\\', '/').endswith('/' + name + '.cpp'))
        command = re.sub(r'^\s*(?:"[^"]*cl\.exe"|\S*cl\.exe)\s+', '', entry['command'], flags=re.I)
        command = '/I"' + str((upstream / rel).parent) + '" ' + command
        command = re.sub(r'/Fo\S+', lambda m: '/Fo"' + str(obj) + '"', command)
        command = re.sub(r'/Fd\S+', lambda m: '/Fd"' + str(obj.with_suffix('.pdb')) + '"', command)
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
                            patched_sha256=sha(source), object_sha256=sha(obj), rsp_sha256=sha(rsp)))
    lines = (build / 'build.ninja').read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith('build bin\\Vita3K.exe:'))
    rule, *inputs = lines[start].split(':', 1)[1].split('|', 1)[0].split()
    variables = {}
    for line in lines[start + 1:]:
        if not line.startswith('  '):
            break
        key, _, value = line.strip().partition(' = ')
        variables[key] = value
    def np(path):
        return str(path).replace('\\', '/').replace(':', '$:').replace(' ', '$ ')
    executable = out / 'bin/Vita3K.exe'
    objdir = out / 'link-obj'
    objdir.mkdir()
    nl = ['include CMakeFiles/rules.ninja', 'builddir = ' + np(out), '',
          'build %s: %s %s %s' % (np(executable), rule,
              ' '.join(np(out / (n + '.obj')) for n in names), ' '.join(inputs))]
    values = {k: variables[k] for k in ('CONFIG', 'FLAGS', 'LINK_FLAGS', 'LINK_LIBRARIES')}
    values.update(MANIFESTS=np(upstream / 'vita3k/Windows.manifest'), OBJECT_DIR=np(objdir),
                  PRE_LINK='cd .', POST_BUILD='cd .', TARGET_FILE=np(executable),
                  TARGET_IMPLIB=np(out / 'Vita3K.lib'), TARGET_PDB=np(out / 'Vita3K.pdb'),
                  RSP_FILE=np(out / 'link.rsp'))
    nl.extend('  %s = %s' % item for item in values.items())
    ninja = out / 'link.ninja'
    ninja.write_text('\n'.join(nl) + '\n')
    bat = out / 'link.bat'
    bat.write_text('@echo off\ncall "' + str(args.vcvars.resolve()) + '" >nul 2>nul\n'
                   'set "TEMP=' + str(out / 'tmp') + '"\nset "TMP=' + str(out / 'tmp') + '"\n'
                   '"' + str(args.ninja.resolve()) + '" -f "' + str(ninja) + '" "' + str(executable) + '"\n')
    with (out / 'link.log').open('wb') as log:
        subprocess.run(['cmd.exe', '/c', str(bat)], cwd=build,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    library_inputs = {}
    for name in set(inputs + variables['LINK_LIBRARIES'].split()):
        path = build / name
        if path.is_file():
            library_inputs[str(path)] = sha(path)
    runtime_inputs = {}
    if args.runtime_template:
        template = args.runtime_template.resolve()
        if args.depth_preservation:
            supplement_dir = build / 'external/openssl/x64/bin'
            if not supplement_dir.is_dir():
                supplement_dir = build / 'bin'
            runtime_records, runtime_supplements, _license = \
                copy_runtime_allowlisted(template, out / 'bin', upstream,
                                         supplement_dir)
            runtime_inputs.update(runtime_records)
            runtime_inputs.update(runtime_supplements)
        else:
            for source in template.rglob('*'):
                relative = source.relative_to(template)
                if not source.is_file() or (len(relative.parts) == 1 and source.suffix.lower() != '.dll'):
                    continue
                if len(relative.parts) > 1 and relative.parts[0] not in (
                        'data', 'icons', 'platforms', 'imageformats', 'styles', 'translations',
                        'gui-configs', 'tls', 'plugins', 'shaders-builtin'):
                    continue
                destination = out / 'bin' / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, destination)
                runtime_inputs[relative.as_posix()] = sha(source)
    shutil.copyfile(__file__, out / 'build-driver.py')
    summary = dict(schema=1, multicall=args.multicall,
                   depth_preservation=args.depth_preservation,
                   safe_texture_trace=args.safe_texture_trace,
                   runtime_allowlist='M6-B16' if args.depth_preservation else 'p5-legacy',
                   tool_sha256=sha(__file__), compile_database_sha256=sha(db_path),
                   sources=records, libraries=library_inputs, runtime_inputs=runtime_inputs,
                   executable=str(executable), executable_sha256=sha(executable))
    (out / 'build.json').write_text(json.dumps(summary, indent=2) + '\n')
    shutil.copyfile(upstream / 'COPYING.txt', out / 'bin/COPYING.txt')
    print(json.dumps({k: v for k, v in summary.items() if k not in ('libraries', 'runtime_inputs')}, indent=2))


if __name__ == '__main__':
    main()

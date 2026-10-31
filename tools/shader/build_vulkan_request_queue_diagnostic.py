"""Build an isolated Vita3K Vulkan request-queue diagnostic backend.

The blocker this tool targets is in the pinned emulator, not in native sceGxm:
Vita3K's renderer::finish() pushes a callback to VKState::request_queue and
blocks on it whenever the Vulkan backend uses memory mapping. That queue is
drained only by VKContext::gpu_request_wait_thread, which is created when the
first GXM context is constructed. A sceGxmTransferFinish() before any context
(such as the native loader's GXM bring-up) therefore waits forever.

The existing P5 temp-bank and USSE multicall fixes are carried forward because
their shader frontend is shared by Vulkan. No OpenGL renderer patches are used.
Only the patched translation units are recompiled with the pinned MSVC command
lines from the read-only build-debug compilation database; the resulting
executable is linked in a new artifact directory and the runtime allowlist is
copied from an existing Vulkan runtime template. The upstream source/build
trees and all prior evidence remain untouched.

Without --fix-request-queue the candidate only traces the finish path and still
hangs, which is the unfixed control. With the flag, finish() skips the callback
wait when no request-queue consumer exists; the synchronous NOP already fenced
command execution. This is a host backend candidate; native Vita rendering is
not patched.
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
HEADER_ASSET = PATCH_SRC / 'b45-request-queue-owner.h'
HEADER_REL = 'renderer/vulkan/request_queue_owner.h'
WRITE_STAGING_HEADER = PATCH_SRC / 'b46-file-write-staging.h'
WRITE_STAGING_REL = 'io/file_write_staging.h'
TURNS = (
    ('spirv_recompiler', 'vita3k/shader/src/spirv_recompiler.cpp',
     'e96d630524a7cab3057bc60d97e859c0e52db3e9f8dceb9f56a78254289db842',
     ('p5-temp-array.patch',)),
    ('usse_program_analyzer', 'vita3k/shader/src/usse_program_analyzer.cpp',
     '1041c3d4fdb36ed1cecbc4ad8f82b590288c452b0d9c28a19e0a784f7d1731a8',
     ('p5-multicall-analyzer.patch',)),
    ('SceLibKernel', 'vita3k/modules/SceLibKernel/SceLibKernel.cpp',
     '197d82971d1a1525bf7e2f35b9a80b6ed9c7eba6fc5e5f1254fc6bf4f9987caf',
     ('p5-app-exit-evidence.patch',)),
    ('sync', 'vita3k/renderer/src/sync.cpp',
     '9deb5bc9044c4a1e9e4640042fc1d9a2ea45f9406760db203ad030d8f451a1fd',
     ('b45-vulkan-request-queue-trace.patch',)),
    ('vulkan_creation', 'vita3k/renderer/src/vulkan/creation.cpp',
     'cff1f2da0eff2facc56c877b090ceb6947ab01476f0d67d398b4a6356c5cdcc1',
     ('b45-vulkan-request-queue-owner.patch',)),
)
# Added only for --fix-file-write so the B45 control keeps its exact source set.
WRITE_STAGING_TURN = (
    'io_state_functions', 'vita3k/io/src/state_functions.cpp',
    '9c8be9830cdadb6e5398b69c45c5928e1ce3e10b66546450fc9fe21f701fb95b',
    ('b46-file-write-staging.patch',))
# B48 audio lifecycle fix. AudioState::open_port dereferenced a null adapter
# after a failed backend init (the observed cubeb "Could not initialize cubeb
# context" path), so the guest's sceAudioOutOpenPort crashed the host instead
# of receiving SCE_AUDIO_OUT_ERROR_NOT_OPENED. The patch returns a real open
# failure and only sets port volume when both adapter and port exist.
AUDIO_TURN = (
    'audio_state', 'vita3k/audio/src/audio.cpp',
    '17c5626ce782217691fa62e539c4dc0ac9c3b711f653e7fc6ecc7f6e821c385c',
    ('b48-audio-adapter-guard.patch',))
AUDIO_BACKENDS = ('SDL', 'Cubeb')
# B47 diagnostics: log the GPU/CPU surface synchronization decisions for the
# selftest surface address range (0x70000000..0x80000000) only. Diagnostic
# candidate; not used for acceptance. The texture-lookup trace keeps the
# original dirty-branch semantics (braces added around the existing return).
# The scene trace logs draw state for the same address range.
SCENE_SHA = '18ebfd998b063b01a5525c7c71d70730c34c39514d5097c986816b82097a135f'
B47_SCENE_TURN = (
    'vulkan_scene', 'vita3k/renderer/src/vulkan/scene.cpp', SCENE_SHA,
    ('b47-scene-draw-trace.patch',))
B47_TRACE_TURNS = (
    ('vulkan_surface_cache', 'vita3k/renderer/src/vulkan/surface_cache.cpp',
     '03291b4b345d63b646358b5716b42f6f1d9aa85f0ca33e7422abfa6e1124c261',
     ('b47-surface-sync-trace-fixed.patch',)),
    ('vulkan_context', 'vita3k/renderer/src/vulkan/context.cpp',
     '217e1218f72d91e67237251cf85cab3bd287d7d8753f6f03f7c9528e1695aa14',
     ('b47-context-sync-trace-fixed.patch',)),
    B47_SCENE_TURN,
)
# B47 fix: copy a completed scene's linear color surface back to guest memory
# at scene completion, so the first guest read after sceGxmFinish is coherent.
B47_FIX_TURNS = (
    ('vulkan_surface_cache', 'vita3k/renderer/src/vulkan/surface_cache.cpp',
     '03291b4b345d63b646358b5716b42f6f1d9aa85f0ca33e7422abfa6e1124c261',
     ('b47-force-surface-writeback.patch',)),
    ('vulkan_context', 'vita3k/renderer/src/vulkan/context.cpp',
     '217e1218f72d91e67237251cf85cab3bd287d7d8753f6f03f7c9528e1695aa14',
     ('b47-context-force-writeback.patch',)),
)
# Fix plus diagnostic in one patched translation unit per source. The separate
# trace/fix patch sets cannot be stacked because both rewrite the same sync
# guard, so this combined set is generated from the same upstream text.
B47_COMBINED_TURNS = (
    ('vulkan_surface_cache', 'vita3k/renderer/src/vulkan/surface_cache.cpp',
     '03291b4b345d63b646358b5716b42f6f1d9aa85f0ca33e7422abfa6e1124c261',
     ('b47-combined-surface.patch',)),
    ('vulkan_context', 'vita3k/renderer/src/vulkan/context.cpp',
     '217e1218f72d91e67237251cf85cab3bd287d7d8753f6f03f7c9528e1695aa14',
     ('b47-combined-context.patch',)),
    B47_SCENE_TURN,
)
SURFACE_CACHE_HEADER_REL = 'vita3k/renderer/include/renderer/vulkan/surface_cache.h'
SURFACE_CACHE_HEADER_SHA = '1615fdc2b4b3fe680aff1e6eedcab6692e79542615b4e5bfa2f3f8239dc8f069'
SURFACE_CACHE_HEADER_OLD = '    ColorSurfaceCacheInfo *perform_surface_sync();'
SURFACE_CACHE_HEADER_NEW = '    ColorSurfaceCacheInfo *perform_surface_sync(bool force = false);'
RUNTIME_DIRS = ('data', 'icons', 'imageformats', 'platforms', 'styles',
                'translations', 'gui-configs', 'tls', 'plugins',
                'shaders-builtin')
SKIP_DIRS = {'cache', 'logs', 'patch', 'shaderlog', 'texturelog'}


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


def rewrite_audio_backend(text, backend):
    """Return the base config with exactly one audio-backend line for backend.

    The YAML merge ignores a requested value equal to the built-in default
    (SDL), so the executable-directory base config must agree with the
    requested config; otherwise an old Cubeb value survives the merge.
    """
    if backend not in AUDIO_BACKENDS:
        raise ValueError('unsupported audio backend: ' + str(backend))
    pattern = re.compile(r'(?m)^audio-backend:[^\r\n]*$')
    if len(pattern.findall(text)) != 1:
        raise ValueError('base config must contain exactly one audio-backend line')
    return pattern.sub('audio-backend: ' + backend, text)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--upstream', type=Path, required=True)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--ninja', type=Path, required=True)
    ap.add_argument('--vcvars', type=Path, required=True)
    ap.add_argument('--runtime-template', type=Path, required=True)
    ap.add_argument('--fix-request-queue', action='store_true')
    ap.add_argument('--fix-file-write', action='store_true')
    ap.add_argument('--trace-surface-sync', action='store_true')
    ap.add_argument('--fix-surface-writeback', action='store_true')
    ap.add_argument('--fix-audio-adapter', action='store_true')
    ap.add_argument('--base-audio-backend', choices=AUDIO_BACKENDS)
    args = ap.parse_args()
    upstream, build, out = (p.resolve() for p in
                            (args.upstream, args.build, args.output))
    if out.exists():
        raise ValueError('output must be a new directory; preserve evidence')
    if not (args.runtime_template / 'config.yml').is_file():
        raise ValueError('runtime template must carry its base config.yml')
    template_config = (args.runtime_template / 'config.yml').read_text(encoding='utf-8')
    if not re.search(r'(?m)^backend-renderer:\s*Vulkan\s*$', template_config):
        raise ValueError('runtime template base config is not Vulkan')
    header_text = HEADER_ASSET.read_text(encoding='utf-8')
    db = json.loads((build / 'compile_commands.json').read_text())
    (out / 'src').mkdir(parents=True)
    (out / 'bin').mkdir()
    (out / 'tmp').mkdir()
    include_root = out / 'include'
    header_copy = include_root / HEADER_REL
    header_copy.parent.mkdir(parents=True)
    header_copy.write_text(header_text, encoding='utf-8', newline='\n')
    header_record = dict(source=str(HEADER_ASSET.relative_to(ROOT)).replace('\\', '/'),
                         asset_sha256=sha(HEADER_ASSET),
                         installed_sha256=sha(header_copy))
    write_staging_header = None
    if args.fix_file_write:
        write_copy = include_root / WRITE_STAGING_REL
        write_copy.parent.mkdir(parents=True)
        write_copy.write_text(WRITE_STAGING_HEADER.read_text(encoding='utf-8'),
                              encoding='utf-8', newline='\n')
        write_staging_header = dict(
            source=str(WRITE_STAGING_HEADER.relative_to(ROOT)).replace('\\', '/'),
            asset_sha256=sha(WRITE_STAGING_HEADER),
            installed_sha256=sha(write_copy))
    turns = list(TURNS)
    if args.fix_file_write:
        turns.append(WRITE_STAGING_TURN)
    if args.fix_audio_adapter:
        turns.append(AUDIO_TURN)
    if args.trace_surface_sync and args.fix_surface_writeback:
        turns.extend(B47_COMBINED_TURNS)
    elif args.trace_surface_sync:
        turns.extend(B47_TRACE_TURNS)
    elif args.fix_surface_writeback:
        turns.extend(B47_FIX_TURNS)
    seen_sources = set()
    for name, rel, *_ in turns:
        if name in seen_sources:
            raise ValueError('duplicate translation unit in one build: ' + name)
        seen_sources.add(name)
    surface_sync_header = None
    if args.fix_surface_writeback:
        header_src = upstream / SURFACE_CACHE_HEADER_REL
        if sha(header_src) != SURFACE_CACHE_HEADER_SHA:
            raise ValueError('source identity mismatch: ' + SURFACE_CACHE_HEADER_REL)
        header_text = header_src.read_text(encoding='utf-8')
        if header_text.count(SURFACE_CACHE_HEADER_OLD) != 1:
            raise ValueError('surface cache header declaration does not match uniquely')
        patched_header = header_text.replace(SURFACE_CACHE_HEADER_OLD,
                                              SURFACE_CACHE_HEADER_NEW, 1)
        header_dst = include_root / 'renderer/vulkan/surface_cache.h'
        header_dst.parent.mkdir(parents=True, exist_ok=True)
        header_dst.write_text(patched_header, encoding='utf-8', newline='\n')
        surface_sync_header = dict(
            source=SURFACE_CACHE_HEADER_REL,
            original_sha256=SURFACE_CACHE_HEADER_SHA,
            installed_sha256=sha(header_dst),
            replacement='perform_surface_sync(bool force = false)')
    records = []
    for name, rel, digest, patches in turns:
        if name == 'sync' and args.fix_request_queue:
            patches = (*patches, 'b45-vulkan-request-queue-skip.patch')
        path = upstream / rel
        if sha(path) != digest:
            raise ValueError('source identity mismatch: ' + rel)
        original = path.read_text(encoding='utf-8')
        patched = original
        for patch_name in patches:
            candidate = PATCH_SRC / patch_name
            patched = apply_patch(patched, candidate.read_text(encoding='utf-8'))
        source, obj = out / 'src' / (name + '.cpp'), out / (name + '.obj')
        source.write_text(patched, encoding='utf-8', newline='\n')
        (out / (name + '.patch')).write_text(''.join(difflib.unified_diff(
            original.splitlines(True), patched.splitlines(True),
            fromfile=rel, tofile=rel)), encoding='utf-8', newline='\n')
        entry = next(e for e in db
                     if e['file'].replace('\\', '/').endswith('/' + rel))
        command = re.sub(r'^\s*(?:"[^"]*cl\.exe"|\S*cl\.exe)\s+', '',
                         entry['command'], flags=re.I)
        command = '/I"' + str(path.parent) + '" /I"' + str(include_root) + '" ' + command
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
                            rsp_sha256=sha(rsp), patches=list(patches),
                            patch_sha256={p: sha(PATCH_SRC / p) for p in patches}))
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
              ' '.join(np(out / (n + '.obj')) for n, *_ in turns),
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
    for name, rel, digest, _ in turns:
        if sha(upstream / rel) != digest:
            raise ValueError('upstream source changed during build: ' + rel)
    base_audio = None
    if args.base_audio_backend:
        base_config_path = out / 'bin/config.yml'
        base_text = base_config_path.read_text(encoding='utf-8')
        if not re.search(r'(?m)^backend-renderer:\s*Vulkan\s*$', base_text):
            raise ValueError('runtime template base config is not Vulkan')
        base_text = rewrite_audio_backend(base_text, args.base_audio_backend)
        base_config_path.write_text(base_text, encoding='utf-8', newline='\n')
        if not re.search(r'(?m)^audio-backend:\s*' + args.base_audio_backend + r'\s*$',
                         base_text):
            raise ValueError('base audio backend rewrite did not take effect')
        base_audio = dict(path='bin/config.yml',
                          audio_backend=args.base_audio_backend,
                          sha256=sha(base_config_path))
    summary = dict(schema=1, sources=records, runtime_inputs=copied,
                   request_queue_fix=args.fix_request_queue,
                   file_write_staging=args.fix_file_write,
                   surface_sync_trace=args.trace_surface_sync,
                   surface_sync_writeback=args.fix_surface_writeback,
                   scene_draw_trace=args.trace_surface_sync,
                   combined_fix_trace=(args.trace_surface_sync and
                                       args.fix_surface_writeback),
                   audio_adapter_fix=args.fix_audio_adapter,
                   base_audio=base_audio,
                   surface_sync_header=surface_sync_header,
                   header=header_record,
                   write_staging_header=write_staging_header,
                   executable=str(executable), executable_sha256=sha(executable),
                   builder_sha256=sha(Path(__file__)))
    (out / 'build.json').write_text(json.dumps(summary, indent=2) + '\n',
                                    encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items()
                      if k != 'runtime_inputs'}, indent=2))


if __name__ == '__main__':
    main()

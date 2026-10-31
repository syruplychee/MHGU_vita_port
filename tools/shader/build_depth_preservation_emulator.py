"""Build the M6-B16 control/patched diagnostic emulator pair from pinned inputs.

Only one translation unit (surface_cache.cpp) is recompiled with a bounded
patch; the patched object is placed before the existing static renderer
library when relinking.  The control is a copy of the independently built
build-debug executable (not the locked runtime), copied byte-for-byte.  All
source, library and executable hashes are captured before and after.  Nothing
in the upstream source tree, the build tree, the locked runtime or user data is
modified.  This is a host diagnostic backend, never part of the Vita package.
"""
import argparse
import datetime
import difflib
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

SOURCE_REL = 'vita3k/renderer/src/gl/surface_cache.cpp'
SOURCE_PIN = 'b2e73b4ae6f5601d9356fca4ba34b1f74dde90ca2fa3fed563e66dc29a87db48'
UPSTREAM_COMMIT = 'd7edcbe979b792d86de100107ad9813878ca3ea9'
CONTROL_PIN = '765510db484c90bbc96e971c0eaf348e3486c74f2a46655f6ea47ddd27f83a55'
LOCKED_PIN = '5e676531f1699cecc712f4d5623d6ac0643e534b026ea19cc19ec27dc6ae79d6'
VPK_REL = 'build/m6b15-depth/vita_port_gxm_depth.vpk'
VPK_PIN = 'd2fdcefec4dabf117eab2579842a9c56c73f4fe555baef0ca27df430aae62a4e'
PATCH_REL = 'tools/shader/patches/m6-depth-preservation.patch'
VERIFIER_REL = 'tools/shader/verify_gxm_depth_probe.py'
PROBE_REL = 'tools/run_gxm_probe.ps1'
TASK = 'M6-B16'
ARTIFACT_REL = 'artifacts/m6b16-depth-preservation-20261026'

ALLOWED_RUNTIME_DIRS = (
    'data', 'icons', 'imageformats', 'platforms', 'styles', 'translations',
    'gui-configs', 'tls', 'plugins', 'shaders-builtin',
)
EXCLUDED_RUNTIME_DIRS = {
    'cache', 'logs', 'config', 'configs', 'savedata', 'save-data', 'gameapp',
    'game-app', 'patch', 'shaderlog', 'texturelog',
}
EXCLUDED_RUNTIME_FILES = {'config.yml', 'vita3k.log'}
SUPPLEMENT_DLLS = ('libssl-4-x64.dll', 'libcrypto-4-x64.dll')


def sha(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n',
                    encoding='utf-8', newline='\n')


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def apply_patch(text, patch):
    """Apply the pinned hunks, requiring each old context to be unique."""
    hunks = re.split(r'^@@[^\n]*\n', patch, flags=re.M)[1:]
    if not hunks:
        raise ValueError('patch contains no hunks')
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


def copy_runtime(template, dest, upstream, supplement_dir):
    """Copy only the runtime allowlist; never copy caches, configs or user data.

    The executable directly imports two OpenSSL DLLs that the runtime template
    no longer contains; they are supplied read-only from the pinned build tree.
    """
    template = template.resolve()
    records = {}
    supplements = {}
    seen = set()

    def copy_one(source, relative):
        destination = dest / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        source_hash, copy_hash = sha(source), sha(destination)
        if source_hash != copy_hash:
            raise ValueError('runtime copy hash mismatch: ' + relative.as_posix())
        return copy_hash

    for source in sorted(template.glob('*.dll')):
        if source.name.lower() in EXCLUDED_RUNTIME_FILES:
            continue
        records[source.name] = copy_one(source, Path(source.name))
    for name in ALLOWED_RUNTIME_DIRS:
        source_dir = template / name
        if not source_dir.is_dir():
            continue
        for root, dirs, files in os.walk(source_dir, followlinks=True):
            root_path = Path(root)
            try:
                real = root_path.resolve()
            except OSError:
                dirs[:] = []
                continue
            if real in seen:
                dirs[:] = []
                continue
            seen.add(real)
            dirs[:] = sorted(d for d in dirs if d.lower() not in EXCLUDED_RUNTIME_DIRS)
            for filename in sorted(files):
                lowered = filename.lower()
                if lowered in EXCLUDED_RUNTIME_FILES or lowered.endswith(('.log', '.ilk', '.pdb')):
                    continue
                source = root_path / filename
                relative = source.relative_to(template)
                records[relative.as_posix()] = copy_one(source, relative)
    for name in SUPPLEMENT_DLLS:
        if name in records:
            continue
        source = supplement_dir / name
        if not source.is_file():
            raise ValueError('missing direct-import runtime DLL: ' + name)
        supplements[name] = copy_one(source, Path(name))
    license_source = upstream / 'COPYING.txt'
    license_dest = dest / 'COPYING.txt'
    shutil.copyfile(license_source, license_dest)
    return records, supplements, sha(license_source)


def parse_link_block(build_ninja):
    lines = build_ninja.read_text(encoding='utf-8').splitlines()
    try:
        start = next(i for i, line in enumerate(lines)
                     if line.startswith('build bin\\Vita3K.exe:'))
    except StopIteration as exc:
        raise ValueError('build.ninja has no bin\\Vita3K.exe link target') from exc
    rule, *inputs = lines[start].split(':', 1)[1].split('|', 1)[0].split()
    variables = {}
    for line in lines[start + 1:]:
        if not line.startswith('  '):
            break
        key, _, value = line.strip().partition(' = ')
        variables[key] = value
    for key in ('CONFIG', 'FLAGS', 'LINK_FLAGS', 'LINK_LIBRARIES', 'MANIFESTS'):
        if key not in variables:
            raise ValueError('build.ninja link target lacks ' + key)
    return rule, inputs, variables


def hash_link_inputs(build, inputs, link_libraries):
    records = {}
    for raw in list(inputs) + link_libraries.split():
        name = raw.replace('$:', ':').replace('$ ', ' ')
        path = Path(name)
        if not path.is_absolute():
            path = build / name
        if path.is_file():
            records[str(path.resolve())] = sha(path)
    return records


def parse_compile_entry(db_path, source_path):
    db = json.loads(db_path.read_text(encoding='utf-8'))
    wanted = source_path.resolve()
    matches = []
    for entry in db:
        try:
            if Path(entry['file']).resolve() == wanted:
                matches.append(entry)
        except OSError:
            continue
    if len(matches) != 1:
        raise ValueError('compile database does not reference the pinned source exactly once')
    entry = matches[0]
    command = entry.get('command', '')
    if not command:
        raise ValueError('compile database entry has no command')
    if sha(entry['file']) != SOURCE_PIN:
        raise ValueError('compile database source does not match the pinned hash')
    if '/Fo' not in command or '/Fd' not in command:
        raise ValueError('unexpected compile command shape (missing /Fo or /Fd)')
    return entry


def run_logged(bat, log, cwd, extra_env=None):
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    with open(log, 'wb') as stream:
        result = subprocess.run(['cmd.exe', '/c', str(bat)], cwd=cwd,
                                stdout=stream, stderr=subprocess.STDOUT, env=env)
    if result.returncode != 0:
        raise RuntimeError('command failed (%d); see %s' % (result.returncode, log))


def compile_patched(args, out, entry, patched_text, original_text):
    patched_dir = out / 'patched'
    obj = patched_dir / 'surface_cache.obj'
    source = patched_dir / 'src/surface_cache.cpp'
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_text(patched_text, encoding='utf-8', newline='\n')
    (patched_dir / 'surface_cache.patch').write_text(
        ''.join(difflib.unified_diff(original_text.splitlines(True),
                                     patched_text.splitlines(True),
                                     fromfile=SOURCE_REL, tofile=SOURCE_REL)),
        encoding='utf-8', newline='\n')
    command = re.sub(r'^\s*(?:"[^"]*cl\.exe"|\S*cl\.exe)\s+', '',
                     entry['command'], flags=re.I)
    command = '/I"' + str((args.upstream.resolve() / SOURCE_REL).parent) + '" ' + command
    command = re.sub(r'/Fo\S+', lambda m: '/Fo"' + str(obj) + '"', command)
    command = re.sub(r'/Fd\S+', lambda m: '/Fd"' + str(obj.with_suffix('.pdb')) + '"', command)
    if not re.search(r'-c\s+\S+\s*$', command):
        raise ValueError('compile command does not end in -c <source>')
    command = re.sub(r'-c\s+\S+\s*$', lambda m: '-c "' + str(source) + '"', command)
    rsp = patched_dir / 'surface_cache.rsp'
    rsp.write_text(command + '\n', encoding='utf-8')
    bat = patched_dir / 'surface_cache.bat'
    bat.write_text(
        '@echo off\ncall "' + str(args.vcvars.resolve()) + '" >nul 2>nul\n'
        'set "TEMP=' + str(patched_dir / 'tmp') + '"\n'
        'set "TMP=' + str(patched_dir / 'tmp') + '"\n'
        'cl.exe @"' + str(rsp) + '"\n', encoding='utf-8')
    run_logged(bat, patched_dir / 'surface_cache.log', args.build)
    if not obj.is_file() or obj.stat().st_size == 0:
        raise RuntimeError('patched object was not produced')
    return dict(source=source, object=obj, rsp=rsp, bat=bat,
                patch=patched_dir / 'surface_cache.patch')


def link_patched(args, out, rule, inputs, variables, override_object):
    patched_dir = out / 'patched'
    executable = patched_dir / 'bin/Vita3K.exe'
    executable.parent.mkdir(parents=True, exist_ok=True)
    object_dir = patched_dir / 'link-obj'
    object_dir.mkdir()

    def np(path):
        return str(path).replace('\\', '/').replace(':', '$:').replace(' ', '$ ')

    lines = ['include CMakeFiles/rules.ninja', 'builddir = ' + np(out), '',
             'build %s: %s %s %s' % (np(executable), rule, np(override_object),
                                     ' '.join(inputs))]
    values = {key: variables[key] for key in
              ('CONFIG', 'FLAGS', 'LINK_FLAGS', 'LINK_LIBRARIES')}
    values.update(
        MANIFESTS=np(args.upstream.resolve() / 'vita3k/Windows.manifest'),
        OBJECT_DIR=np(object_dir), PRE_LINK='cd .', POST_BUILD='cd .',
        TARGET_FILE=np(executable), TARGET_IMPLIB=np(patched_dir / 'Vita3K.lib'),
        TARGET_PDB=np(patched_dir / 'Vita3K.pdb'),
        RSP_FILE=np(patched_dir / 'link.rsp'))
    lines.extend('  %s = %s' % item for item in values.items())
    ninja_file = patched_dir / 'link.ninja'
    ninja_file.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    bat = patched_dir / 'link.bat'
    bat.write_text(
        '@echo off\ncall "' + str(args.vcvars.resolve()) + '" >nul 2>nul\n'
        'set "TEMP=' + str(patched_dir / 'tmp') + '"\n'
        'set "TMP=' + str(patched_dir / 'tmp') + '"\n'
        '"' + str(args.ninja.resolve()) + '" -f "' + str(ninja_file) + '" "' + str(executable) + '"\n',
        encoding='utf-8')
    run_logged(bat, patched_dir / 'link.log', args.build)
    if not executable.is_file():
        raise RuntimeError('patched executable was not produced')
    (executable.parent / (executable.stem + '.ilk')).unlink(missing_ok=True)
    instructions = dict(
        driver='ninja', ninja_file=str(ninja_file), rule=rule,
        override_object=str(override_object),
        input_order='override object first, then every original build input',
        original_inputs=list(inputs), variables=values,
        build_ninja=str(args.build / 'build.ninja'))
    write_json(patched_dir / 'link-instructions.json', instructions)
    return executable, ninja_file, bat


def build(args):
    out = args.output.resolve()
    if out.exists():
        raise ValueError('output must be a new directory; preserve previous evidence')
    upstream = args.upstream.resolve()
    build_dir = args.build.resolve()
    if out.is_relative_to(upstream) or upstream.is_relative_to(out):
        raise ValueError('output and upstream must be independent')
    if out.is_relative_to(build_dir) or build_dir.is_relative_to(out):
        raise ValueError('output and build tree must be independent')

    source_path = upstream / SOURCE_REL
    archive_source = args.archive_source.resolve()
    compile_db = build_dir / 'compile_commands.json'
    build_ninja = build_dir / 'build.ninja'
    patch_path = (args.tools_root / PATCH_REL).resolve()
    vpk_path = (args.tools_root / VPK_REL).resolve()
    verifier_path = (args.tools_root / VERIFIER_REL).resolve()
    probe_path = (args.tools_root / PROBE_REL).resolve()

    for required in (source_path, archive_source, compile_db, build_ninja,
                     patch_path, args.control, args.runtime_template, args.ninja,
                     args.vcvars, vpk_path, verifier_path, probe_path):
        if not Path(required).exists():
            raise ValueError('missing required input: ' + str(required))
    if sha(source_path) != SOURCE_PIN or sha(archive_source) != SOURCE_PIN:
        raise ValueError('pinned surface_cache.cpp identity mismatch')
    entry = parse_compile_entry(compile_db, source_path)
    commit = subprocess.run(['git', '-C', str(upstream), 'rev-parse', 'HEAD'],
                            capture_output=True, text=True, check=True)
    if commit.stdout.strip() != UPSTREAM_COMMIT:
        raise ValueError('compile database upstream commit differs from source pin')
    control_origin = args.control.resolve()
    if sha(control_origin) != CONTROL_PIN:
        raise ValueError('control executable identity mismatch')
    locked = [p.resolve() for p in args.locked_exe]
    for path in locked:
        if not path.is_file() or sha(path) != LOCKED_PIN:
            raise ValueError('locked runtime identity mismatch: ' + str(path))
    if sha(vpk_path) != VPK_PIN:
        raise ValueError('B15 VPK identity mismatch')

    rule, inputs, variables = parse_link_block(build_ninja)
    link_inputs_before = hash_link_inputs(build_dir, inputs, variables['LINK_LIBRARIES'])
    libraries_before = {k: v for k, v in link_inputs_before.items()
                        if k.lower().endswith('.lib')}
    before = dict(
        source=sha(source_path), archive_source=sha(archive_source),
        compile_db=sha(compile_db), build_ninja=sha(build_ninja),
        control_origin=sha(control_origin),
        locked={str(p): sha(p) for p in locked},
        link_inputs=link_inputs_before,
    )

    original_text = source_path.read_text(encoding='utf-8')
    patched_text = apply_patch(original_text,
                               patch_path.read_text(encoding='utf-8'))
    if patched_text == original_text:
        raise ValueError('patch did not change the source')
    if 'glClearDepthf' in patched_text or 'glClear(GL_COLOR_BUFFER_BIT);' not in patched_text:
        raise ValueError('patched source does not implement the bounded depth-preservation change')

    out.mkdir(parents=True)
    (out / 'control/bin').mkdir(parents=True)
    (out / 'patched/tmp').mkdir(parents=True)

    control_template = args.runtime_template.resolve()
    supplement_dir = build_dir / 'external/openssl/x64/bin'
    if not supplement_dir.is_dir():
        supplement_dir = build_dir / 'bin'
    control_runtime, control_supplement, control_license = copy_runtime(
        control_template, out / 'control/bin', upstream, supplement_dir)
    control_all_runtime = dict(control_runtime)
    control_all_runtime.update(control_supplement)
    shutil.copyfile(control_origin, out / 'control/bin/Vita3K.exe')
    control_copy = sha(out / 'control/bin/Vita3K.exe')
    if control_copy != CONTROL_PIN:
        raise ValueError('copied control executable changed identity')
    write_json(out / 'control/build.json', dict(
        schema=1, task=TASK, method='byte-copy of the independent build-debug control',
        origin=str(control_origin), origin_sha256=sha(control_origin),
        copied_sha256=control_copy, control_pin=CONTROL_PIN,
        locked_pin=LOCKED_PIN, control_is_locked_runtime=False,
        source=SOURCE_REL, source_sha256=SOURCE_PIN,
        archive_source=str(archive_source), upstream_commit=UPSTREAM_COMMIT,
        runtime_template=str(control_template), runtime_files=control_all_runtime,
        runtime_template_files=control_runtime,
        runtime_supplement_files=control_supplement,
        runtime_supplement_source=str(supplement_dir),
        runtime_supplement_reason='direct PE imports absent from the runtime template; '
                                  'sourced read-only from the pinned build tree',
        license_sha256=control_license, generated_utc=utc_now()))

    compiled = compile_patched(args, out, entry, patched_text, original_text)
    executable, ninja_file, link_bat = link_patched(
        args, out, rule, inputs, variables, compiled['object'])
    patched_hash = sha(executable)
    if patched_hash == CONTROL_PIN:
        raise ValueError('patched executable is unexpectedly identical to control')
    if patched_hash == LOCKED_PIN:
        raise ValueError('patched executable is unexpectedly identical to the locked runtime')

    patched_runtime, patched_supplement, patched_license = copy_runtime(
        control_template, out / 'patched/bin', upstream, supplement_dir)
    patched_all_runtime = dict(patched_runtime)
    patched_all_runtime.update(patched_supplement)
    if (patched_runtime, patched_supplement) != (control_runtime, control_supplement):
        raise ValueError('control and patched runtime copies differ')

    link_inputs_after = hash_link_inputs(build_dir, inputs, variables['LINK_LIBRARIES'])
    after = dict(
        source=sha(source_path), archive_source=sha(archive_source),
        compile_db=sha(compile_db), build_ninja=sha(build_ninja),
        control_origin=sha(control_origin),
        locked={str(p): sha(p) for p in locked},
        link_inputs=link_inputs_after,
    )
    write_json(out / 'patched/build.json', dict(
        schema=1, task=TASK, method='relink all unchanged build inputs with one patched object',
        source=SOURCE_REL, original_sha256=SOURCE_PIN,
        patched_source=str(compiled['source']), patched_source_sha256=sha(compiled['source']),
        applied_patch_sha256=sha(compiled['patch']), pinned_patch=str(patch_path),
        pinned_patch_sha256=sha(patch_path), object=str(compiled['object']),
        object_sha256=sha(compiled['object']), rsp_sha256=sha(compiled['rsp']),
        compile_bat_sha256=sha(compiled['bat']),
        compile_entry=entry, link_ninja=str(ninja_file),
        link_ninja_sha256=sha(ninja_file), link_bat_sha256=sha(link_bat),
        link_instructions=str(out / 'patched/link-instructions.json'),
        link_input_order='override object before original static renderer library',
        link_inputs=link_inputs_before, libraries=libraries_before,
        runtime_template=str(control_template), runtime_files=patched_all_runtime,
        runtime_template_files=patched_runtime,
        runtime_supplement_files=patched_supplement,
        runtime_supplement_source=str(supplement_dir),
        license_sha256=patched_license, executable=str(executable),
        executable_sha256=patched_hash, control_sha256=CONTROL_PIN,
        locked_sha256=LOCKED_PIN, generated_utc=utc_now()))

    guards = []

    def guard(name, ok, detail):
        guards.append(dict(check=name, ok=bool(ok), detail=detail))

    guard('pinned source unchanged', after['source'] == before['source'] == SOURCE_PIN,
          after['source'])
    guard('archive source unchanged', after['archive_source'] == before['archive_source'] == SOURCE_PIN,
          after['archive_source'])
    guard('compile database unchanged', after['compile_db'] == before['compile_db'],
          after['compile_db'])
    guard('build.ninja unchanged', after['build_ninja'] == before['build_ninja'],
          after['build_ninja'])
    guard('all link inputs unchanged', after['link_inputs'] == before['link_inputs'],
          '%d inputs' % len(after['link_inputs']))
    guard('control origin unchanged', after['control_origin'] == before['control_origin'] == CONTROL_PIN,
          after['control_origin'])
    guard('locked runtimes unchanged', after['locked'] == before['locked'] ==
          {str(p): LOCKED_PIN for p in locked}, after['locked'])
    guard('control copy identity', control_copy == CONTROL_PIN and
          (out / 'control/bin/Vita3K.exe').read_bytes()[:2] == b'MZ', control_copy)
    guard('patched executable distinct', patched_hash not in (CONTROL_PIN, LOCKED_PIN) and
          executable.read_bytes()[:2] == b'MZ', patched_hash)
    guard('patched source markers', 'glClearDepthf' not in compiled['source'].read_text(encoding='utf-8')
          and 'glClear(GL_COLOR_BUFFER_BIT);' in compiled['source'].read_text(encoding='utf-8'),
          str(compiled['source']))
    guard('control/patched runtime copies identical',
          control_all_runtime == patched_all_runtime and
          control_supplement == patched_supplement, '%d files' % len(control_all_runtime))
    guard('runtime template files unchanged',
          all(sha(control_template / rel) == digest for rel, digest in control_runtime.items()),
          '%d files' % len(control_runtime))
    guard('runtime supplement files unchanged',
          all(sha(supplement_dir / rel) == digest for rel, digest in control_supplement.items()),
          '%d files' % len(control_supplement))
    guard('criteria tools present and hashed',
          all(p.is_file() for p in (verifier_path, probe_path)),
          {str(verifier_path): sha(verifier_path), str(probe_path): sha(probe_path)})

    failed = [item for item in guards if not item['ok']]
    write_json(out / 'static-guards.json', dict(
        schema=1, task=TASK, generated_utc=utc_now(),
        verdict='PASS' if not failed else 'FAIL', guards=guards))
    if failed:
        raise RuntimeError('static guards failed: ' + ', '.join(g['check'] for g in failed))

    run_control = (
        'pwsh -NoProfile -File tools/run_gxm_probe.ps1 '
        '-EvidenceDirectory ' + ARTIFACT_REL + '/run-control -Renderer OpenGL '
        '-Vpk ' + VPK_REL + ' -TitleId MHGUDP001 '
        '-DataFiles gxm-depth.log,gxm-depth.rgba -TimeoutSeconds 300 '
        '-Emulator ' + ARTIFACT_REL + '/control/bin/Vita3K.exe')
    run_patched = (
        'pwsh -NoProfile -File tools/run_gxm_probe.ps1 '
        '-EvidenceDirectory ' + ARTIFACT_REL + '/run-patched -Renderer OpenGL '
        '-Vpk ' + VPK_REL + ' -TitleId MHGUDP001 '
        '-DataFiles gxm-depth.log,gxm-depth.rgba -TimeoutSeconds 300 '
        '-Emulator ' + ARTIFACT_REL + '/patched/bin/Vita3K.exe')
    verify = (
        "$env:PYTHONUTF8=1; & '" + str(args.python) + "' "
        + VERIFIER_REL + ' --log <evidence>/gxm-depth.log --dump <evidence>/gxm-depth.rgba '
        + '--report <evidence>/verify.json')
    manifest = dict(
        schema=1, task=TASK, generated_utc=utc_now(),
        claim_limit='host diagnostic backend only; no game, save, production, '
                    'hardware or village claim until Codex runs both binaries',
        paths=dict(upstream=str(upstream), archive_source=str(archive_source),
                   build=str(build_dir), compile_db=str(compile_db),
                   build_ninja=str(build_ninja), control_origin=str(control_origin),
                   locked_exes=[str(p) for p in locked],
                   runtime_template=str(control_template), patch=str(patch_path),
                   vpk=str(vpk_path), verifier=str(verifier_path), probe=str(probe_path)),
        pins=dict(source_sha256=SOURCE_PIN, upstream_commit=UPSTREAM_COMMIT,
                  control_sha256=CONTROL_PIN, locked_sha256=LOCKED_PIN,
                  vpk_sha256=VPK_PIN),
        toolchain=dict(python=str(args.python), python_version=sys.version.split()[0],
                       cl_path=re.search(r'"?([^"]*cl\.exe)"?', entry['command']).group(1),
                       ninja=str(args.ninja), vcvars=str(args.vcvars),
                       compile_database_sha256=sha(compile_db),
                       build_ninja_sha256=sha(build_ninja)),
        criteria_sha256={str(verifier_path): sha(verifier_path),
                         str(probe_path): sha(probe_path), str(vpk_path): sha(vpk_path)},
        control=dict(executable=str(out / 'control/bin/Vita3K.exe'),
                     sha256=control_copy, build_manifest=str(out / 'control/build.json')),
        patched=dict(executable=str(executable), sha256=patched_hash,
                     build_manifest=str(out / 'patched/build.json')),
        integrity=dict(before=before, after=after,
                       source_unchanged=before['source'] == after['source'],
                       archive_source_unchanged=before['archive_source'] == after['archive_source'],
                       compile_db_unchanged=before['compile_db'] == after['compile_db'],
                       build_ninja_unchanged=before['build_ninja'] == after['build_ninja'],
                       link_inputs_unchanged=before['link_inputs'] == after['link_inputs'],
                       locked_exes_unchanged=before['locked'] == after['locked'],
                       runtime_template_unchanged=all(
                           sha(control_template / rel) == digest
                           for rel, digest in control_runtime.items()),
                       runtime_supplement_unchanged=all(
                           sha(supplement_dir / rel) == digest
                           for rel, digest in control_supplement.items())),
        runtime=dict(template=str(control_template), files=control_all_runtime,
                     template_files=control_runtime,
                     supplement_files=control_supplement,
                     supplement_source=str(supplement_dir),
                     supplement_reason='direct PE imports absent from the runtime '
                                       'template; sourced read-only from the pinned '
                                       'build tree',
                     excluded_dirs=sorted(EXCLUDED_RUNTIME_DIRS),
                     excluded_note='no cache/logs/config/savedata/gameapp or user input copied'),
        guards=guards,
        run_instructions=dict(
            vpk=VPK_REL, vpk_sha256=VPK_PIN, title_id='MHGUDP001',
            renderer='OpenGL', data_files=['gxm-depth.log', 'gxm-depth.rgba'],
            timeout_seconds=300, control=run_control, patched=run_patched,
            verify=verify,
            evidence_directories=[ARTIFACT_REL + '/run-control',
                                  ARTIFACT_REL + '/run-patched'],
            expected_control='unchanged verifier must reproduce the B15 case 9 '
                             'reuse_far_rejected FAIL for causal control',
            expected_patched='not claimed here; judge only with the unchanged '
                             'tools/shader/verify_gxm_depth_probe.py'))
    write_json(out / 'manifest.json', manifest)
    print(json.dumps(dict(
        control=control_copy, patched=patched_hash,
        guards=len(guards), output=str(out),
        manifest=str(out / 'manifest.json')), indent=2))


def verify_only(out):
    manifest = json.loads((out / 'manifest.json').read_text(encoding='utf-8'))
    paths = {k: Path(v) for k, v in manifest['paths'].items() if k != 'locked_exes'}
    pins = manifest['pins']
    integrity = manifest['integrity']
    guards = []

    def guard(name, ok, detail):
        guards.append(dict(check=name, ok=bool(ok), detail=detail))

    guard('pinned source', sha(paths['upstream'] / SOURCE_REL) == pins['source_sha256'],
          pins['source_sha256'])
    guard('archive source', sha(paths['archive_source']) == pins['source_sha256'],
          pins['source_sha256'])
    guard('compile database', sha(paths['compile_db']) ==
          manifest['toolchain']['compile_database_sha256'], manifest['toolchain']['compile_database_sha256'])
    guard('build.ninja', sha(paths['build_ninja']) ==
          manifest['toolchain']['build_ninja_sha256'], manifest['toolchain']['build_ninja_sha256'])
    guard('control origin', sha(paths['control_origin']) == pins['control_sha256'],
          pins['control_sha256'])
    for locked in manifest['paths']['locked_exes']:
        guard('locked runtime ' + Path(locked).name, sha(locked) == pins['locked_sha256'],
              pins['locked_sha256'])
    current_inputs = {}
    for name, digest in integrity['before']['link_inputs'].items():
        current_inputs[name] = sha(name)
        guard('link input ' + Path(name).name, current_inputs[name] == digest, digest)
    control_exe = Path(manifest['control']['executable'])
    patched_exe = Path(manifest['patched']['executable'])
    guard('control executable', sha(control_exe) == pins['control_sha256'],
          manifest['control']['sha256'])
    guard('patched executable', sha(patched_exe) == manifest['patched']['sha256'] and
          manifest['patched']['sha256'] != pins['control_sha256'] and
          manifest['patched']['sha256'] != pins['locked_sha256'],
          manifest['patched']['sha256'])
    patched_source = Path(manifest['patched']['build_manifest']).parent / 'src/surface_cache.cpp'
    patched_text = patched_source.read_text(encoding='utf-8')
    guard('patched source markers', 'glClearDepthf' not in patched_text and
          'glClear(GL_COLOR_BUFFER_BIT);' in patched_text and
          sha(patched_source) == json.loads(
              Path(manifest['patched']['build_manifest']).read_text(encoding='utf-8')
          )['patched_source_sha256'], str(patched_source))
    for rel, digest in manifest['runtime']['files'].items():
        guard('control runtime ' + rel, sha(control_exe.parent / rel) == digest, digest)
        guard('patched runtime ' + rel, sha(patched_exe.parent / rel) == digest, digest)
    for rel, digest in manifest['runtime']['template_files'].items():
        guard('runtime template ' + rel, sha(paths['runtime_template'] / rel) == digest, digest)
    supplement_source = Path(manifest['runtime']['supplement_source'])
    for rel, digest in manifest['runtime']['supplement_files'].items():
        guard('runtime supplement ' + rel, sha(supplement_source / rel) == digest, digest)
    guard('criteria tools', all(sha(p) == digest for p, digest in
                                manifest['criteria_sha256'].items()),
          manifest['criteria_sha256'])
    failed = [g for g in guards if not g['ok']]
    report = dict(schema=1, task=TASK, generated_utc=utc_now(),
                  verdict='PASS' if not failed else 'FAIL', guards=guards)
    write_json(out / 'static-guards-verify.json', report)
    print(json.dumps(dict(verdict=report['verdict'], checks=len(guards),
                          output=str(out / 'static-guards-verify.json')), indent=2))
    if failed:
        raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--upstream', type=Path)
    parser.add_argument('--archive-source', type=Path)
    parser.add_argument('--build', type=Path)
    parser.add_argument('--control', type=Path)
    parser.add_argument('--locked-exe', type=Path, action='append', default=[])
    parser.add_argument('--runtime-template', type=Path)
    parser.add_argument('--ninja', type=Path)
    parser.add_argument('--vcvars', type=Path)
    parser.add_argument('--python', type=Path, default=Path(sys.executable))
    parser.add_argument('--tools-root', type=Path,
                        default=Path(__file__).resolve().parent.parent.parent)
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    if args.verify_only:
        verify_only(args.output.resolve())
        return
    required = ('upstream', 'archive_source', 'build', 'control', 'locked_exe',
                'runtime_template', 'ninja', 'vcvars')
    missing = [name for name in required if not getattr(args, name)]
    if missing:
        parser.error('missing required arguments: ' + ', '.join(missing))
    if len(args.locked_exe) != 2:
        parser.error('exactly two --locked-exe identities are required')
    try:
        build(args)
    except Exception as exc:
        root = args.output.resolve()
        if root.exists():
            write_json(root / 'failure.json', dict(
                schema=1, task=TASK, generated_utc=utc_now(),
                error=type(exc).__name__ + ': ' + str(exc),
                claim='build failed; no fix claimed'))
        raise


if __name__ == '__main__':
    main()

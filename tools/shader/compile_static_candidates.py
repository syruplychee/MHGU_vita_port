"""Compile verified offline Cg candidates to GXP with resumable content keys.

This is a compiler gate only. Pipeline linking, vertex input types,
specialization completeness and target pixel equivalence are separate checks.

Resume contract
---------------
All outputs are keyed by a content key over (schema, shader_id, stage,
maxwell_sha256, cg_sha256, environment_sha256, catalogue summary identity,
compiler executable/service/libshacccg hashes, profile and flags). A record is
reused only when its key matches the current build AND the GXP file on disk has
the recorded length + SHA and a valid GXP header. Cached failures are reused by
default so a resumed run does not hammer the serial compiler; --retry-failed
re-attempts them and records the previous attempt.

Every compile writes to a temporary file and is renamed only after the GXP
header/length check. A timeout is a recorded result, not a silent skip.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from convert_cache import compile_serial

SCHEMA = 1
GXP_MAGIC = b'GXP\0'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode()


def valid_gxp(path, expected_bytes=None):
    if not path.is_file():
        return False, 'missing'
    data = path.read_bytes()
    if len(data) < 32:
        return False, 'short'
    if data[:4] != GXP_MAGIC:
        return False, 'magic'
    declared = struct.unpack_from('<I', data, 8)[0]
    if declared > len(data) or declared < 32:
        return False, 'declared-size'
    if expected_bytes is not None and len(data) != expected_bytes:
        return False, 'size-mismatch'
    return True, data


def classify_compiler_failure(returncode, log):
    """Separate a Shacc service timeout from a real compiler rejection.

    Shacc prints "Compiler service timed out." and returns nonzero when its
    internal quota expires.  That is not a source/compiler diagnostic; callers
    must not record it as a rejected shader.  Only a nonzero return without
    that marker is a compiler rejection.
    """
    text = log.decode('utf-8', 'replace')
    if 'Compiler service timed out' in text:
        return 'service_timeout', 'Shacc service quota expired (returncode %s)' % returncode
    return 'compiler_rejected', None


def item_key(entry, profile, identity, flags=None):
    return hashlib.sha256(canonical({
        **{field: entry[field] for field in
           ('shader_id', 'stage', 'maxwell_sha256', 'cg_sha256', 'environment_sha256')},
        'profile': profile, 'flags': identity['flags'] if flags is None else flags,
        **{field: identity[field] for field in
           ('catalogue_summary_sha256', 'shacc_sha256', 'libshacccg_sha256')},
    })).hexdigest()


def load_reuse_source(build_dir, catalogue, identity):
    """Validate a historical build without relabelling its compiler provenance."""
    build_dir, catalogue = Path(build_dir).resolve(), Path(catalogue).resolve()
    manifest_path = build_dir / 'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    previous = manifest['build_identity']
    summary_path = catalogue / 'summary.json'
    summary = json.loads(summary_path.read_text(encoding='utf-8'))
    if previous['catalogue_summary_sha256'] != digest(summary_path):
        raise ValueError('reuse build belongs to a different catalogue')
    if summary['outputs']['shader-catalogue.jsonl'] != digest(catalogue / 'shader-catalogue.jsonl'):
        raise ValueError('reuse catalogue hash mismatch')
    for field in ('shacc_sha256', 'libshacccg_sha256', 'service_vpk_sha256',
                  'config_sha256', 'flags'):
        if previous.get(field) != identity.get(field):
            raise ValueError('reuse compiler identity mismatch: ' + field)
    entries = {r['shader_id']: r for r in
               map(json.loads, (catalogue / 'shader-catalogue.jsonl').read_text(encoding='utf-8').splitlines())}
    records = {r['shader_id']: r for r in manifest['programs']}
    if len(records) != len(manifest['programs']):
        raise ValueError('duplicate reuse build record')
    return {'root': build_dir, 'catalogue': catalogue, 'entries': entries,
            'records': records, 'identity': previous,
            'manifest_sha256': digest(manifest_path)}


def reuse_unchanged(source, entry, profile, identity, output, flags=None):
    """Reuse only identical Cg, stage, exact Maxwell and environment inputs.

    A new catalogue key describes the new assembly. The old key and complete
    compiler identity remain explicit, so reuse cannot imply recompilation.
    """
    old = source['records'].get(entry['shader_id'])
    old_entry = source['entries'].get(entry['shader_id'])
    if old is None or old_entry is None:
        return None
    flags = identity['flags'] if flags is None else flags
    old_flags = old.get('flags', source['identity']['flags'])
    if old_flags != flags:
        return None
    fields = ('stage', 'maxwell_sha256', 'cg_sha256', 'environment_sha256')
    if any(old_entry.get(k) != entry.get(k) for k in fields):
        return None
    if any(old.get(k) != entry.get(k) for k in fields) or old.get('profile') != profile:
        raise ValueError('reuse record differs from its catalogue: ' + entry['shader_id'])
    if old['content_key'] != item_key(old_entry, profile, source['identity'], old_flags):
        raise ValueError('reuse record content key mismatch: ' + entry['shader_id'])
    old_cg = (source['catalogue'] / old_entry['cg_candidate']).resolve()
    if not old_cg.is_relative_to(source['catalogue']) or digest(old_cg) != entry['cg_sha256']:
        raise ValueError('reuse Cg hash/path mismatch: ' + entry['shader_id'])
    if old.get('status') == 'compiled':
        old_gxp = (source['root'] / old['gxp']).resolve()
        if not old_gxp.is_relative_to(source['root']):
            raise ValueError('reuse GXP path escapes build')
        ok, data = valid_gxp(old_gxp, old.get('gxp_bytes'))
        if not ok or hashlib.sha256(data).hexdigest() != old.get('gxp_sha256') or not old.get('gxp_verified'):
            raise ValueError('reuse GXP hash/header mismatch: ' + entry['shader_id'])
        shutil.copyfile(old_gxp, output / 'shaders' / (entry['shader_id'] + '.gxp'))
    elif old.get('status') != 'rejected' or not old.get('failure_kind'):
        raise ValueError('reuse record has no valid compiler outcome')
    old_log = (source['root'] / old['log']).resolve()
    if not old_log.is_relative_to(source['root']):
        raise ValueError('reuse log path escapes build')
    shutil.copyfile(old_log, output / 'logs' / (entry['shader_id'] + '.compile.txt'))
    result = dict(old)
    result.update(content_key=item_key(entry, profile, identity, flags), flags=flags,
                  lowering_notes=entry['lowering_notes'],
                  pipeline_linked=False, pixel_verified=False,
                  reuse='identical Cg + exact stage/Maxwell/environment + compiler + verified GXP',
                  compilation_provenance={
                      'manifest_sha256': source['manifest_sha256'],
                      'build_identity': source['identity'],
                      'content_key': old['content_key'],
                      'record_sha256': hashlib.sha256(canonical(old)).hexdigest(),
                      'log_sha256': digest(old_log),
                      'ancestor': old.get('compilation_provenance'),
                  })
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalogue', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--only', help='Comma-separated exact shader IDs; omission compiles all emitted candidates')
    parser.add_argument('--only-file', type=Path,
                        help='File with one exact shader ID per line; avoids the command-line length limit')
    parser.add_argument('--retry-failed', action='store_true',
                        help='Re-attempt items whose cached result is not a compiled GXP')
    parser.add_argument('--reuse-from', type=Path,
                        help='Prior build: import only byte-identical Cg with verified input/toolchain/GXP identities')
    parser.add_argument('--reuse-catalogue', type=Path,
                        help='Original catalogue belonging to --reuse-from; required together')
    parser.add_argument('--optimization-map', type=Path,
                        help='Explicit shader-id -> ["-O0"] or ["-O1"] overrides; defaults remain -O0')
    parser.add_argument('--limit', type=int, help='Compile at most N items after selection (for a chunked run)')
    parser.add_argument('--timeout', type=int, default=60, help='Per-item compiler timeout in seconds')
    parser.add_argument('--service-timeout', type=int,
                        help='Explicit Shacc service quota for a bounded diagnostic; default keeps the batch cap')
    args = parser.parse_args()
    if (args.reuse_from is None) != (args.reuse_catalogue is None):
        raise ValueError('--reuse-from and --reuse-catalogue are required together')
    if args.service_timeout is not None and not 5 <= args.service_timeout <= 600:
        raise ValueError('--service-timeout must be bounded to 5..600 seconds')

    repo = Path(__file__).resolve().parents[2]
    catalogue = args.catalogue.resolve()
    summary_path = catalogue / 'summary.json'
    summary = json.loads(summary_path.read_text(encoding='utf-8'))
    if summary['outputs']['shader-catalogue.jsonl'] != digest(catalogue / 'shader-catalogue.jsonl'):
        raise ValueError('catalogue hash mismatch')
    if summary['shader_ir_sha256'] != digest(Path(__file__).with_name('shader_ir.py')):
        raise ValueError('catalogue belongs to a different IR backend')
    with (catalogue / 'shader-catalogue.jsonl').open('r', encoding='utf-8') as stream:
        entries = [json.loads(line) for line in stream]
    selection = args.only
    if args.only_file is not None:
        if selection is not None:
            raise ValueError('use --only or --only-file, not both')
        selection = ','.join(line.strip() for line in
                             args.only_file.read_text(encoding='utf-8').splitlines() if line.strip())
    selected = set(selection.split(',')) if selection else None
    overrides = json.loads(args.optimization_map.read_text(encoding='utf-8')) if args.optimization_map else {}
    if (not isinstance(overrides, dict) or not set(overrides) <= {e['shader_id'] for e in entries} or
            any(value not in (['-O0'], ['-O1']) for value in overrides.values())):
        raise ValueError('invalid per-stage optimization map')
    candidates = [e for e in entries if e.get('cg_candidate') and
                  (selected is None or e['shader_id'] in selected)]
    if not candidates or selected is not None and selected != {e['shader_id'] for e in candidates}:
        raise ValueError('selection contains unknown or ineligible Cg candidates')

    config = repo / 'local/shader-tools/shacc.conf'
    executable = repo / 'local/shader-tools/shacc-bin/Vita3K-Shacc-windows-x86_64/vita3k-shacc.exe'
    values = dict(line.split('=', 1) for line in config.read_text(encoding='utf-8').splitlines() if line)
    compiler = Path(values['root']) / 'fs/ur0/data/external/libshacccg.suprx'
    service = Path(values['service-vpk'])
    lock = json.loads((repo / 'config/graphics/shader-toolchain.json').read_text(encoding='utf-8'))
    identity = {
        'shacc_sha256': digest(executable),
        'service_vpk_sha256': digest(service),
        'libshacccg_sha256': digest(compiler),
        'config_sha256': digest(config),
        'tool_sha256': digest(Path(__file__)),
        'catalogue_summary_sha256': digest(summary_path),
        'shader_ir_sha256': digest(Path(__file__).with_name('shader_ir.py')),
        'flags': ['-O0'],
    }
    if args.optimization_map:
        identity['optimization_map_sha256'] = digest(args.optimization_map)
    if (identity['shacc_sha256'] != lock['shacc']['executable_sha256'] or
            identity['service_vpk_sha256'] != lock['shacc']['service_vpk_sha256'] or
            identity['libshacccg_sha256'] != lock['libshacccg']['sha256']):
        raise ValueError('shader compiler identity mismatch')
    reuse_source = (load_reuse_source(args.reuse_from, args.reuse_catalogue, identity)
                    if args.reuse_from else None)

    output = args.output.resolve()
    build_path = output / 'build.json'
    build = {'schema': SCHEMA, 'identity': identity,
             'content_key_fields': ['shader_id', 'stage', 'maxwell_sha256', 'cg_sha256',
                                    'environment_sha256', 'profile', 'flags',
                                    'identity.catalogue_summary_sha256',
                                    'identity.shacc_sha256',
                                    'identity.libshacccg_sha256'],
             'pipeline_linked': False, 'pixel_verified': False}
    cached_records = {}
    if output.exists() and any(output.iterdir()):
        if not build_path.is_file():
            raise ValueError('non-empty output without build.json; use a new directory')
        previous = json.loads(build_path.read_text(encoding='utf-8'))
        if previous.get('identity') != identity:
            raise ValueError('output was produced by a different catalogue/toolchain; use a new directory')
        records_dir = output / 'records'
        if records_dir.is_dir():
            for record_path in sorted(records_dir.glob('*.json')):
                try:
                    record = json.loads(record_path.read_text(encoding='utf-8'))
                except (OSError, ValueError):
                    continue
                if record.get('shader_id'):
                    cached_records[record['shader_id']] = record
    else:
        output.mkdir(parents=True, exist_ok=True)
    (output / 'records').mkdir(exist_ok=True)
    (output / 'shaders').mkdir(exist_ok=True)
    (output / 'logs').mkdir(exist_ok=True)
    (output / 'tmp').mkdir(exist_ok=True)
    build_path.write_text(json.dumps(build, indent=2, sort_keys=True) + '\n',
                          encoding='utf-8', newline='\n')

    manifest_records = []
    counters = Counter()
    compiled = 0
    processed = 0
    for entry in candidates:
        if args.limit is not None and processed >= args.limit:
            break
        processed += 1
        name = entry['shader_id']
        profile = 'sce_vp_psp2' if entry['stage'] == 'vert' else 'sce_fp_psp2'
        effective_flags = overrides.get(name, identity['flags'])
        key = item_key(entry, profile, identity, effective_flags)
        gxp_path = output / 'shaders' / (name + '.gxp')
        log_path = output / 'logs' / (name + '.compile.txt')
        record_path = output / 'records' / (name + '.json')
        cached = cached_records.get(name)
        record = None
        source = (catalogue / entry['cg_candidate']).resolve()
        if not source.is_relative_to(catalogue) or digest(source) != entry['cg_sha256']:
            raise ValueError('candidate source hash/path mismatch: ' + name)
        if entry['specialization_required'] or entry['cg_lowering'] != 'accepted':
            raise ValueError('candidate has unresolved compilation prerequisites: ' + name)

        if cached is not None and cached.get('content_key') == key:
            ok, data = valid_gxp(gxp_path, cached.get('gxp_bytes'))
            if cached.get('status') == 'compiled' and ok and \
                    hashlib.sha256(data).hexdigest() == cached.get('gxp_sha256'):
                counters['reused'] += 1
                compiled += 1
                record = dict(cached)
                record['reuse'] = 'content-key + gxp hash verified'
            elif cached.get('status') != 'compiled' and not args.retry_failed:
                counters['cached_failure'] += 1
                record = dict(cached)
                record['reuse'] = 'cached failure; pass --retry-failed to re-attempt'
            else:
                counters['stale_result'] += 1
        elif cached is not None:
            counters['stale_result'] += 1

        if record is None and cached is None and reuse_source is not None:
            previous_record = reuse_source['records'].get(name, {})
            if previous_record.get('status') == 'compiled' or not args.retry_failed:
                record = reuse_unchanged(reuse_source, entry, profile, identity, output, effective_flags)
                if record is not None:
                    if record['status'] == 'compiled':
                        compiled += 1
                        counters['imported_unchanged'] += 1
                    else:
                        counters['imported_failure'] += 1

        if record is None:
            source = (catalogue / entry['cg_candidate']).resolve()
            if not source.is_relative_to(catalogue) or digest(source) != entry['cg_sha256']:
                raise ValueError('candidate source hash/path mismatch: ' + name)
            if entry['specialization_required'] or entry['cg_lowering'] != 'accepted':
                raise ValueError('candidate has unresolved compilation prerequisites: ' + name)
            tmp_gxp = output / 'tmp' / (name + '.gxp')
            tmp_log = output / 'tmp' / (name + '.log')
            for stale in (tmp_gxp, tmp_log):
                if stale.exists():
                    stale.unlink()
            service_timeout = (args.service_timeout if args.service_timeout is not None
                               else min(args.timeout - 5, 40))
            outer_timeout = (service_timeout + 15 if args.service_timeout is not None
                             else 50)
            command = [str(executable), '--config', str(config), '-profile', profile, *effective_flags,
                       '-o', str(tmp_gxp), str(source), '--timeout', str(service_timeout)]
            attempt = int((cached or {}).get('attempt', 0)) + 1
            outcome = 'error'
            failure_kind = None
            returncode = None
            detail = None
            try:
                result = compile_serial(command, config.parent, timeout=outer_timeout)
                returncode = result.returncode
                tmp_log.write_bytes(result.stdout + result.stderr)
                ok, data = valid_gxp(tmp_gxp)
                if result.returncode != 0:
                    outcome = 'rejected'
                    failure_kind, detail = classify_compiler_failure(
                        result.returncode, result.stdout + result.stderr)
                elif not ok:
                    outcome = 'rejected'
                    failure_kind = 'invalid_output'
                    detail = ok is False and data if isinstance(data, str) else None
                else:
                    outcome = 'compiled'
            except subprocess.TimeoutExpired as error:
                returncode = None
                outcome = 'rejected'
                failure_kind = 'timeout'
                detail = f'compiler exceeded {outer_timeout}s outer limit'
                tmp_log.write_bytes((error.stdout or b'') + (error.stderr or b''))
            except Exception as error:  # preserve the failure, never skip it
                outcome = 'rejected'
                failure_kind = 'exception'
                detail = f'{type(error).__name__}: {error}'
                tmp_log.write_text(detail + '\n', encoding='utf-8')

            if outcome == 'compiled':
                data = tmp_gxp.read_bytes()
                os.replace(tmp_gxp, gxp_path)
                compiled += 1
                counters['compiled'] += 1
                record = {'shader_id': name, 'stage': entry['stage'], 'profile': profile,
                          'content_key': key, 'status': 'compiled', 'attempt': attempt,
                          'maxwell_sha256': entry['maxwell_sha256'],
                          'environment_sha256': entry['environment_sha256'],
                          'cg_sha256': entry['cg_sha256'],
                          'compiler_returncode': returncode,
                          'service_timeout_seconds': service_timeout,
                          'gxp_verified': True, 'gxp_sha256': hashlib.sha256(data).hexdigest(),
                          'gxp_bytes': len(data), 'gxp': f'shaders/{name}.gxp',
                          'log': f'logs/{name}.compile.txt',
                          'pipeline_linked': False, 'pixel_verified': False,
                          'lowering_notes': entry['lowering_notes']}
            else:
                if tmp_gxp.exists():
                    tmp_gxp.unlink()
                counters[failure_kind] += 1
                record = {'shader_id': name, 'stage': entry['stage'], 'profile': profile,
                          'content_key': key, 'status': 'rejected',
                          'failure_kind': failure_kind, 'attempt': attempt,
                          'maxwell_sha256': entry['maxwell_sha256'],
                          'environment_sha256': entry['environment_sha256'],
                          'cg_sha256': entry['cg_sha256'],
                          'compiler_returncode': returncode, 'detail': detail,
                          'service_timeout_seconds': service_timeout,
                          'gxp_verified': False, 'gxp_sha256': None, 'gxp_bytes': None,
                          'gxp': None, 'log': f'logs/{name}.compile.txt',
                          'pipeline_linked': False, 'pixel_verified': False,
                          'lowering_notes': entry['lowering_notes']}
            os.replace(tmp_log, log_path)
            if cached is not None and attempt > 1:
                record['previous'] = {k: cached.get(k) for k in
                                       ('status', 'failure_kind', 'content_key', 'attempt')}
            record['flags'] = effective_flags
            record_path.write_text(json.dumps(record, indent=2, sort_keys=True) + '\n',
                                   encoding='utf-8', newline='\n')
            print(name, outcome.upper() if outcome == 'compiled' else (failure_kind or 'FAILED'),
                  flush=True)
        else:
            # Rebuild the per-item record file when resuming so a crashed run
            # leaves every selected item represented.
            record_path.write_text(json.dumps(record, indent=2, sort_keys=True) + '\n',
                                   encoding='utf-8', newline='\n')

        manifest_records.append(record)

    manifest = {
        'schema': SCHEMA,
        'build_identity': identity,
        'selection': {'only': sorted(selected) if selected else None,
                      'limit': args.limit, 'retry_failed': args.retry_failed},
        'programs': manifest_records,
        'selected': len(manifest_records),
        'compiled': compiled,
        'rejected': len(manifest_records) - compiled,
        'counts': dict(counters),
        'failures': [{'shader_id': r['shader_id'], 'failure_kind': r.get('failure_kind'),
                      'detail': r.get('detail')} for r in manifest_records
                     if r['status'] != 'compiled'],
        'installed': False,
        'pipeline_linked': False,
        'pixel_verified': False,
        'semantic_equivalence_verified': False,
    }
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n',
                                          encoding='utf-8', newline='\n')
    try:
        (output / 'tmp').rmdir()
    except OSError:
        pass
    print(json.dumps({'selected': manifest['selected'], 'compiled': compiled,
                      'rejected': manifest['rejected'], 'counts': manifest['counts']},
                     ensure_ascii=False), flush=True)
    if manifest['rejected']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()

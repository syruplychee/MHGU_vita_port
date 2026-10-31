"""Retry one timed-out candidate without changing its compiler flags or identity.

The batch driver's 40-second service deadline can expire before a large shader
finishes. This explicit retry keeps the same content key and compiler inputs,
archives the old record/log, and records the retry driver and longer deadline.
It updates only a rejected record in a current-IR build, never a baseline GXP.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from compile_static_candidates import canonical, digest, item_key, valid_gxp

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--catalogue', type=Path, required=True)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--shader-id', required=True)
    p.add_argument('--timeout', type=int, default=300)
    a = p.parse_args()
    if not 60 <= a.timeout <= 600:
        raise ValueError('retry deadline must be bounded to 60..600 seconds')
    build, catalogue = a.build.resolve(), a.catalogue.resolve()
    manifest_path = build / 'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    identity = manifest['build_identity']
    if identity['shader_ir_sha256'] != digest(Path(__file__).with_name('shader_ir.py')):
        raise ValueError('only current-IR builds can be retried')
    if identity['catalogue_summary_sha256'] != digest(catalogue / 'summary.json'):
        raise ValueError('catalogue identity mismatch')
    entries = {r['shader_id']: r for r in map(json.loads,
               (catalogue / 'shader-catalogue.jsonl').read_text(encoding='utf-8').splitlines())}
    entry = entries[a.shader_id]
    previous = next(r for r in manifest['programs'] if r['shader_id'] == a.shader_id)
    if previous['status'] != 'rejected':
        raise ValueError('retry is only for an explicit failure')
    key = item_key(entry, previous['profile'], identity)
    source = (catalogue / entry['cg_candidate']).resolve()
    if previous['content_key'] != key or not source.is_relative_to(catalogue) or digest(source) != entry['cg_sha256']:
        raise ValueError('candidate identity mismatch')
    config = ROOT / 'local/shader-tools/shacc.conf'
    exe = ROOT / 'local/shader-tools/shacc-bin/Vita3K-Shacc-windows-x86_64/vita3k-shacc.exe'
    values = dict(line.split('=', 1) for line in config.read_text(encoding='utf-8').splitlines() if line)
    compiler = Path(values['root']) / 'fs/ur0/data/external/libshacccg.suprx'
    for field, path in (('config_sha256', config), ('shacc_sha256', exe),
                        ('service_vpk_sha256', Path(values['service-vpk'])), ('libshacccg_sha256', compiler)):
        if identity[field] != digest(path):
            raise ValueError('compiler identity mismatch: ' + field)
    archive = build / ('retry-%s-attempt%d' % (a.shader_id, previous['attempt']))
    archive.mkdir()  # refuse to overwrite an earlier attempt
    (archive / 'record.json').write_bytes((build / 'records' / (a.shader_id + '.json')).read_bytes())
    (archive / 'compile.txt').write_bytes((build / previous['log']).read_bytes())
    target = archive / 'retry.gxp'
    command = [str(exe), '--config', str(config), '-profile', previous['profile'],
               *identity['flags'], '-o', str(target), str(source), '--timeout', str(a.timeout - 5)]
    lock = config.parent / '.vp-shacc.lock'
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        os.write(fd, str(os.getpid()).encode())
        os.close(fd)
        result = subprocess.run(command, capture_output=True, timeout=a.timeout)
    finally:
        lock.unlink()
    (archive / 'retry.compile.txt').write_bytes(result.stdout + result.stderr)
    ok, data = valid_gxp(target)
    if result.returncode or not ok:
        raise RuntimeError('retry still failed; archived result in ' + str(archive))
    record = {**previous, 'attempt': previous['attempt'] + 1, 'status': 'compiled',
              'gxp': 'shaders/' + a.shader_id + '.gxp', 'gxp_bytes': len(data),
              'gxp_sha256': hashlib.sha256(data).hexdigest(), 'gxp_verified': True,
              'compiler_returncode': result.returncode, 'failure_kind': None, 'detail': None,
              'retry_provenance': {'driver_sha256': digest(Path(__file__)),
                                   'previous_manifest_sha256': digest(manifest_path),
                                   'previous_record_sha256': hashlib.sha256(canonical(previous)).hexdigest(),
                                   'previous_attempt': previous['attempt'], 'timeout_seconds': a.timeout,
                                   'command': command}}
    os.replace(target, build / record['gxp'])
    (build / record['log']).write_bytes(result.stdout + result.stderr)
    (build / 'records' / (a.shader_id + '.json')).write_text(
        json.dumps(record, indent=2, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
    manifest['programs'] = [record if r['shader_id'] == a.shader_id else r for r in manifest['programs']]
    manifest['compiled'] = sum(r['status'] == 'compiled' for r in manifest['programs'])
    manifest['rejected'] = len(manifest['programs']) - manifest['compiled']
    manifest['failures'] = [f for f in manifest['failures'] if f['shader_id'] != a.shader_id]
    manifest['retry_driver_sha256'] = digest(Path(__file__))
    manifest['counts']['explicit_timeout_retry_success'] = manifest['counts'].get('explicit_timeout_retry_success', 0) + 1
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
    print(a.shader_id, 'COMPILED; original flags and content key retained', flush=True)


if __name__ == '__main__':
    main()

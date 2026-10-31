"""Audit P4/P5 provenance, cold cache, and backend evidence independently."""
import argparse
import hashlib
import json
import re
import struct
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def gxp_facts(path):
    data = Path(path).read_bytes()
    if data[:4] != b'GXP\0':
        raise ValueError('not a GXP: ' + str(path))
    size = struct.unpack_from('<I', data, 8)[0]
    temp1 = struct.unpack_from('<I', data, 52)[0]
    temp2 = struct.unpack_from('<H', data, 56)[0]
    return {
        'internal_sha256': sha(data[:size]),
        'temp_vec4_required': (max(temp1, temp2) + 3) // 4,
        'temp_reg_count1': temp1,
        'temp_reg_count2': temp2,
        'bytes': len(data),
        'sha256': sha(data),
    }


def parse_backend(log_path, expected_count=None):
    ready, failed, draws, exceptions = [], [], [], []
    get_fail = 0
    last_draw_ts = None
    closed_ts = None
    for line in Path(log_path).read_text(encoding='utf-8', errors='replace').splitlines():
        message = re.sub(r'^\[[^\]]+\] \|\w\| \[[^\]]+\]: ', '', line)
        complete = re.fullmatch(r'VP_GXM_PROBE_COMPLETE terminal=(\d+)/(\d+)', message)
        if ('Game closed:' in message or message == 'VP_GXM_APP_EXIT status=0'
                or (complete and int(complete[1]) == int(complete[2]) == expected_count)):
            match = re.match(r'^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]', line)
            if match:
                closed_ts = (int(match.group(1)) * 3600 + int(match.group(2)) * 60 +
                             int(match.group(3)) + int(match.group(4)) / 1000.0)
        if message.startswith('VP_GXM_PROGRAM_READY') or message.startswith('VP_GXM_PROGRAM_CACHED'):
            ready.append(line)
        elif message.startswith('VP_GXM_PROGRAM_FAIL'):
            failed.append(line)
        elif message.startswith('VP_GXM_GLDRAW '):
            draws.append(line)
            match = re.match(r'^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]', line)
            if match:
                last_draw_ts = (int(match.group(1)) * 3600 + int(match.group(2)) * 60 +
                                int(match.group(3)) + int(match.group(4)) / 1000.0)
        elif 'Fail to get program!' in line:
            get_fail += 1
        elif 'EXCEPTION' in line:
            match = re.match(r'^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]', line)
            ts = None
            if match:
                ts = (int(match.group(1)) * 3600 + int(match.group(2)) * 60 +
                      int(match.group(3)) + int(match.group(4)) / 1000.0)
            exceptions.append({'ts': ts, 'text': line[:200]})
    return {'ready': ready, 'failed': failed, 'draws': draws,
            'fail_to_get_program': get_fail, 'exceptions': exceptions,
            'last_draw_ts': last_draw_ts, 'closed_ts': closed_ts}


def audit(bundle, attempts, vpk, emulator, require_backend=False,
          require_cold=False):
    bundle, vpk, emulator = map(Path, (bundle, vpk, emulator))
    attempts = list(map(Path, attempts))
    contract = json.loads((bundle / 'run-contract.json').read_text())
    manifest = json.loads((bundle / 'pair-manifest.json').read_text())
    programs = {program['token']: program for program in manifest.get('programs', [])}
    problems, evidence = [], []
    def check(condition, message):
        if not condition:
            problems.append(message)
    check(sha((bundle / 'shader-index.bin').read_bytes()) == contract['index_sha256'], 'bundle index drift')
    check(sha((bundle / 'pair-manifest.json').read_bytes()) == contract['manifest_sha256'], 'bundle manifest drift')
    check(manifest['index_sha256'] == contract['index_sha256'], 'manifest index mismatch')
    for name, digest in contract['sources'].items():
        check(sha((ROOT / name).read_bytes()) == digest, 'source drift: ' + name)
    vpk_sha, emulator_sha = sha(vpk.read_bytes()), sha(emulator.read_bytes())
    target_gxp = {}
    for stage in manifest['stages']:
        facts = gxp_facts(bundle / 'shaders' / (stage['variant_key'] + '.gxp'))
        check(facts['sha256'] == stage['gxp_sha256'],
              'bundle GXP mismatch: ' + stage['shader_id'])
        target_gxp[stage['index']] = facts
    with zipfile.ZipFile(vpk) as package:
        eboot_sha = sha(package.read('eboot.bin'))
        check(package.read('shader-index.bin') == (bundle / 'shader-index.bin').read_bytes(), 'VPK index mismatch')
        for stage in manifest['stages']:
            name = 'shaders/' + stage['variant_key'] + '.gxp'
            digest = stage['gxp_sha256']
            check(sha(package.read(name)) == digest, 'VPK GXP mismatch: ' + name)
            check(sha((bundle / name).read_bytes()) == digest, 'bundle GXP mismatch: ' + name)
        for kind in ('v', 'f'):
            name = 'control_' + kind + '.gxp'
            check(sha(package.read(name)) == manifest['control'][kind]['sha256'], 'control payload mismatch: ' + kind)
    previous = ''
    previous_record = None
    previous_directory = None
    for number, directory in enumerate(attempts):
        record = json.loads((directory / 'process.json').read_text())
        path = directory / 'gxm-s2p.log'
        raw = path.read_bytes()
        text = raw.decode('utf-8')
        # process.json counts Windows Get-Content characters including CRLF.
        chars = len(raw.decode('utf-8'))
        prior = record.get('prior_log_chars', 0)
        expected_prior = len(previous)
        check(prior == expected_prior, f'attempt {number + 1}: resume offset mismatch')
        check(text.startswith(previous), f'attempt {number + 1}: cumulative log was rewritten')
        new = text[len(previous):] if text.startswith(previous) else text
        new = new.replace('\r\n', '\n')
        check(len(re.findall(r'^RUN-ID ', new, re.M)) == 1, f'attempt {number + 1}: expected one new RUN-ID')
        ids = re.findall(r'^RUN-ID identity=(\S+) index=(\S+) source=(\S+)$', new, re.M)
        check(ids == [(contract['identity'], contract['index_sha256'], contract['source_sha256'])],
              f'attempt {number + 1}: run identity mismatch')
        controls = re.findall(r'^CONTROL pixel=.*$', new, re.M)
        expected_control = 'CONTROL pixel=ff000040 cov=0 nonzero=1024 hash=1838662e81f6c325'
        if re.search(r'^BEGIN ', new, re.M):
            check(controls == [expected_control], f'attempt {number + 1}: control is not the expected uniform 64x16 red-quarter image')
        results = re.findall(r'^RESULT (COMPLETE|FAIL)\b[^\n]*', new, re.M)
        if record['result'] == 'emulator exited before RESULT' and record.get('natural_exit'):
            check(not results, f'attempt {number + 1}: natural exit unexpectedly has a verdict')
        else:
            check(bool(results) and results[-1] == record['result'], f'attempt {number + 1}: process verdict mismatch')
        if number + 1 < len(attempts):
            watchdog = bool(re.search(r'^TIMEOUT token=\d+ seconds=20 detail=watchdog$', new, re.M))
            natural_exit = record.get('natural_exit') and record['result'] == 'emulator exited before RESULT'
            check(watchdog or natural_exit, f'attempt {number + 1}: resume has no authoritative stop')
        host_crashes = re.findall(r'^PAIR token=(\d+) status=crash stage=\w+ detail=host-emulator-exit ', new, re.M)
        if previous_record and previous_record['result'] == 'emulator exited before RESULT':
            receipt_path = directory / 'host-exit.json'
            if not receipt_path.is_file():
                problems.append(f'attempt {number + 1}: missing host exit evidence')
            else:
                receipt = json.loads(receipt_path.read_text())
                check(receipt['previous_log_sha256'].lower() == sha(previous.encode()), 'host receipt log mismatch')
                check(receipt['previous_process_sha256'].lower() == sha((previous_directory / 'process.json').read_bytes()), 'host receipt process mismatch')
                pending = re.findall(r'^BEGIN token=(\d+) ', previous, re.M)[-1]
                check(host_crashes == [pending] and receipt['token'] == int(pending), 'host receipt pending token mismatch')
                check(receipt['exit_code'] == previous_record['exit_code'], 'host receipt exit code mismatch')
                fnv = 0xcbf29ce484222325
                for byte in previous.encode():
                    fnv = ((fnv ^ byte) * 0x100000001b3) & ((1 << 64) - 1)
                expected = f"HOST-EXIT identity={contract['identity']} token={pending} log={fnv:016x}\n"
                check((directory / 'host-exit.receipt').read_bytes() == expected.encode('ascii'), 'host receipt payload mismatch')
        else:
            check(not host_crashes, f'attempt {number + 1}: host crash without previous exit')
        for key, digest in (('vpk_sha256', vpk_sha), ('eboot_sha256', eboot_sha), ('emulator_sha256', emulator_sha)):
            check(record[key].lower() == digest, f'attempt {number + 1}: {key} mismatch')
        check(bool(record.get('title_id')), f'attempt {number + 1}: title mismatch')
        config = (directory / 'config.yml').read_text()
        check('backend-renderer: OpenGL' in config and 'OpenGL' in record['arguments'],
              f'attempt {number + 1}: renderer mismatch')
        attempt_evidence = dict(directory=directory.as_posix(), log_sha256=sha(raw),
                                process_sha256=sha((directory / 'process.json').read_bytes()),
                                config_sha256=sha((directory / 'config.yml').read_bytes()),
                                prior_log_chars=prior, log_chars=chars,
                                result=record['result'])
        if require_cold:
            check(record.get('cold_cache') is True,
                  f'attempt {number + 1}: process is not marked cold_cache')
            check(record.get('cache_before_present') is False or record.get('cache_launch') == [],
                  f'attempt {number + 1}: no evidence of empty cache at launch')
        cache_after = record.get('cache_after') or []
        if isinstance(cache_after, dict):
            cache_after = [cache_after]
        cache_dir = directory / 'cache-after'
        cache_files = {}
        if cache_after:
            for item in cache_after:
                relative = item.get('path', '')
                archived = cache_dir / relative
                check(archived.is_file(),
                      f'attempt {number + 1}: cache-after file missing: {relative}')
                if archived.is_file():
                    check(sha(archived.read_bytes()) == item.get('sha256'),
                          f'attempt {number + 1}: cache-after hash mismatch: {relative}')
                    check(archived.stat().st_size == item.get('bytes'),
                          f'attempt {number + 1}: cache-after size mismatch: {relative}')
                    cache_files[relative] = archived
        if require_backend:
            check(bool(cache_after), f'attempt {number + 1}: no cache-after evidence')
            backend_log = directory / 'emulator.log'
            check(backend_log.is_file(), f'attempt {number + 1}: missing emulator.log')
            if backend_log.is_file():
                check(sha(backend_log.read_bytes()) == record.get('emulator_log_sha256'),
                      f'attempt {number + 1}: emulator log hash mismatch')
                backend = parse_backend(backend_log, manifest['counts']['programs'])
                check(backend['fail_to_get_program'] == 0,
                      f'attempt {number + 1}: Fail to get program present')
                check(bool(backend['draws']),
                      f'attempt {number + 1}: no VP_GXM_GLDRAW evidence')
                check(not backend['failed'], f'attempt {number + 1}: backend program failed')
                ready_text = '\n'.join(backend['ready'])
                for token, program in programs.items():
                    vs = target_gxp[program['vertex_stage']]['internal_sha256']
                    fs = target_gxp[program['fragment_stage']]['internal_sha256']
                    check(vs in ready_text and fs in ready_text,
                          f'attempt {number + 1}: token {token} has no program identity in backend log')
                for stage_index, facts in target_gxp.items():
                    if manifest['stages'][stage_index]['stage'] != 'vert':
                        continue
                    hits = [name for name in cache_files
                            if name.endswith('-' + facts['internal_sha256'] + '.vert')]
                    check(bool(hits),
                          f'attempt {number + 1}: no VS GLSL for stage {stage_index}')
                    for name in hits:
                        text_glsl = cache_files[name].read_text(
                            encoding='utf-8', errors='replace')
                        array = re.search(r'vec4\s+r\[(\d+)\]\s*;', text_glsl)
                        body = re.sub(r'vec4\s+r\[\d+\]\s*;', '', text_glsl)
                        indexes = [int(v) for v in re.findall(r'r\[(\d+)\]', body)]
                        if array:
                            check(int(array.group(1)) >= facts['temp_vec4_required'],
                                  f'attempt {number + 1}: GLSL array smaller than temp1 for stage {stage_index}')
                            check(not indexes or max(indexes) < int(array.group(1)),
                                  f'attempt {number + 1}: GLSL temp index out of bounds for stage {stage_index}')
                        else:
                            problems.append(
                                f'attempt {number + 1}: no r[] declaration for stage {stage_index}')
                during = [event for event in backend['exceptions']
                          if record.get('result') != 'COMPLETE' or event['ts'] is None
                          or backend['closed_ts'] is None or event['ts'] < backend['closed_ts']]
                check(not during, f'attempt {number + 1}: exception during draws')
                attempt_evidence['backend'] = {
                    'ready': len(backend['ready']),
                    'failed': len(backend['failed']),
                    'draws': len(backend['draws']),
                    'post_last_draw_exceptions': len(backend['exceptions']) - len(during),
                }
        attempt_evidence['cache_files'] = len(cache_files)
        evidence.append(attempt_evidence)
        previous = text
        previous_record = record
        previous_directory = directory
    check(bool(attempts), 'no attempts')
    count = manifest['counts']['programs']
    check(bool(re.fullmatch(r'RESULT COMPLETE terminal=%d/%d' % (count, count),
                           previous.rstrip().split('\n')[-1] if previous else '')), 'no final complete traversal')
    return dict(schema=1, provenance_verified=not problems, problems=problems,
                run_identity=contract['identity'], index_sha256=contract['index_sha256'],
                vpk_sha256=vpk_sha, eboot_sha256=eboot_sha, emulator_sha256=emulator_sha,
                sources=contract['sources'], attempts=evidence)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--attempt', type=Path, action='append', required=True)
    parser.add_argument('--vpk', type=Path, required=True)
    parser.add_argument('--emulator', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--require-backend-evidence', action='store_true')
    parser.add_argument('--require-cold-cache', action='store_true')
    args = parser.parse_args()
    result = audit(args.bundle, args.attempt, args.vpk, args.emulator,
                   require_backend=args.require_backend_evidence,
                   require_cold=args.require_cold_cache)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['provenance_verified'] else 1)

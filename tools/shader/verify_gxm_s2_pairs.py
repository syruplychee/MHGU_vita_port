"""Verify the S2/P4 observed pair/ABI run.

The device log is produced by src/probes/gxm_s2_pair_probe.c.  This tool checks
that every program token reached one terminal state, that the old set ran
before the expanded set, that the logged pair key recomputes from the manifest
identity and the reflected attribute layout, that a stage's reflected ABI is
stable wherever it appears, and that the pixel pass was deterministic.  Pair
link/bind/draw failures and classified crashes are reported as failed pairs,
not as verifier inconsistencies; structural gaps are listed in `problems`, and
`all_pairs_passed` distinguishes a complete matrix that hit classified
failures.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_gxm_s2_execution import evaluate_execution

FNV_OFFSET = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3
MASK64 = (1 << 64) - 1
VIEWPORT = 1
INDEX_SOURCE_16BIT = 0
BLEND_ZERO = 6
BLEND_STATE = ','.join(['0'] * BLEND_ZERO)
PIXEL_CAP = 64 * 16
ABI_KINDS = {'v', 'f'}

STAGES = {'create', 'bind', 'execute', 'pixel'}


def fnv_bytes(value, data):
    for byte in data:
        value ^= byte
        value = (value * FNV_PRIME) & MASK64
    return value


def fnv_u32(value, number):
    return fnv_bytes(value, int(number & 0xffffffff).to_bytes(4, 'little'))


def fnv_u64(value, number):
    return fnv_bytes(value, int(number & MASK64).to_bytes(8, 'little'))


def pair_key(vs_stage, fs_stage, vs_key, fs_key, stride, layout):
    value = FNV_OFFSET
    value = fnv_u32(value, vs_stage)
    value = fnv_u32(value, fs_stage)
    value = fnv_bytes(value, bytes.fromhex(vs_key))
    value = fnv_bytes(value, bytes.fromhex(fs_key))
    value = fnv_u32(value, VIEWPORT)
    value = fnv_u32(value, INDEX_SOURCE_16BIT)
    value = fnv_u32(value, stride)
    value = fnv_u64(value, layout)
    for _ in range(BLEND_ZERO):
        value = fnv_u32(value, 0)
    return value


def parse_fields(line):
    return {key: value for key, value in re.findall(r'(\w+)=([^ ]+)', line)}


def parse_log(path):
    lines = Path(path).read_text(encoding='utf-8', errors='replace').splitlines()
    pairs = []
    abis = []
    index = {}
    control = {}
    result = None
    gxp = []
    p5cases = []
    pixels = []
    probe = []
    steps = []
    for number, line in enumerate(lines):
        if line.startswith('INDEX ok '):
            fields = parse_fields(line)
            index = {key: int(fields[key]) for key in
                     ('stages', 'variants', 'programs', 'abi') if key in fields}
        elif line.startswith('CONTROL pixel='):
            control = parse_fields(line)
        elif line.startswith('ABI token='):
            fields = parse_fields(line)
            fields['line'] = number
            abis.append(fields)
        elif line.startswith('PAIR token='):
            fields = parse_fields(line)
            fields['line'] = number
            pairs.append(fields)
        elif line.startswith('GXP token='):
            gxp.append(line)
        elif line.startswith('P5-CASE token='):
            p5cases.append(line)
        elif line.startswith('PIXEL token='):
            pixels.append(line)
        elif line.startswith('PROBE '):
            probe.append(line)
        elif line.startswith('STEP token='):
            steps.append(line)
        elif line.startswith('RESULT '):
            result = line.split()[1]
    return {'pairs': pairs, 'abis': abis, 'index': index,
            'control': control, 'result': result, 'lines': len(lines),
            'gxp': gxp, 'p5cases': p5cases, 'pixels': pixels,
            'probe': probe, 'steps': steps}


def verify(log_path, manifest_path, emulator_log=None, process_path=None,
           cache_dir=None, expectations_path=None, require_execution=False):
    manifest = json.loads(Path(manifest_path).read_text(encoding='utf-8'))
    log = parse_log(log_path)
    problems = []
    counts = manifest['counts']
    phases = manifest['phases']
    expected_programs = counts['programs']
    programs = {program['token']: program for program in manifest['programs']}
    stages = {stage['index']: stage for stage in manifest['stages']}
    if len(programs) != expected_programs:
        problems.append('manifest program count mismatch')
    if len(stages) != counts['stages']:
        problems.append('manifest stage count mismatch')
    for program in programs.values():
        for role, kind in (('vertex_stage', 'vert'), ('fragment_stage', 'frag')):
            stage = stages.get(program.get(role))
            if stage is None or stage.get('stage') != kind:
                problems.append('manifest token %s %s kind mismatch' % (program['token'], role))
    if not log['result'] == 'COMPLETE':
        problems.append('device log has no RESULT COMPLETE')
    if not log['index']:
        problems.append('device log has no INDEX ok line')
    else:
        for key, expected in (('stages', counts['stages']),
                              ('variants', counts['variants']),
                              ('programs', counts['programs'])):
            if log['index'].get(key) != expected:
                problems.append('index %s=%s expected %s'
                                % (key, log['index'].get(key), expected))
    if not log['control']:
        problems.append('device log has no CONTROL pixel line')
    elif log['control'].get('pixel') in (None, '00000000'):
        problems.append('control draw wrote no pixel: %s' % log['control'])

    by_token = {}
    for pair in log['pairs']:
        try:
            token = int(pair['token'])
        except (KeyError, ValueError):
            problems.append('pair line without token: %s' % pair)
            continue
        if token in by_token:
            problems.append('duplicate terminal line for token %d' % token)
            continue
        if token not in programs:
            problems.append('terminal line for unknown token %d' % token)
            continue
        by_token[token] = pair
    missing = sorted(set(programs) - set(by_token))
    for token in missing:
        problems.append('token %d has no terminal state' % token)

    old_tokens = set(range(phases['old']['first'], phases['old']['last'] + 1))
    expanded_tokens = set(range(phases['expanded']['first'],
                                phases['expanded']['last'] + 1))
    if len(old_tokens) != counts['old'] or len(expanded_tokens) != counts['expanded'] or \
            old_tokens & expanded_tokens or old_tokens | expanded_tokens != set(programs):
        problems.append('manifest phase boundaries/counts mismatch')
    for token, program in programs.items():
        expected_phase = 'old' if token in old_tokens else 'expanded'
        if program.get('phase') != expected_phase:
            problems.append('manifest token %d phase mismatch' % token)
    old_lines = [by_token[t]['line'] for t in old_tokens if t in by_token]
    expanded_lines = [by_token[t]['line'] for t in expanded_tokens if t in by_token]
    if old_lines and expanded_lines and max(old_lines) > min(expanded_lines):
        problems.append('expanded pair ran before an old pair')

    status_counts = {'ok': 0, 'fail': 0, 'crash': 0}
    stage_counts = {'create': 0, 'bind': 0, 'execute': 0, 'pixel': 0}
    old_ok = old_fail = old_covered = 0
    expanded_ok = expanded_fail = expanded_covered = 0
    pixel_empty = 0
    covered = 0
    failures = []
    pair_hashes = {}
    for token in sorted(by_token):
        pair = by_token[token]
        program = programs[token]
        status = pair.get('status', '')
        stage = pair.get('stage', '')
        phase = program['phase']
        if status not in status_counts:
            problems.append('token %d has invalid status %s' % (token, status))
            continue
        status_counts[status] += 1
        if pair.get('viewport') != str(VIEWPORT):
            problems.append('token %d viewport mismatch' % token)
        if pair.get('isrc') != str(INDEX_SOURCE_16BIT):
            problems.append('token %d index source mismatch' % token)
        if pair.get('blend') != BLEND_STATE:
            problems.append('token %d blend mismatch' % token)
        try:
            cov = int(pair['cov'])
            nonzero = int(pair['nonzero'])
        except (KeyError, ValueError):
            cov = nonzero = None
            problems.append('token %d has unreadable pixel counts' % token)
        if cov is not None and not 0 <= cov <= PIXEL_CAP:
            problems.append('token %d coverage %d outside 0..%d'
                            % (token, cov, PIXEL_CAP))
        if nonzero is not None and not 0 <= nonzero <= PIXEL_CAP:
            problems.append('token %d nonzero %d outside 0..%d'
                            % (token, nonzero, PIXEL_CAP))
        if status == 'ok':
            if stage != 'none' or pair.get('vp') != '1' or pair.get('fp') != '1':
                problems.append('token %d ok result lacks successful program creation' % token)
            for field, cap in (('attrs', 16), ('uni_v', 128), ('uni_f', 128), ('smp_v', 16), ('smp_f', 16)):
                try:
                    valid = 0 <= int(pair[field]) <= cap
                except (KeyError, ValueError):
                    valid = False
                if not valid:
                    problems.append('token %d invalid %s count' % (token, field))
            if phase == 'old':
                old_ok += 1
            else:
                expanded_ok += 1
            if pair.get('stable') != '1':
                problems.append('token %d pixel result is not stable' % token)
            if cov is None:
                problems.append('token %d has no pixel coverage' % token)
            elif 0 <= cov <= PIXEL_CAP:
                if cov > 0:
                    covered += 1
                    if phase == 'old':
                        old_covered += 1
                    else:
                        expanded_covered += 1
                else:
                    pixel_empty += 1
            if pair.get('vs_stage') != str(program['vertex_stage']) or \
                    pair.get('fs_stage') != str(program['fragment_stage']):
                problems.append('token %d stage identity mismatch' % token)
                continue
            try:
                layout = int(pair['layout'], 16)
                stride = int(pair['stride'])
                if not 0 < stride <= 512 or stride % 4:
                    problems.append('token %d invalid vertex stride' % token)
                if not re.fullmatch(r'[0-9a-fA-F]{16}', pair.get('hash', '')):
                    problems.append('token %d invalid pixel hash' % token)
                expected_key = pair_key(
                    program['vertex_stage'], program['fragment_stage'],
                    stages[program['vertex_stage']]['variant_key'],
                    stages[program['fragment_stage']]['variant_key'],
                    stride, layout)
                if int(pair['key'], 16) != expected_key:
                    problems.append('token %d pair key does not recompute' % token)
            except (KeyError, ValueError):
                problems.append('token %d has an unreadable pair key' % token)
            try:
                identity = (program['vertex_stage'], program['fragment_stage'])
                digest = (pair['hash'], pair['cov'])
                if identity in pair_hashes and pair_hashes[identity] != digest:
                    problems.append('pair %s pixel hash changed between phases'
                                    % (identity,))
                pair_hashes[identity] = digest
            except KeyError:
                problems.append('token %d has no pixel hash' % token)
        else:
            if phase == 'old':
                old_fail += 1
            else:
                expanded_fail += 1
            if pair.get('vs_stage') != str(program['vertex_stage']) or \
                    pair.get('fs_stage') != str(program['fragment_stage']):
                problems.append('token %d stage identity mismatch' % token)
            if stage not in STAGES:
                problems.append('token %d failed with invalid stage %s'
                                % (token, stage))
            else:
                stage_counts[stage] += 1
            failures.append({'token': token, 'status': status, 'stage': stage,
                             'detail': pair.get('detail', ''),
                             'vs_stage': program['vertex_stage'],
                             'fs_stage': program['fragment_stage']})

    # Stage reflection must not change when the same stage is linked again.
    abi_by_stage = {}
    abi_by_token = {}
    for abi in log['abis']:
        try:
            token = int(abi['token'])
            kind = abi['kind']
            index = int(abi['index'])
            digest = abi['digest']
            attrs = abi['attrs']
            uniforms = abi['uni']
            samplers = abi['smp']
            layout = abi['layout']
        except (KeyError, ValueError):
            problems.append('unreadable ABI line: %s' % abi)
            continue
        if index not in stages:
            problems.append('ABI line for unknown stage %d' % index)
            continue
        if token not in programs:
            problems.append('ABI line for unknown token %d' % token)
            continue
        if kind not in ABI_KINDS:
            problems.append('ABI token %d has invalid kind %s' % (token, kind))
            continue
        try:
            if not (0 <= int(attrs) <= 16 and 0 <= int(uniforms) <= 128 and 0 <= int(samplers) <= 16):
                raise ValueError('ABI counts out of bounds')
            if not re.fullmatch(r'[0-9a-fA-F]{16}', digest) or not re.fullmatch(r'[0-9a-fA-F]{16}', layout):
                raise ValueError('ABI digest/layout malformed')
            if 'cube' in abi and not 0 <= int(abi['cube'], 16) <= 0xffff:
                raise ValueError('ABI cube mask out of bounds')
            if 'ub_bytes' in abi and not 0 <= int(abi['ub_bytes']) <= 0xffffffff:
                raise ValueError('ABI uniform size out of bounds')
        except ValueError:
            problems.append('ABI token %d has invalid counts/digest/layout' % token)
            continue
        expected_index = (programs[token]['vertex_stage'] if kind == 'v'
                          else programs[token]['fragment_stage'])
        if index != expected_index:
            problems.append('ABI token %d kind=%s joins stage %d expected %d'
                            % (token, kind, index, expected_index))
            continue
        expected_kind = 'vert' if kind == 'v' else 'frag'
        if stages[index].get('stage') != expected_kind:
            problems.append('ABI token %d kind=%s joins %s stage %d'
                            % (token, kind, stages[index].get('stage'), index))
            continue
        signature = (digest, attrs, uniforms, samplers, layout,
                     abi.get('cube'), abi.get('ub_bytes'))
        if index in abi_by_stage and abi_by_stage[index] != signature:
            problems.append('stage %d reflection is not stable' % index)
        abi_by_stage[index] = signature
        abi_by_token[(token, kind)] = abi
    missing_abi = set()
    for token, pair in by_token.items():
        if pair.get('status') != 'ok':
            continue
        program = programs[token]
        for kind, mapping in (('v', (('attrs', 'attrs'), ('uni_v', 'uni'), ('smp_v', 'smp'), ('layout', 'layout'))),
                              ('f', (('uni_f', 'uni'), ('smp_f', 'smp')))):
            own_abi = abi_by_token.get((token, kind))
            if own_abi is None:
                problems.append('token %d lacks its own %s ABI' % (token, kind))
            elif any(pair.get(field) != own_abi.get(abi_field) for field, abi_field in mapping):
                problems.append('token %d pair/%s ABI counts/layout mismatch' % (token, kind))
        for stage_index in (program['vertex_stage'], program['fragment_stage']):
            if stage_index not in abi_by_stage:
                missing_abi.add(stage_index)
    for stage_index in sorted(missing_abi):
        problems.append('no ABI line for stage %d of an ok pair' % stage_index)

    if old_ok and old_covered == 0:
        problems.append('no old observed pair wrote any pixel')

    all_pairs_passed = (not problems and len(by_token) == expected_programs and
                        status_counts['ok'] == expected_programs)
    execution = None
    if emulator_log is not None or process_path is not None:
        process = (json.loads(Path(process_path).read_text(encoding='utf-8'))
                   if process_path is not None else None)
        expectations = (json.loads(Path(expectations_path).read_text(encoding='utf-8'))
                        if expectations_path is not None else None)
        execution = evaluate_execution(
            log, manifest, Path(manifest_path).parent, emulator_log, process,
            cache_dir=cache_dir, expectations=expectations,
            require_execution=require_execution)
    summary = {
        'schema': 1,
        'complete': not problems,
        'all_pairs_passed': all_pairs_passed,
        'log_sha256': hashlib.sha256(Path(log_path).read_bytes()).hexdigest(),
        'manifest_sha256': hashlib.sha256(Path(manifest_path).read_bytes()).hexdigest(),
        'index_sha256': manifest.get('index_sha256'),
        'programs': expected_programs,
        'terminal': len(by_token),
        'status': status_counts,
        'failure_stages': stage_counts,
        'old': {'ok': old_ok, 'fail': old_fail, 'covered': old_covered},
        'expanded': {'ok': expanded_ok, 'fail': expanded_fail,
                     'covered': expanded_covered},
        'pixel': {'covered': covered, 'empty': pixel_empty,
                  'control': log['control'].get('pixel')},
        'stages_reflected': len(abi_by_stage),
        'failures': failures,
        'problems': problems,
        'execution_required': require_execution,
        'execution': execution,
        'execution_complete': (execution or {}).get('execution_complete', False),
        'content_complete': (execution or {}).get('content_complete', False),
    }
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--emulator-log', type=Path)
    parser.add_argument('--process', type=Path)
    parser.add_argument('--cache-dir', type=Path,
                        help='archived cache-after directory for GLSL checks')
    parser.add_argument('--expectations', type=Path,
                        help='independent Cg-interpreter expectations JSON')
    parser.add_argument('--require-execution', action='store_true')
    parser.add_argument('--require-content', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    summary = verify(args.log, args.manifest,
                     emulator_log=args.emulator_log,
                     process_path=args.process,
                     cache_dir=args.cache_dir,
                     expectations_path=args.expectations,
                     require_execution=args.require_execution)
    text = json.dumps(summary, indent=1, sort_keys=True) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding='utf-8', newline='\n')
    print(text)
    ok = summary['complete']
    if args.require_execution:
        ok = ok and summary['execution_complete']
    if args.require_content:
        ok = ok and summary['content_complete']
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

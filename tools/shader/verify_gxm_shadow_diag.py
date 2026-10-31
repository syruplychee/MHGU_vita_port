#!/usr/bin/env python3
"""Strict verifier for the M6-B22 precision and four-call liveness diagnostics.

The device log is only a set of declarations.  Every raw pixel of every
16384-byte case slice is recomputed here from pinned inputs and the independent
shadow_reference oracle.  The precision mode also checks the exact D24
rational-to-binary32 boundary model, not just an 8-bit first pixel.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import sys
from fractions import Fraction
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import shadow_reference

SMALL = 64
CASE_BYTES = SMALL * SMALL * 4
D24_MAX = (1 << 24) - 1
TOLERANCE = 1
CASE_TIMEOUT_US = 20_000_000
EMULATOR_SHA256 = \
    '8239FDBB215206379B861B7BBA126C2F7D3B11BDDBF01A5CB9E6D17ABA3651D3'
SAMPLE_MODEL = 'D24_UNORM_TO_BINARY32'
BOUNDS_BITS = (0x3F3FFFFF, 0x3F400000, 0x3F400001)
PRECISION_EXPECTED = {
    'precision_df32_075': (255, 255, 255, 255),
    'precision_s8d24_075': (255, 255, 255, 255),
    'precision_df32_025_negative': (0, 0, 255, 255),
    'precision_s8d24_025_negative': (0, 0, 255, 255),
}
PRECISION_UV = {
    'precision_df32_075': (0.25, 0.75),
    'precision_s8d24_075': (0.25, 0.75),
    'precision_df32_025_negative': (0.75, 0.25),
    'precision_s8d24_025_negative': (0.75, 0.25),
}
LIVENESS_CONFIGS = (
    ('corner', (((0.5, 0.5), 0.9), ((0.5, 0.5), 0.5),
                ((0.5, 0.5), 0.0), ((0.25, 0.25), 0.5))),
    ('edges', (((0.5, 0.25), 0.125), ((0.5, 0.75), 0.5),
               ((0.25, 0.5), 0.5), ((0.75, 0.5), 0.1))),
    ('quadrants_low_ref', (((0.25, 0.25), 0.1), ((0.75, 0.25), 0.1),
                           ((0.25, 0.75), 0.1), ((0.75, 0.75), 0.1))),
    ('quadrants_high_ref', (((0.25, 0.25), 0.9), ((0.75, 0.25), 0.9),
                            ((0.25, 0.75), 0.9), ((0.75, 0.75), 0.9))),
)
LIVENESS_WEIGHTS = (0.25, 0.125, 0.0625, 0.03125)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def f32(value: float) -> float:
    return struct.unpack('<f', struct.pack('<f', float(value)))[0]


def f32_bits(value: float) -> int:
    return struct.unpack('<I', struct.pack('<f', f32(value)))[0]


def f32_from_bits(bits: int) -> float:
    return struct.unpack('<f', struct.pack('<I', bits & 0xFFFFFFFF))[0]


def d24_code(value: float) -> int:
    return int(math.floor(f32(value) * D24_MAX + 0.5))


def d24_binary32(value: float) -> float:
    return f32(d24_code(value) / D24_MAX)


def exact_f32(bits: int) -> Fraction:
    exponent = (bits >> 23) & 0xFF
    mantissa = bits & 0x7FFFFF
    if exponent == 0:
        return Fraction(mantissa, 1 << 149)
    return Fraction((1 << 23) + mantissa, 1) * Fraction(2) ** (exponent - 150)


def nearest_f32_bits(value: Fraction) -> int:
    if value < 0:
        return nearest_f32_bits(-value) | 0x80000000
    if value == 0:
        return 0
    lo, hi = 0, 0x7F800000
    while lo < hi:
        mid = (lo + hi) // 2
        if exact_f32(mid) < value:
            lo = mid + 1
        else:
            hi = mid
    candidates = [lo - 1, lo] if lo else [lo]
    return min(candidates, key=lambda bits: (abs(exact_f32(bits) - value),
                                              bits & 1))


def precision_model_report() -> dict:
    expected_bits = {0.0: 0x00000000, 0.25: 0x3E800001,
                     0.75: 0x3F400000, 1.0: 0x3F800000}
    rows = []
    for depth, want in expected_bits.items():
        exact = Fraction(d24_code(depth), D24_MAX)
        bits = nearest_f32_bits(exact)
        rows.append({
            'depth': depth,
            'd24_code': d24_code(depth),
            'exact': '%d/%d' % (exact.numerator, exact.denominator),
            'nearest_binary32_bits': '0x%08x' % bits,
            'expected_binary32_bits': '0x%08x' % want,
            'match': bits == want and
                     f32_from_bits(bits) == d24_binary32(depth),
        })
    legacy = Fraction(d24_code(0.75), D24_MAX)
    return {
        'rows': rows,
        'all_match': all(row['match'] for row in rows),
        'd24_precision_asymmetry':
            '0.25 rounds to 0x3e800001; it is not assumed to remain 0x3e800000',
        'legacy_075_exact': '%d/%d' % (legacy.numerator, legacy.denominator),
        'legacy_075_as_double': float(legacy),
        'legacy_075_nearest_bits': '0x%08x' % nearest_f32_bits(legacy),
    }


def kv_tokens(text: str) -> dict[str, str]:
    return dict(re.findall(r'([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)', text))


class GridRow:
    def __init__(self, grid: 'Grid', y: int):
        self.grid = grid
        self.y = y

    def __getitem__(self, x: int) -> float:
        return self.grid.value(x, self.y)


class Grid:
    """Pattern A quadrants decoded with the versioned D24->binary32 model."""

    def __init__(self, size: int = SMALL):
        self.mid = size // 2

    def __getitem__(self, y: int) -> GridRow:
        return GridRow(self, y)

    def value(self, x: int, y: int) -> float:
        if not (0 <= x < SMALL and 0 <= y < SMALL):
            return d24_binary32(0.75)
        left = x < self.mid
        top = y < self.mid
        return d24_binary32(0.0 if top and left else
                           0.25 if top else
                           0.75 if left else
                           1.0)


def liveness_expected(mode: str, calls) -> dict:
    grid = Grid()
    plan = {'compare_func': 1, 'filter': mode, 'wrap_u': 2, 'wrap_v': 2,
            'border_depth': 0.0}
    sample = (shadow_reference.sample_point if mode == 'point'
              else shadow_reference.sample_linear)
    taps = [sample(grid, (SMALL, SMALL), (f32(u), f32(v)), f32(ref), plan)
            for (u, v), ref in calls]
    acc = taps[0] * LIVENESS_WEIGHTS[0]
    acc = acc + taps[1] * LIVENESS_WEIGHTS[1]
    acc = acc + taps[2] * LIVENESS_WEIGHTS[2]
    acc = acc + ((taps[3] if taps[0] > 0.5 else 0.0) * LIVENESS_WEIGHTS[3])
    return {'taps': taps, 'value': acc,
            'expected8': int(math.floor(f32(acc) * 255.0 + 0.5))}


def parse_log(text: str, problems: list[str], kind: str) -> dict:
    parsed = {'schema': None, 'identity': [], 'cases': {}, 'results': {},
              'produces': {}, 'summary': None, 'verdict': None,
              'fatal': []}
    for line in text.splitlines():
        if line.startswith('DIAG_SCHEMA '):
            parsed['schema'] = line.split(None, 1)[1].strip()
        elif line.startswith('IDENTITY '):
            parsed['identity'].append(kv_tokens(line))
        elif line.startswith('PRODUCE '):
            tokens = kv_tokens(line)
            parsed['produces'][tokens.get('tag', '')] = tokens
        elif line.startswith('CASE_RESULT '):
            tokens = kv_tokens(line)
            try:
                case_id = int(tokens['id'])
            except (KeyError, ValueError):
                problems.append('malformed CASE_RESULT: ' + line)
                continue
            if case_id in parsed['results']:
                problems.append('duplicate CASE_RESULT id %d' % case_id)
            parsed['results'][case_id] = tokens
        elif line.startswith('CASE '):
            tokens = kv_tokens(line)
            try:
                case_id = int(tokens['id'])
            except (KeyError, ValueError):
                problems.append('malformed CASE: ' + line)
                continue
            if case_id in parsed['cases']:
                problems.append('duplicate CASE id %d' % case_id)
            parsed['cases'][case_id] = tokens
        elif line.startswith('SUMMARY '):
            if parsed['summary'] is not None:
                problems.append('duplicate SUMMARY line')
            parsed['summary'] = kv_tokens(line)
        elif line.startswith('RESULT '):
            parsed['verdict'] = line.split(None, 1)[1].strip()
        elif re.search(r'(?i)(FAIL|FAILURE|MISMATCH|ABORT|EXCEPTION)', line):
            if line.startswith('CASE_RESULT'):
                continue
            parsed['fatal'].append(line)
    return parsed


def check_process(process_path: Path | None, problems: list[str]) -> dict:
    record = {'path': str(process_path) if process_path else None,
              'present': False}
    if process_path is None:
        return record
    try:
        data = process_path.read_bytes()
        process = json.loads(data.decode('utf-8'))
    except (OSError, ValueError) as error:
        problems.append('cannot read process record: %s' % error)
        return record
    record.update({'present': True, 'sha256': sha256_bytes(data),
                   'result': process.get('result'),
                   'emulator_sha256': process.get('emulator_sha256'),
                   'vpk_sha256': process.get('vpk_sha256'),
                   'title_id': process.get('title_id'),
                   'natural_exit': process.get('natural_exit')})
    if process.get('result') != 'COMPLETE':
        problems.append('process result is %r, not COMPLETE' %
                        process.get('result'))
    if process.get('emulator_sha256') != EMULATOR_SHA256:
        problems.append('emulator SHA %r != B16 %s' %
                        (process.get('emulator_sha256'), EMULATOR_SHA256))
    if process.get('title_id') != 'MHGUSH001':
        problems.append('title %r != MHGUSH001' % process.get('title_id'))
    return record


def check_contract_precision(contract: dict, problems: list[str]) -> dict:
    probe = contract.get('precision_probe') or {}
    if contract.get('sample_model') != SAMPLE_MODEL:
        problems.append('contract sample_model %r != %r' %
                        (contract.get('sample_model'), SAMPLE_MODEL))
    if probe.get('sample_model') != SAMPLE_MODEL:
        problems.append('precision probe sample_model %r' %
                        probe.get('sample_model'))
    if list(probe.get('bounds_bits') or []) != list(BOUNDS_BITS):
        problems.append('contract precision bounds %r != %r' %
                        (probe.get('bounds_bits'), BOUNDS_BITS))
    cases = probe.get('cases') or []
    names = [case.get('name') for case in cases]
    if names != list(PRECISION_EXPECTED):
        problems.append('contract precision case names %r' % names)
    for case in cases:
        name = case.get('name')
        expected = tuple(case.get('expected') or ())
        if expected != PRECISION_EXPECTED.get(name):
            problems.append('%s: contract expected %r != %r' %
                            (name, expected, PRECISION_EXPECTED.get(name)))
        uv = PRECISION_UV.get(name)
        if uv is not None and (case.get('u_bits') != f32_bits(uv[0]) or
                               case.get('v_bits') != f32_bits(uv[1])):
            problems.append('%s: contract uv bits are not the pinned probe' %
                            name)
    return probe


def check_contract_liveness(contract: dict, problems: list[str]) -> dict:
    probe = contract.get('liveness_probe') or {}
    if list(probe.get('weights') or []) != list(LIVENESS_WEIGHTS):
        problems.append('contract liveness weights %r' %
                        (probe.get('weights'),))
    configs = probe.get('configs') or []
    names = [config.get('name') for config in configs]
    if names != [name for name, _ in LIVENESS_CONFIGS]:
        problems.append('contract liveness config names %r' % names)
    cases = probe.get('cases') or []
    if len(cases) != 8:
        problems.append('contract liveness has %d cases, expected 8' %
                        len(cases))
    for case in cases:
        mode = case.get('mode')
        config = next((calls for name, calls in LIVENESS_CONFIGS
                       if name == case.get('config')), None)
        if config is None or mode not in ('point', 'linear'):
            problems.append('%s: unknown mode/config' % case.get('name'))
            continue
        calls = case.get('calls') or []
        pinned = [{'u_bits': f32_bits(u), 'v_bits': f32_bits(v),
                   'ref_bits': f32_bits(ref)} for (u, v), ref in config]
        if calls != pinned:
            problems.append('%s: contract calls are not the pinned config' %
                            case.get('name'))
        expected = liveness_expected(mode, config)
        declared = case.get('expected') or {}
        if declared.get('point8') != expected['expected8']:
            problems.append('%s: contract expected8 %r != oracle %d' %
                            (case.get('name'), declared.get('point8'),
                             expected['expected8']))
    return probe


def verify(log_path: Path, dump_path: Path, contract_path: Path,
           kind: str, emulator_log_path: Path | None = None,
           process_path: Path | None = None) -> dict:
    problems: list[str] = []
    model = precision_model_report()
    if not model['all_match']:
        problems.append('exact D24 rational/binary32 model mismatch')
    if model['legacy_075_as_double'] == 0.75:
        problems.append('legacy double model unexpectedly equals 0.75')
    if model['legacy_075_nearest_bits'] != '0x3f400000':
        problems.append('legacy 0.75 does not round to binary32 0.75')

    contract_data = contract_path.read_bytes()
    contract = json.loads(contract_data.decode('utf-8'))
    if kind == 'precision':
        contract_probe = check_contract_precision(contract, problems)
        expected_cases = PRECISION_EXPECTED
        case_count = 4
        gxp_key = 'gxp_sha256'
        source_key = 'source_sha256'
        identity_gxp_key = 'precision_gxp'
        identity_source_key = 'precision_source'
    else:
        contract_probe = check_contract_liveness(contract, problems)
        expected_cases = None
        case_count = 8
        gxp_key = 'gxp_sha256'
        source_key = 'source_sha256'
        identity_gxp_key = 'liveness_gxp'
        identity_source_key = 'liveness_source'
    gxp_sha = contract_probe.get(gxp_key)
    source_sha = contract_probe.get(source_key)

    log_data = log_path.read_bytes()
    text = log_data.decode('utf-8', errors='replace')
    log = parse_log(text, problems, kind)
    dump = dump_path.read_bytes()
    if len(dump) != case_count * CASE_BYTES:
        problems.append('dump is %d bytes, expected %d' %
                        (len(dump), case_count * CASE_BYTES))
    if log['schema'] != '1':
        problems.append('DIAG_SCHEMA is %r, not 1' % log['schema'])
    if len(log['identity']) != 1:
        problems.append('expected one IDENTITY line, found %d' %
                        len(log['identity']))
    identity = log['identity'][0] if log['identity'] else {}
    if identity.get('kind') != kind:
        problems.append('IDENTITY kind %r != %r' %
                        (identity.get('kind'), kind))
    if identity.get('title') != 'MHGUSH001':
        problems.append('IDENTITY title %r != MHGUSH001' %
                        identity.get('title'))
    if identity.get('sample_model') != SAMPLE_MODEL:
        problems.append('IDENTITY sample_model %r != %r' %
                        (identity.get('sample_model'), SAMPLE_MODEL))
    if identity.get(identity_gxp_key) != gxp_sha:
        problems.append('IDENTITY GXP %r != contract %r' %
                        (identity.get(identity_gxp_key), gxp_sha))
    if identity.get(identity_source_key) != source_sha:
        problems.append('IDENTITY source %r != contract %r' %
                        (identity.get(identity_source_key), source_sha))
    if log['verdict'] != 'COMPLETE':
        problems.append('RESULT is %r, not COMPLETE' % log['verdict'])
    summary = log['summary'] or {}
    if summary.get('cases') != str(case_count):
        problems.append('SUMMARY cases %r != %d' %
                        (summary.get('cases'), case_count))
    if summary.get('dump_bytes') != str(case_count * CASE_BYTES):
        problems.append('SUMMARY dump_bytes %r != %d' %
                        (summary.get('dump_bytes'),
                         case_count * CASE_BYTES))
    if summary.get('failed') != '0':
        problems.append('SUMMARY failed %r != 0' % summary.get('failed'))
    for line in log['fatal']:
        problems.append('device failure line: ' + line)

    if set(log['cases']) != set(range(1, case_count + 1)):
        problems.append('CASE id set %r' % sorted(log['cases']))
    if set(log['results']) != set(range(1, case_count + 1)):
        problems.append('CASE_RESULT id set %r' % sorted(log['results']))

    pixel_bad_total = 0
    case_expected = {}
    if kind == 'precision':
        for case_id in range(1, case_count + 1):
            case = log['cases'].get(case_id)
            if case is None:
                continue
            name = case.get('name')
            expected = expected_cases.get(name)
            if expected is None:
                problems.append('unknown precision case %r' % name)
                continue
            case_expected[case_id] = expected
            uv = PRECISION_UV[name]
            if (int(case.get('u_bits', '0'), 0) != f32_bits(uv[0]) or
                    int(case.get('v_bits', '0'), 0) != f32_bits(uv[1])):
                problems.append('%s: log uv bits differ from the pinned input'
                                % name)
            for key, value in (('lower', BOUNDS_BITS[0]),
                               ('equal', BOUNDS_BITS[1]),
                               ('upper', BOUNDS_BITS[2])):
                if int(case.get(key, '0'), 0) != value:
                    problems.append('%s: log %s bound %r != 0x%08x' %
                                    (name, key, case.get(key), value))
    else:
        for case_id in range(1, case_count + 1):
            case = log['cases'].get(case_id)
            if case is None:
                continue
            name = case.get('name')
            mode = 'point' if case.get('mode') == 'point' else \
                   'linear' if case.get('mode') == 'linear' else None
            config_name = name.rsplit('_', 1)[-1] if name else ''
            if mode is None:
                problems.append('%s: log mode %r' % (name, case.get('mode')))
                continue
            # The config name is encoded in the case name after "liveness_".
            prefix = 'liveness_%s_' % mode
            if not name.startswith(prefix):
                problems.append('%s: case name does not encode mode/config' %
                                name)
                continue
            config_name = name[len(prefix):]
            config = next((calls for label, calls in LIVENESS_CONFIGS
                           if label == config_name), None)
            if config is None:
                problems.append('%s: unknown liveness config %r' %
                                (name, config_name))
                continue
            expected = liveness_expected(mode, config)['expected8']
            case_expected[case_id] = (expected, expected, expected, 255)
            for index, ((u, v), ref) in enumerate(config):
                prefix_bits = 'c%d' % index
                tokens = case.get(prefix_bits, '').split(',')
                if [int(token, 0) for token in tokens] != [
                        f32_bits(u), f32_bits(v), f32_bits(ref)]:
                    problems.append('%s: log %s input bits differ from pinned'
                                    % (name, prefix_bits))
            if case.get('expected8') != str(expected):
                problems.append('%s: log expected8 %r != oracle %d' %
                                (name, case.get('expected8'), expected))
        distinct = sorted(set(value[0] for value in case_expected.values()))
        if len(distinct) < 4:
            problems.append('liveness expectations are not four distinct '
                            'values: %r' % distinct)
        if not any(mode == 'linear' for mode in
                   (name.split('_')[1] for name in
                    (log['cases'][i].get('name', '') for i in log['cases']))):
            problems.append('liveness has no linear/PCF case')

    for case_id in range(1, case_count + 1):
        result = log['results'].get(case_id)
        if result is None:
            continue
        name = (log['cases'].get(case_id) or {}).get('name', 'case%d' % case_id)
        expected = case_expected.get(case_id)
        offset = (case_id - 1) * CASE_BYTES
        if int(result.get('dump_offset', '-1')) != offset:
            problems.append('%s: dump offset %r != %d' %
                            (name, result.get('dump_offset'), offset))
        if int(result.get('dump_bytes', '-1')) != CASE_BYTES:
            problems.append('%s: dump bytes %r != %d' %
                            (name, result.get('dump_bytes'), CASE_BYTES))
        if result.get('executed') != '1':
            problems.append('%s: executed %r' % (name, result.get('executed')))
        if result.get('readback') != '1':
            problems.append('%s: readback %r' % (name, result.get('readback')))
        try:
            elapsed = int(result.get('elapsed_us', '-1'))
        except ValueError:
            elapsed = -1
        if not 0 <= elapsed <= CASE_TIMEOUT_US:
            problems.append('%s: elapsed %r outside %d us' %
                            (name, result.get('elapsed_us'),
                             CASE_TIMEOUT_US))
        if expected is not None:
            data = dump[offset:offset + CASE_BYTES]
            bad = 0
            for p in range(0, CASE_BYTES, 4):
                pixel = data[p:p + 4]
                if tuple(pixel) != expected:
                    bad += 1
            pixel_bad_total += bad
            if bad:
                problems.append('%s: %d/%d raw pixels differ from %r first=%r'
                                % (name, bad, SMALL * SMALL, expected,
                                   tuple(data[0:4])))
            if result.get('status') != 'PASS':
                problems.append('%s: device status %r' %
                                (name, result.get('status')))

    process = check_process(process_path, problems)
    emulator_log = {'path': str(emulator_log_path) if emulator_log_path
                    else None, 'present': False}
    if emulator_log_path is not None:
        try:
            engine = emulator_log_path.read_bytes()
            emulator_log.update({'present': True,
                                 'sha256': sha256_bytes(engine)})
            engine_text = engine.decode('utf-8', errors='replace')
            exceptions = [line for line in engine_text.splitlines()
                          if re.search(r'(?i)(EXCEPTION_[A-Z_]+|abort\()',
                                       line)]
            emulator_log['exception_lines'] = exceptions[:20]
        except OSError as error:
            problems.append('missing emulator log: %s' % error)

    return {
        'schema': 1,
        'task': 'M6-B22',
        'kind': kind,
        'contract': str(contract_path),
        'contract_sha256': sha256_bytes(contract_data),
        'log': str(log_path),
        'log_sha256': sha256_bytes(log_data),
        'dump': str(dump_path),
        'dump_bytes': len(dump),
        'dump_sha256': sha256_bytes(dump),
        'gxp_sha256': gxp_sha,
        'source_sha256': source_sha,
        'identity': identity,
        'case_count': case_count,
        'pixel_bad_total': pixel_bad_total,
        'precision_model': model,
        'process': process,
        'emulator_log': emulator_log,
        'summary': summary,
        'verdict': log['verdict'],
        'complete': not problems,
        'problems': problems,
        'claims': {
            'gpu_raw_pixels_verified': not problems,
            'all_pixels_checked': True,
            'production_stage_admitted': False,
            'actual_game_rendered': False,
            'hardware_verified': False,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kind', choices=('precision', 'liveness'),
                        required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--dump', type=Path, required=True)
    parser.add_argument('--contract', type=Path, required=True)
    parser.add_argument('--emulator-log', type=Path)
    parser.add_argument('--process', type=Path)
    parser.add_argument('--report', type=Path)
    arguments = parser.parse_args()
    summary = verify(arguments.log, arguments.dump, arguments.contract,
                     arguments.kind, arguments.emulator_log,
                     arguments.process)
    if arguments.report:
        arguments.report.write_text(json.dumps(summary, indent=2) + '\n',
                                    encoding='utf-8', newline='\n')
    print(json.dumps(summary, indent=2))
    return 0 if summary['complete'] else 1


if __name__ == '__main__':
    sys.exit(main())

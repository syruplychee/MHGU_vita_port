#!/usr/bin/env python3
"""Verify the M6-B20B software-shadow content probe from raw evidence.

The device log is treated as a set of declarations and terminal states, never
as proof.  Every expected 8-bit value is recomputed here from the fixed case
contract and the independent ``shadow_reference`` oracle; the raw 64x64 RGBA
readback must then agree within one 8-bit LSB.  A CASE_RESULT PASS string, a
green SHADOW_PLAN_REJECT row, or a complete-looking dump is insufficient.

Strict checks:

  * exactly 65 unique cases, ids 1..65, declarations matching the contract
    (name, program, resource, produce, A/B/A phase, TSC words, ref/uv bits and
    all four oracle expectations);
  * exactly 65 terminal CASE_RESULT rows, each with one 16384-byte dump slice;
    offsets tile the 1,064,960-byte payload with no hole, overlap or duplicate;
  * every pixel of every consumer surface matches the independently computed
    point/compare-first-PCF/dispatch values, alpha is 255, and device counts
    agree with the bytes;
  * the two compare-first negative cases produce a raw result distinguishable
    from the rejected depth-first order;
  * DF32 and S8D24 A/B/A groups return to byte-identical A images on one VA and
    B changes the pixels;
  * all 17 planner negatives appear exactly once with the contract path, code,
    independent status name, pass=1 and draw=0;
  * O0 and O1 are reported as separate mechanisms: distinct GXP identities are
    checked, O1 must report program readiness and its one smoke case must run,
    and a refusal/timeout/abort is an explicit failure;
  * the atlas producer is a real 1024x3072 allocation beyond row 2048 under the
    32 MiB cap, and its two cases are checked in the same raw way;
  * RESULT COMPLETE and a clean SUMMARY are required, with any device failure
    line reported.

The image is evidence about this locked OpenGL backend only.  It stores depth
through GL_DEPTH24_STENCIL8 even for a native DF32 allocation, so the tolerance
is one 8-bit LSB; this is not F32 hardware equivalence.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import struct
import sys
from fractions import Fraction
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shadow_helpers
import shadow_reference

SMALL = 64
ATLAS_W = 1024
ATLAS_H = 3072
ATLAS_REGION = (480, 2584, 543, 2647)
ATLAS_BYTES = ATLAS_W * ATLAS_H * 4
ALLOC_CAP = 32 * 1024 * 1024
CASE_BYTES = SMALL * SMALL * 4
EXPECTED_CASES = 65
EXPECTED_CASE_BYTES = EXPECTED_CASES * CASE_BYTES
EXPECTED_PLANNER_REJECTIONS = 17
EXPECTED_TOLERANCE = 1
CASE_TIMEOUT_US = 20_000_000
D24_MAX = (1 << 24) - 1
SAMPLE_MODEL = 'D24_UNORM_TO_BINARY32'
ORACLE_VERSION = 2
LEGACY_SAMPLE_MODEL = 'D24_UNORM_TO_BINARY64'

RES_DF32 = 0
RES_S8D24 = 1
RES_ATLAS = 2
PROGRAM_O0 = 0
PROGRAM_O1 = 1

# These names are pinned here rather than read back from the device or the
# contract, so a silently renamed/dropped anchor case cannot pass.
ANCHOR_NAMES = {
    1: 'cmp0_df32_br',
    8: 'cmp7_df32_br',
    13: 'point_boundary_df32',
    14: 'linear_boundary_pcf_df32',
    19: 'corner_border_one_df32',
    29: 'aba_a1_df32',
    30: 'aba_b_df32',
    31: 'aba_a2_df32',
    60: 'aba_a1_s8d24',
    61: 'aba_b_s8d24',
    62: 'aba_a2_s8d24',
    63: 'o1_smoke_point_boundary',
    64: 'atlas_point_row2600',
    65: 'atlas_linear_row2600',
}

DEPTH_VIEW_STATUS = {
    -1: 'bad-argument', -2: 'not-depth-format', -3: 'unsupported-depth-format',
    -4: 'not-block-linear', -5: 'not-2d', -6: 'layers-or-volume', -7: 'mips',
    -8: 'msaa', -9: 'unnormalized', -10: 'sparse', -11: 'srgb',
    -12: 'shape-mismatch', -13: 'non64-or-stride', -14: 'va-mismatch',
    -15: 'format-class-mismatch', -16: 'depth-unproduced',
    -17: 'depth-cleared-unmaterialized', -18: 'content-generation',
    -19: 'depth-compare-unsupported', -20: 'filter-or-mip-unsupported',
    -21: 'wrap-unsupported', -22: 'anisotropy-unsupported',
    -23: 'tsc-unsupported', -24: 'component-type-unsupported',
    -25: 'swizzle-unsupported', -26: 'shader-usage-not-r-only',
    -27: 'shadow-contract-incomplete', -28: 'shadow-descriptor-not-depth',
    -29: 'shadow-policy-refused',
}
SHADOW_STATUS = {
    0: 'ok', -1: 'bad-argument', -2: 'texture-type-not-2d',
    -3: 'texture-not-depth', -4: 'compare-disabled',
    -5: 'wrap-u-unsupported', -6: 'wrap-v-unsupported',
    -7: 'mag-filter-unsupported', -8: 'min-filter-unsupported',
    -9: 'filter-mismatch', -10: 'mipmap-filter-unsupported',
    -11: 'mip-level-unknown', -12: 'mip-level-present',
    -13: 'reduction-unsupported', -14: 'anisotropy-unsupported',
    -15: 'cubemap-anisotropy-unsupported',
    -16: 'cubemap-interface-filtering-unsupported',
    -17: 'lod-bias-unsupported', -18: 'float-coord-normalization-unsupported',
    -19: 'trilinear-optimization-unsupported', -20: 'min-lod-clamp-unsupported',
    -21: 'srgb-unsupported', -22: 'border-depth-nonfinite',
    -23: 'border-depth-out-of-range', -24: 'extent-invalid',
}

FATAL_PREFIXES = (
    'CASE_PLAN_FAIL', 'CASE_PLAN_STATE_FAIL', 'PRODUCE_UNAVAILABLE',
    'SHADOW_TEXTURE_BIND_FAIL', 'SHADOW_UNIFORM_RESERVE_FAIL',
    'SHADOW_COORD_FAIL', 'SHADOW_EXTENT_FAIL', 'SHADOW_STATE_FAIL',
    'SHADOW_BORDER_FAIL', 'CASE_VIEW_MISSING', 'CASE_TIMEOUT',
    'O1_CASE_REFUSED', 'O1_PROGRAM_FAILURE', 'CASE_RESOURCE_UNAVAILABLE',
    'GPU_ALLOC_FAIL', 'DUMP_WRITE_FAIL', 'DUMP_OPEN_FAIL', 'CASES_ABORTED',
)

GL_ERROR_RE = re.compile(
    r'(?i)(GL_INVALID_[A-Z_]+|GL_OUT_OF_MEMORY|glGetError|OpenGL error|'
    r'GL error|USSETranslatorVisitor|program link failed|'
    r'link status.*false)')
RENDERER_FORMAT_RE = re.compile(r'Unknown base format')
EXCEPTION_RE = re.compile(
    r'(?i)(EXCEPTION_[A-Z_]+|STATUS_STACK_BUFFER_OVERRUN|fastfail|abort\()')


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def f32(value) -> float:
    return struct.unpack('<f', struct.pack('<f', float(value)))[0]


def f32_from_bits(bits: int) -> float:
    return struct.unpack('<f', struct.pack('<I', bits & 0xFFFFFFFF))[0]


def f32_bits(value: float) -> int:
    return struct.unpack('<I', struct.pack('<f', f32(value)))[0]


def d24_code(value: float) -> int:
    return int(math.floor(f32(value) * D24_MAX + 0.5))


def d24_binary64(value: float) -> float:
    return d24_code(value) / D24_MAX


def d24_binary32(value: float) -> float:
    """Versioned model: D24 unorm decode followed by binary32 rounding."""
    return f32(d24_code(value) / D24_MAX)


def f32_bits_of_exact(value: Fraction) -> int:
    """Round an exact rational to binary32, ties to even, from first principles.

    This deliberately does not use struct.pack for the decision: candidates
    are ordered by their exact rational value and ties are resolved by the
    even mantissa bit.
    """
    if value < 0:
        return f32_bits_of_exact(-value) | 0x80000000
    if value == 0:
        return 0

    def exact(bits: int) -> Fraction:
        exponent = (bits >> 23) & 0xFF
        mantissa = bits & 0x7FFFFF
        if exponent == 0:
            return Fraction(mantissa, 1 << 149)
        return Fraction((1 << 23) + mantissa, 1) * Fraction(2) ** (exponent - 150)

    lo, hi = 0, 0x7F800000
    while lo < hi:
        mid = (lo + hi) // 2
        if exact(mid) < value:
            lo = mid + 1
        else:
            hi = mid
    candidates = []
    if lo > 0:
        candidates.append(lo - 1)
    candidates.append(lo)
    best = min(candidates, key=lambda bits: (abs(exact(bits) - value),
                                             bits & 1))
    return best


def check_precision_model() -> dict:
    checks = []
    # Exact D24 decode rounded to binary32.  The 0.25 case is deliberately
    # not assumed to stay 0.25: 4194304/16777215 is just above the midpoint
    # between 0.25 and its next float, so the exact round is 0x3e800001.
    expected_bits = {0.0: 0x00000000, 0.25: 0x3E800001,
                     0.75: 0x3F400000, 1.0: 0x3F800000}
    for depth, want_bits in expected_bits.items():
        code = d24_code(depth)
        exact = Fraction(code, D24_MAX)
        bits = f32_bits_of_exact(exact)
        value = struct.unpack('<f', struct.pack('<I', bits))[0]
        checks.append({
            'depth': depth,
            'd24_code': code,
            'exact_decode': '%d/%d' % (code, D24_MAX),
            'nearest_binary32_bits': '0x%08x' % bits,
            'expected_binary32_bits': '0x%08x' % want_bits,
            'binary32_value': value,
            'matches_exact_bits': bits == want_bits,
            'model_matches_exact_round': d24_binary32(depth) == value,
        })
    legacy_075 = Fraction(d24_code(0.75), D24_MAX)
    legacy_bits = f32_bits_of_exact(legacy_075)
    return {
        'sample_model': SAMPLE_MODEL,
        'legacy_sample_model': LEGACY_SAMPLE_MODEL,
        'checks': checks,
        'legacy_075_exact': '%d/%d' % (legacy_075.numerator,
                                       legacy_075.denominator),
        'legacy_075_nearest_binary32_bits': '0x%08x' % legacy_bits,
        'all_match': all(row['matches_exact_bits'] and
                         row['model_matches_exact_round']
                         for row in checks),
        'd24_precision_asymmetry':
            '0.25 decodes to 0x3e800001, not 0x3e800000; the minimal GPU '
            'cases only exercise 0.75 and the 0.25 negative bound',
        'legacy_equals_075_as_double': float(legacy_075) == 0.75,
        'legacy_rounds_to_075_as_binary32': legacy_bits == 0x3F400000,
    }


def u8(value: float) -> int:
    return int(math.floor(f32(value) * 255.0 + 0.5))


def kv_tokens(text: str) -> dict[str, str]:
    return dict(re.findall(r'([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)', text))


class PatternRow:
    def __init__(self, grid: 'PatternGrid', y: int):
        self.grid = grid
        self.y = y

    def __getitem__(self, x: int) -> float:
        return self.grid.value(x, self.y)


class PatternGrid:
    """Sparse model of the four-quadrant depths written by the GPU producer.

    Texel values use the same versioned D24_UNORM_TO_BINARY32 precision model
    as the shader's depth read.  Border values are TSC binary32 words and do
    not pass through this decode.
    """

    def __init__(self, width: int, height: int, region, pattern: int):
        self.width = width
        self.height = height
        self.x0, self.y0, self.x1, self.y1 = region
        self.mid_x = self.x0 + (self.x1 - self.x0 + 1) // 2
        self.mid_y = self.y0 + (self.y1 - self.y0 + 1) // 2
        self.pattern = pattern

    def __getitem__(self, y: int) -> PatternRow:
        return PatternRow(self, y)

    def value(self, x: int, y: int) -> float:
        if not (self.x0 <= x <= self.x1 and self.y0 <= y <= self.y1):
            return d24_binary32(0.75)
        left = x < self.mid_x
        top = y < self.mid_y
        if self.pattern == 1:
            return d24_binary32(0.0 if top and left else
                                0.25 if top else
                                0.75 if left else
                                1.0)
        return d24_binary32(1.0 if top and left else
                            0.75 if top else
                            0.25 if left else
                            0.0)


def dims_for(resource: int):
    return (SMALL, SMALL) if resource != RES_ATLAS else (ATLAS_W, ATLAS_H)


def region_for(resource: int):
    return (0, 0, SMALL - 1, SMALL - 1) if resource != RES_ATLAS else ATLAS_REGION


def recompute_case(case: dict, pattern: int) -> dict:
    width, height = dims_for(case['resource'])
    grid = PatternGrid(width, height, region_for(case['resource']), pattern)
    plan = {
        'compare_func': case['compare_func'],
        'filter': 'point' if case['filter'] == 1 else 'linear',
        'wrap_u': case['wrap_u'],
        'wrap_v': case['wrap_v'],
        'border_depth': f32(case['border']),
    }
    uv = (f32(f32_from_bits(case['u_bits'])),
          f32(f32_from_bits(case['v_bits'])))
    ref = f32(f32_from_bits(case['ref_bits']))
    point = shadow_reference.sample_point(grid, (width, height), uv, ref, plan)
    linear = shadow_reference.sample_linear(grid, (width, height), uv, ref, plan)
    depth_first = shadow_reference.sample_linear_filter_depth_first(
        grid, (width, height), uv, ref, plan)
    return {
        'uv': uv,
        'ref': ref,
        'point8': u8(point),
        'linear8': u8(linear),
        'dispatch8': u8(linear if case['filter'] == 2 else point),
        'depthfirst8': u8(depth_first),
        'pattern': pattern,
    }


def check_contract_shape(contract: dict, problems: list[str]):
    cases = contract.get('cases')
    if not isinstance(cases, list):
        problems.append('contract has no case list')
        return []
    if len(cases) != EXPECTED_CASES:
        problems.append('contract has %d cases, expected %d' %
                        (len(cases), EXPECTED_CASES))
    rejections = contract.get('planner_rejections')
    if not isinstance(rejections, list) or \
            len(rejections) != EXPECTED_PLANNER_REJECTIONS:
        problems.append('contract has %r planner rejections, expected %d' %
                        (len(rejections) if isinstance(rejections, list)
                         else rejections, EXPECTED_PLANNER_REJECTIONS))
    ids = [case.get('id') for case in cases]
    names = [case.get('name') for case in cases]
    if ids != list(range(1, len(cases) + 1)):
        problems.append('contract case ids are not 1..%d in order' % len(cases))
    if len(set(ids)) != len(ids) or len(set(names)) != len(names):
        problems.append('contract has duplicate ids or names')
    for case in cases:
        if case.get('tolerance') != EXPECTED_TOLERANCE:
            problems.append('%s: contract tolerance %r != %d' %
                            (case.get('name'), case.get('tolerance'),
                             EXPECTED_TOLERANCE))
        resource = case.get('resource')
        if resource not in (RES_DF32, RES_S8D24, RES_ATLAS):
            problems.append('%s: unknown resource %r' %
                            (case.get('name'), resource))
            continue
        if dims_for(resource) != (case.get('width'), case.get('height')):
            problems.append('%s: contract dimensions are not the fixed surface' %
                            case.get('name'))
        if list(region_for(resource)) != case.get('region'):
            problems.append('%s: contract region is not the fixed region' %
                            case.get('name'))
        if resource == RES_ATLAS:
            if case['region'][3] <= 2048:
                problems.append('%s: atlas producer is not beyond row 2048' %
                                case.get('name'))
        elif case.get('width') != SMALL or case.get('height') != SMALL:
            problems.append('%s: small resource is not 64x64' % case.get('name'))
    for case_id, name in ANCHOR_NAMES.items():
        found = next((case for case in cases if case.get('id') == case_id), None)
        if found is None or found.get('name') != name:
            problems.append('anchor case %d is not %r' % (case_id, name))
    funcs = {case.get('compare_func') for case in cases}
    filters = {case.get('filter') for case in cases}
    wraps = set()
    for case in cases:
        wraps.add(case.get('wrap_u'))
        wraps.add(case.get('wrap_v'))
    if funcs != set(range(8)):
        problems.append('compare function coverage is not 0..7: %r' % sorted(funcs))
    if filters != {1, 2}:
        problems.append('filter coverage is not point+linear: %r' % sorted(filters))
    if wraps != {0, 1, 2, 3}:
        problems.append('wrap coverage is not 0..3: %r' % sorted(wraps))
    if contract.get('policy_sha256') != shadow_helpers.policy_sha256():
        problems.append('contract policy SHA differs from the current policy source')
    if contract.get('strategy') != shadow_helpers.STRATEGY:
        problems.append('contract strategy %r != %r' %
                        (contract.get('strategy'), shadow_helpers.STRATEGY))
    if contract.get('emitter_sha256') != shadow_helpers.emitter_source_sha256():
        problems.append('contract emitter SHA differs from the current emitter source')
    if contract.get('helper_sha256') != shadow_helpers.helper_source_sha256():
        problems.append('contract helper SHA differs from the current helper source')
    if contract.get('oracle_version') != ORACLE_VERSION:
        problems.append('contract oracle_version %r != %d' %
                        (contract.get('oracle_version'), ORACLE_VERSION))
    if contract.get('sample_model') != SAMPLE_MODEL:
        problems.append('contract sample_model %r != %r' %
                        (contract.get('sample_model'), SAMPLE_MODEL))
    model = contract.get('oracle_model') or {}
    changed = model.get('changed_cases') or []
    if [row.get('id') for row in changed] != [10, 41]:
        problems.append('contract oracle model does not enumerate cases 10 '
                        'and 41: %r' % [row.get('id') for row in changed])
    auxiliary = model.get('auxiliary_depthfirst_changes') or []
    if [row.get('id') for row in auxiliary] != [13, 14, 44, 45, 63, 65]:
        problems.append('contract auxiliary depth-first changes are not the '
                        'six boundary cases: %r' %
                        [row.get('id') for row in auxiliary])
    if model.get('device_expected_changed_ids') != [10, 41]:
        problems.append('contract device_expected_changed_ids is %r' %
                        (model.get('device_expected_changed_ids'),))
    if model.get('legacy_sample_model') != LEGACY_SAMPLE_MODEL:
        problems.append('contract legacy sample model %r != %r' %
                        (model.get('legacy_sample_model'),
                         LEGACY_SAMPLE_MODEL))
    if model.get('inputs_unchanged') is not True:
        problems.append('contract does not assert unchanged model inputs')
    for row in changed:
        case = next((case for case in cases
                     if case.get('id') == row.get('id')), None)
        if case is None:
            continue
        if row.get('legacy_expected') == row.get('versioned_expected'):
            problems.append('%s: model change has identical expectations'
                            % case.get('name'))
        if case.get('expected') != row.get('versioned_expected'):
            problems.append('%s: contract expected != recorded versioned model'
                            % case.get('name'))
    return cases


def expected_status_name(path: str, code: int):
    if path == 'depth-view':
        return DEPTH_VIEW_STATUS.get(code)
    if path == 'tsc':
        return SHADOW_STATUS.get(code)
    return None


def parse_log(text: str, problems: list[str]):
    parsed = {
        'schema': None,
        'identity': [],
        'payload_contract': None,
        'planner_contract': None,
        'cases': {},
        'results': {},
        'dumps': {},
        'produces': {},
        'plan_rejects': {},
        'o1_ready': [],
        'o1_failure': [],
        'atlas_status': [],
        'summary': [],
        'verdict': None,
        'fatal_lines': [],
        'other_failure_lines': [],
    }

    def put(table: dict, key, value, label: str):
        if key in table:
            problems.append('%s %r appears more than once' % (label, key))
            return
        table[key] = value

    for line in text.splitlines():
        if line.startswith('PROBE_SCHEMA '):
            parsed['schema'] = line.split(None, 1)[1].strip()
        elif line.startswith('IDENTITY '):
            parsed['identity'].append(kv_tokens(line))
        elif line.startswith('PAYLOAD_CONTRACT '):
            parsed['payload_contract'] = kv_tokens(line)
        elif line.startswith('PLANNER_REJECT_CONTRACT '):
            parsed['planner_contract'] = kv_tokens(line)
        elif line.startswith('CASE_RESULT '):
            tokens = kv_tokens(line)
            try:
                case_id = int(tokens['id'])
            except (KeyError, ValueError):
                problems.append('malformed CASE_RESULT: %s' % line)
                continue
            put(parsed['results'], case_id, tokens, 'CASE_RESULT')
        elif line.startswith('CASE '):
            tokens = kv_tokens(line)
            try:
                case_id = int(tokens['id'])
            except (KeyError, ValueError):
                problems.append('malformed CASE: %s' % line)
                continue
            put(parsed['cases'], case_id, tokens, 'CASE')
        elif line.startswith('DUMP '):
            tokens = kv_tokens(line)
            try:
                offset = int(tokens['offset'])
            except (KeyError, ValueError):
                problems.append('malformed DUMP: %s' % line)
                continue
            put(parsed['dumps'], offset, tokens, 'DUMP offset')
        elif line.startswith('PRODUCE '):
            tokens = kv_tokens(line)
            tag = tokens.get('tag')
            if tag is None:
                problems.append('malformed PRODUCE: %s' % line)
                continue
            put(parsed['produces'], tag, tokens, 'PRODUCE tag')
        elif line.startswith('SHADOW_PLAN_REJECT '):
            tokens = kv_tokens(line)
            name = tokens.get('name')
            if name is None:
                problems.append('malformed SHADOW_PLAN_REJECT: %s' % line)
                continue
            put(parsed['plan_rejects'], name, tokens, 'SHADOW_PLAN_REJECT')
        elif line.startswith('O1_PROGRAM_READY '):
            parsed['o1_ready'].append(kv_tokens(line))
        elif line.startswith('O1_PROGRAM_FAILURE '):
            parsed['o1_failure'].append(kv_tokens(line))
        elif line.startswith('ATLAS_STATUS '):
            parsed['atlas_status'].append(kv_tokens(line))
        elif line.startswith('SUMMARY '):
            parsed['summary'].append(kv_tokens(line))
        elif line.startswith('RESULT '):
            if parsed['verdict'] is not None:
                problems.append('more than one RESULT line')
            parsed['verdict'] = line.split(None, 1)[1].strip()
        else:
            head = line.split(None, 1)[0] if line.strip() else ''
            if head in FATAL_PREFIXES:
                parsed['fatal_lines'].append(line)
            elif head.endswith('_FAIL') or head.endswith('_FAILURE'):
                parsed['other_failure_lines'].append(line)
    return parsed


def verify(log_path: Path, dump_path: Path, contract_path: Path,
           emulator_log_path: Path | None = None,
           process_path: Path | None = None) -> dict:
    problems: list[str] = []
    precision_model = check_precision_model()
    if not precision_model['all_match']:
        problems.append('independent D24->binary32 model mismatch')
    if precision_model['legacy_equals_075_as_double']:
        problems.append('legacy double oracle unexpectedly equals 0.75')
    if not precision_model['legacy_rounds_to_075_as_binary32']:
        problems.append('legacy 0.75 decode does not round to binary32 0.75')
    contract_data = contract_path.read_bytes()
    contract = json.loads(contract_data.decode('utf-8'))
    contract_sha = sha256_bytes(contract_data)
    cases = check_contract_shape(contract, problems)
    case_by_id = {case['id']: case for case in cases}

    try:
        log_data = log_path.read_bytes()
    except OSError as error:
        problems.append('missing log file %s: %s' % (log_path, error))
        log_data = b''
    try:
        dump = dump_path.read_bytes()
    except OSError as error:
        problems.append('missing dump file %s: %s' % (dump_path, error))
        dump = b''
    text = log_data.decode('utf-8', errors='replace')
    log = parse_log(text, problems)

    # Independent oracle: walk cases in contract order while following the
    # producer pattern state, then compare the contract's copied expectations.
    oracle: dict[int, dict] = {}
    pattern_state = {RES_DF32: None, RES_S8D24: None, RES_ATLAS: None}
    for case in cases:
        resource = case.get('resource')
        produce = case.get('produce', 0)
        if produce:
            if produce not in (1, 2):
                problems.append('%s: invalid produce pattern %r' %
                                (case.get('name'), produce))
                continue
            pattern_state[resource] = produce
        pattern = pattern_state.get(resource)
        if pattern is None:
            problems.append('%s: no produced pattern before sampling' %
                            case.get('name'))
            continue
        expected = recompute_case(case, pattern)
        oracle[case['id']] = expected
        declared = case.get('expected') or {}
        for key in ('point8', 'linear8', 'dispatch8', 'depthfirst8'):
            if declared.get(key) != expected[key]:
                problems.append('%s: independent oracle %s=%d but contract says %r'
                                % (case.get('name'), key, expected[key],
                                   declared.get(key)))
        if (f32(declared.get('uv', [0, 0])[0]) != expected['uv'][0] or
                f32(declared.get('uv', [0, 0])[1]) != expected['uv'][1]):
            problems.append('%s: contract uv is not the f32 case value' %
                            case.get('name'))
        if f32(declared.get('ref', 0.0)) != expected['ref']:
            problems.append('%s: contract ref is not the f32 case value' %
                            case.get('name'))
        if case.get('negative_depth_first'):
            if expected['linear8'] == expected['depthfirst8']:
                problems.append('%s: oracle cannot distinguish compare-first '
                                'from depth-first' % case.get('name'))
            elif abs(expected['linear8'] - expected['depthfirst8']) <= \
                    EXPECTED_TOLERANCE:
                problems.append('%s: negative margin is within the tolerance' %
                                case.get('name'))

    # ------------------------------------------------------------------
    # Identity, fixed counts and verdict.
    # ------------------------------------------------------------------
    stages = contract.get('stages') or {}
    if log['schema'] != '3':
        problems.append('PROBE_SCHEMA is %r, not 3' % log['schema'])
    if len(log['identity']) != 1:
        problems.append('expected one IDENTITY line, found %d' %
                        len(log['identity']))
    identity = log['identity'][0] if log['identity'] else {}
    if identity:
        if identity.get('title') != 'MHGUSH001':
            problems.append('IDENTITY title %r != MHGUSH001' %
                            identity.get('title'))
        for key, contract_key in (('policy_sha256', 'policy_sha256'),
                                  ('helper_sha256', 'helper_sha256'),
                                  ('strategy', 'strategy'),
                                  ('emitter_sha256', 'emitter_sha256')):
            if identity.get(key) != contract.get(contract_key):
                problems.append('IDENTITY %s %r != contract %r' %
                                (key, identity.get(key),
                                 contract.get(contract_key)))
        o0_sha = (stages.get('shadow_probe_f_o0') or {}).get('gxp_sha256')
        o1_sha = (stages.get('shadow_probe_f_o1') or {}).get('gxp_sha256')
        if identity.get('o0_gxp') != o0_sha:
            problems.append('IDENTITY O0 GXP %r != contract %r' %
                            (identity.get('o0_gxp'), o0_sha))
        if identity.get('o1_gxp') != o1_sha:
            problems.append('IDENTITY O1 GXP %r != contract %r' %
                            (identity.get('o1_gxp'), o1_sha))
        if not o0_sha or not o1_sha or o0_sha == o1_sha:
            problems.append('O0 and O1 GXP identities are not separate')

    payload = log['payload_contract'] or {}
    for key, value in (('bytes', EXPECTED_CASE_BYTES),
                       ('cases', EXPECTED_CASES),
                       ('case_bytes', CASE_BYTES),
                       ('small', SMALL),
                       ('atlas', '%dx%d' % (ATLAS_W, ATLAS_H)),
                       ('atlas_bytes', ATLAS_BYTES),
                       ('cap', ALLOC_CAP)):
        if payload.get(key) != str(value):
            problems.append('PAYLOAD_CONTRACT %s %r != %r' %
                            (key, payload.get(key), str(value)))
    if (log['planner_contract'] or {}).get('count') != \
            str(EXPECTED_PLANNER_REJECTIONS):
        problems.append('PLANNER_REJECT_CONTRACT count is not %d' %
                        EXPECTED_PLANNER_REJECTIONS)
    if log['verdict'] != 'COMPLETE':
        problems.append('RESULT is %r, not COMPLETE' % log['verdict'])
    if len(log['summary']) != 1:
        problems.append('expected one SUMMARY line, found %d' %
                        len(log['summary']))
    summary_fields = log['summary'][0] if log['summary'] else {}
    if summary_fields:
        for key, value in (('cases', EXPECTED_CASES),
                           ('dump_bytes', EXPECTED_CASE_BYTES),
                           ('failed', 0), ('o1_ready', 1),
                           ('atlas_available', 1)):
            if summary_fields.get(key) != str(value):
                problems.append('SUMMARY %s %r != %r' %
                                (key, summary_fields.get(key), str(value)))
    for line in log['fatal_lines']:
        problems.append('device failure line: %s' % line)
    for line in log['other_failure_lines']:
        problems.append('device %s line: %s' % (line.split(None, 1)[0], line))

    # ------------------------------------------------------------------
    # Declarations and terminal states: unique, complete, contract-exact.
    # ------------------------------------------------------------------
    case_ids = set(log['cases'])
    result_ids = set(log['results'])
    expected_ids = set(case_by_id)
    if case_ids != expected_ids:
        problems.append('CASE id set mismatch: missing=%r extra=%r' %
                        (sorted(expected_ids - case_ids),
                         sorted(case_ids - expected_ids)))
    if result_ids != expected_ids:
        problems.append('CASE_RESULT id set mismatch: missing=%r extra=%r' %
                        (sorted(expected_ids - result_ids),
                         sorted(result_ids - expected_ids)))
    if len(log['cases']) != EXPECTED_CASES or \
            len(log['results']) != EXPECTED_CASES:
        problems.append('declaration/result counts are %d/%d, expected %d/%d' %
                        (len(log['cases']), len(log['results']),
                         EXPECTED_CASES, EXPECTED_CASES))

    for case_id in sorted(expected_ids & case_ids):
        case = case_by_id[case_id]
        tokens = log['cases'][case_id]
        comparisons = {
            'name': case['name'], 'program': str(case['program']),
            'resource': str(case['resource']), 'produce': str(case['produce']),
            'aba': str(case['aba']),
            'negative': str(case['negative_depth_first']),
            'tsc0': '%08x' % case['tsc'][0],
            'tsc1': '%08x' % case['tsc'][1],
            'tsc4': '%08x' % case['tsc'][4],
            'depthfirst': str(oracle.get(case_id, {}).get('depthfirst8')),
            'tolerance': str(case['tolerance']),
        }
        for key, value in comparisons.items():
            if tokens.get(key) != value:
                problems.append('%s: CASE %s %r != contract %r' %
                                (case['name'], key, tokens.get(key), value))
        expected_triple = '%d,%d,%d' % (
            oracle.get(case_id, {}).get('point8', -1),
            oracle.get(case_id, {}).get('linear8', -1),
            oracle.get(case_id, {}).get('dispatch8', -1))
        if tokens.get('expected') != expected_triple:
            problems.append('%s: CASE expected %r != oracle %r' %
                            (case['name'], tokens.get('expected'),
                             expected_triple))
        try:
            decl_ref = f32(float(tokens.get('ref', 'nan')))
            u_text, v_text = tokens.get('uv', ',').split(',', 1)
            decl_uv = (f32(float(u_text)), f32(float(v_text)))
        except (TypeError, ValueError):
            problems.append('%s: malformed CASE ref/uv' % case['name'])
        else:
            if decl_ref != f32_from_bits(case['ref_bits']):
                problems.append('%s: CASE ref does not match ref_bits' %
                                case['name'])
            if decl_uv != (f32_from_bits(case['u_bits']),
                           f32_from_bits(case['v_bits'])):
                problems.append('%s: CASE uv does not match u/v bits' %
                                case['name'])

    # Producer rows, one per contract produce case, in resource order.
    produce_cases = [case for case in cases if case.get('produce')]
    if set(log['produces']) != {'case%02u' % case['id']
                                for case in produce_cases}:
        problems.append('PRODUCE tag set does not match the contract produce set')
    next_generation = {RES_DF32: 2, RES_S8D24: 2, RES_ATLAS: 2}
    for case in produce_cases:
        tag = 'case%02u' % case['id']
        row = log['produces'].get(tag)
        if row is None:
            problems.append('%s: missing PRODUCE row %s' %
                            (case['name'], tag))
            continue
        width, height = dims_for(case['resource'])
        expected_region = '%d,%d..%d,%d' % region_for(case['resource'])
        for key, value in (('resource', str(case['resource'])),
                           ('size', '%dx%d' % (width, height)),
                           ('pattern', str(case['produce'])),
                           ('region', expected_region)):
            if row.get(key) != value:
                problems.append('%s: PRODUCE %s %r != %r' %
                                (case['name'], key, row.get(key), value))
        try:
            generation = int(row.get('generation', '0'))
        except ValueError:
            generation = 0
        if generation != next_generation[case['resource']]:
            problems.append('%s: PRODUCE generation %d != expected %d' %
                            (case['name'], generation,
                             next_generation[case['resource']]))
        next_generation[case['resource']] += 1
        va = row.get('va')
        if not va or va in ('0', '0x0', '(nil)'):
            problems.append('%s: PRODUCE VA is empty' % case['name'])

    # ------------------------------------------------------------------
    # Raw payload layout and per-case pixel decisions.
    # ------------------------------------------------------------------
    layout_ok = True
    if len(dump) != EXPECTED_CASE_BYTES:
        problems.append('payload is %d bytes, contract expects %d' %
                        (len(dump), EXPECTED_CASE_BYTES))
        layout_ok = False
    expected_offsets = {case['id']: (case['id'] - 1) * CASE_BYTES
                        for case in cases}
    if set(log['dumps']) != set(expected_offsets.values()):
        problems.append('DUMP offset set is not the contiguous %d-case payload'
                        % EXPECTED_CASES)
        layout_ok = False
    for offset, row in log['dumps'].items():
        if row.get('bytes') != str(CASE_BYTES):
            problems.append('DUMP offset %d declares %r bytes, not %d' %
                            (offset, row.get('bytes'), CASE_BYTES))
            layout_ok = False

    slices: list[tuple[int, int, int]] = []
    file_hashes: dict[int, str] = {}
    raw_verified: dict[int, bool] = {}
    negative_ok = True
    aba_groups: dict[tuple[str, int], dict] = {}
    o0_raw = o0_pass = o0_count = 0
    o1_raw = o1_pass = o1_count = 0
    o1_ready_rows = log['o1_ready']
    o1_program_ready = False
    o1_rc = None
    if len(o1_ready_rows) != 1:
        problems.append('expected one O1_PROGRAM_READY row, found %d' %
                        len(o1_ready_rows))
    else:
        o1_ready = o1_ready_rows[0]
        o1_program_ready = o1_ready.get('ready') == '1'
        o1_rc = o1_ready.get('rc')
        if not o1_program_ready:
            problems.append('O1 fragment program was not ready (rc=%r)' % o1_rc)
        stages_o1 = (stages.get('shadow_probe_f_o1') or {}).get('gxp_sha256')
        if o1_ready.get('gxp_sha256') != stages_o1:
            problems.append('O1_PROGRAM_READY GXP %r != contract %r' %
                            (o1_ready.get('gxp_sha256'), stages_o1))
    if log['o1_failure']:
        for row in log['o1_failure']:
            problems.append('O1 program failure exact_gxp=%r stage=%r' %
                            (row.get('exact_gxp'), row.get('stage')))

    for case in cases:
        case_id = case['id']
        name = case['name']
        expected = oracle.get(case_id)
        result = log['results'].get(case_id)
        if result is None:
            problems.append('%s: missing CASE_RESULT' % name)
            continue
        if result.get('status') not in ('PASS', 'FAIL'):
            problems.append('%s: non-terminal status %r' %
                            (name, result.get('status')))
        if result.get('status') != 'PASS':
            problems.append('%s: device status %r' % (name,
                                                      result.get('status')))
        if result.get('executed') != '1' or result.get('readback') != '1':
            problems.append('%s: executed/readback %r/%r' %
                            (name, result.get('executed'),
                             result.get('readback')))
        try:
            tolerance = int(result.get('tolerance', '-1'))
            offset = int(result.get('dump_offset', '-1'))
            size = int(result.get('dump_bytes', '-1'))
            elapsed = int(result.get('elapsed_us', '-1'))
            device_bad = int(result.get('surface_bad', '-1'))
            device_pixels = int(result.get('pixels', '-1'))
            device_channels = (int(result.get('point8', '-1')),
                               int(result.get('linear8', '-1')),
                               int(result.get('dispatch8', '-1')))
        except ValueError:
            problems.append('%s: malformed numeric CASE_RESULT field' % name)
            continue
        if tolerance != EXPECTED_TOLERANCE:
            problems.append('%s: result tolerance %d != %d' %
                            (name, tolerance, EXPECTED_TOLERANCE))
        if elapsed < 0 or elapsed > CASE_TIMEOUT_US:
            problems.append('%s: elapsed %d us is outside the %d us watchdog' %
                            (name, elapsed, CASE_TIMEOUT_US))
        if size != CASE_BYTES:
            problems.append('%s: result dump size %d != %d' %
                            (name, size, CASE_BYTES))
        if offset != expected_offsets.get(case_id):
            problems.append('%s: result offset %d != contract %d' %
                            (name, offset, expected_offsets.get(case_id)))
        if offset < 0 or offset + CASE_BYTES > len(dump):
            problems.append('%s: dump slice %d+%d outside %d bytes' %
                            (name, offset, CASE_BYTES, len(dump)))
            continue
        data = dump[offset:offset + CASE_BYTES]
        slices.append((case_id, offset, CASE_BYTES))
        file_hashes[case_id] = sha256_bytes(data)
        dump_row = log['dumps'].get(offset)
        if dump_row is not None:
            expected_fnv = 'fnv32'
            try:
                value = 2166136261
                for byte in data:
                    value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
            except Exception:
                value = -1
            if dump_row.get(expected_fnv) != '%08x' % value:
                problems.append('%s: DUMP fnv32 disagrees with raw bytes' % name)
        if expected is None:
            continue
        point8 = expected['point8']
        linear8 = expected['linear8']
        dispatch8 = expected['dispatch8']
        bad = 0
        first = tuple(data[0:4])
        for index in range(0, CASE_BYTES, 4):
            pixel = data[index:index + 4]
            if (abs(pixel[0] - point8) > tolerance or
                    abs(pixel[1] - linear8) > tolerance or
                    abs(pixel[2] - dispatch8) > tolerance or
                    pixel[3] != 255):
                bad += 1
        if bad:
            problems.append('%s: %d/%d raw pixels differ from oracle '
                            'point=%d linear=%d dispatch=%d' %
                            (name, bad, SMALL * SMALL, point8, linear8,
                             dispatch8))
        if first[0] != device_channels[0] or first[1] != device_channels[1] or \
                first[2] != device_channels[2]:
            problems.append('%s: device channel report disagrees with raw bytes' %
                            name)
        if device_pixels != SMALL * SMALL or device_bad != bad:
            problems.append('%s: device counts report pixels=%d bad=%d, '
                            'raw says %d/%d' %
                            (name, device_pixels, device_bad,
                             SMALL * SMALL, bad))
        if case['program'] == PROGRAM_O0:
            o0_count += 1
            o0_pass += 1 if result.get('status') == 'PASS' else 0
            if not bad and result.get('status') == 'PASS':
                o0_raw += 1
        else:
            o1_count += 1
            o1_pass += 1 if result.get('status') == 'PASS' else 0
            if not bad and result.get('status') == 'PASS' and o1_program_ready:
                o1_raw += 1
        raw_verified[case_id] = (bad == 0 and result.get('status') == 'PASS')
        if case.get('negative_depth_first'):
            depthfirst8 = expected['depthfirst8']
            if abs(first[1] - linear8) > tolerance or \
                    abs(first[1] - depthfirst8) <= tolerance:
                problems.append('%s: raw compare-first pixel is not distinct '
                                'from depth-first (%r vs %d/%d)' %
                                (name, first[1], linear8, depthfirst8))
                negative_ok = False
        if case.get('aba'):
            label = 'df32' if case['resource'] != RES_S8D24 else 's8d24'
            aba_groups.setdefault((label, case['aba']), {
                'id': case_id, 'name': name, 'hash': file_hashes[case_id],
                'data': data, 'dispatch8': dispatch8,
                'produce': log['produces'].get('case%02u' % case_id, {}),
            })

    # ------------------------------------------------------------------
    # A/B/A semantics on the same VA.
    # ------------------------------------------------------------------
    aba_ok = True
    aba_summary = {}
    labels = {}
    for case in cases:
        if case.get('aba'):
            labels.setdefault(case['resource'], set()).add(case['aba'])
    for resource, phases in labels.items():
        label = 'df32' if resource != RES_S8D24 else 's8d24'
        if phases != {1, 2, 3}:
            problems.append('%s A/B/A phases are %r, not 1,2,3' %
                            (label, sorted(phases)))
            aba_ok = False
            continue
        rows = [aba_groups.get((label, phase)) for phase in (1, 2, 3)]
        if any(row is None for row in rows):
            problems.append('%s A/B/A rows are incomplete' % label)
            aba_ok = False
            continue
        a1, b, a2 = rows
        if a1['data'] != a2['data']:
            problems.append('%s A/B/A A slices are not byte-identical' % label)
            aba_ok = False
        if a1['hash'] == b['hash']:
            problems.append('%s A/B/A B did not change the pixels' % label)
            aba_ok = False
        if a1['dispatch8'] == b['dispatch8']:
            problems.append('%s A/B/A B oracle does not differ from A' % label)
            aba_ok = False
        vas = []
        for phase, row in zip((1, 2, 3), rows):
            produce = row['produce']
            if produce.get('pattern') != str(phase if phase != 3 else 1):
                problems.append('%s A/B/A phase %d PRODUCE pattern %r' %
                                (label, phase, produce.get('pattern')))
                aba_ok = False
            vas.append(produce.get('va'))
        if len(set(vas)) != 1 or not vas[0] or vas[0] in ('0', '0x0', '(nil)'):
            problems.append('%s A/B/A phases do not share one depth VA: %r' %
                            (label, vas))
            aba_ok = False
        aba_summary[label] = {
            'a1': a1['hash'], 'b': b['hash'], 'a2': a2['hash'],
            'same_va': len(set(vas)) == 1, 'va': vas[0],
        }

    # ------------------------------------------------------------------
    # Fixed planner-negative contract.
    # ------------------------------------------------------------------
    planner_ok = True
    contract_rejects = contract.get('planner_rejections') or []
    expected_reject_names = {row.get('name') for row in contract_rejects}
    if set(log['plan_rejects']) != expected_reject_names:
        problems.append('SHADOW_PLAN_REJECT name set mismatch: '
                        'missing=%r extra=%r' %
                        (sorted(expected_reject_names - set(log['plan_rejects'])),
                         sorted(set(log['plan_rejects']) -
                                expected_reject_names)))
        planner_ok = False
    if len(log['plan_rejects']) != EXPECTED_PLANNER_REJECTIONS:
        problems.append('logged %d planner rejections, expected %d' %
                        (len(log['plan_rejects']),
                         EXPECTED_PLANNER_REJECTIONS))
        planner_ok = False
    for contract_row in contract_rejects:
        name = contract_row.get('name')
        row = log['plan_rejects'].get(name)
        if row is None:
            problems.append('%s: planner rejection missing' % name)
            planner_ok = False
            continue
        try:
            expected_code = int(contract_row.get('expected_status'))
            actual = int(row.get('expected', ''))
            logged_actual = int(row.get('actual', ''))
        except (TypeError, ValueError):
            problems.append('%s: malformed planner rejection code' % name)
            planner_ok = False
            continue
        name_path = contract_row.get('path')
        if row.get('path') != name_path:
            problems.append('%s: planner path %r != %r' %
                            (name, row.get('path'), name_path))
            planner_ok = False
        if actual != expected_code or logged_actual != expected_code:
            problems.append('%s: rejection expected/actual %r/%r != %d' %
                            (name, row.get('expected'), row.get('actual'),
                             expected_code))
            planner_ok = False
        status_name = expected_status_name(name_path, expected_code)
        if status_name is None or status_name in ('ok', 'unknown'):
            problems.append('%s: contract rejection code has no named refusal' %
                            name)
            planner_ok = False
        elif row.get('status') != status_name:
            problems.append('%s: status %r != independent name %r' %
                            (name, row.get('status'), status_name))
            planner_ok = False
        if row.get('pass') != '1' or row.get('draw') != '0':
            problems.append('%s: planner row pass/draw %r/%r' %
                            (name, row.get('pass'), row.get('draw')))
            planner_ok = False

    # ------------------------------------------------------------------
    # Atlas allocation and producer bounds.
    # ------------------------------------------------------------------
    atlas_ok = True
    atlas_rows = log['atlas_status']
    if len(atlas_rows) != 1:
        problems.append('expected one ATLAS_STATUS line, found %d' %
                        len(atlas_rows))
        atlas_ok = False
    else:
        row = atlas_rows[0]
        if row.get('available') != '1':
            problems.append('ATLAS_STATUS available=%r' % row.get('available'))
            atlas_ok = False
        if row.get('bytes') != str(ATLAS_BYTES):
            problems.append('ATLAS_STATUS bytes=%r, expected %d' %
                            (row.get('bytes'), ATLAS_BYTES))
            atlas_ok = False
        if row.get('cap') != str(ALLOC_CAP):
            problems.append('ATLAS_STATUS cap=%r, expected %d' %
                            (row.get('cap'), ALLOC_CAP))
            atlas_ok = False
        if ATLAS_BYTES > ALLOC_CAP:
            problems.append('atlas allocation exceeds the 32 MiB cap')
            atlas_ok = False
        for key in ('rt', 'depth'):
            value = row.get(key)
            if not value or value in ('0', '0x0', '(nil)'):
                problems.append('ATLAS_STATUS %s is empty' % key)
                atlas_ok = False
    for case in cases:
        if case['resource'] != RES_ATLAS:
            continue
        if not raw_verified.get(case['id']):
            problems.append('%s: atlas case has no verified raw readback' %
                            case['name'])
            atlas_ok = False
        if case['region'][3] <= 2048:
            problems.append('%s: atlas region does not exceed row 2048' %
                            case['name'])
            atlas_ok = False
        produce = log['produces'].get('case%02u' % case['id'])
        if case['produce'] and produce is not None:
            if produce.get('size') != '%dx%d' % (ATLAS_W, ATLAS_H):
                problems.append('%s: atlas producer size is not 1024x3072' %
                                case['name'])
                atlas_ok = False
            if int(produce.get('region_y1', '-1')) <= 2048 and \
                    produce.get('region') is not None:
                pass

    # ------------------------------------------------------------------
    # Optional emulator log: only explicit driver/link failures are fatal.
    # ------------------------------------------------------------------
    emulator_log = {'path': str(emulator_log_path) if emulator_log_path else None,
                    'present': False, 'sha256': None, 'gl_error_lines': [],
                    'renderer_format_lines': [], 'exception_lines': []}
    if emulator_log_path is not None:
        try:
            engine_data = emulator_log_path.read_bytes()
        except OSError as error:
            problems.append('missing emulator log %s: %s' %
                            (emulator_log_path, error))
        else:
            emulator_log['present'] = True
            emulator_log['sha256'] = sha256_bytes(engine_data)
            engine_text = engine_data.decode('utf-8', errors='replace')
            engine_lines = engine_text.splitlines()
            matches = [line for line in engine_lines
                       if GL_ERROR_RE.search(line)]
            formats = [line for line in engine_lines
                       if RENDERER_FORMAT_RE.search(line)]
            exceptions = [line for line in engine_lines
                          if EXCEPTION_RE.search(line)]
            emulator_log['gl_error_lines'] = matches[:20]
            emulator_log['renderer_format_lines'] = formats[:20]
            emulator_log['exception_lines'] = exceptions[:20]
            if matches:
                problems.append('emulator log has %d GL/USSE failure line(s)' %
                                len(matches))
            if formats:
                problems.append('emulator log has %d renderer format error line(s)' %
                                len(formats))

    # The runner record is optional but, when present, gives the host exit code
    # and installed identity without trusting the device PASS flags.
    process = {'path': str(process_path) if process_path else None,
               'present': False}
    if process_path is not None:
        try:
            process_data = process_path.read_bytes()
            process_record = json.loads(process_data.decode('utf-8'))
        except (OSError, ValueError) as error:
            problems.append('cannot read process record %s: %s' %
                            (process_path, error))
        else:
            process.update({
                'present': True,
                'sha256': sha256_bytes(process_data),
                'result': process_record.get('result'),
                'exit_code': process_record.get('exit_code'),
                'natural_exit': process_record.get('natural_exit'),
                'emulator_sha256': process_record.get('emulator_sha256'),
                'vpk_sha256': process_record.get('vpk_sha256'),
                'eboot_sha256': process_record.get('eboot_sha256'),
                'title_id': process_record.get('title_id'),
                'timeout_seconds': process_record.get('timeout_seconds'),
            })
            if process_record.get('result') != 'COMPLETE':
                problems.append('process result is %r, not COMPLETE' %
                                process_record.get('result'))
            if process_record.get('title_id') != 'MHGUSH001':
                problems.append('process title %r != MHGUSH001' %
                                process_record.get('title_id'))
            exit_code = process_record.get('exit_code')
            if exit_code not in (None, 0):
                problems.append('emulator process exit code %r' % (exit_code,))

    # ------------------------------------------------------------------
    # Payload tiling: exact contiguous slices, one DUMP and one result each.
    # ------------------------------------------------------------------
    by_offset: dict[int, int] = {}
    for case_id, offset, size in slices:
        if offset in by_offset:
            problems.append('case %d shares dump offset %d with case %d' %
                            (case_id, offset, by_offset[offset]))
            layout_ok = False
        by_offset[offset] = case_id
    cursor = 0
    for offset in sorted(by_offset):
        if offset != cursor:
            problems.append('dump offset %d, expected %d (hole or overlap)' %
                            (offset, cursor))
            layout_ok = False
        cursor = offset + CASE_BYTES
    if cursor != len(dump):
        problems.append('dump slices cover %d of %d bytes' % (cursor, len(dump)))
        layout_ok = False
    if len(slices) != EXPECTED_CASES:
        problems.append('verified %d dump slices, expected %d' %
                        (len(slices), EXPECTED_CASES))
        layout_ok = False

    negative_ids = [case['id'] for case in cases
                    if case.get('negative_depth_first')]
    if len(negative_ids) != 2:
        problems.append('contract has %d negative depth-first cases, expected 2' %
                        len(negative_ids))
        negative_ok = False
    if not all(raw_verified.get(case_id) for case_id in negative_ids):
        problems.append('negative depth-first cases have no verified raw readback')
        negative_ok = False

    summary = {
        'schema': 2,
        'task': 'M6-B21',
        'strategy': contract.get('strategy'),
        'emitter_sha256': contract.get('emitter_sha256'),
        'helper_sha256': contract.get('helper_sha256'),
        'policy_sha256': contract.get('policy_sha256'),
        'contract': str(contract_path),
        'contract_sha256': contract_sha,
        'log': str(log_path),
        'log_present': bool(log_data),
        'log_sha256': sha256_bytes(log_data),
        'dump': str(dump_path),
        'dump_present': bool(dump),
        'dump_bytes': len(dump),
        'dump_sha256': sha256_bytes(dump),
        'payload_contract_bytes': EXPECTED_CASE_BYTES,
        'case_bytes': CASE_BYTES,
        'layout_ok': layout_ok,
        'case_count_contract': EXPECTED_CASES,
        'case_declarations': len(log['cases']),
        'case_results': len(log['results']),
        'tolerance': EXPECTED_TOLERANCE,
        'watchdog_us': CASE_TIMEOUT_US,
        'cases_ok': len(raw_verified) == EXPECTED_CASES and
                     all(raw_verified.values()) and not problems,
        'raw_cases_verified': sum(1 for value in raw_verified.values() if value),
        'negative_depth_first_ok': negative_ok,
        'negative_depth_first_cases': len(negative_ids),
        'aba_ok': aba_ok,
        'aba': aba_summary,
        'planner_rejections_ok': planner_ok,
        'planner_rejections_contract': EXPECTED_PLANNER_REJECTIONS,
        'planner_rejections_logged': len(log['plan_rejects']),
        'atlas_ok': atlas_ok,
        'atlas_bytes': ATLAS_BYTES,
        'atlas_alloc_cap': ALLOC_CAP,
        'emulator_log': emulator_log,
        'process': process,
        'o0': {
            'gxp_sha256': (stages.get('shadow_probe_f_o0') or {}).get(
                'gxp_sha256'),
            'optimization': (stages.get('shadow_probe_f_o0') or {}).get(
                'optimization'),
            'cases': o0_count,
            'device_pass': o0_pass,
            'raw_verified': o0_raw,
            'ok': o0_count > 0 and o0_raw == o0_count and o0_pass == o0_count,
        },
        'o1': {
            'gxp_sha256': (stages.get('shadow_probe_f_o1') or {}).get(
                'gxp_sha256'),
            'optimization': (stages.get('shadow_probe_f_o1') or {}).get(
                'optimization'),
            'program_ready': o1_program_ready,
            'program_rc': o1_rc,
            'cases': o1_count,
            'device_pass': o1_pass,
            'raw_verified': o1_raw,
            'failure_rows': len(log['o1_failure']),
            'ok': o1_program_ready and o1_count == 1 and
                  o1_raw == o1_count and o1_pass == o1_count,
        },
        'identity': identity,
        'summary_line': summary_fields,
        'verdict': log['verdict'],
        'result_complete': log['verdict'] == 'COMPLETE',
        'complete': not problems,
        'problems': problems,
        'sample_model': contract.get('sample_model'),
        'oracle_version': contract.get('oracle_version'),
        'oracle_model': contract.get('oracle_model'),
        'precision_model': precision_model,
        'precision_limit': 'the locked OpenGL backend stores '
                           'GL_DEPTH24_STENCIL8 even for native DF32; the '
                           'versioned oracle decodes D24 unorm to binary32, '
                           'checks use one 8-bit LSB tolerance, and this is '
                           'not F32 hardware equivalence',
        'gpu_pixels_verified': not problems,
        'software_shadow_planner_and_pixels_verified': not problems,
        'production_runtime_binding_exercised': False,
        'target_content_tested': False,
        'game_rendered': False,
        'hardware_verified': False,
        'production_modified': False,
        'batched': False,
    }
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--dump', type=Path, required=True)
    parser.add_argument('--contract', type=Path, required=True)
    parser.add_argument('--emulator-log', type=Path)
    parser.add_argument('--process', type=Path)
    parser.add_argument('--report', type=Path)
    arguments = parser.parse_args()
    summary = verify(arguments.log, arguments.dump, arguments.contract,
                     arguments.emulator_log, arguments.process)
    if arguments.report:
        arguments.report.parent.mkdir(parents=True, exist_ok=True)
        arguments.report.write_text(json.dumps(summary, indent=2) + '\n',
                                    encoding='utf-8', newline='\n')
    print(json.dumps(summary, indent=2))
    return 0 if summary['complete'] else 1


if __name__ == '__main__':
    sys.exit(main())

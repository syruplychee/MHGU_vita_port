"""Compile and describe the M6-B20B software-shadow content probe.

The probe answers a bounded question: does the real emitted Cg helper, fed by
the production C TSC planner and a real native depth allocation produced by GPU
draws, return the compare-first point/PCF values computed by the independent
Python oracle through the locked GXM/OpenGL backend?

Nothing here talks to the target or the game.  The fragment programs are
self-authored Cg containing the exact helper source from shadow_helpers.py.  The
depth pattern is produced on the GPU by the probe; no CPU depth is uploaded.

Expected values are computed by shadow_reference.py against a sparse pattern
grid that models the GPU-produced quadrants.  Every value the probe compares is
written here first, and the device is only asked to reproduce it.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert_cache import compile_serial, digest, write_json
import shadow_helpers
import shadow_reference

RES_DF32_64 = 0
RES_S8D24_64 = 1
RES_ATLAS = 2
PROGRAM_O0 = 0
PROGRAM_O1 = 1
PATTERN_A = 1
PATTERN_B = 2

SMALL = 64
ATLAS_W = 1024
ATLAS_H = 3072
ALLOC_CAP = 32 * 1024 * 1024
ATLAS_REGION = (480, 2584, 543, 2647)
D24_MAX = (1 << 24) - 1

FMT_DF32 = 0
FMT_S8D24 = 1

# Versioned backend-oracle precision model.  The locked Vita3K OpenGL backend
# stores both native DF32 and S8D24 depth through GL_DEPTH24_STENCIL8, so a
# depth texel read is a 24-bit unorm value converted to binary32 by the
# texture unit.  The former oracle kept the exact rational as a Python double
# (0.7499999850988388 for 0.75), which is not what the shader compares.  This
# model applies a binary32 conversion *after* the D24 decode.  It is an
# explicit statement about this backend, not a claim of general F32 hardware
# equivalence.
SAMPLE_MODEL = 'D24_UNORM_TO_BINARY32'
LEGACY_SAMPLE_MODEL = 'D24_UNORM_TO_BINARY64'


def f32(value):
    return struct.unpack('<f', struct.pack('<f', float(value)))[0]


def f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', f32(value)))[0]


def d24_code(value):
    """The 24-bit unorm code the locked backend stores for a depth write."""
    return int(math.floor(f32(value) * D24_MAX + 0.5))


def d24_binary64(value):
    """Legacy model: exact rational decode kept as a Python double."""
    return d24_code(value) / D24_MAX


def d24_binary32(value):
    """Versioned model: D24 unorm decode followed by binary32 rounding."""
    return f32(d24_code(value) / D24_MAX)


def u8(value):
    return int(math.floor(f32(value) * 255.0 + 0.5))


def wrap_mode(name):
    return {'repeat': 0, 'mirror': 1, 'clamp': 2, 'border': 3}[name]


class PatternRow:
    def __init__(self, grid, y):
        self.grid = grid
        self.y = y

    def __getitem__(self, x):
        return self.grid.value(x, self.y)


class PatternGrid:
    """Sparse stand-in for depth_rows[y][x]; only sampled texels are computed.

    ``decode`` selects the oracle's depth-texel precision model.  The current
    model is D24 unorm converted to binary32; the legacy double model exists
    only to enumerate the exact expectation change.
    """

    def __init__(self, width, height, region, pattern, decode=d24_binary32):
        self.width = width
        self.height = height
        self.x0, self.y0, self.x1, self.y1 = region
        self.mid_x = self.x0 + (self.x1 - self.x0 + 1) // 2
        self.mid_y = self.y0 + (self.y1 - self.y0 + 1) // 2
        self.pattern = pattern
        self.decode = decode

    def __getitem__(self, y):
        return PatternRow(self, y)

    def value(self, x, y):
        if not (self.x0 <= x <= self.x1 and self.y0 <= y <= self.y1):
            return self.decode(0.75)
        left = x < self.mid_x
        top = y < self.mid_y
        if self.pattern == PATTERN_A:
            return self.decode(0.0 if top and left else
                               0.25 if top else
                               0.75 if left else
                               1.0)
        return self.decode(1.0 if top and left else
                           0.75 if top else
                           0.25 if left else
                           0.0)


def tsc_words(func, filt, wu, wv, border):
    words = [0] * 8
    words[0] = (wu & 7) | ((wv & 7) << 3) | (1 << 9) | ((func & 7) << 10)
    words[1] = (filt & 3) | ((filt & 3) << 4) | (1 << 6)
    words[4] = f32_bits(border)
    return words


def region_for(resource):
    return (0, 0, SMALL - 1, SMALL - 1) if resource != RES_ATLAS else ATLAS_REGION


def dims_for(resource):
    return (SMALL, SMALL) if resource != RES_ATLAS else (ATLAS_W, ATLAS_H)


def oracle_case(resource, grid, uv, ref, func, filt, wu, wv, border):
    width, height = dims_for(resource)
    plan = {
        'compare_func': func,
        'filter': 'point' if filt == 1 else 'linear',
        'wrap_u': wu,
        'wrap_v': wv,
        'border_depth': f32(border),
    }
    uv = (f32(uv[0]), f32(uv[1]))
    ref = f32(ref)
    point = shadow_reference.sample_point(grid, (width, height), uv, ref, plan)
    linear = shadow_reference.sample_linear(grid, (width, height), uv, ref, plan)
    depth_first = shadow_reference.sample_linear_filter_depth_first(
        grid, (width, height), uv, ref, plan)
    return {
        'uv': [uv[0], uv[1]],
        'ref': ref,
        'point8': u8(point) if isinstance(point, float) else int(point * 255),
        'linear8': u8(linear),
        'dispatch8': u8(linear if filt == 2 else point),
        'depthfirst8': u8(depth_first),
    }


def case_table(decode=d24_binary32):
    cases = []
    current_pattern = {RES_DF32_64: None, RES_S8D24_64: None, RES_ATLAS: None}

    def add(name, resource, uv, ref, func, filt, wu, wv, border=0.0,
            produce=0, program=PROGRAM_O0, negative=False, aba=0):
        fmt_name = 'DF32' if resource != RES_S8D24_64 else 'S8D24'
        if resource == RES_ATLAS:
            fmt_name = 'DF32'
        if produce:
            current_pattern[resource] = produce
        if current_pattern[resource] is None:
            raise ValueError('%s has no produced pattern' % name)
        grid = PatternGrid(dims_for(resource)[0], dims_for(resource)[1],
                           region_for(resource), current_pattern[resource],
                           decode)
        expected = oracle_case(resource, grid, uv, ref, func,
                               filt, wu, wv, border)
        region = region_for(resource)
        cases.append({
            'id': len(cases) + 1,
            'name': name,
            'program': program,
            'resource': resource,
            'produce': produce,
            'aba': aba,
            'negative_depth_first': 1 if negative else 0,
            'format_name': fmt_name,
            'width': dims_for(resource)[0],
            'height': dims_for(resource)[1],
            'region': list(region),
            'tsc': tsc_words(func, filt, wu, wv, border),
            'compare_func': func,
            'filter': filt,
            'wrap_u': wu,
            'wrap_v': wv,
            'border': f32(border),
            'ref_bits': f32_bits(ref),
            'u_bits': f32_bits(uv[0]),
            'v_bits': f32_bits(uv[1]),
            'expected': expected,
            'tolerance': 1,
        })

    for resource, label in ((RES_DF32_64, 'df32'), (RES_S8D24_64, 's8d24')):
        first = PATTERN_A
        # Eight compare functions plus false-direction checks.  Bottom-right
        # texel of the quadrant pattern is depth 1.0; bottom-left is 0.75.
        for func in range(8):
            add('cmp%d_%s_br' % (func, label), resource, (0.75, 0.75), 0.5,
                func, 1, 2, 2, produce=first)
            first = 0
        add('cmp1_less_false_%s' % label, resource, (0.25, 0.75), 0.9,
            1, 1, 2, 2)
        add('cmp2_equal_true_%s' % label, resource, (0.25, 0.75), 0.75,
            2, 1, 2, 2)
        add('cmp4_greater_false_%s' % label, resource, (0.25, 0.75), 0.25,
            4, 1, 2, 2)
        add('cmp6_greaterequal_true_%s' % label, resource, (0.25, 0.75), 0.75,
            6, 1, 2, 2)

        # Point vs compare-first linear PCF across the quadrant boundary.
        add('point_boundary_%s' % label, resource, (0.5, 0.5), 0.5,
            1, 1, 2, 2)
        add('linear_boundary_pcf_%s' % label, resource, (0.5, 0.5), 0.5,
            1, 2, 2, 2, negative=True)
        add('linear_edge_pcf_%s' % label, resource, (0.25, 0.5), 0.5,
            1, 2, 2, 2)

        # Edges, corners, negative UV and UV exactly 1.0.
        add('negative_uv_clamp_%s' % label, resource, (-0.25, 0.25), 0.125,
            1, 1, 2, 2)
        add('uv_one_clamp_%s' % label, resource, (1.0, 0.25), 0.125,
            1, 1, 2, 2)
        add('corner_border_zero_%s' % label, resource, (-0.25, -0.25), 0.5,
            1, 1, 3, 3, border=0.0)
        add('corner_border_one_%s' % label, resource, (-0.25, -0.25), 0.5,
            1, 1, 3, 3, border=1.0)

        # All four wrap modes at an out-of-range x coordinate.
        add('wrap_repeat_%s' % label, resource, (1.25, 0.25), 0.125,
            1, 1, 0, 0)
        add('wrap_mirror_%s' % label, resource, (1.25, 0.25), 0.125,
            1, 1, 1, 0)
        add('wrap_clamp_%s' % label, resource, (1.25, 0.25), 0.125,
            1, 1, 2, 0)
        add('wrap_border_%s' % label, resource, (1.25, 0.25), 0.125,
            1, 1, 3, 0, border=0.0)

        # UV change and border R change with the same state.
        add('uv_top_left_%s' % label, resource, (0.25, 0.25), 0.5,
            1, 1, 2, 2)
        add('uv_bottom_right_%s' % label, resource, (0.75, 0.75), 0.5,
            1, 1, 2, 2)
        add('border_zero_%s' % label, resource, (-0.25, 0.25), 0.5,
            1, 1, 3, 0, border=0.0)
        add('border_one_%s' % label, resource, (-0.25, 0.25), 0.5,
            1, 1, 3, 0, border=1.0)
        add('linear_border_pcf_%s' % label, resource, (-0.25, 0.25), 0.5,
            1, 2, 3, 0, border=0.5)

        # Same depth VA/content, A -> B -> A.  The A slices must be byte-equal.
        add('aba_a1_%s' % label, resource, (0.75, 0.75), 0.5,
            1, 1, 2, 2, produce=PATTERN_A, aba=1)
        add('aba_b_%s' % label, resource, (0.75, 0.75), 0.5,
            1, 1, 2, 2, produce=PATTERN_B, aba=2)
        add('aba_a2_%s' % label, resource, (0.75, 0.75), 0.5,
            1, 1, 2, 2, produce=PATTERN_A, aba=3)

    # O1 smoke: same helper and uniforms, compiled at -O1.  It runs after the
    # DF32 A/B/A sequence, so the current pattern is A.
    add('o1_smoke_point_boundary', RES_DF32_64, (0.5, 0.5), 0.5,
        1, 1, 2, 2, program=PROGRAM_O1)

    # Real 1024x3072 depth allocation with a produced band at row 2600+.
    add('atlas_point_row2600', RES_ATLAS,
        ((ATLAS_REGION[0] + 16 + 0.5) / ATLAS_W,
         (ATLAS_REGION[1] + 16 + 0.5) / ATLAS_H), 0.125,
        1, 1, 2, 2, produce=PATTERN_A)
    add('atlas_linear_row2600', RES_ATLAS,
        ((ATLAS_REGION[0] + 32) / ATLAS_W,
         (ATLAS_REGION[1] + 32) / ATLAS_H), 0.5,
        1, 2, 2, 2)

    return cases


def validate(cases):
    seen_names = set()
    seen_ids = set()
    funcs = set()
    filters = set()
    wraps = set()
    resources = {RES_DF32_64: 0, RES_S8D24_64: 0, RES_ATLAS: 0}
    for case in cases:
        if case['id'] in seen_ids or case['name'] in seen_names:
            raise ValueError('duplicate case id or name')
        seen_ids.add(case['id'])
        seen_names.add(case['name'])
        funcs.add(case['compare_func'])
        filters.add(case['filter'])
        wraps.update((case['wrap_u'], case['wrap_v']))
        resources[case['resource']] += 1
        if case['negative_depth_first']:
            if case['expected']['linear8'] == case['expected']['depthfirst8']:
                raise ValueError('%s: compare-first and depth-first agree'
                                 % case['name'])
        if case['resource'] == RES_ATLAS:
            if case['region'][1] <= 2048:
                raise ValueError('atlas producer is not beyond row 2048')
            if ATLAS_W * ATLAS_H * 4 > ALLOC_CAP:
                raise ValueError('atlas depth allocation exceeds the 32 MiB cap')
    if funcs != set(range(8)):
        raise ValueError('compare function coverage incomplete: %r' % funcs)
    if filters != {1, 2}:
        raise ValueError('filter coverage incomplete: %r' % filters)
    if wraps != {0, 1, 2, 3}:
        raise ValueError('wrap coverage incomplete: %r' % wraps)
    for resource, count in resources.items():
        if count == 0:
            raise ValueError('resource %d has no cases' % resource)
    for label in ('df32', 's8d24'):
        phases = [c for c in cases if c['name'].startswith('aba_') and
                  c['name'].endswith(label)]
        if [c['aba'] for c in phases] != [1, 2, 3]:
            raise ValueError('A/B/A phase order is wrong for %s' % label)
        if phases[0]['expected'] != phases[2]['expected']:
            raise ValueError('A/B/A expected outputs differ for %s' % label)
        if phases[0]['expected']['dispatch8'] == phases[1]['expected']['dispatch8']:
            raise ValueError('A/B/A pattern B is not observable for %s' % label)
    if not any(c['program'] == PROGRAM_O1 for c in cases):
        raise ValueError('O1 smoke case missing')
    return cases


PLANNER_REJECTIONS = [
    ('depth-view-wrong-va', 'depth-view', -14),
    ('depth-view-wrong-shape', 'depth-view', -12),
    ('depth-view-wrong-descriptor-type', 'depth-view', -5),
    ('depth-view-unmaterialized', 'depth-view', -16),
    ('depth-view-storage-epoch', 'depth-view', -18),
    ('depth-view-shadow-contract-missing', 'depth-view', -27),
    ('depth-view-shadow-depth-flag', 'depth-view', -28),
    ('depth-view-ordinary-compare', 'depth-view', -19),
    ('shadow-compare-disabled', 'tsc', -4),
    ('shadow-filter-mismatch', 'tsc', -9),
    ('shadow-mip-level-present', 'tsc', -12),
    ('shadow-reduction', 'tsc', -13),
    ('shadow-anisotropy', 'tsc', -14),
    ('shadow-srgb', 'tsc', -21),
    ('shadow-lod-bias', 'tsc', -17),
    ('shadow-wrap-u-unsupported', 'tsc', -5),
    ('shadow-border-out-of-range', 'tsc', -23),
]

# The minimal precision diagnostic compares one depth texel read with the
# binary32 values immediately below, equal to and above 0.75.  A raw channel
# oracle of 255/255/255 means lower < sample == 0.75 < upper on the device.
# The 0.25 cases are negative controls: the same shader must report all three
# comparisons false for the first, equal and third channel respectively.
PRECISION_BOUNDS_BITS = (0x3F3FFFFF, 0x3F400000, 0x3F400001)
PRECISION_CASES = (
    {'id': 1, 'name': 'precision_df32_075', 'resource': RES_DF32_64,
     'format': 'DF32', 'u_bits': f32_bits(0.25), 'v_bits': f32_bits(0.75),
     'expected': (255, 255, 255, 255)},
    {'id': 2, 'name': 'precision_s8d24_075', 'resource': RES_S8D24_64,
     'format': 'S8D24', 'u_bits': f32_bits(0.25), 'v_bits': f32_bits(0.75),
     'expected': (255, 255, 255, 255)},
    {'id': 3, 'name': 'precision_df32_025_negative', 'resource': RES_DF32_64,
     'format': 'DF32', 'u_bits': f32_bits(0.75), 'v_bits': f32_bits(0.25),
     'expected': (0, 0, 255, 255)},
    {'id': 4, 'name': 'precision_s8d24_025_negative',
     'resource': RES_S8D24_64, 'format': 'S8D24',
     'u_bits': f32_bits(0.75), 'v_bits': f32_bits(0.25),
     'expected': (0, 0, 255, 255)},
)

# Four shadow calls per draw with distinct coordinates and references.  Every
# call result is a separately named live value and the accumulator is updated
# between calls; the first result is read again after the fourth call.  The
# same eight (2 modes x 4 configurations) cases are used by the liveness
# probe; the verifier recomputes each scalar from shadow_reference.
LIVENESS_MODES = ('point', 'linear')
LIVENESS_CONFIGS = (
    {'name': 'corner', 'calls': (
        ((0.5, 0.5), 0.9), ((0.5, 0.5), 0.5),
        ((0.5, 0.5), 0.0), ((0.25, 0.25), 0.5))},
    {'name': 'edges', 'calls': (
        ((0.5, 0.25), 0.125), ((0.5, 0.75), 0.5),
        ((0.25, 0.5), 0.5), ((0.75, 0.5), 0.1))},
    {'name': 'quadrants_low_ref', 'calls': (
        ((0.25, 0.25), 0.1), ((0.75, 0.25), 0.1),
        ((0.25, 0.75), 0.1), ((0.75, 0.75), 0.1))},
    {'name': 'quadrants_high_ref', 'calls': (
        ((0.25, 0.25), 0.9), ((0.75, 0.25), 0.9),
        ((0.25, 0.75), 0.9), ((0.75, 0.75), 0.9))},
)
LIVENESS_WEIGHTS = (0.25, 0.125, 0.0625, 0.03125)


def liveness_plan(filter_name):
    return {
        'compare_func': 1,  # Less: ref < depth
        'filter': filter_name,
        'wrap_u': 2,
        'wrap_v': 2,
        'border_depth': 0.0,
    }


def liveness_expected(config, filter_name):
    grid = PatternGrid(SMALL, SMALL, (0, 0, SMALL - 1, SMALL - 1),
                       PATTERN_A, d24_binary32)
    plan = liveness_plan(filter_name)
    sample = (shadow_reference.sample_point if filter_name == 'point'
              else shadow_reference.sample_linear)
    values = [sample(grid, (SMALL, SMALL), (f32(u), f32(v)), f32(ref), plan)
              for (u, v), ref in config['calls']]
    acc = values[0] * LIVENESS_WEIGHTS[0]
    acc = acc + values[1] * LIVENESS_WEIGHTS[1]
    acc = acc + values[2] * LIVENESS_WEIGHTS[2]
    acc = acc + ((values[3] if values[0] > 0.5 else 0.0)
                 * LIVENESS_WEIGHTS[3])
    return {
        'value': acc,
        'point8': u8(acc),
        'taps': [value for value in values],
    }


def liveness_cases():
    cases = []
    for mode in LIVENESS_MODES:
        for config in LIVENESS_CONFIGS:
            expected = liveness_expected(config, mode)
            cases.append({
                'id': len(cases) + 1,
                'name': 'liveness_%s_%s' % (mode, config['name']),
                'mode': mode,
                'config': config['name'],
                'calls': [
                    {'u_bits': f32_bits(u), 'v_bits': f32_bits(v),
                     'ref_bits': f32_bits(ref)}
                    for (u, v), ref in config['calls']
                ],
                'tsc': tsc_words(1, 1 if mode == 'point' else 2, 2, 2, 0.0),
                'expected': expected,
                'tolerance': 1,
            })
    return cases


def oracle_model_record():
    """Prove the versioned model changes only the two Equal cases."""
    current = validate(case_table(d24_binary32))
    legacy = case_table(d24_binary64)
    if len(current) != len(legacy):
        raise ValueError('oracle model case counts differ')
    inputs = ('id', 'name', 'program', 'resource', 'produce', 'aba',
              'negative_depth_first', 'format_name', 'width', 'height',
              'region', 'tsc', 'compare_func', 'filter', 'wrap_u', 'wrap_v',
              'border', 'ref_bits', 'u_bits', 'v_bits', 'tolerance')
    changed = []
    auxiliary = []
    for old, new in zip(legacy, current):
        for key in inputs:
            if old[key] != new[key]:
                raise ValueError('%s: model change altered input field %s'
                                 % (new['name'], key))
        device_keys = ('point8', 'linear8', 'dispatch8')
        if any(old['expected'][key] != new['expected'][key]
               for key in device_keys):
            changed.append({
                'id': new['id'],
                'name': new['name'],
                'legacy_expected': old['expected'],
                'versioned_expected': new['expected'],
                'changed_channels': [key for key in device_keys
                                     if old['expected'][key] !=
                                     new['expected'][key]],
            })
        elif old['expected'] != new['expected']:
            auxiliary.append({
                'id': new['id'],
                'name': new['name'],
                'legacy_depthfirst8': old['expected']['depthfirst8'],
                'versioned_depthfirst8': new['expected']['depthfirst8'],
            })
    if [row['id'] for row in changed] != [10, 41]:
        raise ValueError('oracle model expected exactly cases 10 and 41 to '
                         'change device-facing channels, got %r'
                         % [row['id'] for row in changed])
    if [row['id'] for row in auxiliary] != [13, 14, 44, 45, 63, 65]:
        raise ValueError('oracle model auxiliary depth-first changes are not '
                         'the enumerated six boundary cases: %r'
                         % [row['id'] for row in auxiliary])
    return {
        'sample_model': SAMPLE_MODEL,
        'legacy_sample_model': LEGACY_SAMPLE_MODEL,
        'scope': 'locked Vita3K OpenGL backend stores GL_DEPTH24_STENCIL8 for '
                 'both native DF32 and S8D24; depth texel decode is D24 unorm '
                 'then binary32. Border TSC words are already binary32 and '
                 'receive no D24 decode. This is not a claim of general F32 '
                 'hardware equivalence.',
        'changed_cases': changed,
        'auxiliary_depthfirst_changes': auxiliary,
        'device_expected_changed_ids': [row['id'] for row in changed],
        'inputs_unchanged': True,
    }


def header(cases, hashes, precision_cases, liveness):
    lines = [
        '/* Generated by tools/shader/prepare_gxm_shadow_probe.py. Do not edit. */',
        '#define VP_SHADOW_CASE_COUNT %du' % len(cases),
        '#define VP_SHADOW_RES_DF32_64 %du' % RES_DF32_64,
        '#define VP_SHADOW_RES_S8D24_64 %du' % RES_S8D24_64,
        '#define VP_SHADOW_RES_ATLAS %du' % RES_ATLAS,
        '#define VP_SHADOW_PROGRAM_O0 %du' % PROGRAM_O0,
        '#define VP_SHADOW_PROGRAM_O1 %du' % PROGRAM_O1,
        '#define VP_SHADOW_FMT_DF32 %du' % FMT_DF32,
        '#define VP_SHADOW_FMT_S8D24 %du' % FMT_S8D24,
        '#define VP_SHADOW_SMALL %du' % SMALL,
        '#define VP_SHADOW_ATLAS_W %du' % ATLAS_W,
        '#define VP_SHADOW_ATLAS_H %du' % ATLAS_H,
        '#define VP_SHADOW_ATLAS_REGION_X0 %du' % ATLAS_REGION[0],
        '#define VP_SHADOW_ATLAS_REGION_Y0 %du' % ATLAS_REGION[1],
        '#define VP_SHADOW_ATLAS_REGION_X1 %du' % ATLAS_REGION[2],
        '#define VP_SHADOW_ATLAS_REGION_Y1 %du' % ATLAS_REGION[3],
        '#define VP_SHADOW_ATLAS_BYTES %du' % (ATLAS_W * ATLAS_H * 4),
        '#define VP_SHADOW_ALLOC_CAP %du' % ALLOC_CAP,
        '#define VP_SHADOW_SAMPLE_MODEL "%s"' % SAMPLE_MODEL,
        '#define VP_SHADOW_PRECISION_BOUNDS_LO 0x%08xu' % PRECISION_BOUNDS_BITS[0],
        '#define VP_SHADOW_PRECISION_BOUNDS_EQ 0x%08xu' % PRECISION_BOUNDS_BITS[1],
        '#define VP_SHADOW_PRECISION_BOUNDS_HI 0x%08xu' % PRECISION_BOUNDS_BITS[2],
        '#define VP_SHADOW_POLICY_SHA256 "%s"' % shadow_helpers.policy_sha256(),
        '#define VP_SHADOW_HELPER_SHA256 "%s"' % shadow_helpers.helper_source_sha256(),
        '#define VP_SHADOW_EMITTER_SHA256 "%s"' % shadow_helpers.emitter_source_sha256(),
        '#define VP_SHADOW_STRATEGY "%s"' % shadow_helpers.STRATEGY,
        '#define VP_SHADOW_VERTEX_SHA256 "%s"' % hashes['shadow_v'],
        '#define VP_SHADOW_COLOUR_SHA256 "%s"' % hashes['shadow_colour_f'],
        '#define VP_SHADOW_O0_GXP_SHA256 "%s"' % hashes['shadow_probe_f_o0'],
        '#define VP_SHADOW_O1_GXP_SHA256 "%s"' % hashes['shadow_probe_f_o1'],
        '#define VP_SHADOW_PRECISION_SOURCE_SHA256 "%s"' % hashes['shadow_precision_f_source'],
        '#define VP_SHADOW_PRECISION_GXP_SHA256 "%s"' % hashes['shadow_precision_f'],
        '#define VP_SHADOW_LIVENESS_SOURCE_SHA256 "%s"' % hashes['shadow_liveness_f_source'],
        '#define VP_SHADOW_LIVENESS_GXP_SHA256 "%s"' % hashes['shadow_liveness_f'],
        '#define VP_SHADOW_PRECISION_CASE_COUNT %du' % len(precision_cases),
        '#define VP_SHADOW_LIVENESS_CASE_COUNT %du' % len(liveness),
        'typedef struct {',
        '    unsigned id, program, resource, produce, aba, negative_depth_first;',
        '    unsigned region_x0, region_y0, region_x1, region_y1;',
        '    uint32_t tsc[8];',
        '    unsigned ref_bits, u_bits, v_bits;',
        '    unsigned expected_point8, expected_linear8, expected_dispatch8;',
        '    unsigned expected_depthfirst8, tolerance;',
        '    const char *name;',
        '} vp_shadow_case;',
        'static const vp_shadow_case vp_shadow_cases[VP_SHADOW_CASE_COUNT] = {',
    ]
    for case in cases:
        exp = case['expected']
        lines.append(
            '    {%uu, %uu, %uu, %uu, %uu, %uu, %uu, %uu, %uu, %uu, '
            '{%s}, 0x%08xu, 0x%08xu, 0x%08xu, %uu, %uu, %uu, %uu, %uu, "%s"},'
            % (case['id'], case['program'], case['resource'], case['produce'],
               case['aba'], case['negative_depth_first'],
               case['region'][0], case['region'][1], case['region'][2],
               case['region'][3],
               ', '.join('0x%08xu' % word for word in case['tsc']),
               case['ref_bits'], case['u_bits'], case['v_bits'],
               exp['point8'], exp['linear8'], exp['dispatch8'],
               exp['depthfirst8'], case['tolerance'], case['name']))
    lines.extend([
        '};',
        'typedef struct {',
        '    unsigned id, resource, u_bits, v_bits;',
        '    unsigned expected_r, expected_g, expected_b, expected_a;',
        '    const char *name;',
        '} vp_shadow_precision_case;',
        'static const vp_shadow_precision_case '
        'vp_shadow_precision_cases[VP_SHADOW_PRECISION_CASE_COUNT] = {',
    ])
    for case in precision_cases:
        exp = case['expected']
        lines.append('    {%uu, %uu, 0x%08xu, 0x%08xu, %uu, %uu, %uu, %uu, "%s"},'
                     % (case['id'], case['resource'], case['u_bits'],
                        case['v_bits'], exp[0], exp[1], exp[2], exp[3],
                        case['name']))
    lines.extend([
        '};',
        'typedef struct {',
        '    unsigned id, mode, u_bits[4], v_bits[4], ref_bits[4];',
        '    uint32_t tsc[8];',
        '    unsigned expected8, tolerance;',
        '    const char *name;',
        '} vp_shadow_liveness_case;',
        'static const vp_shadow_liveness_case '
        'vp_shadow_liveness_cases[VP_SHADOW_LIVENESS_CASE_COUNT] = {',
    ])
    for case in liveness:
        calls = case['calls']
        lines.append(
            '    {%uu, %uu, {%s}, {%s}, {%s}, {%s}, %uu, %uu, "%s"},'
            % (case['id'], 0 if case['mode'] == 'point' else 1,
               ', '.join('0x%08xu' % call['u_bits'] for call in calls),
               ', '.join('0x%08xu' % call['v_bits'] for call in calls),
               ', '.join('0x%08xu' % call['ref_bits'] for call in calls),
               ', '.join('0x%08xu' % word for word in case['tsc']),
               case['expected']['point8'], case['tolerance'], case['name']))
    lines.append('};')
    return '\n'.join(lines) + '\n'


def manifest(cases, hashes, stages, header_path, precision_cases, liveness,
             model):
    return {
        'schema': 1,
        'task': 'M6-B22',
        'strategy': shadow_helpers.STRATEGY,
        'sample_model': SAMPLE_MODEL,
        'oracle_version': 2,
        'oracle_model': model,
        'method': 'self-authored Cg generated by the shared statement-inline '
                  'emitter with direct destination writes; GPU-produced depth '
                  'pattern; raw colour readback',
        'policy_sha256': shadow_helpers.policy_sha256(),
        'emitter_sha256': shadow_helpers.emitter_source_sha256(),
        'helper_sha256': shadow_helpers.helper_source_sha256(),
        'stages': stages,
        'header_sha256': digest(header_path),
        'cases': cases,
        'precision_probe': {
            'bounds_bits': list(PRECISION_BOUNDS_BITS),
            'cases': precision_cases,
            'gxp_sha256': hashes['shadow_precision_f'],
            'source_sha256': hashes['shadow_precision_f_source'],
            'sample_model': SAMPLE_MODEL,
        },
        'liveness_probe': {
            'modes': list(LIVENESS_MODES),
            'configs': [
                {'name': config['name'],
                 'calls': [{'u_bits': f32_bits(u), 'v_bits': f32_bits(v),
                            'ref_bits': f32_bits(ref)}
                           for (u, v), ref in config['calls']]}
                for config in LIVENESS_CONFIGS],
            'weights': list(LIVENESS_WEIGHTS),
            'cases': liveness,
            'gxp_sha256': hashes['shadow_liveness_f'],
            'source_sha256': hashes['shadow_liveness_f_source'],
        },
        'planner_rejections': [
            {'name': name, 'path': path, 'expected_status': status}
            for name, path, status in PLANNER_REJECTIONS
        ],
        'precision_note': 'depth texel reads use the versioned '
                          'D24_UNORM_TO_BINARY32 model; tolerance remains one '
                          '8-bit LSB and this is not a claim of general F32 '
                          'hardware equivalence',
        'target_content': False,
        'hardware_verified': False,
        'production_modified': False,
    }


STAGES = (
    ('shadow_v', 'sce_vp_psp2', 'shadow_v.cg', 'O0'),
    ('shadow_colour_f', 'sce_fp_psp2', 'shadow_colour_f.cg', 'O0'),
    ('shadow_probe_f_o0', 'sce_fp_psp2', 'shadow_probe_f_o0.cg', 'O0'),
    ('shadow_probe_f_o1', 'sce_fp_psp2', 'shadow_probe_f_o1.cg', 'O1'),
    ('shadow_precision_f', 'sce_fp_psp2', 'shadow_precision_f.cg', 'O0'),
    ('shadow_liveness_f', 'sce_fp_psp2', 'shadow_liveness_f.cg', 'O0'),
)


def write_sources(stage):
    vertex = '''// Self-authored position-only vertex program for M6-B22.
void main(float4 position : POSITION, out float4 out_position : POSITION) {
    out_position = position;
}
'''
    colour = '''// Self-authored solid-colour fragment program for depth-producing draws.
uniform float4 u_colour;
void main(out float4 out_color : COLOR0) {
    out_color = u_colour;
}
'''
    emitter = shadow_helpers.InlineEmitter()
    body = ['  color = float4(0.0, 0.0, 0.0, 1.0);']
    for mode, component in (('point', 'color.x'), ('linear', 'color.y'),
                            ('dispatch', 'color.z')):
        lines, _ = emitter.emit_sample('tex0', 'u_coord.xyz', '  ', mode,
                                       destination=component)
        body.extend(lines)
    body.append('  color.w = 1.0;')
    fragment = (
        '// Generated by tools/shader/prepare_gxm_shadow_probe.py using '
        'shadow_helpers.InlineEmitter\n'
        f'// strategy {shadow_helpers.STRATEGY}, policy {shadow_helpers.POLICY}. '
        'Each sample writes its output component directly; no shadow helper '
        'function is emitted.\n'
        'uniform sampler2D tex0;\n'
        'uniform float4 tex0_shadow_extent;\n'
        'uniform float4 tex0_shadow_state;\n'
        'uniform float4 tex0_shadow_border;\n'
        'uniform float4 u_coord;\n\n'
        'void main(out float4 color : COLOR0) {\n'
        + '\n'.join(body) + '\n'
        + '}\n')
    precision = (
        '// Self-authored minimal precision diagnostic for M6-B22.  This is a '
        'single raw depth read and three comparisons; it does not use the\n'
        '// statement-inline shadow emitter and cannot be confounded by its '
        'temporaries.  Each channel is written immediately: the former final\n'
        '// float4(gt,eq,lt,1) constructor packed values after scratch reuse '
        'and was not a valid comparison readback.\n'
        'uniform sampler2D tex0;\n'
        'uniform float4 u_probe;\n'
        'uniform float4 u_bounds;\n\n'
        'void main(out float4 color : COLOR0) {\n'
        '  color = float4(0.0, 0.0, 0.0, 1.0);\n'
        '  float depth = tex2Dlod(tex0, float4(u_probe.xy, 0.0, 0.0)).x;\n'
        '  color.x = (depth > u_bounds.x) ? 1.0 : 0.0;\n'
        '  color.y = (depth == u_bounds.y) ? 1.0 : 0.0;\n'
        '  color.z = (depth < u_bounds.z) ? 1.0 : 0.0;\n'
        '  color.w = 1.0;\n'
        '}\n')
    liveness_emitter = shadow_helpers.InlineEmitter()
    live_body = []
    for index in range(4):
        lines, result = liveness_emitter.emit_sample(
            'tex0', 'u_c%u.xyz' % index, '  ', 'dispatch')
        live_body.extend(lines)
        live_body.append('  float s%u = %s;' % (index, result))
        if index == 0:
            live_body.append('  float vp_acc = s0 * 0.25;')
        elif index == 1:
            live_body.append('  vp_acc = vp_acc + s1 * 0.125;')
        elif index == 2:
            live_body.append('  vp_acc = vp_acc + s2 * 0.0625;')
    live_body.append('  vp_acc = vp_acc + ((s0 > 0.5) ? (s3 * 0.03125) '
                     ': 0.0);')
    live_body.append('  color = float4(vp_acc, vp_acc, vp_acc, 1.0);')
    liveness = (
        '// Generated by tools/shader/prepare_gxm_shadow_probe.py using '
        'shadow_helpers.InlineEmitter\n'
        f'// strategy {shadow_helpers.STRATEGY}, policy {shadow_helpers.POLICY}. '
        'Four standalone production-shape shadow calls; every result stays '
        'live and the accumulator is updated between calls.\n'
        'uniform sampler2D tex0;\n'
        'uniform float4 tex0_shadow_extent;\n'
        'uniform float4 tex0_shadow_state;\n'
        'uniform float4 tex0_shadow_border;\n'
        'uniform float4 u_c0;\n'
        'uniform float4 u_c1;\n'
        'uniform float4 u_c2;\n'
        'uniform float4 u_c3;\n\n'
        'void main(out float4 color : COLOR0) {\n'
        + '\n'.join(live_body) + '\n'
        + '}\n')
    (stage / 'shadow_v.cg').write_text(vertex, encoding='utf-8', newline='\n')
    (stage / 'shadow_colour_f.cg').write_text(colour, encoding='utf-8', newline='\n')
    for name in ('shadow_probe_f_o0.cg', 'shadow_probe_f_o1.cg'):
        (stage / name).write_text(fragment, encoding='utf-8', newline='\n')
    (stage / 'shadow_precision_f.cg').write_text(
        precision, encoding='utf-8', newline='\n')
    (stage / 'shadow_liveness_f.cg').write_text(
        liveness, encoding='utf-8', newline='\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shacc', type=Path, required=True)
    parser.add_argument('--shacc-config', type=Path, required=True)
    parser.add_argument('--header', type=Path, required=True)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    arguments = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    config = {}
    for line in arguments.shacc_config.read_text(encoding='utf-8').splitlines():
        if '=' in line:
            key, value = line.split('=', 1)
            config[key.strip()] = value.strip()
    toolchain = json.loads((root / 'config/graphics/shader-toolchain.json')
                           .read_text(encoding='utf-8'))
    shacc = arguments.shacc.resolve()
    compiler = Path(config['root']) / 'fs/ur0/data/external/libshacccg.suprx'
    if (digest(shacc) != toolchain['shacc']['executable_sha256']
            or digest(Path(config['service-vpk'])) != toolchain['shacc']['service_vpk_sha256']
            or digest(compiler) != toolchain['libshacccg']['sha256']):
        raise ValueError('compiler toolchain identity mismatch')

    cases = validate(case_table(d24_binary32))
    model = oracle_model_record()
    precision_cases = list(PRECISION_CASES)
    liveness = liveness_cases()
    arguments.stage.mkdir(parents=True, exist_ok=True)
    write_sources(arguments.stage)
    hashes = {}
    stages = {}
    for stem, profile, source_name, opt in STAGES:
        source = arguments.stage / source_name
        gxp = arguments.stage / ('%s.gxp' % stem)
        if gxp.exists():
            gxp.unlink()
        timeout = '120' if stem in ('shadow_probe_f_o1', 'shadow_liveness_f') \
            else '40'
        args = [str(shacc), '--config', str(arguments.shacc_config.resolve()),
                '-profile', profile, '-' + opt, '-o', str(gxp.resolve()),
                str(source.resolve()), '--timeout', timeout]
        result = compile_serial(args, config['root'],
                                timeout=150 if stem == 'shadow_liveness_f'
                                else 50)
        (arguments.stage / ('%s.shacc.log' % stem)).write_bytes(
            result.stdout + result.stderr)
        if result.returncode or not gxp.exists():
            raise ValueError('%s did not compile; see %s.shacc.log'
                             % (source.name, stem))
        binary = gxp.read_bytes()
        if (len(binary) < 32 or binary[:4] != b'GXP\0'
                or struct.unpack_from('<I', binary, 8)[0] > len(binary)):
            raise ValueError('invalid GXP output header for %s' % source.name)
        hashes[stem] = digest(gxp)
        if stem in ('shadow_precision_f', 'shadow_liveness_f'):
            hashes[stem + '_source'] = digest(source)
        stages[stem] = {
            'profile': profile,
            'optimization': opt,
            'source_sha256': digest(source),
            'source_bytes': source.stat().st_size,
            'gxp_sha256': hashes[stem],
            'gxp_bytes': len(binary),
        }

    arguments.header.parent.mkdir(parents=True, exist_ok=True)
    arguments.header.write_text(header(cases, hashes, precision_cases,
                                       liveness), encoding='utf-8',
                                newline='\n')
    if arguments.report:
        write_json(arguments.report, manifest(cases, hashes, stages,
                                               arguments.header,
                                               precision_cases, liveness,
                                               model))
    print('compiled %d stages, %d cases, strategy %s, emitter %s, O0 %s, O1 %s' % (
        len(STAGES), len(cases), shadow_helpers.STRATEGY,
        shadow_helpers.emitter_source_sha256()[:12],
        hashes['shadow_probe_f_o0'][:12], hashes['shadow_probe_f_o1'][:12]))


if __name__ == '__main__':
    main()

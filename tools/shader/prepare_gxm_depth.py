"""Compile the self-authored M6-B15 depth-mechanism probe shaders and emit their contract.

The probe asks one bounded question: can an actual no-colour GXM producer write
native depth, and can a later regular sampler2D draw read that same native depth
allocation back through the locked renderer?  This tool compiles four
self-authored Cg programs with the locked Shacc toolchain and records the
independent CPU expectations the target probe and the host verifier use.

Nothing in this tool talks to the target, an emulator, or the game.  It does not
rewrite game shaders and it does not lower production shadow behaviour.  All
expected values are the standard D24/UNORM8 quantisation of the stated window
depth; the tool refuses a case whose expectation is not independently
reproducible from that rule.

Guards:

* The Shacc executable, service VPK, and libshacccg identity must match the
  locked shader toolchain record.
* Every stage must produce a fresh GXP with a valid header; a stale file is
  never accepted.
* Numeric expectations are derived here, in Python, from the depth values and
  the documented quantisation, not copied from the target probe.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import struct
import sys

# An embedded interpreter ships a ._pth that keeps the script directory off sys.path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert_cache import compile_serial, digest, write_json

FORMAT_DF32 = 0
FORMAT_S8D24 = 1
KIND_SAMPLE = 0
KIND_TEST_FAR = 1
KIND_TEST_NEAR = 2
KIND_SELECT = 3
KIND_WRONG_VA = 4
KIND_ATLAS = 5

SMALL = 32
BIG = 64
ATLAS_W = 1024
ATLAS_H = 3072
ALLOC_CAP = 32 * 1024 * 1024
D24_MAX = (1 << 24) - 1

PACKED_BLUE = 0xFF0000FF
PACKED_GREEN = 0xFF00FF00

STAGES = (
    ('depth_v', 'sce_vp_psp2'),
    ('depth_uv_v', 'sce_vp_psp2'),
    ('depth_sample_f', 'sce_fp_psp2'),
    ('depth_colour_f', 'sce_fp_psp2'),
)


def f32(value: float) -> float:
    return struct.unpack('<f', struct.pack('<f', value))[0]


def f32_bits(value: float) -> int:
    return struct.unpack('<I', struct.pack('<f', f32(value)))[0]


def depth24(value: float) -> float:
    """The D24 storage value of a window depth; the locked GL backend stores
    GL_DEPTH24_STENCIL8 even for a native DF32 surface."""
    return math.floor(f32(value) * D24_MAX + 0.5) / D24_MAX


def rgba8(value: float) -> int:
    """UNORM8 conversion of a stored depth value, round to nearest."""
    return math.floor(depth24(value) * 255.0 + 0.5)


def depth_case(case_id, name, kind, fmt, width, height, depth, *, expected8=None,
               expected_edge8=None, expected_rgba=None, expected_edge_rgba=None,
               expect_reject=0, probe_x=None, probe_y=None, region=None):
    if region is None:
        inset = width // 4
        region = (inset, inset, width - inset - 1, height - inset - 1)
    if probe_x is None:
        probe_x = (region[0] + region[2]) // 2
    if probe_y is None:
        probe_y = (region[1] + region[3]) // 2
    if expected8 is None and expected_rgba is None:
        expected8 = rgba8(depth)
    if expected_edge8 is None and expected_edge_rgba is None:
        expected_edge8 = rgba8(0.75)
    return {
        'id': case_id,
        'name': name,
        'kind': kind,
        'format': fmt,
        'width': width,
        'height': height,
        'depth_bits': f32_bits(depth),
        'depth': f32(depth),
        'clear_bits': f32_bits(0.75),
        'clear': 0.75,
        'expected8': expected8 if expected8 is not None else 0,
        'expected_edge8': expected_edge8 if expected_edge8 is not None else 0,
        'expected_rgba': expected_rgba if expected_rgba is not None else 0,
        'expected_edge_rgba': expected_edge_rgba if expected_edge_rgba is not None else 0,
        'tolerance': 1,
        'probe_x': probe_x,
        'probe_y': probe_y,
        'region_x0': region[0],
        'region_y0': region[1],
        'region_x1': region[2],
        'region_y1': region[3],
        'expect_reject': expect_reject,
    }


def case_table():
    cases = []
    for fmt, fmt_name in ((FORMAT_DF32, 'DF32'), (FORMAT_S8D24, 'S8D24')):
        for size, size_name in ((SMALL, '32'), (BIG, '64')):
            for depth, depth_name in ((0.25, '025'), (0.625, '0625')):
                cases.append(depth_case(
                    len(cases) + 1,
                    'sample_%s_%s_depth_%s' % (fmt_name.lower(), size_name, depth_name),
                    KIND_SAMPLE, fmt, size, size, depth))
    cases.append(depth_case(len(cases) + 1, 'reuse_far_rejected', KIND_TEST_FAR,
                            FORMAT_DF32, SMALL, SMALL, 0.25,
                            expected_rgba=PACKED_BLUE, expected_edge_rgba=PACKED_BLUE))
    cases.append(depth_case(len(cases) + 1, 'reuse_near_passes', KIND_TEST_NEAR,
                            FORMAT_DF32, SMALL, SMALL, 0.25,
                            expected_rgba=PACKED_GREEN, expected_edge_rgba=PACKED_BLUE))
    cases.append(depth_case(len(cases) + 1, 'select_depth_a', KIND_SELECT,
                            FORMAT_DF32, SMALL, SMALL, 0.25))
    cases.append(depth_case(len(cases) + 1, 'select_depth_b', KIND_SELECT,
                            FORMAT_DF32, SMALL, SMALL, 0.5))
    cases.append(depth_case(len(cases) + 1, 'select_depth_a_again', KIND_SELECT,
                            FORMAT_DF32, SMALL, SMALL, 0.25))
    cases.append(depth_case(len(cases) + 1, 'wrong_va_rejected', KIND_WRONG_VA,
                            FORMAT_DF32, SMALL, SMALL, 0.25, expect_reject=1))
    cases.append(depth_case(
        len(cases) + 1, 'atlas_1024x3072_beyond_2048', KIND_ATLAS,
        FORMAT_DF32, ATLAS_W, ATLAS_H, 0.3,
        expected8=rgba8(0.3), expected_edge8=rgba8(0.75),
        probe_x=516, probe_y=2616, region=(500, 2600, 531, 2631)))
    return cases


def validate(cases):
    seen_names = set()
    seen_ids = set()
    for case in cases:
        if case['id'] in seen_ids or case['name'] in seen_names:
            raise ValueError('duplicate case id or name')
        seen_ids.add(case['id'])
        seen_names.add(case['name'])
        expected = rgba8(struct.unpack('<f', struct.pack('<I', case['depth_bits']))[0])
        if case['expected8'] and case['expected8'] != expected:
            raise ValueError('%s: expected8 is not the D24/UNORM8 quantisation' % case['name'])
        if case['expected_edge8'] and case['expected_edge8'] != rgba8(0.75):
            raise ValueError('%s: edge expectation is not the 0.75 clear' % case['name'])
        if case['kind'] == KIND_ATLAS:
            if case['height'] <= 2048:
                raise ValueError('the atlas case must exceed row 2048')
            if case['width'] > 4096 or case['height'] > 4096:
                raise ValueError('the atlas case exceeds the GXM 4096 dimension bound')
            if case['region_y0'] <= 2048:
                raise ValueError('the atlas producer quad is not beyond row 2048')
            if case['width'] * ((case['height'] + 31) // 32 * 32) * 4 > ALLOC_CAP:
                raise ValueError('the atlas depth allocation exceeds 32 MiB')
    # The 0.25 and 0.625 cases must produce different 8-bit answers.
    pair = {}
    for case in cases:
        if case['kind'] == KIND_SAMPLE:
            pair[(case['format'], case['width'], case['depth'])] = case['expected8']
    for fmt, _name in ((FORMAT_DF32, 'DF32'), (FORMAT_S8D24, 'S8D24')):
        for size in (SMALL, BIG):
            low = pair[(fmt, size, 0.25)]
            high = pair[(fmt, size, 0.625)]
            if low == high:
                raise ValueError('depth 0.25 and 0.625 do not change the 8-bit output')
    return cases


def header(cases, hashes):
    lines = [
        '/* Generated by tools/shader/prepare_gxm_depth.py. Do not edit. */',
        '#define VP_DEPTH_CASE_COUNT %du' % len(cases),
        '#define VP_DEPTH_FMT_DF32 %du' % FORMAT_DF32,
        '#define VP_DEPTH_FMT_S8D24 %du' % FORMAT_S8D24,
        '#define VP_DEPTH_KIND_SAMPLE %du' % KIND_SAMPLE,
        '#define VP_DEPTH_KIND_TEST_FAR %du' % KIND_TEST_FAR,
        '#define VP_DEPTH_KIND_TEST_NEAR %du' % KIND_TEST_NEAR,
        '#define VP_DEPTH_KIND_SELECT %du' % KIND_SELECT,
        '#define VP_DEPTH_KIND_WRONG_VA %du' % KIND_WRONG_VA,
        '#define VP_DEPTH_KIND_ATLAS %du' % KIND_ATLAS,
        '#define VP_DEPTH_SMALL %du' % SMALL,
        '#define VP_DEPTH_BIG %du' % BIG,
        '#define VP_DEPTH_ATLAS_W %du' % ATLAS_W,
        '#define VP_DEPTH_ATLAS_H %du' % ATLAS_H,
        '#define VP_DEPTH_ATLAS_BYTES %du' % (ATLAS_W * ATLAS_H * 4),
        '#define VP_DEPTH_ALLOC_CAP %du' % ALLOC_CAP,
        '#define VP_DEPTH_VERTEX_SHA256 "%s"' % hashes['depth_v'],
        '#define VP_DEPTH_UV_VERTEX_SHA256 "%s"' % hashes['depth_uv_v'],
        '#define VP_DEPTH_SAMPLE_FRAGMENT_SHA256 "%s"' % hashes['depth_sample_f'],
        '#define VP_DEPTH_COLOUR_FRAGMENT_SHA256 "%s"' % hashes['depth_colour_f'],
        'typedef struct {',
        '    unsigned id, kind, format, width, height;',
        '    unsigned depth_bits, clear_bits;',
        '    unsigned expected8, expected_edge8;',
        '    unsigned expected_rgba, expected_edge_rgba;',
        '    unsigned tolerance, probe_x, probe_y;',
        '    unsigned region_x0, region_y0, region_x1, region_y1;',
        '    unsigned expect_reject;',
        '    const char *name;',
        '} vp_depth_case;',
        'static const vp_depth_case vp_depth_cases[VP_DEPTH_CASE_COUNT] = {',
    ]
    for case in cases:
        lines.append(
            '    {%uu, %uu, %uu, %uu, %uu, 0x%08xu, 0x%08xu, %uu, %uu, 0x%08xu, '
            '0x%08xu, %uu, %uu, %uu, %uu, %uu, %uu, %uu, %uu, "%s"},' % (
                case['id'], case['kind'], case['format'], case['width'], case['height'],
                case['depth_bits'], case['clear_bits'], case['expected8'],
                case['expected_edge8'], case['expected_rgba'], case['expected_edge_rgba'],
                case['tolerance'], case['probe_x'], case['probe_y'], case['region_x0'],
                case['region_y0'], case['region_x1'], case['region_y1'],
                case['expect_reject'], case['name']))
    lines.append('};')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shacc', type=Path, required=True)
    parser.add_argument('--shacc-config', type=Path, required=True)
    parser.add_argument('--source', type=Path,
                        default=Path(__file__).resolve().parents[2] / 'src/probes/shaders')
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

    cases = validate(case_table())
    arguments.stage.mkdir(parents=True, exist_ok=True)
    hashes = {}
    stages = {}
    for stem, profile in STAGES:
        source = arguments.source / ('%s.cg' % stem)
        gxp = arguments.stage / ('%s.gxp' % stem)
        if gxp.exists():
            gxp.unlink()
        args = [str(shacc), '--config', str(arguments.shacc_config.resolve()),
                '-profile', profile, '-O0', '-o', str(gxp.resolve()),
                str(source.resolve()), '--timeout', '40']
        result = compile_serial(args, config['root'])
        (arguments.stage / ('%s.shacc.log' % stem)).write_bytes(result.stdout + result.stderr)
        if result.returncode or not gxp.exists():
            raise ValueError('%s did not compile; see %s.shacc.log' % (source.name, stem))
        binary = gxp.read_bytes()
        if (len(binary) < 32 or binary[:4] != b'GXP\0'
                or struct.unpack_from('<I', binary, 8)[0] > len(binary)):
            raise ValueError('invalid GXP output header for %s' % source.name)
        hashes[stem] = digest(gxp)
        stages[stem] = {
            'profile': profile,
            'source_sha256': digest(source),
            'source_bytes': source.stat().st_size,
            'gxp_sha256': hashes[stem],
            'gxp_bytes': len(binary),
        }

    arguments.header.parent.mkdir(parents=True, exist_ok=True)
    arguments.header.write_text(header(cases, hashes), encoding='utf-8', newline='\n')
    if arguments.report:
        write_json(arguments.report, {
            'schema': 1,
            'task': 'M6-B15',
            'method': 'self-authored Cg programs compiled offline by the locked Shacc '
                      'toolchain; no game shader, no production source change',
            'toolchain': {
                'shacc_sha256': digest(shacc),
                'service_vpk_sha256': digest(Path(config['service-vpk'])),
                'libshacccg_sha256': digest(compiler),
            },
            'stages': stages,
            'header_sha256': digest(arguments.header),
            'expected_rule': 'window depth -> D24 round-to-nearest -> UNORM8 '
                             'round-to-nearest; tolerance 1 LSB',
            'target_content': False,
            'hardware_verified': False,
            'production_modified': False,
            'cases': cases,
        })
    print('compiled %d stages, %d cases, vertex %s sample-fragment %s' % (
        len(STAGES), len(cases), hashes['depth_v'][:12], hashes['depth_sample_f'][:12]))


if __name__ == '__main__':
    main()

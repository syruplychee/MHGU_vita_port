#!/usr/bin/env python3
"""Verify the M6-B15 depth-mechanism probe from raw readback and a device log.

The verifier is deliberately independent of the target-side pass flags.  It
reads the combined raw RGBA dump, re-derives every expected 8-bit value from
the stated window depth with the D24/UNORM8 rule, and decides each case from
the bytes.  API success, a green CASE_RESULT status, or a log without a GL
program are not pixel evidence by themselves.

Declared positive cases:

  * eight sampler cases: DF32/S8D24 at 32x32 and 64x64, producer depth 0.25
    and 0.625, with a 0.75 background.  The whole centered interior must be
    the expected grayscale value and the whole outside must be the 0.75
    background; a black interior or a uniform image fails;
  * two depth-test reuse cases on the same produced 0.25 surface across two
    distinct colour buffers: a farther 0.5 draw must leave blue, a nearer
    0.125 draw must write green, and every untouched edge must stay blue;
  * three A/B/A selections: the readback must follow the selected depth
    address (0.25 -> 64, 0.5 -> 128) and return to A unchanged;
  * one wrong-VA negative: a texture view of the untouched colour sentinel
    must not reproduce the A expectation;
  * one 1024x3072 atlas case: a band beyond row 2048 must read back its own
    depth at the corresponding normalized coordinate, the background must
    read back 0.75, and the native depth allocation must stay under 32 MiB.

The image is only evidence about this locked OpenGL backend: it stores depth
as GL_DEPTH24_STENCIL8 even for a native DF32 surface, so the tolerance is one
8-bit LSB and this is not claimed as F32 hardware equivalence.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import sys
from pathlib import Path

D24_MAX = (1 << 24) - 1
ALLOC_CAP = 32 * 1024 * 1024
ATLAS_EXPECTED_BYTES = 1024 * 3072 * 4

# id -> (name, kind).  The verifier does not accept an implicit case set.
EXPECTED_CASES = {
    1: ('sample_df32_32_depth_025', 'sample'),
    2: ('sample_df32_32_depth_0625', 'sample'),
    3: ('sample_df32_64_depth_025', 'sample'),
    4: ('sample_df32_64_depth_0625', 'sample'),
    5: ('sample_s8d24_32_depth_025', 'sample'),
    6: ('sample_s8d24_32_depth_0625', 'sample'),
    7: ('sample_s8d24_64_depth_025', 'sample'),
    8: ('sample_s8d24_64_depth_0625', 'sample'),
    9: ('reuse_far_rejected', 'test_far'),
    10: ('reuse_near_passes', 'test_near'),
    11: ('select_depth_a', 'select'),
    12: ('select_depth_b', 'select'),
    13: ('select_depth_a_again', 'select'),
    14: ('wrong_va_rejected', 'wrong_va'),
    15: ('atlas_1024x3072_beyond_2048', 'atlas'),
}

PAIR_IDS = ((1, 2), (3, 4), (5, 6), (7, 8))

# B18 adds six production-view A/B/A rows (two formats x three phases).  The
# producer uses the same native view planner/init as production but writes the
# depth with a real GPU draw; each dump is one 64x64 consumer readback.
ABA_ROWS = 6
ABA_DUMP_BYTES = 64 * 64 * 4
ABA_PHASES = (
    (0, 0x3E800000, 64),
    (1, 0x3F000000, 128),
    (2, 0x3E800000, 64),
)
ABA_LABELS = ('df32', 's8d24')

# B19 adds explicit clear-materialization rows.  The clear is executed by the
# same native empty-scene helper the production path calls, then sampled with
# the regular uv/sample program.  IDs and names are fixed here so a device log
# cannot silently drop, rename or reorder a clear case.
CLEAR_ROWS = 9
CLEAR_DUMP_BYTES = 64 * 64 * 4
CLEAR_ABA = (
    (101, 'clear_aba_df32_phase0', 'df32', 0, 0x3E800000, 64),
    (102, 'clear_aba_df32_phase1', 'df32', 1, 0x3F200000, 159),
    (103, 'clear_aba_df32_phase2', 'df32', 2, 0x3E800000, 64),
    (104, 'clear_aba_s8d24_phase0', 's8d24', 0, 0x3E800000, 64),
    (105, 'clear_aba_s8d24_phase1', 's8d24', 1, 0x3F200000, 159),
    (106, 'clear_aba_s8d24_phase2', 's8d24', 2, 0x3E800000, 64),
)
CLEAR_DRAW = (
    (107, 'clear_draw_df32', 'df32'),
    (108, 'clear_draw_s8d24', 's8d24'),
)
CLEAR_ATLAS = (109, 'clear_atlas_1024x3072_band')


def f32_from_bits(bits: int) -> float:
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def depth24(value: float) -> float:
    return math.floor(value * D24_MAX + 0.5) / D24_MAX


def rgba8(value: float) -> int:
    return math.floor(depth24(value) * 255.0 + 0.5)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_evidence(path: Path, kind: str, problems: list[str]):
    """Read evidence without turning a missing file into an uncaught crash."""
    try:
        return path.read_bytes()
    except FileNotFoundError:
        problems.append('missing %s file: %s' % (kind, path))
    except OSError as error:
        problems.append('cannot read %s file %s: %s' % (kind, path, error))
    return None


def kv_tokens(text: str) -> dict[str, str]:
    return dict(re.findall(r'([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)', text))


def parse_region(text: str):
    match = re.fullmatch(r'(\d+),(\d+)\.\.(\d+),(\d+)', text)
    if not match:
        raise ValueError('bad region %r' % text)
    return tuple(int(part) for part in match.groups())


def parse_xy(text: str):
    match = re.fullmatch(r'(\d+),(\d+)', text)
    if not match:
        raise ValueError('bad coordinate %r' % text)
    return int(match.group(1)), int(match.group(2))


def packed_rgba(value: int):
    return ((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF,
            (value >> 24) & 0xFF)


def pixel_at(data: bytes, width: int, x: int, y: int):
    offset = (y * width + x) * 4
    if offset + 4 > len(data):
        raise ValueError('pixel outside dump')
    return tuple(data[offset:offset + 4])


def grayscale_match(pixel, expected: int, tolerance: int) -> bool:
    return (all(abs(pixel[channel] - expected) <= tolerance for channel in range(3))
            and pixel[3] == 255)


def scan_regions(data: bytes, width: int, height: int, region, expected: int,
                 expected_edge: int, tolerance: int):
    x0, y0, x1, y1 = region
    inside = outside = bad_inside = bad_outside = 0
    for y in range(height):
        for x in range(width):
            pixel = pixel_at(data, width, x, y)
            if x0 <= x <= x1 and y0 <= y <= y1:
                inside += 1
                if not grayscale_match(pixel, expected, tolerance):
                    bad_inside += 1
            else:
                outside += 1
                if not grayscale_match(pixel, expected_edge, tolerance):
                    bad_outside += 1
    return inside, outside, bad_inside, bad_outside


def verify(log_path: Path, dump_path: Path) -> dict:
    problems: list[str] = []
    log_data = read_evidence(log_path, 'log', problems)
    dump_data = read_evidence(dump_path, 'dump', problems)
    text = log_data.decode('utf-8', errors='replace') if log_data is not None else ''
    dump = dump_data if dump_data is not None else b''
    log_present = log_data is not None
    dump_present = dump_data is not None
    cases: dict[int, dict] = {}
    results: dict[int, dict] = {}
    aba_rows: list[dict] = []
    clear_decls: dict[int, dict] = {}
    clear_results: dict[int, dict] = {}
    dumps: dict[int, int] = {}
    sentinel: dict | None = None
    atlas_bounds = None
    atlas_status = None
    verdict = None

    for line in text.splitlines():
        if line.startswith('CASE_RESULT '):
            tokens = kv_tokens(line)
            try:
                case_id = int(tokens['id'])
            except (KeyError, ValueError):
                problems.append('malformed CASE_RESULT line: %s' % line)
                continue
            if case_id in results:
                problems.append('case %d has more than one result' % case_id)
            results[case_id] = tokens
        elif line.startswith('CASE '):
            tokens = kv_tokens(line)
            try:
                case_id = int(tokens['id'])
            except (KeyError, ValueError):
                problems.append('malformed CASE line: %s' % line)
                continue
            if case_id in cases:
                problems.append('case %d declared more than once' % case_id)
            cases[case_id] = tokens
        elif line.startswith('CASE_FAIL '):
            problems.append('device reported %s' % line)
        elif line.startswith('PROD_ABA '):
            tokens = kv_tokens(line)
            try:
                tokens['_phase'] = int(tokens['phase'])
                tokens['_offset'] = int(tokens['dump_offset'])
                tokens['_expected'] = int(tokens['expected8'])
            except (KeyError, ValueError):
                problems.append('malformed PROD_ABA line: %s' % line)
                continue
            aba_rows.append(tokens)
        elif line.startswith('CLEAR '):
            tokens = kv_tokens(line)
            try:
                clear_id = int(tokens['id'])
            except (KeyError, ValueError):
                problems.append('malformed CLEAR line: %s' % line)
                continue
            if clear_id in clear_decls:
                problems.append('clear case %d declared more than once' % clear_id)
            clear_decls[clear_id] = tokens
        elif line.startswith('CLEAR_RESULT '):
            tokens = kv_tokens(line)
            try:
                clear_id = int(tokens['id'])
            except (KeyError, ValueError):
                problems.append('malformed CLEAR_RESULT line: %s' % line)
                continue
            if clear_id in clear_results:
                problems.append('clear case %d has more than one result' % clear_id)
            clear_results[clear_id] = tokens
        elif line.startswith('DUMP '):
            tokens = kv_tokens(line)
            try:
                dump_at = int(tokens['offset'])
                dump_bytes = int(tokens['bytes'])
            except (KeyError, ValueError):
                problems.append('malformed DUMP line: %s' % line)
                continue
            if dump_at in dumps:
                problems.append('dump offset %d appears more than once' % dump_at)
            dumps[dump_at] = dump_bytes
        elif line.startswith('SENTINEL_CHECK '):
            sentinel = kv_tokens(line)
        elif line.startswith('ATLAS_BOUNDS '):
            atlas_bounds = kv_tokens(line)
        elif line.startswith('ATLAS_STATUS '):
            atlas_status = kv_tokens(line)
        elif line.startswith('RESULT '):
            verdict = line.split(None, 1)[1].strip()

    for case_id, (name, kind) in EXPECTED_CASES.items():
        tokens = cases.get(case_id)
        if not tokens:
            problems.append('case %d (%s) is missing' % (case_id, name))
            continue
        if tokens.get('name') != name:
            problems.append('case %d name %r != %r' % (case_id, tokens.get('name'), name))
        if tokens.get('kind') != kind:
            problems.append('case %d kind %r != %r' % (case_id, tokens.get('kind'), kind))
    for case_id in cases:
        if case_id not in EXPECTED_CASES:
            problems.append('unexpected case id %d' % case_id)

    case_summaries = []
    hashes: dict[int, str] = {}
    centre: dict[int, tuple] = {}
    case_slices: list[tuple[str, int, int]] = []
    case_sizes: dict[int, int] = {}

    for case_id in sorted(cases):
        if case_id not in EXPECTED_CASES:
            continue
        name, kind = EXPECTED_CASES[case_id]
        declaration = cases[case_id]
        result = results.get(case_id)
        if not result:
            problems.append('%s: missing CASE_RESULT' % name)
            continue
        if result.get('status') != 'PASS':
            problems.append('%s: device status %r' % (name, result.get('status')))
        try:
            offset = int(result['dump_offset'])
            size = int(result['dump_bytes'])
        except (KeyError, ValueError):
            problems.append('%s: missing dump offset/size' % name)
            continue
        if offset < 0 or size <= 0 or offset + size > len(dump):
            problems.append('%s: dump slice %d+%d outside %d bytes' %
                            (name, offset, size, len(dump)))
            continue
        case_slices.append((name, offset, size))
        case_sizes[case_id] = size
        data = dump[offset:offset + size]
        hashes[case_id] = sha256(data)
        width = int(declaration.get('width', '0'))
        height = int(declaration.get('height', '0'))
        if width <= 0 or height <= 0:
            problems.append('%s: missing dimensions' % name)
            continue

        if kind == 'sample' or kind == 'select':
            try:
                bits = int(declaration['depth_bits'], 0)
                depth = f32_from_bits(bits)
                expected = rgba8(depth)
                expected_edge = rgba8(0.75)
                tolerance = int(declaration.get('tolerance', '99'))
                region = parse_region(declaration['region'])
            except (KeyError, ValueError) as error:
                problems.append('%s: bad declaration (%s)' % (name, error))
                continue
            if int(declaration.get('expected8', '-1')) != expected:
                problems.append('%s: declared expected8 is not the independent %d' %
                                (name, expected))
            if int(declaration.get('expected_edge8', '-1')) != expected_edge:
                problems.append('%s: declared edge expectation is not %d' %
                                (name, expected_edge))
            if tolerance > 1:
                problems.append('%s: tolerance %d exceeds the 1-LSB contract' %
                                (name, tolerance))
            if size != width * height * 4:
                problems.append('%s: dump size %d != %d' % (name, size, width * height * 4))
                continue
            inside, outside, bad_inside, bad_outside = scan_regions(
                data, width, height, region, expected, expected_edge, tolerance)
            if inside == 0 or outside == 0 or bad_inside or bad_outside:
                problems.append('%s: interior_bad=%d edge_bad=%d (inside=%d outside=%d)' %
                                (name, bad_inside, bad_outside, inside, outside))
            if int(result.get('region_bad', '-1')) != bad_inside or \
                    int(result.get('edge_bad', '-1')) != bad_outside:
                problems.append('%s: device counts disagree with the raw dump' % name)
            centre[case_id] = pixel_at(data, width,
                                       (region[0] + region[2]) // 2,
                                       (region[1] + region[3]) // 2)
            if centre[case_id][:3] == (0, 0, 0):
                problems.append('%s: interior is black' % name)

        elif kind in ('test_far', 'test_near'):
            try:
                region = parse_region(declaration['region'])
                expected = packed_rgba(int(declaration['expected_rgba'], 0))
                expected_edge = packed_rgba(int(declaration['expected_edge_rgba'], 0))
            except (KeyError, ValueError) as error:
                problems.append('%s: bad declaration (%s)' % (name, error))
                continue
            if size != width * height * 4:
                problems.append('%s: dump size mismatch' % name)
                continue
            x0, y0, x1, y1 = region
            inside = outside = bad_inside = bad_outside = 0
            for y in range(height):
                for x in range(width):
                    pixel = pixel_at(data, width, x, y)
                    if x0 <= x <= x1 and y0 <= y <= y1:
                        inside += 1
                        if pixel != expected:
                            bad_inside += 1
                    else:
                        outside += 1
                        if pixel != expected_edge:
                            bad_outside += 1
            if inside == 0 or outside == 0 or bad_inside or bad_outside:
                problems.append('%s: interior_bad=%d edge_bad=%d' %
                                (name, bad_inside, bad_outside))
            if int(result.get('interior_bad', '-1')) != bad_inside or \
                    int(result.get('edge_bad', '-1')) != bad_outside:
                problems.append('%s: device counts disagree with the raw dump' % name)
            centre[case_id] = pixel_at(data, width,
                                       (region[0] + region[2]) // 2,
                                       (region[1] + region[3]) // 2)
            if name.startswith('reuse_near') and centre[case_id][:3] != (0, 255, 0):
                problems.append('%s: near draw did not write green' % name)
            if name.startswith('reuse_far') and centre[case_id][:3] != (0, 0, 255):
                problems.append('%s: far draw did not leave blue' % name)

        elif kind == 'wrong_va':
            try:
                bits = int(declaration['depth_bits'], 0)
                expected = rgba8(f32_from_bits(bits))
                tolerance = int(declaration.get('tolerance', '99'))
                probe = parse_xy(declaration['probe'])
            except (KeyError, ValueError) as error:
                problems.append('%s: bad declaration (%s)' % (name, error))
                continue
            if tolerance > 1:
                problems.append('%s: tolerance exceeds the contract' % name)
            if size != width * height * 4:
                problems.append('%s: dump size mismatch' % name)
                continue
            pixel = pixel_at(data, width, probe[0], probe[1])
            if grayscale_match(pixel, expected, tolerance):
                problems.append('%s: wrong-VA reference reproduced the expectation' % name)
            if result.get('rejected') != '1' or result.get('matched_expected') != '0':
                problems.append('%s: device did not report rejection' % name)
            centre[case_id] = pixel

        elif kind == 'atlas':
            try:
                bits = int(declaration['depth_bits'], 0)
                expected = rgba8(f32_from_bits(bits))
                expected_edge = rgba8(0.75)
                tolerance = int(declaration.get('tolerance', '99'))
                left_xy = parse_xy(result['left_xy'])
                right_xy = parse_xy(result['right_xy'])
                atlas_bytes = int(result['atlas_bytes'])
            except (KeyError, ValueError) as error:
                problems.append('%s: bad declaration (%s)' % (name, error))
                continue
            if height <= 2048:
                problems.append('%s: height does not exceed row 2048' % name)
            if atlas_bytes != ATLAS_EXPECTED_BYTES or atlas_bytes > ALLOC_CAP:
                problems.append('%s: atlas bytes %d are not the bounded 12 MiB' %
                                (name, atlas_bytes))
            if int(declaration.get('bytes', '-1')) != atlas_bytes:
                problems.append('%s: declared bytes disagree with the result' % name)
            if int(declaration.get('expected8', '-1')) != expected:
                problems.append('%s: declared band expectation is not %d' %
                                (name, expected))
            if int(declaration.get('expected_edge8', '-1')) != expected_edge:
                problems.append('%s: declared atlas background is not %d' %
                                (name, expected_edge))
            if tolerance > 1:
                problems.append('%s: tolerance exceeds the contract' % name)
            if atlas_bounds is None or atlas_bounds.get('ok') != '1':
                problems.append('%s: bounds check did not pass' % name)
            if atlas_status is None or atlas_status.get('available') != '1':
                problems.append('%s: atlas was not available' % name)
            if result.get('bounds_ok') != '1':
                problems.append('%s: device bounds check did not pass' % name)
            if size != 64 * 64 * 4:
                problems.append('%s: unexpected consumer dump size %d' % (name, size))
                continue
            left = pixel_at(data, 64, *left_xy)
            right = pixel_at(data, 64, *right_xy)
            left_band = grayscale_match(left, expected, tolerance)
            left_edge = grayscale_match(left, expected_edge, tolerance)
            right_band = grayscale_match(right, expected, tolerance)
            right_edge = grayscale_match(right, expected_edge, tolerance)
            if not ((left_band and right_edge) or (left_edge and right_band)):
                problems.append('%s: samples did not cover band and background' % name)
            centre[case_id] = left

    # Declared output changes must be visible in the readback, not only in the
    # expectation table.
    for low_id, high_id in PAIR_IDS:
        if low_id in centre and high_id in centre:
            if centre[low_id][:3] == centre[high_id][:3]:
                problems.append('cases %d/%d did not change the output' %
                                (low_id, high_id))
    if 11 in hashes and 12 in hashes and hashes[11] == hashes[12]:
        problems.append('select A and select B produced the same image')
    if 11 in hashes and 13 in hashes and hashes[11] != hashes[13]:
        problems.append('select A did not return to its own image after B')
    if 9 in hashes and 10 in hashes and hashes[9] == hashes[10]:
        problems.append('depth-test far/near cases produced the same image')

    # ------------------------------------------------------------------
    # B18 production-view A/B/A rows.  These are decided from the raw dump;
    # `rgba=` in the device line is a screen-corner probe and is not used.
    # ------------------------------------------------------------------
    aba_ok = True
    aba_hashes: dict[tuple[str, int], str] = {}
    all_slices = list(case_slices)
    if aba_rows:
        if len(aba_rows) != ABA_ROWS:
            problems.append('expected %d PROD_ABA rows, found %d' %
                            (ABA_ROWS, len(aba_rows)))
            aba_ok = False
        seen_aba: dict[tuple[str, int], dict] = {}
        for row in aba_rows:
            label = row.get('label')
            phase = row['_phase']
            key = (label, phase)
            if key in seen_aba:
                problems.append('duplicate PROD_ABA row label=%r phase=%d' % key)
                aba_ok = False
                continue
            seen_aba[key] = row
        if sorted(label for label, _ in seen_aba) != ['df32', 'df32', 'df32',
                                                       's8d24', 's8d24', 's8d24']:
            problems.append('PROD_ABA labels are not exactly df32/s8d24 x3')
            aba_ok = False
        for label in ABA_LABELS:
            for phase, bits, expected8 in ABA_PHASES:
                row = seen_aba.get((label, phase))
                name = 'aba_%s_phase%d' % (label, phase)
                if row is None:
                    problems.append('%s: missing PROD_ABA row' % name)
                    aba_ok = False
                    continue
                if int(row.get('bits', '0'), 0) != bits:
                    problems.append('%s: bits %r != 0x%08x' %
                                    (name, row.get('bits'), bits))
                    aba_ok = False
                if row['_expected'] != expected8:
                    problems.append('%s: expected8 %d != %d' %
                                    (name, row['_expected'], expected8))
                    aba_ok = False
                if int(row.get('expected_edge8', '-1')) != rgba8(0.75):
                    problems.append('%s: edge expectation is not the 0.75 clear' % name)
                    aba_ok = False
                if row.get('status') != 'PASS' or row.get('executed') != '1' or \
                        row.get('readback') != '1':
                    problems.append('%s: device did not report an executed readback' % name)
                    aba_ok = False
                if row.get('region_bad') != '0' or row.get('edge_bad') != '0':
                    problems.append('%s: device region/edge counts disagree' % name)
                    aba_ok = False
                offset = row['_offset']
                size = ABA_DUMP_BYTES
                if offset < 0 or offset + size > len(dump):
                    problems.append('%s: dump slice %d+%d outside %d bytes' %
                                    (name, offset, size, len(dump)))
                    aba_ok = False
                    continue
                all_slices.append((name, offset, size))
                data = dump[offset:offset + size]
                aba_hashes[(label, phase)] = sha256(data)
                if size != 64 * 64 * 4:
                    problems.append('%s: unexpected dump size %d' % (name, size))
                    aba_ok = False
                    continue
                inside, outside, bad_inside, bad_outside = scan_regions(
                    data, 64, 64, (16, 16, 47, 47), expected8, rgba8(0.75), 1)
                if inside == 0 or outside == 0 or bad_inside or bad_outside:
                    problems.append('%s: interior_bad=%d edge_bad=%d' %
                                    (name, bad_inside, bad_outside))
                    aba_ok = False
        for label in ABA_LABELS:
            h0 = aba_hashes.get((label, 0))
            h1 = aba_hashes.get((label, 1))
            h2 = aba_hashes.get((label, 2))
            if h0 is not None and h1 is not None and h0 == h1:
                problems.append('%s A/B/A did not change the output' % label)
                aba_ok = False
            if h0 is not None and h2 is not None and h0 != h2:
                problems.append('%s A/B/A did not return to A' % label)
                aba_ok = False

    # ------------------------------------------------------------------
    # B19 clear-materialization rows: the empty-scene native clear is the only
    # producer; the sample program consumes the same native allocation.
    # ------------------------------------------------------------------
    clear_ok = True
    clear_hashes: dict[int, str] = {}
    if clear_decls or clear_results:
        expected_clear = {row[0]: row for row in CLEAR_ABA + CLEAR_DRAW}
        expected_ids = set(expected_clear) | {CLEAR_ATLAS[0]}
        if set(clear_decls) != expected_ids:
            problems.append('clear case set is not the fixed 9-case contract: %s' %
                            sorted(clear_decls))
            clear_ok = False
        for clear_id, name, label, phase, bits, expected8 in CLEAR_ABA:
            declaration = clear_decls.get(clear_id)
            result = clear_results.get(clear_id)
            if declaration is None or result is None:
                problems.append('%s: missing clear declaration/result' % name)
                clear_ok = False
                continue
            if declaration.get('name') != name or declaration.get('kind') != 'clear_aba':
                problems.append('%s: declaration name/kind mismatch' % name)
                clear_ok = False
            if int(declaration.get('clear_bits', '0'), 0) != bits:
                problems.append('%s: clear_bits %r != 0x%08x' %
                                (name, declaration.get('clear_bits'), bits))
                clear_ok = False
            if int(declaration.get('expected8', '-1')) != expected8:
                problems.append('%s: declared expected8 is not %d' %
                                (name, expected8))
                clear_ok = False
            if result.get('status') != 'PASS' or result.get('executed') != '1' or \
                    result.get('readback') != '1':
                problems.append('%s: clear was not executed/read back' % name)
                clear_ok = False
            try:
                offset = int(result['dump_offset'])
                size = int(result['dump_bytes'])
            except (KeyError, ValueError):
                problems.append('%s: missing dump offset/size' % name)
                clear_ok = False
                continue
            if size != CLEAR_DUMP_BYTES or offset < 0 or offset + size > len(dump):
                problems.append('%s: dump slice %d+%d is not %d bytes' %
                                (name, offset, size, CLEAR_DUMP_BYTES))
                clear_ok = False
                continue
            all_slices.append((name, offset, size))
            data = dump[offset:offset + size]
            clear_hashes[clear_id] = sha256(data)
            bad = 0
            for index in range(0, len(data), 4):
                pixel = tuple(data[index:index + 4])
                if not grayscale_match(pixel, expected8, 1):
                    bad += 1
            if bad or int(result.get('all_bad', '-1')) != 0:
                problems.append('%s: %d pixels differ from the clear value (device all_bad=%r)' %
                                (name, bad, result.get('all_bad')))
                clear_ok = False
            if int(result.get('count', '-1')) != 64 * 64:
                problems.append('%s: device count is not the full 64x64 surface' % name)
                clear_ok = False
        # A/B/A: different addresses for B, identical address back to A, and
        # the raw image must follow 0.25 -> 0.625 -> 0.25.
        for prefix, ids in (('df32', (101, 102, 103)), ('s8d24', (104, 105, 106))):
            decl_a = clear_decls.get(ids[0], {})
            decl_b = clear_decls.get(ids[1], {})
            decl_c = clear_decls.get(ids[2], {})
            addr_a = int(decl_a.get('address', '0'), 0)
            addr_b = int(decl_b.get('address', '0'), 0)
            addr_c = int(decl_c.get('address', '0'), 0)
            if addr_a == 0 or addr_b == 0 or addr_a == addr_b:
                problems.append('%s clear A/B/A does not use two distinct addresses' % prefix)
                clear_ok = False
            if addr_a != addr_c:
                problems.append('%s clear A/B/A did not return to the A address' % prefix)
                clear_ok = False
            h0 = clear_hashes.get(ids[0])
            h1 = clear_hashes.get(ids[1])
            h2 = clear_hashes.get(ids[2])
            if h0 is not None and h1 is not None and h0 == h1:
                problems.append('%s clear A/B/A did not change the output' % prefix)
                clear_ok = False
            if h0 is not None and h2 is not None and h0 != h2:
                problems.append('%s clear A/B/A did not return to A' % prefix)
                clear_ok = False
        for clear_id, name, label in CLEAR_DRAW:
            declaration = clear_decls.get(clear_id)
            result = clear_results.get(clear_id)
            if declaration is None or result is None:
                problems.append('%s: missing clear-draw declaration/result' % name)
                clear_ok = False
                continue
            if declaration.get('kind') != 'clear_draw' or declaration.get('name') != name:
                problems.append('%s: declaration name/kind mismatch' % name)
                clear_ok = False
            try:
                region = parse_region(declaration['region'])
                offset = int(result['dump_offset'])
                size = int(result['dump_bytes'])
            except (KeyError, ValueError) as error:
                problems.append('%s: bad clear-draw declaration (%s)' % (name, error))
                clear_ok = False
                continue
            if size != CLEAR_DUMP_BYTES or offset < 0 or offset + size > len(dump):
                problems.append('%s: dump slice is not the bounded 64x64 consumer' % name)
                clear_ok = False
                continue
            all_slices.append((name, offset, size))
            data = dump[offset:offset + size]
            inside, outside, bad_inside, bad_outside = scan_regions(
                data, 64, 64, region, 64, rgba8(0.75), 1)
            if inside == 0 or outside == 0 or bad_inside or bad_outside:
                problems.append('%s: clear lost outside the drawn centre '
                                '(interior_bad=%d edge_bad=%d)' %
                                (name, bad_inside, bad_outside))
                clear_ok = False
            if int(result.get('region_bad', '-1')) != bad_inside or \
                    int(result.get('edge_bad', '-1')) != bad_outside:
                problems.append('%s: device counts disagree with the raw dump' % name)
                clear_ok = False
        clear_atlas_id, clear_atlas_name = CLEAR_ATLAS
        declaration = clear_decls.get(clear_atlas_id)
        result = clear_results.get(clear_atlas_id)
        if declaration is None or result is None:
            problems.append('%s: missing atlas clear declaration/result' % clear_atlas_name)
            clear_ok = False
        else:
            if declaration.get('kind') != 'clear_atlas' or \
                    declaration.get('name') != clear_atlas_name:
                problems.append('%s: declaration name/kind mismatch' % clear_atlas_name)
                clear_ok = False
            if int(declaration.get('width', '0')) != 1024 or \
                    int(declaration.get('height', '0')) != 3072:
                problems.append('%s: clear atlas is not 1024x3072' % clear_atlas_name)
                clear_ok = False
            try:
                expected = int(declaration['expected8'])
                expected_edge = int(declaration['expected_edge8'])
                left_xy = parse_xy(result['left_xy'])
                right_xy = parse_xy(result['right_xy'])
                atlas_bytes = int(result['atlas_bytes'])
                offset = int(result['dump_offset'])
                size = int(result['dump_bytes'])
            except (KeyError, ValueError) as error:
                problems.append('%s: bad atlas clear declaration (%s)' %
                                (clear_atlas_name, error))
                clear_ok = False
                expected = expected_edge = -1
                left_xy = right_xy = (0, 0)
                atlas_bytes = -1
                offset = size = -1
            if expected != rgba8(0.3) or expected_edge != rgba8(0.75):
                problems.append('%s: atlas band/background expectation is not the '
                                'independent D24 rule' % clear_atlas_name)
                clear_ok = False
            if atlas_bytes != ATLAS_EXPECTED_BYTES or atlas_bytes > ALLOC_CAP:
                problems.append('%s: atlas bytes %d are not the bounded 12 MiB' %
                                (clear_atlas_name, atlas_bytes))
                clear_ok = False
            if size != CLEAR_DUMP_BYTES or offset < 0 or offset + size > len(dump):
                problems.append('%s: dump slice is not the bounded 64x64 consumer' %
                                clear_atlas_name)
                clear_ok = False
            else:
                all_slices.append((clear_atlas_name, offset, size))
                data = dump[offset:offset + size]
                left = pixel_at(data, 64, *left_xy)
                right = pixel_at(data, 64, *right_xy)
                left_band = grayscale_match(left, expected, 1)
                left_edge = grayscale_match(left, expected_edge, 1)
                right_band = grayscale_match(right, expected, 1)
                right_edge = grayscale_match(right, expected_edge, 1)
                if not ((left_band and right_edge) or (left_edge and right_band)):
                    problems.append('%s: samples did not cover band and background' %
                                    clear_atlas_name)
                    clear_ok = False
            if result.get('pair_ok') != '1' or result.get('bounds_ok') != '1':
                problems.append('%s: device pair/bounds check did not pass' %
                                clear_atlas_name)
                clear_ok = False
            if atlas_bounds is None or atlas_bounds.get('ok') != '1':
                problems.append('%s: atlas bounds were not checked' % clear_atlas_name)
                clear_ok = False

    # ------------------------------------------------------------------
    # Strict payload layout: every declared readback owns a unique slice and
    # the slices must tile the whole file with no hole or overlap.  The total
    # is derived from the fixed contract, not from the device's claim.
    # ------------------------------------------------------------------
    layout_ok = True
    expected_payload = 0
    for case_id, (name, kind) in EXPECTED_CASES.items():
        declaration = cases.get(case_id)
        if not declaration:
            continue
        try:
            width = int(declaration['width'])
            height = int(declaration['height'])
        except (KeyError, ValueError):
            continue
        expected_payload += 64 * 64 * 4 if kind == 'atlas' else width * height * 4
    if aba_rows:
        expected_payload += ABA_ROWS * ABA_DUMP_BYTES
    if clear_decls:
        expected_payload += CLEAR_ROWS * CLEAR_DUMP_BYTES
    if len(dump) != expected_payload:
        problems.append('payload is %d bytes, contract expects %d' %
                        (len(dump), expected_payload))
        layout_ok = False
    by_offset: dict[int, tuple[str, int]] = {}
    for name, offset, size in all_slices:
        if offset in by_offset:
            problems.append('%s and %s share dump offset %d' %
                            (by_offset[offset][0], name, offset))
            layout_ok = False
            continue
        by_offset[offset] = (name, size)
    cursor = 0
    for offset in sorted(by_offset):
        name, size = by_offset[offset]
        if offset != cursor:
            problems.append('%s: %s dump starts at %d, expected %d (hole/overlap)' %
                            ('layout', name, offset, cursor))
            layout_ok = False
        if offset not in dumps:
            problems.append('%s: no DUMP line for offset %d' % (name, offset))
            layout_ok = False
        elif dumps[offset] != size:
            problems.append('%s: DUMP bytes %d != declared slice %d' %
                            (name, dumps[offset], size))
            layout_ok = False
        cursor = offset + size
    if cursor != len(dump):
        problems.append('dump slices cover %d of %d bytes' % (cursor, len(dump)))
        layout_ok = False
    for offset, size in dumps.items():
        if offset not in by_offset:
            problems.append('DUMP at %d (%d bytes) has no case result' % (offset, size))
            layout_ok = False

    if sentinel is None:
        problems.append('missing SENTINEL_CHECK')
    else:
        if sentinel.get('unchanged') != '1':
            problems.append('colour sentinel changed across the no-colour passes')
        if sentinel.get('before') != sentinel.get('after'):
            problems.append('colour sentinel hash mismatch')
        if sentinel.get('bound_as_producer') != '0':
            problems.append('colour sentinel was bound as a producer surface')

    if verdict != 'COMPLETE':
        problems.append('RESULT is %r, not COMPLETE' % verdict)

    summary = {
        'schema': 2,
        'task': 'M6-B15/B18/B19',
        'log': str(log_path),
        'dump': str(dump_path),
        'log_present': log_present,
        'dump_present': dump_present,
        'log_bytes': len(log_data) if log_data is not None else 0,
        'dump_bytes': len(dump),
        'payload_contract_bytes': expected_payload,
        'layout_ok': layout_ok,
        'verdict': verdict,
        'complete': not problems,
        'problems': problems,
        'cases_declared': len(cases),
        'case_results': len(results),
        'aba_rows': len(aba_rows),
        'aba_ok': aba_ok,
        'aba_hashes': {'%s_phase%d' % key: value
                       for key, value in sorted(aba_hashes.items())},
        'clear_rows': len(clear_decls),
        'clear_ok': clear_ok,
        'clear_hashes': {str(key): value
                         for key, value in sorted(clear_hashes.items())},
        'case_hashes': {EXPECTED_CASES[i][0]: hashes[i] for i in sorted(hashes)},
        'precision_limit': 'locked OpenGL backend stores GL_DEPTH24_STENCIL8 even for '
                           'native DF32; checks use one 8-bit LSB tolerance, not F32 '
                           'hardware equivalence',
        'atlas_depth_bytes': ATLAS_EXPECTED_BYTES,
        'atlas_alloc_cap': ALLOC_CAP,
        'target_content_tested': True,
        'target_content_proven': not problems,
        'hardware_verified': False,
        'production_modified': False,
        'village_rendered': False,
    }
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--dump', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    arguments = parser.parse_args()
    summary = verify(arguments.log, arguments.dump)
    if arguments.report:
        arguments.report.parent.mkdir(parents=True, exist_ok=True)
        arguments.report.write_text(json.dumps(summary, indent=2) + '\n',
                                    encoding='utf-8', newline='\n')
    print(json.dumps(summary, indent=2))
    return 0 if summary['complete'] else 1


if __name__ == '__main__':
    sys.exit(main())

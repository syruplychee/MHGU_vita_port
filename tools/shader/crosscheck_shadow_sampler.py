"""Cross-check the C shadow TSC planner against the Python policy.

The C implementation is compiled from src/platform/gxm_shadow_sampler.c; this
tool asks it to decode a generated matrix (all compare functions, both filters,
all four wrap modes, boundary values and every named refusal) and compares the
results with tools/shader/shadow_helpers.py.  Known field values are also
asserted literally, so two mechanically identical decoders cannot self-certify.

Run it from Windows with the repository Python, passing the WSL path of a Linux
test binary:

    python tools/shader/crosscheck_shadow_sampler.py \\
        --binary /mnt/c/.../build/host-tests/m6b20/shadow-sampler-test \\
        --work artifacts/m6b20-shadow-binding-20261027

The binary must support ``--crosscheck <tsv>`` (tests/gxm_shadow_sampler.c).
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shadow_helpers


def f32_bits(value: float) -> int:
    return struct.unpack('<I', struct.pack('<f', float(value)))[0]


COMPARE = 1
WRAP = 0
FILTER = 1
MIP = 1


def tsc(func=COMPARE, filt=FILTER, wu=WRAP, wv=0, border=0.0,
        w0_extra=0, w1_extra=0, w2=0, w3=0, w4=None):
    words = [0] * 8
    words[0] = (wu & 7) | ((wv & 7) << 3) | (1 << 9) | ((func & 7) << 10) | w0_extra
    words[1] = (filt & 3) | ((filt & 3) << 4) | (MIP << 6) | w1_extra
    words[2] = w2
    words[3] = w3
    words[4] = f32_bits(border) if w4 is None else w4
    return words


CASES = []


def mutated(words, index, value):
    out = list(words)
    out[index] = value
    return out


def add(name, words, *, texture_type=2, depth=1, max_mip=0, width=64, height=64,
        expected_status=None, expected_code=None, known=None):
    CASES.append({
        'name': name,
        'words': list(words),
        'texture_type': texture_type,
        'depth': depth,
        'max_mip': max_mip,
        'width': width,
        'height': height,
        'expected_status': expected_status,
        'expected_code': expected_code,
        'known': known or {},
    })


def build_cases():
    for func in range(8):
        for filt in (1, 2):
            for wu in range(4):
                for wv in range(4):
                    border = 0.25 if (wu == 3 or wv == 3) else 0.0
                    add('ok_f%d_t%d_u%d_v%d' % (func, filt, wu, wv),
                        tsc(func, filt, wu, wv, border))
    add('known_layout', tsc(6, 2, 3, 1, 0.75), width=128, height=64,
        known={'compare_func': 6, 'maxwell_filter': 2, 'filter_code': 1,
               'wrap_u': 3, 'wrap_v': 1, 'border_used': 1,
               'extent': [128.0, 64.0, 1.0 / 128.0, 1.0 / 64.0],
               'state': [6.0, 1.0, 3.0, 1.0], 'border': [0.75, 0.0, 0.0, 0.0]})

    add('reject_type_1', tsc(), texture_type=1, expected_status=-2,
        expected_code='texture-type-not-2d')
    add('reject_type_0', tsc(), texture_type=0, expected_status=-2,
        expected_code='texture-type-not-2d')
    add('reject_depth_0', tsc(), depth=0, expected_status=-3,
        expected_code='texture-not-depth')
    add('reject_depth_2', tsc(), depth=2, expected_status=-3,
        expected_code='texture-not-depth')
    add('reject_compare_off', mutated(tsc(), 0, tsc()[0] & ~(1 << 9)),
        expected_status=-4, expected_code='compare-disabled')
    for wu in range(4, 8):
        add('reject_wrap_u_%d' % wu, tsc(wu=wu), expected_status=-5,
            expected_code='wrap-mode-unsupported')
    for wv in range(4, 8):
        add('reject_wrap_v_%d' % wv, tsc(wv=wv), expected_status=-6,
            expected_code='wrap-mode-unsupported')
    add('reject_mag_0', mutated(tsc(filt=1), 1, tsc(filt=1)[1] & ~3),
        expected_status=-7, expected_code='filter-enum-unsupported')
    add('reject_mag_3', mutated(tsc(filt=1), 1, (tsc(filt=1)[1] & ~3) | 3),
        expected_status=-7, expected_code='filter-enum-unsupported')
    add('reject_min_0',
        mutated(tsc(filt=1), 1, tsc(filt=1)[1] & ~(3 << 4)),
        expected_status=-8, expected_code='filter-enum-unsupported')
    add('reject_min_3',
        mutated(tsc(filt=1), 1, (tsc(filt=1)[1] & ~(3 << 4)) | (3 << 4)),
        expected_status=-8, expected_code='filter-enum-unsupported')
    add('reject_mismatch', mutated(tsc(filt=1), 1, 1 | (2 << 4) | (1 << 6)),
        expected_status=-9, expected_code='filter-mismatch')
    add('reject_mip_filter_0', mutated(tsc(), 1, tsc()[1] & ~(3 << 6)),
        expected_status=-10, expected_code='mipmap-filter-unsupported')
    add('reject_mip_unknown', tsc(), max_mip=-1, expected_status=-11,
        expected_code='mip-level-unknown')
    add('reject_mip_present', tsc(), max_mip=1, expected_status=-12,
        expected_code='mip-level-present')
    add('reject_reduction', tsc(w1_extra=1 << 10), expected_status=-13,
        expected_code='reduction-unsupported')
    add('reject_anisotropy', tsc(w0_extra=1 << 20), expected_status=-14,
        expected_code='anisotropy-unsupported')
    # policy-2: cubemap-only bits on a 2D depth sampler are inert.
    add('accept_cubemap_aniso', tsc(w1_extra=1 << 8))
    add('accept_cubemap_interface', tsc(w1_extra=1 << 9))
    add('reject_lod_bias_pos', tsc(w1_extra=1 << 12), expected_status=-17,
        expected_code='lod-bias-unsupported')
    add('reject_lod_bias_neg', tsc(w1_extra=0x1fff << 12),
        expected_status=-17, expected_code='lod-bias-unsupported')
    add('reject_float_coord', tsc(w1_extra=1 << 25), expected_status=-18,
        expected_code='float-coord-normalization-unsupported')
    add('reject_trilinear', tsc(w1_extra=1 << 26), expected_status=-19,
        expected_code='trilinear-optimization-unsupported')
    add('reject_min_lod', mutated(tsc(), 2, 1), expected_status=-20,
        expected_code='min-lod-clamp-unsupported')
    # policy-2: sRGB conversion is inert for a depth-format TIC.
    add('accept_srgb', tsc(w0_extra=1 << 13))
    add('reject_border_nan', tsc(wu=3, border=0.0, w4=0x7fc00000),
        expected_status=-22, expected_code='border-depth-nonfinite')
    add('reject_border_inf', tsc(wv=3, border=0.0, w4=0x7f800000),
        expected_status=-22, expected_code='border-depth-nonfinite')
    add('reject_border_high', tsc(wu=3, border=1.5), expected_status=-23,
        expected_code='border-depth-out-of-range')
    add('reject_border_low', tsc(wu=3, border=-0.25), expected_status=-23,
        expected_code='border-depth-out-of-range')
    add('accept_border_nan_unused', tsc(w4=0x7fc00000))
    add('accept_border_high_unused', tsc(border=2.0))
    # Extent refusal is C/DEPTH_VIEW-specific; the Python TSC policy does not
    # receive an extent.  It is covered by tests/gxm_shadow_sampler.c.


def wsl_path(path: Path) -> str:
    text = str(path.resolve())
    if len(text) >= 2 and text[1] == ':':
        drive = text[0].lower()
        return '/mnt/%s/%s' % (drive, text[3:].replace('\\', '/'))
    return text.replace('\\', '/')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',
                        help='WSL path to the compiled C test binary')
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--json', type=Path)
    parser.add_argument('--emit-tsv', action='store_true',
                        help='only write the generated case TSV')
    parser.add_argument('--c-output', type=Path,
                        help='read C output produced separately instead of '
                             'launching WSL')
    args = parser.parse_args()

    build_cases()
    args.work.mkdir(parents=True, exist_ok=True)
    tsv = args.work / 'shadow-cases.tsv'
    lines = []
    for case in CASES:
        lines.append('%s %s %u %u %d %u %u' % (
            case['name'], ' '.join('%u' % word for word in case['words']),
            case['texture_type'], case['depth'], case['max_mip'],
            case['width'], case['height']))
    tsv.write_text('\n'.join(lines) + '\n', encoding='ascii', newline='\n')
    if args.emit_tsv:
        print(str(tsv))
        return 0

    if args.c_output:
        text = args.c_output.read_text(encoding='ascii')
    else:
        if not args.binary:
            raise SystemExit('either --binary or --c-output is required')
        proc = subprocess.run(
            ['wsl', '-e', args.binary, '--crosscheck', wsl_path(tsv)],
            capture_output=True, text=True)
        if proc.returncode != 0:
            print(proc.stdout)
            print(proc.stderr, file=sys.stderr)
            raise SystemExit('C cross-check binary failed: %d'
                             % proc.returncode)
        text = proc.stdout

    actual = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) != 15:
            raise SystemExit('bad C output line: %r' % line)
        name = fields[0]
        actual[name] = {
            'status': int(fields[1]),
            'compare_func': int(fields[2]),
            'maxwell_filter': int(fields[3]),
            'filter_code': int(fields[4]),
            'wrap_u': int(fields[5]),
            'wrap_v': int(fields[6]),
            'border_used': int(fields[7]),
            'extent': [float.fromhex(fields[8]), float.fromhex(fields[9])],
            'state': [float.fromhex(fields[10]), float.fromhex(fields[11]),
                      float.fromhex(fields[12]), float.fromhex(fields[13])],
            'border': [float.fromhex(fields[14])],
        }

    problems = []
    checked = 0
    for case in CASES:
        name = case['name']
        checked += 1
        if name not in actual:
            problems.append('%s: missing C result' % name)
            continue
        c = actual[name]
        max_mip = None if case['max_mip'] < 0 else case['max_mip']
        py = shadow_helpers.plan_tsc_shadow(
            case['words'], case['texture_type'], case['depth'], max_mip)
        if case['expected_status'] is None:
            if not py['supported']:
                problems.append('%s: Python refused but expected accepted: %r'
                                % (name, py['rejections']))
                continue
            if c['status'] != 0:
                problems.append('%s: C status %d, expected 0'
                                % (name, c['status']))
                continue
            expected = {
                'compare_func': py['compare_func'],
                'maxwell_filter': 1 if py['filter'] == 'point' else 2,
                'filter_code': 0 if py['filter'] == 'point' else 1,
                'wrap_u': py['wrap_u'],
                'wrap_v': py['wrap_v'],
                'border_used': 1 if py['border_used'] else 0,
                'extent': [float(case['width']), float(case['height'])],
                'state': [float(py['compare_func']),
                          0.0 if py['filter'] == 'point' else 1.0,
                          float(py['wrap_u']), float(py['wrap_v'])],
                'border': [py['border_depth'] if py['border_used'] else 0.0],
            }
            for key, value in expected.items():
                if key == 'extent':
                    if c['extent'] != value[:2]:
                        problems.append('%s: extent %r != %r'
                                        % (name, c['extent'], value[:2]))
                elif key == 'state':
                    if c['state'] != value:
                        problems.append('%s: state %r != %r'
                                        % (name, c['state'], value))
                elif key == 'border':
                    if c['border'] != value:
                        problems.append('%s: border %r != %r'
                                        % (name, c['border'], value))
                elif c[key] != value:
                    problems.append('%s: %s %r != %r'
                                    % (name, key, c[key], value))
            for key, value in case['known'].items():
                if key in ('extent', 'state', 'border'):
                    if c[key] != value[:len(c[key])]:
                        problems.append('%s: known %s %r != %r'
                                        % (name, key, c[key], value))
                elif c[key] != value:
                    problems.append('%s: known %s %r != %r'
                                    % (name, key, c[key], value))
        else:
            if py['supported']:
                problems.append('%s: Python accepted but expected refusal'
                                % name)
                continue
            codes = [item['code'] for item in py['rejections']]
            if case['expected_code'] is not None and \
                    case['expected_code'] not in codes:
                problems.append('%s: Python codes %r missing %s'
                                % (name, codes, case['expected_code']))
            if c['status'] != case['expected_status']:
                problems.append('%s: C status %d != %d'
                                % (name, c['status'],
                                   case['expected_status']))

    summary = {
        'cases': len(CASES),
        'checked': checked,
        'c_output_lines': len(actual),
        'problems': problems,
        'python_policy_sha256': shadow_helpers.policy_sha256(),
        'python_helper_sha256': shadow_helpers.helper_source_sha256(),
        'passed': not problems,
    }
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(summary, indent=2) + '\n',
                             encoding='utf-8', newline='\n')
    print(json.dumps(summary, indent=2))
    if problems:
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

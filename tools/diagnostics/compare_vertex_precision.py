"""Compare captured GPU clip positions before/after lossless vertex uploads.

The original root contains gxp32.bin and gxp16.bin. The candidate root contains
ordinary.bin (RGBA32F) and packed.bin (two RG32F texels). Both roots have the
same cases.txt and per-draw meta.txt. No game data is included in this tool.
Exit 0: exact candidate positions, 1: changed positions, 2: invalid evidence.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import sys


def positions(path, count):
    data = path.read_bytes()
    if len(data) != count * 16:
        raise ValueError(f'{path}: clip position count differs from metadata')
    result = list(struct.iter_unpack('<4f', data))
    if not all(math.isfinite(x) for point in result for x in point):
        raise ValueError(f'{path}: nonfinite clip position')
    if not any(point[3] > 0 for point in result):
        raise ValueError(f'{path}: no vertex has positive W')
    return result


def screen_error(a, b):
    if a[3] <= 0 or b[3] <= 0:
        return None
    return math.hypot((a[0] / a[3] - b[0] / b[3]) * 480,
                      (a[1] / a[3] - b[1] / b[3]) * 270)


def compare(original, candidate):
    names = (candidate / 'cases.txt').read_text(encoding='utf-8').splitlines()
    if not names or len(set(names)) != len(names):
        raise ValueError('empty or duplicate candidate cases')
    if names != (original / 'cases.txt').read_text(encoding='utf-8').splitlines():
        raise ValueError('original and candidate case lists differ')
    rows = []
    for name in names:
        if not name or name in ('.', '..') or any(c in name for c in '/\\:'):
            raise ValueError('invalid draw directory name')
        old, new = original / name, candidate / name
        meta = (new / 'meta.txt').read_bytes()
        if meta != (old / 'meta.txt').read_bytes():
            raise ValueError(f'{name}: metadata differs')
        fields = meta.split()
        if len(fields) != 4 or not all(int(x) >= 0 for x in fields):
            raise ValueError(f'{name}: invalid metadata')
        count = int(fields[0])
        if count == 0:
            raise ValueError(f'{name}: no vertices')
        full = positions(old / 'gxp32.bin', count)
        half = positions(old / 'gxp16.bin', count)
        ordinary = positions(new / 'ordinary.bin', count)
        packed = positions(new / 'packed.bin', count)
        baseline_different = sum(a != b for x, y in zip(full, packed) for a, b in zip(x, y))
        layout_different = sum(a != b for x, y in zip(ordinary, packed) for a, b in zip(x, y))
        errors = [screen_error(a, b) for a, b in zip(full, packed)]
        half_errors = [screen_error(a, b) for a, b in zip(full, half)]
        errors = [x for x in errors if x is not None]
        half_errors = [x for x in half_errors if x is not None]
        if not errors or not half_errors:
            raise ValueError(f'{name}: no comparable projected vertices')
        rows.append({'draw': name, 'vertices': count,
                     'baseline_different_floats': baseline_different,
                     'layout_different_floats': layout_different,
                     'max_pixels': max(errors), 'old_half_max_pixels': max(half_errors)})
    return {'draws': len(rows), 'vertices': sum(x['vertices'] for x in rows),
            'baseline_different_floats': sum(x['baseline_different_floats'] for x in rows),
            'layout_different_floats': sum(x['layout_different_floats'] for x in rows),
            'max_pixels': max(x['max_pixels'] for x in rows),
            'old_half_max_pixels': max(x['old_half_max_pixels'] for x in rows), 'rows': rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        result = compare(args.original, args.candidate)
        text = json.dumps(result, indent=2) + '\n'
        if args.output:
            args.output.write_text(text, encoding='utf-8')
        print(json.dumps({k: v for k, v in result.items() if k != 'rows'}))
        return int(bool(result['baseline_different_floats'] or result['layout_different_floats']))
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())

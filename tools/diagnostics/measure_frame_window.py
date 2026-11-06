"""Measure a stable scene window from cumulative whole-frame interval reports.

Reports measure intervals between displayed frames, not time spent inside the
Present function. Quantiles are 1 ms ranges; bucket 511 includes all outliers.
No boot average or single title-bar FPS is substituted for missing evidence.
"""
import argparse
import json
import math
from pathlib import Path
import re
import sys

PATTERN = re.compile(r'VP_GXM_FRAME_INTERVALS t_us=(\d+) count=(\d+) sum_us=(\d+) width_us=1000\n')
BUCKETS = re.compile(r'VP_GXM_FRAME_BUCKETS t_us=(\d+) offset=(\d+) hist=([0-9,]+)\n')


def quantile(histogram, fraction):
    rank = math.ceil(sum(histogram) * fraction)
    running = 0
    for i, value in enumerate(histogram):
        running += value
        if running >= rank:
            return {'min_ms': i, 'max_ms': i + 1 if i < 511 else None}
    raise ValueError('empty frame histogram')


def measure(run, start, end):
    life = json.loads((run / 'process-lifecycle.json').read_text(encoding='utf-8-sig'))
    saves = json.loads((run / 'save-protection.json').read_text(encoding='utf-8-sig'))
    if (life['exit_code'] != 0 or life['classification'] != 'exited' or
            life.get('caller_stop') or life.get('crash_suspected') or
            not saves['verified'] or not saves['markers_verified']):
        raise ValueError(f'{run}: run/restore did not pass')
    text = (run / 'gxm-runtime.log').read_text(encoding='utf-8')
    chunks = {}
    for match in BUCKETS.finditer(text):
        t, offset, raw = match.groups()
        if not start * 1000000 <= int(t) <= end * 1000000:
            continue
        values = [int(x) for x in raw.split(',')]
        offset = int(offset)
        if len(values) != 64 or offset not in range(0, 512, 64):
            raise ValueError('invalid histogram chunk')
        row_chunks = chunks.setdefault(int(t), {})
        if offset in row_chunks:
            raise ValueError('duplicate histogram chunk')
        row_chunks[offset] = values
    rows = []
    for match in PATTERN.finditer(text):
        t, count, total = map(int, match.groups())
        if not start * 1000000 <= t <= end * 1000000:
            continue
        row_chunks = chunks.get(t, {})
        if len(row_chunks) != 8:
            raise ValueError('incomplete frame histogram')
        histogram = [x for offset in range(0, 512, 64) for x in row_chunks[offset]]
        row = {'t': t, 'count': count, 'sum': total, 'hist': histogram}
        if len(histogram) != 512 or sum(histogram) != row['count']:
            raise ValueError('invalid cumulative histogram')
        if start * 1000000 <= row['t'] <= end * 1000000:
            rows.append(row)
    if len(rows) < 2:
        raise ValueError(f'{run}: stable window reports missing')
    a, b = rows[0], rows[-1]
    samples = b['count'] - a['count']
    elapsed = b['sum'] - a['sum']
    histogram = [y - x for x, y in zip(a['hist'], b['hist'])]
    if samples < 128 or sum(histogram) != samples or min(histogram) < 0 or elapsed <= 0:
        raise ValueError('insufficient/inconsistent interval samples')
    if elapsed != b['t'] - a['t']:
        raise ValueError('interval sum differs from the observed frame timestamps')
    return {'run': str(run), 'start_s': a['t'] / 1000000, 'end_s': b['t'] / 1000000,
            'frames': samples, 'mean_ms': elapsed / samples / 1000,
            'fps': samples * 1000000 / elapsed, 'p50': quantile(histogram, .5),
            'p95': quantile(histogram, .95), 'histogram': histogram,
            'scope': 'scene identity requires visual review; no hardware or speedup significance claim'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runs', type=Path, nargs='+')
    parser.add_argument('--start', type=float, default=230)
    parser.add_argument('--end', type=float, default=290)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        if not 0 <= args.start < args.end:
            raise ValueError('invalid time window')
        rows = [measure(run, args.start, args.end) for run in args.runs]
        args.output.write_text(json.dumps(rows, indent=2) + '\n', encoding='utf-8')
        for row in rows:
            print(json.dumps({k: v for k, v in row.items() if k != 'histogram'}))
        return 0
    except (OSError, ValueError, KeyError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())

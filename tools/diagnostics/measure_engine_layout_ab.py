"""Measure alternating layout paths within one protected Vita3K run.

Use with make_engine_layout_ab.py. Ignore 64 displayed frames at each phase
boundary; keep 256 frame intervals per phase. Scene identity needs visual review.
"""
import argparse
import json
from pathlib import Path
import re

from measure_frame_window import PATTERN, measure, quantile

PHASE = re.compile(r'ENGINE_LAYOUT_AB_PHASE frame=(\d+) start=(\d+) phase=(\d+) enabled=(\d+) t_us=(\d+)')
COUNTER = re.compile(r'ENGINE_LAYOUT_AB_COUNT entering_mode=(\d+)\s+'
                     r'ENGINE_LAYOUT_STATS ([^\n]+)')


def measure_ab(run):
    text = (run / 'gxm-runtime.log').read_text(encoding='utf-8')
    phases = [tuple(map(int, row)) for row in PHASE.findall(text)]
    if len(phases) != 5:
        raise ValueError('four complete A/B phases and the final boundary are required')
    first = phases[0][0]
    for i, (frame, start, phase, enabled, _) in enumerate(phases):
        if (frame != first + i * 384 or start != first or phase != i or
                enabled != (1 if i == 4 else i % 2)):
            raise ValueError('unexpected A/B phase order or length')
    headers = [tuple(map(int, row)) for row in PATTERN.findall(text)]
    rows = []
    for frame, _, phase, enabled, _ in phases[:4]:
        window = [row for row in headers if frame + 64 <= row[1] + 1 <= frame + 320]
        if len(window) != 9:
            raise ValueError('phase histogram reports missing')
        result = measure(run, (window[0][0] - 1) / 1e6, (window[-1][0] + 1) / 1e6)
        if result['frames'] != 256:
            raise ValueError('phase must contain exactly 256 measured intervals')
        result.update(phase=phase, enabled=enabled)
        rows.append(result)
    raw_counts = COUNTER.findall((run / 'engine-layout-ab.log').read_text(encoding='utf-8'))
    if len(raw_counts) != 5:
        raise ValueError('A/B producer snapshots missing')
    counts = []
    for i, (mode, raw) in enumerate(raw_counts):
        snapshot = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', raw)}
        if int(mode) != phases[i][3] or snapshot['rejected'] or snapshot['full'] or snapshot['other_calls']:
            raise ValueError('A/B path or record validation did not pass')
        counts.append(snapshot)
    for i in range(4):
        hits = counts[i + 1]['hits'] - counts[i]['hits']
        originals = counts[i + 1]['original'] - counts[i]['original']
        if (i % 2 == 0 and (hits != 0 or originals <= 0)) or (i % 2 and hits <= 0):
            raise ValueError('old/new producer paths did not alternate')
    groups = {}
    for enabled in (0, 1):
        selected = [row for row in rows if row['enabled'] == enabled]
        histogram = [sum(row['histogram'][i] for row in selected) for i in range(512)]
        samples = sum(row['frames'] for row in selected)
        mean = sum(row['mean_ms'] * row['frames'] for row in selected) / samples
        groups['prepared' if enabled else 'original'] = dict(
            frames=samples, mean_ms=mean, fps=1000 / mean,
            p50=quantile(histogram, .5), p95=quantile(histogram, .95))
    return dict(run=str(run), phases=rows, counters=counts, aggregate=groups,
                mean_change_percent=(groups['prepared']['mean_ms'] /
                                     groups['original']['mean_ms'] - 1) * 100,
                scope='one process, fixed scene, common wrapper; not hardware or statistical significance')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = measure_ab(args.run)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k not in ('phases', 'counters')}))


if __name__ == '__main__':
    main()

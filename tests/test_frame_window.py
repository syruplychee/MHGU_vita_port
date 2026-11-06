"""Frame measurements reject incomplete or failed runs instead of boot averages."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/diagnostics'))
from measure_frame_window import measure
from measure_engine_layout_ab import measure_ab


def report(t, count, total, histogram):
    text = f'VP_GXM_FRAME_INTERVALS t_us={t} count={count} sum_us={total} width_us=1000\n'
    for offset in range(0, 512, 64):
        values = ','.join(map(str, histogram[offset:offset + 64]))
        text += f'VP_GXM_FRAME_BUCKETS t_us={t} offset={offset} hist={values}\n'
    return text


class FrameWindowTests(unittest.TestCase):
    def test_alternating_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'process-lifecycle.json').write_text(json.dumps(
                {'exit_code': 0, 'classification': 'exited'}))
            (root / 'save-protection.json').write_text(json.dumps(
                {'verified': True, 'markers_verified': True}))
            hist = [0] * 512
            hist[150] = 31
            t, total = 300000000, 31 * 150000
            text, counters = '', ''
            hits, originals = 0, 28
            for phase in range(5):
                frame = 32 + phase * 384
                mode = 1 if phase == 4 else phase % 2
                text += (f'ENGINE_LAYOUT_AB_PHASE frame={frame} start=32 '
                         f'phase={phase} enabled={mode} t_us={t}\n')
                counters += (f'ENGINE_LAYOUT_AB_COUNT entering_mode={mode}\n'
                             f'ENGINE_LAYOUT_STATS hits={hits} original={originals} '
                             'rejected=0 full=0 other_calls=0\n')
                if phase == 4:
                    break
                interval = 140000 if mode else 150000
                for step in range(0, 384, 32):
                    text += report(t, frame + step - 1, total, hist)
                    hist[interval // 1000] += 32
                    t += interval * 32
                    total += interval * 32
                if mode:
                    hits += 1000
                else:
                    originals += 1000
            (root / 'gxm-runtime.log').write_text(text)
            path = root / 'engine-layout-ab.log'
            path.write_text(counters)
            result = measure_ab(root)
            self.assertEqual(result['aggregate']['original']['frames'], 512)
            self.assertEqual(result['aggregate']['original']['mean_ms'], 150)
            self.assertEqual(result['aggregate']['prepared']['mean_ms'], 140)
            path.write_text(counters.replace('rejected=0', 'rejected=1', 1))
            with self.assertRaises(ValueError):
                measure_ab(root)

    def test_window_and_rejected_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'process-lifecycle.json').write_text(json.dumps(
                {'exit_code': 0, 'classification': 'exited', 'caller_stop': False}))
            (root / 'save-protection.json').write_text(json.dumps(
                {'verified': True, 'markers_verified': True}))
            a = [0] * 512
            a[33] = 100
            b = a.copy()
            b[150] = b[162] = 96
            text = report(230000000, 100, 10000000, a) + report(260000000, 292, 40000000, b)
            path = root / 'gxm-runtime.log'
            path.write_text(text)
            result = measure(root, 230, 290)
            self.assertEqual(result['frames'], 192)
            self.assertEqual(result['mean_ms'], 156.25)
            self.assertEqual(result['p50'], {'min_ms': 150, 'max_ms': 151})
            self.assertEqual(result['p95'], {'min_ms': 162, 'max_ms': 163})
            path.write_text(text.rsplit('VP_GXM_FRAME_BUCKETS', 1)[0])
            with self.assertRaises(ValueError):
                measure(root, 230, 290)
            path.write_text(text)
            with self.assertRaises(ValueError):
                measure(root, 270, 290)
            (root / 'process-lifecycle.json').write_text(json.dumps(
                {'exit_code': 0, 'classification': 'exited', 'caller_stop': True}))
            with self.assertRaises(ValueError):
                measure(root, 230, 290)


if __name__ == '__main__':
    unittest.main()

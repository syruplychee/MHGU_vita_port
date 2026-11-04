import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("gpu_wait", Path(__file__).parents[1] /
                                             "tools/diagnostics/analyze_gpu_wait.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def snapshot(snap, at, frames, calls):
    return (f"VP_GPU_WAIT snap={snap} t_us={at} frames={frames} row=0 rows=1 "
            f"cause=arena-reuse calls={calls} real={calls} polls=0 failures=0 "
            f"block_us={calls * 100} pending_sum=0 draws=0 sync=0 "
            f"b0=0 b1={calls} b2=0 b3=0 b4=0\n"
            f"VP_GPU_WAIT_TOTAL snap={snap} t_us={at} frames={frames} "
            f"real={calls} block_us={calls * 100}\n")


class WaitMetrics(unittest.TestCase):
    def setUp(self):
        self.text = snapshot(1, 1000000, 32, 2) + snapshot(2, 2000000, 64, 12)

    def test_interval_differences(self):
        result = mod.analyze(mod.parse_snapshots(self.text), 0, 3000000)
        self.assertEqual(result["frames"], 32)
        self.assertEqual(result["causes"]["arena-reuse"]["real"], 10)
        self.assertEqual(result["notify_ms_per_frame"], 0.03125)

    def test_reject_invalid_snapshots(self):
        for text in (self.text.rsplit("VP_GPU_WAIT_TOTAL", 1)[0],
                     self.text.replace("row=0", "row=1", 1),
                     self.text.replace("rows=1", "rows=2", 1),
                     self.text.replace("real=2 block_us=200", "real=3 block_us=200"),
                     self.text.replace("calls=12", "calls=11"),
                     self.text.replace("failures=0", "failures=-1", 1),
                     self.text + snapshot(3, 3000000, 96, 1),
                     self.text + snapshot(2, 3000000, 96, 15),
                     self.text.replace("b1=12", "b1=11")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                mod.parse_snapshots(text)

    def test_reject_insufficient_window(self):
        with self.assertRaises(ValueError):
            mod.analyze(mod.parse_snapshots(self.text), 1500000, 2500000)


if __name__ == "__main__":
    unittest.main()

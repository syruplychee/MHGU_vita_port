"""Regression tests for logged movie event timing evidence."""

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "tools/runtime/measure_movie.py"
SPEC = importlib.util.spec_from_file_location("measure_movie", MODULE_PATH)
movie = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(movie)


def event(n, t_us, kind="DECODE_TIME", rc=0):
    return (f"VP_MEDIA_VIDEO_{kind} fd=1 n={n} rc={rc} "
            f"total_us=2000 api_us=1000 copy_us=500 t_us={t_us}\n")


class MovieTimingTests(unittest.TestCase):
    def measure(self, text, start=0, end=None):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "sdk-dso.log"
            log.write_text(text, encoding="utf-8", newline="\n")
            result = movie.measure(log, start, end)
            self.assertEqual(result["source_sha256"], hashlib.sha256(log.read_bytes()).hexdigest())
            self.assertEqual(result["scope"], movie.SCOPE)
            return result

    def test_duplicates_do_not_inflate_rate(self):
        text = event(0, 100) + event(1, 1000000)
        text += "".join(event(1, time) for time in range(1010000, 1100000, 10000))
        text += event(2, 1033333) + event(3, 1066666)
        result = self.measure(text)["decoder"]
        self.assertTrue(result["available"])
        self.assertEqual(result["samples"], 3)
        self.assertAlmostEqual(result["event_count_rate_hz"], 30, places=3)
        self.assertAlmostEqual(result["intervals"]["mean_ms"], 33.333, places=3)
        self.assertFalse(result["intervals"]["partial"])

    def test_gap_is_explicit_and_not_a_frame_interval(self):
        result = self.measure(event(1, 1000000) + event(31, 2000000))["decoder"]
        self.assertEqual(result["samples"], 2)
        self.assertEqual(result["event_count_rate_hz"], 30)
        self.assertEqual(result["gaps"], [{"after_n": 1, "before_n": 31, "missing": 29}])
        self.assertTrue(result["intervals"]["partial"])
        self.assertFalse(result["intervals"]["available"])

    def test_window_and_failed_records(self):
        text = event(1, 500000) + event(1, 1100000) + event(2, 1200000, rc=-1)
        text += event(2, 1300000) + event(3, 1400000) + event(4, 1500000)
        result = self.measure(text, 1.1, 1.4)
        self.assertEqual(result["decoder"]["first_n"], 2)
        self.assertEqual(result["decoder"]["last_n"], 3)
        self.assertEqual(result["decoder"]["samples"], 2)
        self.assertEqual(result["window"], {"start_s": 1.1, "end_s": 1.4, "inclusive": True})

    def test_approximately_30_hz_for_both_event_types(self):
        text = "".join(event(n, 1000000 + round(n * 1000000 / 30), kind)
                       for n in range(1, 91)
                       for kind in ("DECODE_TIME", "COMPOSITE_TIME"))
        result = self.measure(text)
        self.assertTrue(result["available"])
        for kind in ("decoder", "compositor"):
            self.assertAlmostEqual(result[kind]["event_count_rate_hz"], 30, places=4)
            self.assertEqual(result[kind]["intervals"]["samples"], 89)
            self.assertEqual(result[kind]["total_mean_ms"], 2)
            self.assertEqual(result[kind]["api_mean_ms"], 1)
            self.assertEqual(result[kind]["copy_mean_ms"], 0.5)

    def test_readback_groups_planes_without_frame_claim(self):
        text = ("VP_MEDIA_VIDEO_READBACK n=1 rows=720 total_us=2000 t_us=1000000\n"
                "VP_MEDIA_VIDEO_READBACK n=2 rows=360 total_us=1000 t_us=1000001\n"
                "VP_MEDIA_VIDEO_READBACK n=3 rows=720 total_us=4000 t_us=2000000\n")
        result = self.measure(text)
        self.assertFalse(result["available"])
        readback = result["readback"]
        self.assertTrue(readback["available"])
        self.assertEqual(readback["samples"], 3)
        self.assertEqual(readback["by_rows"]["720"]["mean_ms"], 3)
        self.assertEqual(readback["by_rows"]["720"]["samples"], 2)
        self.assertEqual(readback["by_rows"]["360"]["samples"], 1)
        self.assertNotIn("event_count_rate_hz", readback)

    def test_insufficient_records_are_unavailable(self):
        for text in ("unrelated line\n", event(1, 100), event(1, 100) + event(2, 100)):
            result = self.measure(text)
            self.assertFalse(result["available"])
            self.assertFalse(result["decoder"]["available"])
            self.assertIsNone(result["decoder"]["event_count_rate_hz"])
            self.assertFalse(result["compositor"]["available"])

    def test_bad_window_is_rejected(self):
        for start, end in ((-1, None), (2, 1), (float("nan"), None), (0, float("inf"))):
            with self.assertRaises(ValueError):
                self.measure("", start, end)


if __name__ == "__main__":
    unittest.main()

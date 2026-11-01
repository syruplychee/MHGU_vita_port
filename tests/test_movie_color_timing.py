"""Regression tests for logged movie color-conversion probe timing."""

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "tools/runtime/measure_movie_color.py"
SPEC = importlib.util.spec_from_file_location("measure_movie_color", MODULE_PATH)
color = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(color)


def record(n, mode="rgb", entry_us=None, elapsed_us=5000, width=640,
           height=360, size=345600, thread="0x148"):
    if entry_us is None:
        entry_us = 1_000_000 + n * 100_000
    t_us = entry_us + elapsed_us
    return (f"VP_MOVIE_COLOR_TIME n={n} mode={mode} thread={thread} width={width} "
            f"height={height} destination=0x80000000 source_y=0x80000100 "
            f"source_uv=0x80000200 bytes={size} elapsed_us={elapsed_us} "
            f"entry_us={entry_us} t_us={t_us}\n")


class MovieColorTimingTests(unittest.TestCase):
    def measure(self, text, start=0, end=None):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "sdk-dso.log"
            log.write_text(text, encoding="utf-8", newline="\n")
            result = color.measure(log, start, end)
            self.assertEqual(result["source_sha256"], hashlib.sha256(log.read_bytes()).hexdigest())
            self.assertEqual(result["scope"], color.SCOPE)
            return result

    def test_modes_are_separate_and_reported_in_milliseconds(self):
        text = (record(1, "rgb", 1_000_000, 1000)
                + record(2, "rgb", 1_100_000, 2000)
                + record(3, "rgb", 1_200_000, 3000)
                + record(4, "nv12", 1_300_000, 4000)
                + record(5, "nv12", 1_400_000, 5000))
        result = self.measure(text)
        self.assertTrue(result["available"])
        self.assertEqual(result["samples"], 5)
        rgb = result["modes"]["rgb"]
        self.assertEqual(rgb["samples"], 3)
        self.assertEqual(rgb["elapsed_ms"]["mean_ms"], 2)
        self.assertEqual(rgb["elapsed_ms"]["median_ms"], 2)
        self.assertAlmostEqual(rgb["elapsed_ms"]["p95_ms"], 2.9, places=4)
        self.assertEqual(rgb["elapsed_ms"]["min_ms"], 1)
        self.assertEqual(rgb["elapsed_ms"]["max_ms"], 3)
        self.assertEqual(rgb["threads"], ["0x148"])
        self.assertEqual(result["modes"]["nv12"]["elapsed_ms"]["mean_ms"], 4.5)
        self.assertAlmostEqual(result["completion"]["rate_hz"], 4 / 0.404, places=4)

    def test_duplicate_completion_numbers_do_not_inflate_samples(self):
        text = record(1, elapsed_us=1000) + record(1, elapsed_us=9000) + record(2, elapsed_us=2000)
        result = self.measure(text)
        self.assertEqual(result["samples"], 2)
        self.assertEqual(result["sampling"]["duplicates"], 1)
        self.assertEqual(result["modes"]["rgb"]["elapsed_ms"]["mean_ms"], 1.5)

    def test_window_is_inclusive_on_end_time(self):
        text = (record(1, entry_us=1_000_000, elapsed_us=1000)
                + record(2, entry_us=1_500_000, elapsed_us=1000)
                + record(3, entry_us=1_995_000, elapsed_us=5000))
        result = self.measure(text, 1.1, 2.0)
        self.assertEqual(result["samples"], 2)
        self.assertEqual(result["completion"]["first_n"], 2)
        self.assertEqual(result["completion"]["last_n"], 3)
        self.assertEqual(result["window"], {"start_s": 1.1, "end_s": 2.0, "inclusive": True})

    def test_invalid_records_are_skipped(self):
        text = (record(0)
                + record(6, mode="yuv")
                + "VP_MOVIE_COLOR_TIME n=7 mode=rgb thread=0x148 width=640 height=360 "
                  "elapsed_us=1000 entry_us=1000000 t_us=1001000\n"
                + record(8, elapsed_us=-1)
                + "VP_MOVIE_COLOR_TIME n=9 mode=rgb thread=0x148 width=640 height=360 "
                  "bytes=100 elapsed_us=1000 entry_us=2000000 t_us=1999000\n"
                + record(10))
        result = self.measure(text)
        self.assertEqual(result["samples"], 1)
        self.assertEqual(result["modes"]["rgb"]["samples"], 1)
        self.assertFalse(result["completion"]["available"])

    def test_disabled_probe_has_no_samples_and_says_why(self):
        result = self.measure("MOVIE_COLOR_PROBE_MODE enabled=0 module=subsdk0\n")
        self.assertFalse(result["available"])
        self.assertFalse(result["probe"]["enabled"])
        self.assertIn("marker", result["reason"])
        self.assertFalse(result["modes"]["rgb"]["available"])

    def test_enabled_probe_without_completions_reports_no_hits(self):
        patches = "".join(
            f"MOVIE_COLOR_PROBE_PATCH site=0x33c4f{4 + index} word=0x0 "
            f"service=0x24{0xc + index:x} island=0x80000000 status=0\n"
            for index in range(4))
        result = self.measure("MOVIE_COLOR_PROBE_MODE enabled=1 module=subsdk0\n" + patches)
        self.assertFalse(result["available"])
        self.assertTrue(result["probe"]["enabled"])
        self.assertTrue(result["probe"]["accepted"])
        self.assertEqual(len(result["probe"]["patch_sites"]), 4)
        self.assertIn("No completed", result["reason"])

    def test_rejected_probe_is_reported(self):
        text = ("MOVIE_COLOR_PROBE_MODE enabled=1 module=subsdk0\n"
                "MOVIE_COLOR_PROBE_REJECT status=-3\n")
        result = self.measure(text)
        self.assertFalse(result["available"])
        self.assertTrue(result["probe"]["rejected"])
        self.assertFalse(result["probe"]["accepted"])
        self.assertIn("rejected", result["reason"])

    def test_completion_rate_uses_counter_not_logged_line_count(self):
        text = (record(1, entry_us=1_000_000, elapsed_us=5000)
                + record(2001, entry_us=2_000_000, elapsed_us=5000))
        result = self.measure(text)
        self.assertAlmostEqual(result["completion"]["rate_hz"], 2000, places=4)
        self.assertIn("every 32th", result["sampling"]["regime"])
        self.assertEqual(result["sampling"]["completion_span"], 2001)
        self.assertEqual(result["sampling"]["unlogged_completions"], 1999)

    def test_bad_window_is_rejected(self):
        for start, end in ((-1, None), (2, 1), (float("nan"), None), (0, float("inf"))):
            with self.assertRaises(ValueError):
                self.measure("", start, end)

    def test_production_log_format_still_matches_parser(self):
        source = (Path(__file__).resolve().parents[1] / "src/loader/sdk_dso.c").read_text(
            encoding="utf-8")
        self.assertIn('"VP_MOVIE_COLOR_TIME n=%u mode=%s thread=0x%x width=%u height=%u "', source)
        self.assertIn('"destination=0x%08x source_y=0x%08x source_uv=0x%08x bytes=%u "', source)
        self.assertIn(r'"elapsed_us=%llu entry_us=%llu t_us=%llu\n"', source)


if __name__ == "__main__":
    unittest.main()

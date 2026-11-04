"""Engine-preparation A/B analyzer tests (standard library unittest only)."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/engine"))
from analyze_prepare_ab import analyze_logs, validate_phases  # noqa: E402

PHASES = [
    {"mode": "original", "start_us": 1_000_000, "end_us": 4_000_000},
    {"mode": "native", "start_us": 4_100_000, "end_us": 7_100_000},
    {"mode": "original", "start_us": 7_200_000, "end_us": 10_200_000},
    {"mode": "native", "start_us": 10_300_000, "end_us": 13_300_000},
]
WARMUP_US = 1_000_000
ELAPSED = {
    "original": [100, 110, 120, 130, 140, 150, 160, 170, 180, 190],
    "native": [50, 60, 70, 80, 90, 100, 110, 120, 130, 140],
    "fast": [75, 85, 95, 105, 115, 125, 135, 145, 155, 165],
    "shared": [95, 105, 115, 125, 135, 145, 155, 165, 175, 185],
    "deep": [90, 100, 110, 120, 130, 140, 150, 160, 170, 180],
    "deep-recycle": [85, 95, 105, 115, 125, 135, 145, 155, 165, 175],
    "deep-clear": [87, 97, 107, 117, 127, 137, 147, 157, 167, 177],
}
ENCODED = {"original": 100, "native": 200, "fast": 150, "shared": 120,
           "deep": 110, "deep-recycle": 110, "deep-clear": 110}


def prepare_line(mode, entry_us, elapsed_us, encoded, growth, presents):
    return (f"ENGINE_PREPARE_TIME mode={mode} thread=0x1 context=0x00001000 "
            f"entry_us={entry_us} t_us={entry_us + elapsed_us} "
            f"elapsed_us={elapsed_us} encoded_bytes={encoded} growth={growth} "
            f"presents={presents}")


def window_line(t_us, presents):
    return (f"VP_GXM_PERF_WINDOW kind=colour draws=1 failures=0 skips=0 "
            f"t_us={t_us} window_us=2000000 presents={presents}")


def build_case(phases, skip_stable=(), growth_phase=1, windows=True,
               presents_advance=True):
    sdk, gxm = [], []
    presents = 100
    for index, phase in enumerate(phases):
        mode = phase["mode"]
        # An opposite-mode sample inside the warmup must be ignored rather than
        # treated as a mode mismatch.
        warm_mode = "native" if mode == "original" else "original"
        warm_entry = phase["start_us"] + WARMUP_US // 2
        sdk.append(prepare_line(warm_mode, warm_entry, 999999, 0, 0, presents))
        if index not in skip_stable:
            for step, elapsed in enumerate(ELAPSED[mode]):
                entry = phase["start_us"] + WARMUP_US + 100000 + step * 100000
                growth = 1 if index == growth_phase and step == 0 else 0
                encoded = 0 if growth else ENCODED[mode]
                if presents_advance:
                    presents += 10
                sdk.append(prepare_line(
                    mode, entry, elapsed, encoded, growth, presents))
        if windows:
            start = index * 100000 + 1000
            gxm.append(window_line(phase["start_us"] + 2_000_000, start))
            gxm.append(window_line(phase["start_us"] + 3_000_000, start + 200))
    return "\n".join(sdk) + "\n", "\n".join(gxm) + "\n"


class PrepareAbTests(unittest.TestCase):
    def test_alternating_phases_and_known_percentiles(self):
        sdk, gxm = build_case(PHASES)
        report = analyze_logs(sdk, gxm, PHASES, warmup_seconds=1.0)
        self.assertEqual(report["phase_count"], 4)
        first = report["phases"][0]
        self.assertEqual(first["mode"], "original")
        self.assertEqual(first["samples"], 10)
        self.assertEqual(first["warmup_excluded"], 1)
        # Known ten-sample distribution: 100..190 in steps of 10.
        self.assertEqual(first["elapsed_us"]["sum"], 1450)
        self.assertEqual(first["elapsed_us"]["mean"], 145)
        self.assertEqual(first["elapsed_us"]["median"], 145)
        self.assertEqual(first["elapsed_us"]["p95"], 190)
        self.assertEqual(first["elapsed_us"]["p99"], 190)
        self.assertEqual(first["elapsed_us"]["max"], 190)
        self.assertEqual(first["encoded_bytes"]["sum"], 1000)
        self.assertEqual(first["encoded_bytes"]["growth_samples"], 0)
        native = report["phases"][1]
        self.assertEqual(native["samples"], 10)
        self.assertEqual(native["elapsed_us"]["sum"], 950)
        self.assertEqual(native["elapsed_us"]["mean"], 95)
        self.assertEqual(native["elapsed_us"]["p95"], 140)
        # One growth sample is reported separately, never summed as zero bytes.
        self.assertEqual(native["encoded_bytes"]["sum"], 1800)
        self.assertEqual(native["encoded_bytes"]["samples"], 9)
        self.assertEqual(native["encoded_bytes"]["growth_samples"], 1)
        # 1,000,000 us between two GXM endpoints with 200 presents.
        present = first["present"]
        self.assertEqual(present["source"], "gxm-perf-window-colour")
        self.assertEqual(present["delta_presents"], 200)
        self.assertEqual(present["mean_interval_us"], 5000)
        self.assertEqual(present["presents_per_second"], 200)
        aggregate = report["aggregate"]
        self.assertEqual(aggregate["original"]["samples"], 20)
        self.assertEqual(aggregate["original"]["elapsed_us"]["sum"], 2900)
        self.assertEqual(aggregate["native"]["elapsed_us"]["sum"], 1900)
        self.assertEqual(aggregate["native"]["encoded_bytes"]["sum"], 3800)
        self.assertEqual(aggregate["native"]["growth_samples"], 1)
        self.assertEqual(report["delta"]["baseline_mode"], "original")
        self.assertEqual(report["delta"]["comparison_mode"], "native")
        self.assertAlmostEqual(
            report["delta"]["fields"]["mean_us"]["percent"], -34.48275862068966)
        self.assertTrue(report["decision"].startswith("none"))

    def test_sdk_present_fallback(self):
        sdk, gxm = build_case(PHASES, windows=False)
        report = analyze_logs(sdk, gxm, PHASES, warmup_seconds=1.0)
        present = report["phases"][0]["present"]
        self.assertEqual(present["source"], "sdk-ipc-present")
        # Ten stable samples advance the cumulative counter by 90.
        self.assertEqual(present["delta_presents"], 90)
        self.assertGreater(present["mean_interval_us"], 0)

    def test_empty_phase_and_missing_present_rejected(self):
        sdk, gxm = build_case(PHASES, skip_stable=(1,))
        with self.assertRaisesRegex(ValueError, "no samples after"):
            analyze_logs(sdk, gxm, PHASES, warmup_seconds=1.0)
        sdk, gxm = build_case(PHASES, windows=False, presents_advance=False)
        with self.assertRaisesRegex(ValueError, "no sufficient present endpoints"):
            analyze_logs(sdk, gxm, PHASES, warmup_seconds=1.0)
        sdk, gxm = build_case(PHASES, windows=False)
        gxm = "\n".join(gxm.splitlines()[:1]) + "\n"
        sdk = "\n".join(
            line if "presents=" not in line else
            line.rsplit("presents=", 1)[0] + "presents=7"
            for line in sdk.splitlines()) + "\n"
        with self.assertRaisesRegex(ValueError, "no sufficient present endpoints"):
            analyze_logs(sdk, gxm, PHASES, warmup_seconds=1.0)

    def test_warmup_boundary_excludes_only_before_start(self):
        sdk, gxm = build_case(PHASES)
        base = analyze_logs(sdk, gxm, PHASES, warmup_seconds=1.0)
        phase = PHASES[0]
        before = prepare_line(
            "original", phase["start_us"] + WARMUP_US - 1, 777, 1, 0, 5000)
        at = prepare_line(
            "original", phase["start_us"] + WARMUP_US, 777, 1, 0, 5000)
        result = analyze_logs(
            sdk + before + "\n" + at + "\n", gxm, PHASES, warmup_seconds=1.0)
        self.assertEqual(result["phases"][0]["samples"],
                         base["phases"][0]["samples"] + 1)
        self.assertEqual(result["phases"][0]["warmup_excluded"],
                         base["phases"][0]["warmup_excluded"] + 1)

    def test_sample_crossing_phase_boundary_rejected(self):
        sdk, gxm = build_case(PHASES)
        crossing = prepare_line(
            "original", PHASES[0]["end_us"] - 1000, 101000, 10, 0, 6000)
        with self.assertRaisesRegex(ValueError, "crosses a phase boundary"):
            analyze_logs(sdk + crossing + "\n", gxm, PHASES, warmup_seconds=1.0)

    def test_phase_order_and_overlap_rejected(self):
        wrong = [dict(row) for row in PHASES]
        wrong[1]["mode"] = "original"
        with self.assertRaisesRegex(ValueError, "alternate"):
            validate_phases(wrong)
        overlap = [dict(row) for row in PHASES]
        overlap[1]["start_us"] = overlap[0]["end_us"]
        with self.assertRaisesRegex(ValueError, "before the previous phase"):
            validate_phases(overlap)
        with self.assertRaisesRegex(ValueError, "at least four phases"):
            validate_phases(PHASES[:3])
        bad_phase = [dict(row) for row in PHASES]
        bad_phase[1]["mode"] = "4"
        with self.assertRaisesRegex(ValueError, "unknown mode"):
            validate_phases(bad_phase)

    def test_future_fast_mode_name_is_accepted(self):
        phases = [
            {"mode": "original", "start_us": 1_000_000, "end_us": 4_000_000},
            {"mode": "fast", "start_us": 4_100_000, "end_us": 7_100_000},
            {"mode": "original", "start_us": 7_200_000, "end_us": 10_200_000},
            {"mode": "fast", "start_us": 10_300_000, "end_us": 13_300_000},
        ]
        sdk, gxm = build_case(phases)
        report = analyze_logs(sdk, gxm, phases, warmup_seconds=1.0)
        self.assertEqual(report["delta"]["baseline_mode"], "original")
        self.assertEqual(report["delta"]["comparison_mode"], "fast")
        self.assertIn("fast", report["aggregate"])

    def test_shared_mode_name_is_accepted(self):
        phases = [
            {"mode": "original", "start_us": 1_000_000, "end_us": 4_000_000},
            {"mode": "shared", "start_us": 4_100_000, "end_us": 7_100_000},
            {"mode": "original", "start_us": 7_200_000, "end_us": 10_200_000},
            {"mode": "shared", "start_us": 10_300_000, "end_us": 13_300_000},
        ]
        sdk, gxm = build_case(phases)
        report = analyze_logs(sdk, gxm, phases, warmup_seconds=1.0)
        self.assertEqual(report["delta"]["baseline_mode"], "original")
        self.assertEqual(report["delta"]["comparison_mode"], "shared")
        self.assertIn("shared", report["aggregate"])
        self.assertEqual(report["phases"][1]["samples"], 10)

    def test_recycle_mode_name_is_accepted(self):
        phases = [dict(p, mode="deep" if i % 2 == 0 else "deep-recycle")
                  for i, p in enumerate(PHASES)]
        sdk, gxm = build_case(phases)
        report = analyze_logs(sdk, gxm, phases, warmup_seconds=1.0)
        self.assertIn("deep-recycle", report["aggregate"])

    def test_clear_defer_mode_name_is_accepted(self):
        phases = [dict(p, mode="deep-recycle" if i % 2 == 0 else "deep-clear")
                  for i, p in enumerate(PHASES)]
        sdk, gxm = build_case(phases)
        report = analyze_logs(sdk, gxm, phases, warmup_seconds=1.0)
        self.assertIn("deep-clear", report["aggregate"])
        self.assertIn("deep-recycle", report["aggregate"])
        self.assertEqual(report["delta"]["comparison_mode"], "deep-clear")
        # A raw control digit is not a phase name.
        with self.assertRaisesRegex(ValueError, "unknown mode"):
            validate_phases([
                {"mode": "deep-recycle", "start_us": 1_000_000,
                 "end_us": 4_000_000},
                {"mode": "6", "start_us": 4_100_000, "end_us": 7_100_000},
                {"mode": "deep-recycle", "start_us": 7_200_000,
                 "end_us": 10_200_000},
                {"mode": "deep-clear", "start_us": 10_300_000,
                 "end_us": 13_300_000},
            ])

    def test_deep_mode_name_is_accepted(self):
        phases = [
            {"mode": "original", "start_us": 1_000_000, "end_us": 4_000_000},
            {"mode": "deep", "start_us": 4_100_000, "end_us": 7_100_000},
            {"mode": "original", "start_us": 7_200_000, "end_us": 10_200_000},
            {"mode": "deep", "start_us": 10_300_000, "end_us": 13_300_000},
        ]
        sdk, gxm = build_case(phases)
        report = analyze_logs(sdk, gxm, phases, warmup_seconds=1.0)
        self.assertEqual(report["delta"]["baseline_mode"], "original")
        self.assertEqual(report["delta"]["comparison_mode"], "deep")
        self.assertIn("deep", report["aggregate"])
        self.assertEqual(report["phases"][1]["samples"], 10)

    def test_unknown_mode_and_bad_elapsed_rejected(self):
        sdk, gxm = build_case(PHASES)
        bad_mode = sdk.replace("mode=original", "mode=other", 1)
        with self.assertRaisesRegex(ValueError, "unknown preparation mode"):
            analyze_logs(bad_mode, gxm, PHASES, warmup_seconds=1.0)
        # The runtime accepts control digits 0..5, but a logged mode=4 is not
        # a phase name; the phase name for mode 4 is "deep".
        mode4 = sdk.replace("mode=original", "mode=4", 1)
        with self.assertRaisesRegex(ValueError, "unknown preparation mode"):
            analyze_logs(mode4, gxm, PHASES, warmup_seconds=1.0)
        bad_elapsed = sdk.replace("elapsed_us=100 ", "elapsed_us=101 ", 1)
        with self.assertRaisesRegex(ValueError, "elapsed_us does not match"):
            analyze_logs(bad_elapsed, gxm, PHASES, warmup_seconds=1.0)


if __name__ == "__main__":
    unittest.main()

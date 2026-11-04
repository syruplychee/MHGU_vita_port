import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "engine" / "analyze_frame_cost.py"

spec = importlib.util.spec_from_file_location("analyze_frame_cost", TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

CATEGORIES = module.CATEGORIES


def row_values(calls=0, incl=0, self_us=0, max_us=0, root_calls=0,
               root_incl=0):
    return dict(calls=calls, incl_us=incl, self_us=self_us, max_us=max_us,
                root_calls=root_calls, root_incl_us=root_incl)


def snapshot_lines(seq, epoch, t_us, frames, categories=None, errors=0,
                   control_error=0, depth=0, mode=1, reason="periodic",
                   omit_row=None, row_identity=None):
    categories = categories or {}
    rows = []
    total_errors = errors
    for index, name in enumerate(CATEGORIES):
        if name == omit_row:
            continue
        values = categories.get(name, row_values())
        identity = (3, seq, epoch, t_us, frames)
        if row_identity is not None and index == 0:
            identity = row_identity
        rows.append(
            "VP_FRAME_COST_SNAPSHOT_ROW schema=%u seq=%u epoch=%u mode=1 "
            "t_us=%u frames=%u row=%u cat=%s calls=%u incl_us=%u self_us=%u "
            "max_us=%u root_calls=%u root_incl_us=%u" % (
                identity + (index, name, values["calls"], values["incl_us"],
                            values["self_us"], values["max_us"],
                            values["root_calls"], values["root_incl_us"])))
    begin = (
        "VP_FRAME_COST_SNAPSHOT_BEGIN schema=3 seq=%u epoch=%u mode=1 "
        "t_us=%u frames=%u rows=%u depth=%u errors=%u mismatch=%u overflow=0 "
        "underflow=0 clock_regress=0 missing_time=0 incomplete=0 "
        "control_error=%u clocks=10 poll_clocks=2 boundaries=3 reason=%s" % (
            seq, epoch, t_us, frames, len(CATEGORIES), depth, total_errors,
            errors, control_error, reason))
    end = (
        "VP_FRAME_COST_SNAPSHOT_END schema=3 seq=%u epoch=%u mode=1 t_us=%u "
        "frames=%u rows=%u errors=%u" % (
            seq, epoch, t_us, frames, len(CATEGORIES), total_errors))
    identity = f"schema=3 seq={seq} epoch={epoch} mode=1 t_us={t_us} frames={frames}"
    details = ["VP_FRAME_COST_GXM " + identity + f" kind={kind} " +
               " ".join(f"{key}=0" for key in module.GXM_DETAIL_FIELDS) for kind in (0, 1)]
    details += ["VP_FRAME_COST_TEXTURE " + identity + " " +
                " ".join(f"{key}=0" for key in module.TEXTURE_DETAIL_FIELDS)]
    return "\n".join([begin] + rows + details + [end])


def category_map(entry_calls, entry_incl, entry_self, entry_root_calls,
                 entry_root_incl):
    values = {name: row_values() for name in CATEGORIES}
    values["entry"] = row_values(entry_calls, entry_incl, entry_self,
                                 entry_incl, entry_root_calls,
                                 entry_root_incl)
    child_us = entry_incl - entry_self
    if child_us:
        values["dispatch"] = row_values(entry_calls, child_us, child_us, child_us)
    return values


def valid_log():
    first = snapshot_lines(
        1, 3, 10_000_000, 32,
        category_map(10, 1_000_000, 400_000, 10, 1_000_000))
    second = snapshot_lines(
        2, 3, 13_200_000, 64,
        category_map(20, 3_000_000, 900_000, 20, 3_000_000))
    return first + "\n" + second + "\n"


class FrameCostAnalyzerTests(unittest.TestCase):
    def test_valid_delta_and_signed_residual(self):
        report = module.analyze(valid_log(), [(9_000_000, 14_000_000)])
        window = report["windows"][0]
        self.assertEqual(window["frames"], 32)
        self.assertEqual(window["wall_us"], 3_200_000)
        self.assertEqual(window["frame_wall_us"], 100_000)
        self.assertEqual(window["categories"]["entry"]["calls"], 10)
        self.assertEqual(window["categories"]["entry"]["incl_us"], 2_000_000)
        self.assertEqual(window["categories"]["entry"]["self_us"], 500_000)
        self.assertEqual(window["measured_root_us"], 2_000_000)
        self.assertEqual(window["residual_us"], 1_200_000)
        self.assertFalse(window["closure_complete"])
        self.assertTrue(window["window_valid"])
        self.assertFalse(report["performance_pass"])
        self.assertFalse(report["speedup_claim"])
        self.assertIn("process evidence", report["process_evidence_reason"])
        self.assertEqual(len(report["inputs"]["gxm_log"]["sha256"]), 64)

    def test_negative_residual_is_preserved(self):
        first = snapshot_lines(
            1, 1, 1_000_000, 32,
            category_map(1, 4_000_000, 4_000_000, 1, 4_000_000))
        second = snapshot_lines(
            2, 1, 2_000_000, 64,
            category_map(2, 6_000_000, 6_000_000, 2, 6_000_000))
        report = module.analyze(first + "\n" + second,
                                [(0, 2_000_000)])
        window = report["windows"][0]
        self.assertEqual(window["residual_us"], -1_000_000)
        self.assertLess(window["residual_us_per_frame"], 0)
        self.assertFalse(window["window_valid"])
        self.assertFalse(window["closure_complete"])
        self.assertTrue(any("exceed" in reason
                            for reason in window["residual_reason"]))
        self.assertFalse(report["performance_pass"])

    def test_missing_row_rejected(self):
        text = valid_log().replace(
            "VP_FRAME_COST_SNAPSHOT_ROW schema=3 seq=2 epoch=3 mode=1 "
            "t_us=13200000 frames=64 row=5", "VP_FRAME_COST_SNAPSHOT_ROW "
            "schema=3 seq=2 epoch=3 mode=1 t_us=13200000 frames=64 row=99")
        with self.assertRaises(ValueError):
            module.parse_snapshots(text)

    def test_unreconciled_self_times_rejected(self):
        text = valid_log().replace("self_us=400000", "self_us=400001")
        with self.assertRaisesRegex(ValueError, "reconcile"):
            module.parse_snapshots(text)

    def test_missing_aligned_detail_rejected(self):
        text = "\n".join(line for line in valid_log().splitlines()
                         if not line.startswith("VP_FRAME_COST_TEXTURE "))
        with self.assertRaisesRegex(ValueError, "detail"):
            module.parse_snapshots(text)

    def test_truncated_snapshot_rejected(self):
        with self.assertRaises(ValueError):
            module.parse_snapshots(valid_log().split(
                "VP_FRAME_COST_SNAPSHOT_END")[0])

    def test_epoch_change_rejected(self):
        first = snapshot_lines(
            1, 1, 1_000_000, 32,
            category_map(1, 100, 100, 1, 100))
        second = snapshot_lines(
            2, 2, 2_000_000, 64,
            category_map(2, 200, 200, 2, 200))
        with self.assertRaises(ValueError):
            module.analyze(first + "\n" + second, [(0, 3_000_000)])

    def test_counter_regression_rejected(self):
        first = snapshot_lines(
            1, 1, 1_000_000, 32,
            category_map(5, 500, 500, 5, 500))
        second = snapshot_lines(
            2, 1, 2_000_000, 64,
            category_map(4, 400, 400, 4, 400))
        with self.assertRaises(ValueError):
            module.analyze(first + "\n" + second, [(0, 3_000_000)])

    def test_profiler_error_rejected(self):
        first = snapshot_lines(
            1, 1, 1_000_000, 32,
            category_map(1, 100, 100, 1, 100), errors=1)
        second = snapshot_lines(
            2, 1, 2_000_000, 64,
            category_map(2, 200, 200, 2, 200), errors=1)
        with self.assertRaises(ValueError):
            module.analyze(first + "\n" + second, [(0, 3_000_000)])

    def test_open_span_snapshot_rejected(self):
        first = snapshot_lines(
            1, 1, 1_000_000, 32,
            category_map(1, 100, 100, 1, 100), depth=1)
        with self.assertRaises(ValueError):
            module.parse_snapshots(first)

    def test_frame_step_must_be_32(self):
        first = snapshot_lines(
            1, 1, 1_000_000, 32,
            category_map(1, 100, 100, 1, 100))
        second = snapshot_lines(
            2, 1, 2_000_000, 96,
            category_map(2, 200, 200, 2, 200))
        with self.assertRaises(ValueError):
            module.analyze(first + "\n" + second, [(0, 3_000_000)])

    def test_row_identity_mismatch_rejected(self):
        text = valid_log().replace(
            "VP_FRAME_COST_SNAPSHOT_ROW schema=3 seq=2 epoch=3 mode=1 "
            "t_us=13200000 frames=64 row=0",
            "VP_FRAME_COST_SNAPSHOT_ROW schema=3 seq=2 epoch=3 mode=1 "
            "t_us=13200001 frames=64 row=0")
        with self.assertRaises(ValueError):
            module.parse_snapshots(text)

    def test_multiple_windows(self):
        third = snapshot_lines(
            3, 3, 16_400_000, 96,
            category_map(30, 4_000_000, 1_200_000, 30, 4_000_000))
        text = valid_log() + third + "\n"
        report = module.analyze(text, [(9_000_000, 14_000_000),
                                       (13_000_000, 17_000_000)])
        self.assertEqual(len(report["windows"]), 2)
        self.assertEqual(report["windows"][1]["frames"], 32)
        self.assertEqual(report["windows"][1]["categories"]["entry"]
                         ["incl_us"], 1_000_000)

    def test_evidence_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            process_path = directory / "process.json"
            save_path = directory / "save.json"
            process_path.write_text(json.dumps(
                {"exit_code": 0, "classification": "exited", "caller_stop": False,
                 "crash_suspected": False, "pid_reused": False,
                 "expected_start_time_checked": True}), encoding="utf-8")
            save_path.write_text(json.dumps(
                {"restored": True, "verified": True,
                 "markers_verified": True}), encoding="utf-8")
            process = module._evidence_from_file(process_path)
            save = module._evidence_from_file(save_path)
            report = module.analyze(valid_log(), [(9_000_000, 14_000_000)],
                                    process_evidence=process,
                                    save_evidence=save)
            self.assertFalse(report["performance_pass"])
            self.assertFalse(report["minimum_128_frames_per_window"])
            complete = "\n".join(snapshot_lines(i, 3, i * 3_200_000, i * 32,
                category_map(i * 10, i * 1_000_000, i * 400_000,
                             i * 10, i * 1_000_000)) for i in range(1, 7))
            report = module.analyze(complete, [(0, 20_000_000)],
                                     process_evidence=process, save_evidence=save)
            self.assertTrue(report["performance_pass"])
            self.assertFalse(report["speedup_claim"])
            self.assertEqual(len(process["sha256"]), 64)

    def test_mode0_interference_descriptive(self):
        mode0 = (
            "VP_FRAME_COST_MARKER enabled=1 path=x\n"
            "VP_FRAME_COST_POINT schema=3 epoch=1 mode=0 t_us=9000000 "
            "frames=32 clocks=0 poll_clocks=4 boundaries=4 control_error=0 "
            "reason=mode-0\n"
            "VP_GXM_PERF_PRESENT frames=32 t_us=10000000 present_fn_us=1 "
            "present_fn_avg_us=1 present_fn_max_us=1 interval_us=1 "
            "interval_avg_us=1 interval_max_us=1 g_ok=0 g_fail=0 g_skip=0\n"
            "VP_GXM_PERF_PRESENT frames=64 t_us=11600000 present_fn_us=1 "
            "present_fn_avg_us=1 present_fn_max_us=1 interval_us=1 "
            "interval_avg_us=1 interval_max_us=1 g_ok=0 g_fail=0 g_skip=0\n")
        report = module.analyze(valid_log(), [(9_000_000, 14_000_000)],
                                mode0_text=mode0)
        comparison = report["mode0_interference"]
        self.assertTrue(comparison["marker_enabled"])
        self.assertEqual(comparison["mode0_points_in_log"], 1)
        entry = comparison["comparisons"][0]
        self.assertTrue(entry["available"])
        self.assertTrue(entry["descriptive_only"])
        self.assertAlmostEqual(entry["mode0_frame_wall_us"], 50_000.0)

    def test_unverified_restore_and_missing_exit_code_rejected(self):
        self.assertFalse(module.check_save_evidence(
            {"values": {"restored": True, "verified": False,
                        "markers_verified": True}})[0])
        self.assertFalse(module.check_process_evidence(
            {"values": {"normal_exit": True}})[0])
        self.assertFalse(module.check_process_evidence(
            {"values": {"classification": "exited", "exit_code": 0,
                        "caller_stop": True, "crash_suspected": False,
                        "pid_reused": False, "expected_start_time_checked": True}})[0])


if __name__ == "__main__":
    unittest.main()

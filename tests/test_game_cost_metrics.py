import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "game_cost", Path(__file__).resolve().parents[1] / "tools/engine/analyze_game_cost.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
resource_spec = importlib.util.spec_from_file_location(
    "frame_resources", Path(__file__).resolve().parents[1] / "tools/engine/analyze_frame_resources.py")
resources = importlib.util.module_from_spec(resource_spec)
resource_spec.loader.exec_module(resources)


def sample(site, depth, start, end, self_us, bridge=0):
    return (f"GAME_COST_TIME site={site} slot=0 thread=0x1 depth={depth} "
            f"context=0x1000 caller=0x2000 entry_us={start} t_us={end} "
            f"incl_us={end-start} self_us={self_us} bridge_us={bridge} bridge_calls=1 "
            "presents_before=1 presents_after=2\n")


class GameCostMetrics(unittest.TestCase):
    def test_resource_boundaries_and_repeated_identity(self):
        lines = []
        for frame in range(5):
            lines.append(f"VP_FRAME_RESOURCE frame={frame} key=1000,32,32,8,2,128,0,2842,1,1,0 "
                         "raw_bytes=4096 output_bytes=4096 hit=0 changed=0 live_keys=0 gpu_only=0")
        report = resources.analyze("\n".join(lines))
        self.assertEqual(report["interior_frames"], 3)
        self.assertEqual(report["totals"]["absent_misses"], 3)
        self.assertEqual(report["totals"]["absent_after_earlier_frame_request"], 2)
        self.assertIn("do not prove unchanged", " ".join(report["limitations"]))

    def test_resource_key_must_be_complete(self):
        with self.assertRaisesRegex(ValueError, "key"):
            resources.analyze("VP_FRAME_RESOURCE key=1000,32 frame=1")

    def window(self, schema=2):
        return {"profile_schema": schema, "actual_window_us": [0, 1000],
                "frames": 10, "epoch": 1, "measured_root_us": 600}

    def test_nested_original_and_worker_overlap_are_not_added(self):
        sdk = sample(2, 1, 200, 800, 600, 500) + sample(0, 0, 100, 1000, 300, 50)
        gxm = "VP_FRAME_COST_ROOT schema=2 epoch=1 cat=entry entry_us=300 t_us=900 frames=1\n"
        row = module.analyze(sdk, gxm, [self.window()])["windows"][0]
        self.assertEqual(row["original_root_union_us"], 900)
        self.assertEqual(row["timeline"], {
            "worker_us": 600, "original_outside_worker_us": 300,
            "original_worker_overlap_us": 600, "neither_us": 100,
            "covered_percent": 90.0, "flat_sum_us": 1000})
        site = row["sites"][module.SITES[0]]
        self.assertEqual(site["self_without_bridge_us_per_frame"], 25)

    def test_union_clips_boundaries_and_merges_overlap(self):
        self.assertEqual(module.union([(-10, 15), (5, 20), (25, 70)], 0, 30),
                         [(0, 20), (25, 30)])

    def test_missing_journal_rejected_for_new_schema(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            module.analyze(sample(0, 0, 100, 1000, 900), "", [self.window()])
        result = module.analyze(sample(0, 0, 100, 1000, 900), "", [self.window(1)])
        self.assertIsNone(result["windows"][0]["timeline"])

    def test_incomplete_journal_rejected(self):
        with self.assertRaisesRegex(ValueError, "cumulative"):
            module.analyze(sample(0, 0, 100, 1000, 900),
                "VP_FRAME_COST_ROOT epoch=1 entry_us=300 t_us=800 frames=1\n",
                [self.window()])

    def test_original_bridge_cannot_exceed_self(self):
        with self.assertRaisesRegex(ValueError, "invalid"):
            module.analyze(sample(0, 0, 100, 1000, 100, 101), "", [self.window(1)])


if __name__ == "__main__":
    unittest.main()

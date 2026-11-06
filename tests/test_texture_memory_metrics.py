"""Reject misleading texture-memory experiment evidence."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "engine"))
from analyze_texture_memory import memory_window, validate_runtime_errors, COUNTERS, MODES

MIB = 1024 * 1024
WINDOW = {"actual_window_us": [100, 500], "frames": 128, "snapshot_seq": [1, 5]}


def samples(mode):
    _, memory_mode, budget = MODES[mode]
    split = int(mode == 8)
    physical = 96 * MIB if mode == 7 else 64 * MIB
    cached = 32 * MIB if split else 0
    rows = []
    for index in range(5):
        row = dict(t_us=100 + index * 100, frames=320 + index * 32,
                   memory_mode=memory_mode, budget=budget, split=split,
                   raw_class=split, raw_cached=split, raw_mapped=1 - split,
                   decoded_mapped=1, physical_bytes=physical, peak_bytes=physical,
                   raw_cached_bytes=cached, decoded_mapped_bytes=physical - cached,
                   raw_cached_alloc_bytes=cached,
                   decoded_mapped_alloc_bytes=physical - cached - 4096,
                   pool_used_bytes=physical - 4096, pool_peak_used_bytes=physical,
                   pool_live=90, pool_peak_live=91, region_failed=0,
                   user_free=0, cdram_free=100 * MIB)
        row.update({key: index for key in COUNTERS})
        rows.append(row)
    return rows


def logs(rows, mode, extra=""):
    gxm = "\n".join("VP_TEX_MEMORY_METRICS " + " ".join(
        f"{key}={value}" for key, value in row.items()) for row in rows)
    sdk = f"ENGINE_PREPARE_PHASE mode={MODES[mode][0]} t_us=50\n" + extra
    return gxm, sdk


class MemoryMetricsTests(unittest.TestCase):
    def test_game_fatal_is_rejected_even_if_emulator_later_exits_zero(self):
        for failure in ("SDK_THREAD_POOL_FATAL ", "VP_TEX_POOL_MEMORY_FAIL ",
                        "VP_TEX_POOL_HEAP_TYPE_FAIL ", "VP_GPU_ENTRY_REJECT "):
            with self.assertRaisesRegex(ValueError, "runtime reported"):
                validate_runtime_errors("", failure + "action=process-exit")

    def test_large_budget_proves_resident_over_64(self):
        result = memory_window(*logs(samples(7), 7), WINDOW, 7)
        self.assertTrue(result["resident_over_64_mib"])
        self.assertEqual(result["physical_bytes"], {"min": 96 * MIB, "max": 96 * MIB})
        self.assertEqual(result["counters_delta"]["region_kernel_allocs"], 4)

    def test_split_uses_shared_budget_and_cached_raw(self):
        result = memory_window(*logs(samples(8), 8), WINDOW, 8)
        self.assertEqual(result["budget_bytes"], 64 * MIB)
        self.assertEqual(result["raw_cached_bytes"]["min"], 32 * MIB)
        self.assertFalse(result["resident_over_64_mib"])

    def test_over_budget_and_wrong_placement_rejected(self):
        rows = samples(8)
        rows[2]["physical_bytes"] = 80 * MIB
        with self.assertRaisesRegex(ValueError, "physical memory"):
            memory_window(*logs(rows, 8), WINDOW, 8)
        rows = samples(8)
        rows[2]["raw_mapped"] = 1
        with self.assertRaisesRegex(ValueError, "placement"):
            memory_window(*logs(rows, 8), WINDOW, 8)

    def test_exact_endpoints_and_complete_snapshots_required(self):
        for rows in (samples(7)[1:], samples(7)[:-1], samples(7)[::2]):
            with self.assertRaises(ValueError):
                memory_window(*logs(rows, 7), WINDOW, 7)

    def test_cross_mode_and_counter_reset_rejected(self):
        rows = samples(7)
        with self.assertRaisesRegex(ValueError, "phase"):
            memory_window(*logs(rows, 7, "ENGINE_PREPARE_PHASE mode=deep t_us=300\n"),
                          WINDOW, 7)
        rows[3]["pool_evictions"] = 0
        with self.assertRaisesRegex(ValueError, "counter reset"):
            memory_window(*logs(rows, 7), WINDOW, 7)

    def test_logical_and_physical_occupancy_must_reconcile(self):
        rows = samples(4)
        rows[1]["pool_used_bytes"] -= 4096
        with self.assertRaisesRegex(ValueError, "occupancy"):
            memory_window(*logs(rows, 4), WINDOW, 4)


if __name__ == "__main__":
    unittest.main()

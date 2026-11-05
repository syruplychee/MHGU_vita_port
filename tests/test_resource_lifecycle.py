"""Do not attribute reused texture addresses to stale resource owners."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "engine"))
from analyze_resource_lifecycle import analyze


def event(seq, kind="load-commit", obj=0x1000, name="font-atlas"):
    return (f"RESOURCE_LIFECYCLE event={seq} kind={kind} object={obj:#x} "
            f"lr=0x100 sp=0x200 thread=0x1 vptr=0x300 name={name} gpu=0x2000 "
            "width=1024 height=1024 depth=1 lod=0")


def gpu(seq):
    words = [0] * 132
    words[46:48] = [0x200, 5]
    return f"RESOURCE_GPU_OBJECT event={seq} gpu=0x2000 words=" + ",".join(f"{w:08x}" for w in words)


def requests():
    return "\n".join(f"VP_FRAME_RESOURCE frame={frame} "
                     "key=500000200,1024,1024,8,3,1024,4,123,1,10,0 "
                     "raw_bytes=4194304 output_bytes=4194304 hit=0 changed=0 live_keys=0 gpu_only=0"
                     for frame in (1, 2, 3))


class LifecycleTests(unittest.TestCase):
    def test_join_current_load(self):
        report = analyze("\n".join([event(1), gpu(1), requests()]))
        self.assertTrue(report["all_large_associated"])
        self.assertTrue(report["repeated_large_rgba8"][0]["unique_live_object"])
        self.assertFalse(report["complete_content_write_coverage"])

    def test_all_observed_removal_paths_invalidate(self):
        for kind in ("load-start", "replace", "release", "destroy", "destroy-delete"):
            with self.subTest(kind=kind):
                report = analyze("\n".join([event(1), gpu(1), event(2, kind), requests()]))
                self.assertFalse(report["all_large_associated"])

    def test_reused_va_requires_old_owner_removal(self):
        old = [event(1, name="old-atlas"), gpu(1)]
        new = [event(3, obj=0x3000, name="new-atlas"), gpu(3), requests()]
        self.assertFalse(analyze("\n".join(old + new))["all_large_associated"])
        report = analyze("\n".join(old + [event(2, "destroy-delete")] + new))
        self.assertTrue(report["all_large_associated"])
        self.assertEqual(report["repeated_large_rgba8"][0]["owner_requests"], {"new-atlas": 1})

    def test_same_name_does_not_prove_unique_object(self):
        report = analyze("\n".join([event(1), gpu(1), event(2, obj=0x3000), gpu(2), requests()]))
        self.assertTrue(report["all_large_associated"])
        self.assertFalse(report["repeated_large_rgba8"][0]["unique_live_object"])

    def test_truncated_snapshot_cannot_prove_ownership(self):
        report = analyze("\n".join([event(1), gpu(1)[:-9], requests()]))
        self.assertFalse(report["all_large_associated"])
        self.assertEqual(report["malformed_records"], {"gpu_word_count": 1})


if __name__ == "__main__":
    unittest.main()

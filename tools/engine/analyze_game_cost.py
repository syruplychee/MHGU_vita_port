"""Reconcile original A32 function intervals with single-worker cost snapshots.

All intervals use the same game process clock. Original function wall includes
blocking and overlaps worker execution. Interval unions, rather than sums of
threads or parent/child times, provide the frame coverage calculation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics

FIELDS = re.compile(r"([a-z_0-9]+)=([^\s]+)")
SITES = ("frame-stages@3da8fc", "command-submit@bbe154",
         "queue-record@bbe66c", "frame-end@bc0874")


def union(intervals, low, high):
    result = []
    for start, end in sorted(intervals):
        start, end = max(start, low), min(end, high)
        if end <= start:
            continue
        if result and start <= result[-1][1]:
            result[-1] = result[-1][0], max(result[-1][1], end)
        else:
            result.append((start, end))
    return result


def duration(intervals):
    return sum(end - start for start, end in intervals)


def analyze(sdk_text, gxm_text, windows):
    if "GAME_COST_ERROR " in sdk_text:
        raise ValueError("original-game probe reported an accounting error")
    samples = []
    for line in sdk_text.splitlines():
        if not line.startswith("GAME_COST_TIME "):
            continue
        raw = dict(FIELDS.findall(line))
        row = {key: int(raw[key], 0) for key in raw}
        if (row["site"] not in range(len(SITES)) or
                row["t_us"] - row["entry_us"] != row["incl_us"] or
                not 0 <= row["self_us"] <= row["incl_us"] or
                row.get("bridge_us", 0) > row["self_us"]):
            raise ValueError("invalid original-game timing row")
        samples.append(row)
    if not samples:
        raise ValueError("original-game timings missing")
    roots = []
    for line in gxm_text.splitlines():
        if line.startswith("VP_FRAME_COST_ROOT "):
            raw = dict(FIELDS.findall(line))
            start, end = int(raw["entry_us"]), int(raw["t_us"])
            if end < start:
                raise ValueError("worker root clock regressed")
            roots.append((int(raw["epoch"]), start, end))
    result = []
    for window in windows:
        low, high = window["actual_window_us"]
        frames = window["frames"]
        row = {"window_us": [low, high], "frames": frames, "sites": {}}
        for site, name in enumerate(SITES):
            selected = [sample for sample in samples if sample["site"] == site
                        and low <= sample["entry_us"] <= sample["t_us"] <= high]
            if not selected:
                row["sites"][name] = {"completed_calls": 0}
                continue
            values = {"completed_calls": len(selected),
                      "slots": sorted({s["slot"] for s in selected}),
                      "contexts": sorted({hex(s["context"]) for s in selected}),
                      "callers": sorted({hex(s["caller"]) for s in selected})}
            for key in ("incl_us", "self_us", "bridge_us", "bridge_calls"):
                if all(key in sample for sample in selected):
                    values[key + "_per_frame"] = sum(s[key] for s in selected) / frames
            values["incl_median_us_per_call"] = statistics.median(s["incl_us"] for s in selected)
            if "bridge_us_per_frame" in values:
                values["self_without_bridge_us_per_frame"] = (
                    values["self_us_per_frame"] - values["bridge_us_per_frame"])
            row["sites"][name] = values
        original = union([(s["entry_us"], s["t_us"]) for s in samples
                          if s["depth"] == 0], low, high)
        row["original_root_union_us"] = duration(original)
        matching_roots = [(start, end) for epoch, start, end in roots
                          if epoch == window["epoch"] and end > low and start < high]
        if matching_roots:
            worker = union(matching_roots, low, high)
            worker_us = duration(worker)
            if worker_us != window["measured_root_us"]:
                raise ValueError("worker interval journal does not match cumulative root difference")
            if sum(min(end, high) - max(start, low) for start, end in matching_roots) != worker_us:
                raise ValueError("worker root intervals overlap")
            both = union(original + worker, low, high)
            coverage = duration(both)
            original_only = coverage - worker_us
            overlap = duration(original) + worker_us - coverage
            row["timeline"] = {
                "worker_us": worker_us,
                "original_outside_worker_us": original_only,
                "original_worker_overlap_us": overlap,
                "neither_us": high - low - coverage,
                "covered_percent": 100 * coverage / (high - low),
                "flat_sum_us": worker_us + original_only + high - low - coverage,
            }
        elif window.get("profile_schema", 1) >= 2:
            raise ValueError("worker interval journal missing for schema 2")
        else:
            row["timeline"] = None
        result.append(row)
    return {"windows": result, "limitations": [
        "original function intervals include blocking, bridge and scheduling wall time",
        "self subtracts only nested original-function probes on the same executor",
        "bridge time is only available in the second probe revision",
        "self without bridge remains wall time, not CPU-cycle or OS-thread CPU time",
        "per-site costs exclude boundary-crossing calls; unions clip intervals exactly",
        "original and worker overlap must never be added as independent frame costs",
        "a matched worker journal proves coverage, not semantic coverage of all gameplay"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--cost-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    cost = json.loads(args.cost_report.read_text())
    if not cost["performance_pass"]:
        raise ValueError("cost report lacks measurement/exit/restoration evidence")
    paths = [args.run / "sdk-dso.log", args.run / "gxm-runtime.log", args.cost_report]
    report = analyze(paths[0].read_text(encoding="utf-8"),
                     paths[1].read_text(encoding="utf-8"), cost["windows"])
    report["inputs"] = [{"path": str(p.resolve()),
                         "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in paths]
    report["tool_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

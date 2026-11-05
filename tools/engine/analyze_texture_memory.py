"""Validate texture-memory A/B windows against exact frame-cost endpoints.

The report describes observed simulator timings. Visual review and hardware
validation remain separate; this tool never promotes an experiment by itself.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

import analyze_frame_cost as frame_cost

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "diagnostics"))
from measure_frame_window import measure, quantile

FIELDS = re.compile(r"([a-z_0-9]+)=([^\s]+)")
MODES = {4: ("deep", 0, 64 * 1024 * 1024),
         7: ("deep-large", 1, 128 * 1024 * 1024),
         8: ("deep-split", 2, 64 * 1024 * 1024)}
COUNTERS = ("region_kernel_allocs", "region_kernel_frees", "pool_evictions",
            "pool_alloc_map", "pool_free_map", "pressure", "nomem")


def memory_window(gxm, sdk, window, mode):
    name, memory_mode, budget = MODES[mode]
    low, high = window["actual_window_us"]
    rows = []
    for line in gxm.splitlines():
        if not line.startswith("VP_TEX_MEMORY_METRICS "):
            continue
        row = {key: int(value) for key, value in FIELDS.findall(line)}
        if low <= row["t_us"] <= high:
            rows.append(row)
    if len(rows) < 2 or rows[0]["t_us"] != low or rows[-1]["t_us"] != high:
        raise ValueError("memory metrics missing at exact cost endpoints")
    if len(rows) != window["snapshot_seq"][1] - window["snapshot_seq"][0] + 1:
        raise ValueError("memory metrics do not cover every cost snapshot")
    for index, row in enumerate(rows):
        split = int(mode == 8)
        if (row["memory_mode"] != memory_mode or row["budget"] != budget or
                row["split"] != split or row["raw_class"] != split or
                row["raw_cached"] != split or row["raw_mapped"] != 1 - split or
                row["decoded_mapped"] != 1):
            raise ValueError("memory mode or placement differs from planned mode")
        if index and (row["frames"] - rows[index - 1]["frames"] != 32 or
                      row["t_us"] <= rows[index - 1]["t_us"]):
            raise ValueError("memory snapshot missing or regressed")
        if (row["physical_bytes"] != row["raw_cached_bytes"] +
                row["decoded_mapped_bytes"] or
                not 0 < row["physical_bytes"] <= row["peak_bytes"] <= budget):
            raise ValueError("combined physical memory exceeds budget or fails reconciliation")
        if (row["pool_used_bytes"] != row["raw_cached_alloc_bytes"] +
                row["decoded_mapped_alloc_bytes"] or
                not 0 < row["pool_used_bytes"] <= row["physical_bytes"] or
                not row["pool_used_bytes"] <= row["pool_peak_used_bytes"] <= budget or
                not 0 < row["pool_live"] <= row["pool_peak_live"] <= 128):
            raise ValueError("pool occupancy does not match region suballocations")
        if (row["region_failed"] or row["raw_cached_alloc_bytes"] > row["raw_cached_bytes"] or
                row["decoded_mapped_alloc_bytes"] > row["decoded_mapped_bytes"]):
            raise ValueError("region failure or class occupancy mismatch")
        if (split and not row["raw_cached_alloc_bytes"]) or (
                not split and row["raw_cached_bytes"]):
            raise ValueError("cached raw residency differs from mode")
        if index and any(row[key] < rows[index - 1][key] for key in COUNTERS):
            raise ValueError("memory counter reset inside measured window")
    if rows[-1]["frames"] - rows[0]["frames"] != window["frames"]:
        raise ValueError("memory and cost frame counts differ")
    phases = []
    for line in sdk.splitlines():
        if line.startswith("ENGINE_PREPARE_PHASE "):
            fields = dict(FIELDS.findall(line))
            phases.append((int(fields["t_us"]), fields["mode"]))
    before = [phase for phase in phases if phase[0] <= low]
    if not before or before[-1][1] != name or any(low < t <= high for t, _ in phases):
        raise ValueError("engine phase differs or changed inside measured window")
    result = {"mode": mode, "memory_mode": memory_mode, "budget_bytes": budget,
              "snapshots": len(rows), "counters_delta": {
                  key: rows[-1][key] - rows[0][key] for key in COUNTERS}}
    for key in ("physical_bytes", "raw_cached_bytes", "decoded_mapped_bytes",
                "pool_used_bytes", "pool_live", "user_free", "cdram_free"):
        result[key] = {"min": min(row[key] for row in rows),
                       "max": max(row[key] for row in rows)}
    result["resident_over_64_mib"] = result["physical_bytes"]["min"] > 64 * 1024 * 1024
    return result


def analyze(run, plan):
    raw = (run / "gxm-runtime.log").read_bytes()
    gxm = raw.decode("utf-8")
    sdk = (run / "sdk-dso.log").read_text(encoding="utf-8")
    for failure in ("VP_TEX_POOL_MODE_FAIL ", "VP_TEX_POOL_MEMORY_FAIL ",
                    "ENGINE_PREPARE_CONTROL_ERROR "):
        if failure in sdk or failure in gxm:
            raise ValueError(f"runtime reported {failure.strip()}")
    windows = [(int(phase["start_s"] * 1000000), int(phase["end_s"] * 1000000))
               for phase in plan]
    cost = frame_cost.analyze(
        gxm, windows,
        process_evidence=frame_cost._evidence_from_file(run / "process-lifecycle.json"),
        save_evidence=frame_cost._evidence_from_file(run / "save-protection.json"),
        source_path=run / "gxm-runtime.log", gxm_sha256=hashlib.sha256(raw).hexdigest())
    if not cost["performance_pass"]:
        raise ValueError("cost windows lack strict measurement/restoration evidence")
    phases = []
    for phase, window in zip(plan, cost["windows"]):
        memory = memory_window(gxm, sdk, window, phase["mode"])
        low, high = window["actual_window_us"]
        histogram = measure(run, (low - 1) / 1000000, (high + 1) / 1000000)
        if (histogram["frames"] != window["frames"] or
                abs(histogram["mean_ms"] * 1000 - window["frame_wall_us"]) > 1e-6):
            raise ValueError("histogram and cost window differ")
        histogram["p99"] = quantile(histogram["histogram"], .99)
        phases.append({"plan": phase, "memory": memory, "cost": window,
                       "frame_intervals": histogram})
    groups = {}
    for name in dict.fromkeys(phase["group"] for phase in plan):
        selected = [phase for phase in phases if phase["plan"]["group"] == name]
        modes = [phase["plan"]["mode"] for phase in selected]
        if len(selected) != 4 or modes[0] != 4 or modes[2] != 4 or (
                modes[1] not in (7, 8) or modes[1] != modes[3]):
            raise ValueError("each experiment must contain baseline/candidate/baseline/candidate")
        aggregate = {}
        for mode in (4, modes[1]):
            same = [phase for phase in selected if phase["plan"]["mode"] == mode]
            frames = sum(phase["cost"]["frames"] for phase in same)
            wall = sum(phase["cost"]["wall_us"] for phase in same)
            aggregate[str(mode)] = {"frames": frames, "mean_ms": wall / frames / 1000}
        pair_changes = [100 * (selected[i + 1]["frame_intervals"]["mean_ms"] /
                              selected[i]["frame_intervals"]["mean_ms"] - 1)
                        for i in (0, 2)]
        change = 100 * (aggregate[str(modes[1])]["mean_ms"] /
                        aggregate["4"]["mean_ms"] - 1)
        groups[name] = {"aggregate": aggregate, "frame_time_change_percent": change,
                        "pair_frame_time_change_percent": pair_changes}
    return {"measurements_valid": True, "speedup_claim": False,
            "phases": phases, "groups": groups,
            "limitations": ["visual review is separate", "Vita3K is not real hardware",
                            "probe overhead is present in every phase",
                            "unified mapped byte count includes both raw and decoded",
                            "cached API allocation does not prove emulated CPU cache behavior"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = analyze(args.run, json.loads(args.plan.read_text(encoding="utf-8-sig")))
    paths = [args.plan, args.run / "gxm-runtime.log", args.run / "sdk-dso.log",
             args.run / "process.json", args.run / "process-lifecycle.json",
             args.run / "save-protection.json"]
    report["inputs"] = [{"path": str(path.resolve()),
                         "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                        for path in paths]
    report["tool_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report["groups"], indent=2))


if __name__ == "__main__":
    main()

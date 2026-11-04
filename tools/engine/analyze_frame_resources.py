"""Summarize bounded metadata-only texture lookup captures.

Repeated complete keys prove repeated identity requests, not unchanged pixels.
Only a real resident byte comparison proves a raw-content change or hit.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re

FIELDS = re.compile(r"([a-z_0-9]+)=([^\s]+)")


def analyze(text):
    rows = []
    for line in text.splitlines():
        if not line.startswith("VP_FRAME_RESOURCE "):
            continue
        raw = dict(FIELDS.findall(line))
        key = raw["key"].split(",")
        if len(key) != 11:
            raise ValueError("incomplete resource key")
        identity = (int(key[0], 16),) + tuple(int(v) for v in key[1:])
        if any(value < 0 for value in identity):
            raise ValueError("negative resource key field")
        row = {name: int(raw[name]) for name in
               ("frame", "raw_bytes", "output_bytes", "hit", "changed", "live_keys", "gpu_only")}
        if min(row.values()) < 0 or row["hit"] not in (0, 1):
            raise ValueError("invalid resource row")
        row["key"] = identity + (row["raw_bytes"],)
        rows.append(row)
    if not rows:
        raise ValueError("bounded resource capture missing")
    frame_ids = sorted({r["frame"] for r in rows})
    segments = []
    for frame in frame_ids:
        if not segments or frame > segments[-1][-1] + 1:
            segments.append([frame])
        else:
            segments[-1].append(frame)
    complete_frames = {frame for segment in segments for frame in segment[1:-1]}
    if not complete_frames:
        raise ValueError("no interior frames after excluding capture boundaries")
    selected = [r for r in rows if r["frame"] in complete_frames]
    groups = defaultdict(list)
    for row in selected:
        groups[row["key"]].append(row)
    resources = []
    for key, events in groups.items():
        first_frame = events[0]["frame"]
        misses = [r for r in events if not r["hit"]]
        absent = [r for r in misses if not r["changed"] and not r["gpu_only"]]
        resources.append({
            "key": [hex(key[0]), *key[1:-1]], "raw_bytes": key[-1], "requests": len(events),
            "frames_requested": len({r["frame"] for r in events}),
            "hits": sum(r["hit"] for r in events), "misses": len(misses),
            "resident_changed_misses": sum(bool(r["changed"]) for r in misses),
            "gpu_only_misses": sum(not r["changed"] and bool(r["gpu_only"]) for r in misses),
            "absent_misses": len(absent),
            "absent_after_earlier_frame_request": sum(r["frame"] > first_frame for r in absent),
            "decoded_output_bytes_on_miss": sum(r["output_bytes"] for r in misses),
            "raw_snapshot_bytes_on_miss": sum(r["raw_bytes"] for r in misses),
        })
    resources.sort(key=lambda r: r["decoded_output_bytes_on_miss"], reverse=True)
    totals = {name: sum(r[name] for r in resources) for name in
              ("requests", "hits", "misses", "resident_changed_misses", "gpu_only_misses",
               "absent_misses", "absent_after_earlier_frame_request",
               "decoded_output_bytes_on_miss", "raw_snapshot_bytes_on_miss")}
    if totals["misses"] != sum(totals[k] for k in
                               ("resident_changed_misses", "gpu_only_misses", "absent_misses")):
        raise ValueError("resource miss reasons do not reconcile")
    return {"capture_segments": [{"first_frame": s[0], "last_frame": s[-1]} for s in segments],
            "interior_frames": len(complete_frames), "unique_keys": len(resources),
            "totals": totals, "per_interior_frame": {k: v / len(complete_frames) for k, v in totals.items()},
            "resources": resources, "limitations": [
                "first and last frame of each contiguous capture are excluded as partial",
                "capture logging is diagnostic and excluded from performance windows",
                "repeated key and absent resident version do not prove unchanged source bytes",
                "absent can mean eviction, retired raw identity, or a new version/address",
                "GPU-only versions remain alive and cannot be released early",
                "metadata capture does not contain pixels or establish whole-game equivalence"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    raw = args.sdk_log.read_bytes()
    report = analyze(raw.decode("utf-8"))
    report["input"] = {"path": str(args.sdk_log.resolve()), "sha256": hashlib.sha256(raw).hexdigest()}
    report["tool_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "resources"}, indent=2))


if __name__ == "__main__":
    main()

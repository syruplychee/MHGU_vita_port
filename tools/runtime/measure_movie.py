#!/usr/bin/env python3
"""Measure logged movie event cadence, without claiming displayed frame rate."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import statistics
from pathlib import Path


SCOPE = "decoder-and-compositor-cadence-only; not display or audio verification"
EVENT_RE = re.compile(r"VP_MEDIA_VIDEO_(DECODE_TIME|COMPOSITE_TIME|READBACK)\b")
FIELD_RE = re.compile(r"\b([a-z_]+)=(-?\d+)\b")


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def distribution(values: list[float]) -> dict:
    if not values:
        return {"available": False, "samples": 0}
    return {
        "available": True,
        "samples": len(values),
        "mean_ms": statistics.mean(values),
        "median_ms": statistics.median(values),
        "p95_ms": percentile(values, 0.95),
        "p99_ms": percentile(values, 0.99),
    }


def event_summary(records: list[dict]) -> dict:
    gaps = []
    intervals = []
    for previous, current in zip(records, records[1:]):
        step = current["n"] - previous["n"]
        elapsed = current["t_us"] - previous["t_us"]
        if step != 1:
            gaps.append({"after_n": previous["n"], "before_n": current["n"],
                         "missing": max(0, step - 1)})
        elif elapsed > 0:
            intervals.append(elapsed / 1000)
    first = records[0] if records else None
    last = records[-1] if records else None
    elapsed_s = (last["t_us"] - first["t_us"]) / 1_000_000 if len(records) >= 2 else None
    available = elapsed_s is not None and elapsed_s > 0 and last["n"] > first["n"]
    result = {
        "available": available,
        "samples": len(records),
        "first_n": first["n"] if first else None,
        "last_n": last["n"] if last else None,
        "first_us": first["t_us"] if first else None,
        "last_us": last["t_us"] if last else None,
        "elapsed_s": elapsed_s,
        "event_count_rate_hz": (last["n"] - first["n"]) / elapsed_s if available else None,
        "gaps": gaps,
        "intervals": {**distribution(intervals), "partial": bool(gaps)},
    }
    if not available:
        result["reason"] = "At least two distinct successful counters with positive elapsed time are required."
    for field in ("total_us", "api_us", "copy_us"):
        values = [record[field] / 1000 for record in records if field in record]
        result[field.replace("_us", "_mean_ms")] = statistics.mean(values) if values else None
    return result


def measure(log: Path, start: float = 0, end: float | None = None) -> dict:
    """Select an inclusive process-time window, deduplicating before selection."""
    if not math.isfinite(start) or start < 0 or (end is not None and
            (not math.isfinite(end) or end < start)):
        raise ValueError("Window must satisfy 0 <= start <= end with finite seconds.")
    content = log.read_bytes()
    selected = {kind: [] for kind in ("DECODE_TIME", "COMPOSITE_TIME", "READBACK")}
    seen = {kind: set() for kind in selected}
    for line in content.decode("utf-8", errors="replace").splitlines():
        event = EVENT_RE.search(line)
        if not event:
            continue
        kind = event.group(1)
        fields = {key: int(value) for key, value in FIELD_RE.findall(line[event.end():])}
        required = {"n", "t_us", "total_us", "rows"} if kind == "READBACK" else {
            "n", "t_us", "total_us", "rc"}
        if not required <= fields.keys() or fields["n"] <= 0 or fields["total_us"] < 0:
            continue
        if kind != "READBACK" and fields["rc"] != 0:
            continue
        if fields["n"] in seen[kind]:
            continue
        seen[kind].add(fields["n"])
        seconds = fields["t_us"] / 1_000_000
        if seconds >= start and (end is None or seconds <= end):
            selected[kind].append(fields)
    decoder = event_summary(selected["DECODE_TIME"])
    compositor = event_summary(selected["COMPOSITE_TIME"])
    grouped = {}
    for record in selected["READBACK"]:
        grouped.setdefault(record["rows"], []).append(record["total_us"] / 1000)
    return {
        "available": decoder["available"] and compositor["available"],
        "scope": SCOPE,
        "source_log": str(log.resolve()),
        "source_sha256": hashlib.sha256(content).hexdigest(),
        "window": {"start_s": start, "end_s": end, "inclusive": True},
        "decoder": decoder,
        "compositor": compositor,
        "readback": {
            "available": bool(grouped),
            "samples": len(selected["READBACK"]),
            "unit": "plane copies; Y and UV copies are not separate frames",
            "by_rows": {str(rows): distribution(values) for rows, values in sorted(grouped.items())},
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--start", type=float, default=0)
    parser.add_argument("--end", type=float)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()
    try:
        result = measure(args.log, args.start, args.end)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    rendered = json.dumps(result, indent=None if args.compact else 2, ensure_ascii=False)
    if args.output:
        args.output.write_text(rendered + "\n", encoding="utf-8", newline="\n")
    print(rendered)


if __name__ == "__main__":
    main()

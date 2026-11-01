#!/usr/bin/env python3
"""Summarize completed VP_MOVIE_COLOR_TIME probe records.

The probe is opt-in and logs the platform's original YUV conversion calls
without replacing them. Each record measures wall time between the probe's
entry and exit points; that interval includes guest scheduling and bridge
work. It is not CPU time, does not verify pixels, and is not a frame rate.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import statistics
from pathlib import Path


SCOPE = ("entry-to-exit wall time of platform color-conversion calls; includes "
         "guest scheduling and bridge work; not CPU time and not display frame rate")
MODES = ("rgb", "nv12")
SAMPLING_ALL_THROUGH = 2000
SAMPLING_EVERY = 32
EVENT_RE = re.compile(r"\bVP_MOVIE_COLOR_TIME\b")
FIELD_RE = re.compile(r"\b([a-z_]+)=([^\s]+)")
PROBE_MODE_RE = re.compile(r"\bMOVIE_COLOR_PROBE_MODE\b")
REJECT_RE = re.compile(r"\bMOVIE_COLOR_PROBE_REJECT\b")
PATCH_RE = re.compile(r"\bMOVIE_COLOR_PROBE_PATCH\b")


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def distribution_ms(values: list[float]) -> dict:
    if not values:
        return {"available": False, "samples": 0}
    return {
        "available": True,
        "samples": len(values),
        "mean_ms": statistics.mean(values),
        "median_ms": statistics.median(values),
        "p95_ms": percentile(values, 0.95),
        "p99_ms": percentile(values, 0.99),
        "min_ms": min(values),
        "max_ms": max(values),
    }


def parse_record(line: str) -> dict | None:
    """Return a validated completion record, or None for unrelated/invalid lines."""
    event = EVENT_RE.search(line)
    if not event:
        return None
    fields = dict(FIELD_RE.findall(line[event.end():]))
    mode = fields.get("mode")
    if mode not in MODES:
        return None
    try:
        record = {
            "n": int(fields["n"]),
            "mode": mode,
            "width": int(fields["width"]),
            "height": int(fields["height"]),
            "bytes": int(fields["bytes"]),
            "elapsed_us": int(fields["elapsed_us"]),
            "entry_us": int(fields["entry_us"]),
            "t_us": int(fields["t_us"]),
        }
    except (KeyError, ValueError):
        return None
    if (record["n"] <= 0 or record["width"] <= 0 or record["height"] <= 0
            or record["bytes"] < 0 or record["elapsed_us"] < 0
            or record["entry_us"] < 0 or record["t_us"] < record["entry_us"]):
        return None
    raw_thread = fields.get("thread")
    if raw_thread:
        try:
            record["thread"] = int(raw_thread, 16)
        except ValueError:
            pass
    return record


def probe_status(lines: list[str]) -> dict:
    enabled = None
    rejected = False
    patches = []
    for line in lines:
        if PROBE_MODE_RE.search(line):
            match = re.search(r"\benabled=(\d+)\b", line)
            if match:
                enabled = match.group(1) == "1"
        if REJECT_RE.search(line):
            rejected = True
        if PATCH_RE.search(line):
            site = re.search(r"\bsite=(0x[0-9a-fA-F]+)\b", line)
            status = re.search(r"\bstatus=(-?\d+)\b", line)
            patches.append({
                "site": site.group(1) if site else None,
                "status": int(status.group(1)) if status else None,
            })
    accepted = None
    if enabled is not None and not rejected:
        accepted = enabled and all(item["status"] == 0 for item in patches)
        if enabled and not patches:
            accepted = None
    return {
        "enabled": enabled,
        "accepted": accepted,
        "rejected": rejected,
        "patch_sites": patches,
    }


def mode_stats(records: list[dict]) -> dict:
    if not records:
        return {"available": False, "samples": 0}
    result = {
        "available": True,
        "samples": len(records),
        "width": statistics.mode([record["width"] for record in records]),
        "height": statistics.mode([record["height"] for record in records]),
        "bytes_min": min(record["bytes"] for record in records),
        "bytes_max": max(record["bytes"] for record in records),
        "elapsed_ms": distribution_ms([record["elapsed_us"] / 1000 for record in records]),
    }
    threads = sorted({record["thread"] for record in records if "thread" in record})
    if threads:
        result["threads"] = [f"0x{thread:x}" for thread in threads]
    return result


def completion_summary(records: list[dict]) -> dict:
    if len(records) < 2:
        return {
            "available": False,
            "reason": "At least two completed conversions with positive elapsed time are required.",
            "scope": "rgb plus nv12 completions; not frames",
        }
    ordered = sorted(records, key=lambda record: record["n"])
    first = ordered[0]
    last = ordered[-1]
    elapsed_s = (last["t_us"] - first["t_us"]) / 1_000_000
    available = elapsed_s > 0 and last["n"] > first["n"]
    return {
        "available": available,
        "scope": "rgb plus nv12 completions; not frames",
        "first_n": first["n"],
        "last_n": last["n"],
        "elapsed_s": elapsed_s if elapsed_s > 0 else None,
        "rate_hz": (last["n"] - first["n"]) / elapsed_s if available else None,
    }


def sampling_summary(records: list[dict], duplicates: int) -> dict:
    if not records:
        return {"logged_samples": 0, "duplicates": duplicates}
    ordered = sorted(records, key=lambda record: record["n"])
    last_n = ordered[-1]["n"]
    span = ordered[-1]["n"] - ordered[0]["n"] + 1
    if last_n <= SAMPLING_ALL_THROUGH:
        regime = f"all completions through n={SAMPLING_ALL_THROUGH}"
    else:
        regime = (f"all completions through n={SAMPLING_ALL_THROUGH}, "
                  f"then every {SAMPLING_EVERY}th completion")
    return {
        "logged_samples": len(records),
        "duplicates": duplicates,
        "first_n": ordered[0]["n"],
        "last_n": last_n,
        "completion_span": span,
        "unlogged_completions": max(0, span - len(records)),
        "regime": regime,
        "note": ("Logged sample counts are not conversion totals after the "
                 "sampling cutoff; completion rate uses the n counter, not line count."),
    }


def unavailable_reason(status: dict) -> str:
    if status["enabled"] is False:
        return ("The opt-in media-color-probe marker was absent at startup; "
                "the original conversion functions ran unmodified.")
    if status["rejected"]:
        return ("Probe patch validation was rejected; see MOVIE_COLOR_PROBE_REJECT "
                "and the startup identity checks.")
    return ("No completed VP_MOVIE_COLOR_TIME records were found in the selected "
            "window; the probe may not be on this movie's conversion path.")


def measure(log: Path, start: float = 0, end: float | None = None) -> dict:
    """Select completed records inside an inclusive process-time window."""
    if not math.isfinite(start) or start < 0 or (end is not None and
            (not math.isfinite(end) or end < start)):
        raise ValueError("Window must satisfy 0 <= start <= end with finite seconds.")
    content = log.read_bytes()
    lines = content.decode("utf-8", errors="replace").splitlines()
    selected = []
    seen = set()
    duplicates = 0
    for line in lines:
        record = parse_record(line)
        if record is None:
            continue
        if record["n"] in seen:
            duplicates += 1
            continue
        seen.add(record["n"])
        seconds = record["t_us"] / 1_000_000
        if seconds >= start and (end is None or seconds <= end):
            selected.append(record)
    grouped = {mode: [] for mode in MODES}
    for record in selected:
        grouped[record["mode"]].append(record)
    status = probe_status(lines)
    result = {
        "available": bool(selected),
        "scope": SCOPE,
        "source_log": str(log.resolve()),
        "source_sha256": hashlib.sha256(content).hexdigest(),
        "window": {"start_s": start, "end_s": end, "inclusive": True},
        "probe": status,
        "samples": len(selected),
        "sampling": sampling_summary(selected, duplicates),
        "completion": completion_summary(selected),
        "modes": {mode: mode_stats(grouped[mode]) for mode in MODES},
    }
    if not selected:
        result["reason"] = unavailable_reason(status)
    return result


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

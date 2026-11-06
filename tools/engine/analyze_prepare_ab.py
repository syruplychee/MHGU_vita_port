"""Analyze same-process engine-preparation A/B timing logs.

The runtime emits one ENGINE_PREPARE_TIME line per completed root call. The
phase list is supplied by the caller because only the driver knows when the
control file was changed. All times are the game process clock
(sceKernelGetProcessTimeWide microseconds); no Windows timestamps are mixed in.

Present intervals are derived from cumulative presentation counters by taking
the difference between two endpoints. The absolute counter value is never
reported as an interval count. Two independent endpoint sources are supported:

  * GXM colour VP_GXM_PERF_WINDOW lines (t_us, presents)
  * ENGINE_PREPARE_TIME lines from the SDK-side vi:obj model (t_us, presents)

This is a measurement report, not an automatic performance verdict. The deltas
are descriptive only.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import sys

MODES = ("original", "native", "fast", "shared", "deep", "deep-recycle",
         "deep-clear", "deep-large", "deep-split", "deep-quiet",
         "deep-compact", "deep-expanded", "deep-large-quiet")
FIELD = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")
REQUIRED_PREPARE = (
    "mode", "thread", "context", "entry_us", "t_us", "elapsed_us",
    "encoded_bytes", "growth", "presents",
)


def _fields(line):
    return {match.group(1): match.group(2) for match in FIELD.finditer(line)}


def _integer(fields, name, line):
    try:
        return int(fields[name], 10)
    except (KeyError, ValueError):
        raise ValueError(f"invalid or missing {name}= in log line: {line}") from None


def parse_prepare(text):
    """Return every well-formed ENGINE_PREPARE_TIME record."""
    rows = []
    for line in text.splitlines():
        if "ENGINE_PREPARE_TIME" not in line:
            continue
        fields = _fields(line)
        missing = [name for name in REQUIRED_PREPARE if name not in fields]
        if missing:
            raise ValueError("ENGINE_PREPARE_TIME missing " + ",".join(missing))
        mode = fields["mode"]
        if mode not in MODES:
            raise ValueError(f"unknown preparation mode {mode!r}")
        entry_us = _integer(fields, "entry_us", line)
        t_us = _integer(fields, "t_us", line)
        elapsed_us = _integer(fields, "elapsed_us", line)
        encoded_bytes = _integer(fields, "encoded_bytes", line)
        growth = _integer(fields, "growth", line)
        presents = _integer(fields, "presents", line)
        if t_us < entry_us:
            raise ValueError(f"t_us precedes entry_us: {line}")
        if elapsed_us != t_us - entry_us:
            raise ValueError(f"elapsed_us does not match t_us-entry_us: {line}")
        if growth not in (0, 1):
            raise ValueError(f"growth must be 0 or 1: {line}")
        if encoded_bytes < 0 or presents < 0:
            raise ValueError(f"negative counter: {line}")
        rows.append({
            "mode": mode,
            "thread": fields["thread"],
            "context": fields["context"],
            "entry_us": entry_us,
            "t_us": t_us,
            "elapsed_us": elapsed_us,
            "encoded_bytes": encoded_bytes,
            "growth": growth,
            "presents": presents,
        })
    return rows


def parse_gxm_windows(text):
    """Return cumulative colour draw-window present samples with a timestamp."""
    rows = []
    for line in text.splitlines():
        if "VP_GXM_PERF_WINDOW" not in line:
            continue
        fields = _fields(line)
        if fields.get("kind") != "colour":
            continue
        if "t_us" not in fields or "presents" not in fields:
            raise ValueError("colour VP_GXM_PERF_WINDOW lacks t_us/presents")
        t_us = _integer(fields, "t_us", line)
        presents = _integer(fields, "presents", line)
        if t_us > 0:
            rows.append({"t_us": t_us, "presents": presents})
    return rows


def validate_phases(phases):
    if not isinstance(phases, list) or len(phases) < 4:
        raise ValueError("at least four phases (A/B/A/B) are required")
    rows = []
    for index, item in enumerate(phases):
        if not isinstance(item, dict):
            raise ValueError(f"phase {index} is not an object")
        mode = item.get("mode")
        if mode not in MODES:
            raise ValueError(f"phase {index} has unknown mode {mode!r}")
        start = item.get("start_us")
        end = item.get("end_us")
        if (not isinstance(start, int) or isinstance(start, bool) or
                not isinstance(end, int) or isinstance(end, bool)):
            raise ValueError(f"phase {index} start_us/end_us must be integers")
        if start < 0 or end <= start:
            raise ValueError(f"phase {index} has an invalid time range")
        if rows and start <= rows[-1]["end_us"]:
            raise ValueError(f"phase {index} starts before the previous phase ends")
        rows.append({"mode": mode, "start_us": start, "end_us": end})
    if rows[0]["mode"] == rows[1]["mode"]:
        raise ValueError("phases must alternate between two modes")
    for index in range(2, len(rows)):
        if rows[index]["mode"] == rows[index - 1]["mode"]:
            raise ValueError("phases must alternate between two modes")
        if rows[index]["mode"] != rows[index - 2]["mode"]:
            raise ValueError("phase order is not A/B/A/B")
    return rows


def _contains(phase, t_us):
    return phase["start_us"] <= t_us <= phase["end_us"]


def _phase_index(phases, t_us):
    for index, phase in enumerate(phases):
        if _contains(phase, t_us):
            return index
    return None


def _stats(values):
    if not values:
        raise ValueError("no samples")
    ordered = sorted(values)
    count = len(ordered)

    # p95/p99 use the nearest-rank definition: ceil(fraction * samples).
    def nearest_rank(fraction):
        rank = max(1, math.ceil(fraction * count))
        return ordered[rank - 1]

    return {
        "samples": count,
        "sum": sum(ordered),
        "mean": sum(ordered) / count,
        "median": statistics.median(ordered),
        "p95": nearest_rank(0.95),
        "p99": nearest_rank(0.99),
        "max": ordered[-1],
    }


def _present_endpoints(points, start_us, end_us):
    """Difference two cumulative-counter endpoints inside one window."""
    window = sorted(
        (point for point in points
         if start_us <= point["t_us"] <= end_us),
        key=lambda point: point["t_us"],
    )
    if len(window) < 2:
        return None, "fewer than two present endpoints"
    first, last = window[0], window[-1]
    if last["t_us"] <= first["t_us"]:
        return None, "present timestamps do not advance"
    if last["presents"] < first["presents"]:
        return None, "present counter decreased (not a positive delta)"
    delta_presents = last["presents"] - first["presents"]
    if delta_presents <= 0:
        return None, "present counter did not advance"
    delta_us = last["t_us"] - first["t_us"]
    return {
        "endpoints": len(window),
        "first_t_us": first["t_us"],
        "last_t_us": last["t_us"],
        "first_presents": first["presents"],
        "last_presents": last["presents"],
        "delta_presents": delta_presents,
        "mean_interval_us": delta_us / delta_presents,
        "presents_per_second": delta_presents * 1000000.0 / delta_us,
    }, None


def analyze_logs(sdk_text, gxm_text, phases, warmup_seconds=5.0):
    """Analyze log text and phase descriptions; return a JSON-serializable dict."""
    if warmup_seconds < 0:
        raise ValueError("warmup_seconds must not be negative")
    phases = validate_phases(phases)
    warmup_us = int(round(warmup_seconds * 1000000.0))
    samples = parse_prepare(sdk_text)
    windows = parse_gxm_windows(gxm_text)

    phase_samples = [[] for _ in phases]
    warmup_excluded = [0] * len(phases)
    outside = 0
    for sample in samples:
        entry_phase = _phase_index(phases, sample["entry_us"])
        leave_phase = _phase_index(phases, sample["t_us"])
        if entry_phase is None and leave_phase is None:
            outside += 1
            continue
        if entry_phase != leave_phase:
            raise ValueError(
                "sample crosses a phase boundary: "
                f"entry_us={sample['entry_us']} t_us={sample['t_us']}")
        phase = phases[entry_phase]
        if sample["entry_us"] < phase["start_us"] + warmup_us:
            warmup_excluded[entry_phase] += 1
            continue
        if sample["mode"] != phase["mode"]:
            raise ValueError(
                f"phase {entry_phase} mode={phase['mode']} but sample at "
                f"entry_us={sample['entry_us']} reports mode={sample['mode']}")
        phase_samples[entry_phase].append(sample)

    reports = []
    for index, phase in enumerate(phases):
        rows = sorted(phase_samples[index], key=lambda row: (row["entry_us"], row["t_us"]))
        if not rows:
            raise ValueError(
                f"phase {index} ({phase['mode']}) has no samples after "
                f"{warmup_seconds:g}s warmup")
        stable_start = phase["start_us"] + warmup_us
        gxm_present, gxm_reason = _present_endpoints(
            windows, stable_start, phase["end_us"])
        sdk_points = [{"t_us": row["t_us"], "presents": row["presents"]}
                      for row in rows]
        sdk_present, sdk_reason = _present_endpoints(
            sdk_points, stable_start, phase["end_us"])
        present = gxm_present or sdk_present
        if present is None:
            raise ValueError(
                f"phase {index} has no sufficient present endpoints "
                f"(gxm: {gxm_reason}; sdk: {sdk_reason})")
        present = dict(present)
        present["source"] = ("gxm-perf-window-colour"
                             if gxm_present is not None
                             else "sdk-ipc-present")

        encoded = [row["encoded_bytes"] for row in rows if row["growth"] == 0]
        growth = sum(row["growth"] for row in rows)
        reports.append({
            "index": index,
            "mode": phase["mode"],
            "start_us": phase["start_us"],
            "end_us": phase["end_us"],
            "samples": len(rows),
            "warmup_excluded": warmup_excluded[index],
            "elapsed_us": _stats([row["elapsed_us"] for row in rows]),
            "encoded_bytes": {
                "samples": len(encoded),
                "growth_samples": growth,
                "sum": sum(encoded),
                "mean": (sum(encoded) / len(encoded)) if encoded else None,
            },
            "present": present,
            "alternate_present": {
                "gxm": gxm_present,
                "sdk": sdk_present,
                "gxm_error": gxm_reason,
                "sdk_error": sdk_reason,
            },
        })

    aggregates = {}
    for phase in reports:
        mode = phase["mode"]
        group = aggregates.setdefault(mode, {
            "phases": 0,
            "samples": 0,
            "elapsed_values": [],
            "encoded_values": [],
            "growth_samples": 0,
        })
        group["phases"] += 1
        group["samples"] += phase["samples"]
        group["elapsed_values"].extend(
            row["elapsed_us"] for row in phase_samples[phase["index"]])
        group["encoded_values"].extend(
            row["encoded_bytes"] for row in phase_samples[phase["index"]]
            if row["growth"] == 0)
        group["growth_samples"] += sum(
            row["growth"] for row in phase_samples[phase["index"]])
    aggregate_report = {}
    for mode, group in aggregates.items():
        values = group.pop("elapsed_values")
        encoded = group.pop("encoded_values")
        group["elapsed_us"] = _stats(values)
        group["encoded_bytes"] = {
            "samples": len(encoded),
            "sum": sum(encoded),
            "mean": (sum(encoded) / len(encoded)) if encoded else None,
        }
        aggregate_report[mode] = group

    order = [phase["mode"] for phase in reports]
    baseline = order[0]
    comparison = order[1]
    deltas = {}
    for key in ("mean", "median", "p95", "p99", "max"):
        before = aggregate_report[baseline]["elapsed_us"][key]
        after = aggregate_report[comparison]["elapsed_us"][key]
        deltas[key + "_us"] = {
            "baseline": before,
            "comparison": after,
            "difference": after - before,
            "percent": ((after / before - 1.0) * 100.0) if before else None,
        }

    return {
        "schema": 1,
        "clock": "sceKernelGetProcessTimeWide microseconds (game process clock)",
        "warmup_seconds": warmup_seconds,
        "phase_count": len(reports),
        "phases": reports,
        "aggregate": aggregate_report,
        "delta": {
            "baseline_mode": baseline,
            "comparison_mode": comparison,
            "fields": deltas,
        },
        "sample_accounting": {
            "total_prepare_lines": len(samples),
            "outside_phases": outside,
            "warmup_excluded": sum(warmup_excluded),
        },
        "decision": (
            "none: this report lists descriptive deltas only; it does not "
            "declare a performance success or failure"),
        "scope": (
            "root elapsed covers target execution from the enter handler's "
            "timestamp through the leave handler's first timestamp, including "
            "the enter bridge return restore and the leave bridge frame save; "
            "both modes pay the same bridge structure and host scheduling on "
            "the same process. It is not pure CPU cycles or GPU time. Phase "
            "windows must be chosen on a stable scene and must exclude movie "
            "startup."),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-log", type=Path, required=True,
                        help="sdk-dso.log containing ENGINE_PREPARE_TIME lines")
    parser.add_argument("--gxm-log", type=Path, required=True,
                        help="gxm-runtime.log containing VP_GXM_PERF_WINDOW lines")
    parser.add_argument("--phases", type=Path, required=True,
                        help="JSON list of {mode,start_us,end_us} on the game clock")
    parser.add_argument("--output", type=Path, required=True,
                        help="new directory for report.json")
    parser.add_argument("--warmup-seconds", type=float, default=5.0)
    parser.add_argument("--run",type=Path,
                        help="Protected run directory; require complete frame histograms and verified exit/restore")
    args = parser.parse_args()
    try:
        if args.output.exists():
            raise ValueError(f"output directory already exists: {args.output}")
        phases = json.loads(args.phases.read_text(encoding="utf-8-sig"))
        report = analyze_logs(
            args.sdk_log.read_text(encoding="utf-8"),
            args.gxm_log.read_text(encoding="utf-8"),
            phases,
            args.warmup_seconds,
        )
        if args.run:
            run=args.run.resolve()
            if args.sdk_log.resolve()!=run/'sdk-dso.log' or args.gxm_log.resolve()!=run/'gxm-runtime.log':
                raise ValueError('SDK/GXM logs must belong to the selected protected run')
            sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'diagnostics'))
            from measure_frame_window import measure,quantile
            groups={}
            for phase in report['phases']:
                frame=measure(run,phase['start_us']/1e6+args.warmup_seconds,phase['end_us']/1e6)
                frame['p99']=quantile(frame['histogram'],.99)
                phase['whole_frames']=frame
                group=groups.setdefault(phase['mode'],dict(frames=0,sum_us=0,histogram=[0]*512))
                group['frames']+=frame['frames']
                group['sum_us']+=frame['mean_ms']*frame['frames']*1000
                group['histogram']=[a+b for a,b in zip(group['histogram'],frame['histogram'])]
            for group in groups.values():
                group['mean_ms']=group['sum_us']/group['frames']/1000
                group['fps']=1000/group['mean_ms']
                for name,fraction in (('p50',.5),('p95',.95),('p99',.99)):
                    group[name]=quantile(group['histogram'],fraction)
            report['whole_frame_aggregate']=groups
        report["inputs"] = {
            "sdk_log": str(args.sdk_log),
            "gxm_log": str(args.gxm_log),
            "phases": str(args.phases),
            "sha256":{str(p):hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in (args.sdk_log,args.gxm_log,args.phases)},
        }
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output / "report.json").write_text(
            json.dumps(report, indent=2, sort_keys=False) + "\n",
            encoding="utf-8")
        summary = {
            "phase_count": report["phase_count"],
            "aggregate": {
                mode: group["elapsed_us"]
                for mode, group in report["aggregate"].items()
            },
            "delta": report["delta"],
            "decision": report["decision"],
            "output": str(args.output / "report.json"),
        }
        print(json.dumps(summary))
        return 0
    except (OSError, ValueError, KeyError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

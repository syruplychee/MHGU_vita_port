"""Analyze bounded frame-cost snapshots from the GXM worker log.

The runtime emits a complete snapshot every 32 presents (and once when mode 1
is closed): one BEGIN line, one ROW per fixed category, one END line, all with
the same schema/seq/epoch/mode/t_us/frames identity. This tool differences two
complete snapshots from the same mode-1 epoch, inside an explicit game-clock
window. It refuses incomplete rows, mixed epochs, counter regressions, profiler
errors and spans that were still open.

Category inclusive times nest; they are never added as independent costs.
self_us subtracts direct children only. Root spans (root_calls) started with an
empty stack and are the only times subtracted from sampled frame wall, so the
residual stays signed and keeps its reason when closure is impossible.

Existing VP_FRAME_BUDGET / VP_GXM_BATCH_ENCODE_PERF / VP_GPU_WAIT_TOTAL lines
are reported separately with their own endpoints and are never mixed into the
deltas. Notification blocking is API wall time, not GPU hardware execution
time.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

SCHEMA = 3
CATEGORIES = (
    "entry",
    "dispatch",
    "mme",
    "draw-submit",
    "clear-submit",
    "tex-lookup",
    "tex-store",
    "tex-decode",
    "tex-tail",
    "gxm-draw",
    "gxm-clear",
    "gxm-flush",
    "wait-notify",
    "wait-finish",
    "draw-state",
    "draw-index-window",
    "draw-cbuf-resolve",
    "draw-resources",
    "draw-final",
    "gxm-request",
    "worker-tail",
    "log",
)
SCHEMA_CATEGORIES = {1: CATEGORIES[:14], 2: CATEGORIES[:19], 3: CATEGORIES}
GXM_DETAIL_FIELDS = ("draws prep_us begin_us encode_us draw_us setup_us vertex_us "
    "index_us texture_us nested_flush_us nested_pending_us nested_notify_us nested_finish_us "
    "validate_us pair_us feedback_us admit_us resolve_us pair_flush_us admit_flush_us "
    "prj_us reserve_us uniform_us policy_us").split()
TEXTURE_DETAIL_FIELDS = ("requests hits misses rgba8_us fp16_us blocks_us cube_us other_us "
    "rgba8_bytes fp16_bytes blocks_bytes cube_bytes other_bytes").split()
RESOURCE_DETAIL_FIELDS = "miss_changed miss_absent miss_gpu_only compare_bytes compare_calls".split()
ROW_FIELDS = ("calls", "incl_us", "self_us", "max_us", "root_calls",
              "root_incl_us")
BEGIN_FIELDS = (
    "schema", "seq", "epoch", "mode", "t_us", "frames", "rows", "depth",
    "errors", "mismatch", "overflow", "underflow", "clock_regress",
    "missing_time", "incomplete", "control_error", "clocks", "poll_clocks",
    "boundaries",
)
PAIR = re.compile(r"([a-z_0-9]+)=([^\s]+)")


def _fields(line):
    return dict(PAIR.findall(line))


def _int(fields, key):
    value = int(fields[key])
    if value < 0:
        raise ValueError(f"negative {key}")
    return value


def _identity(fields):
    return tuple(_int(fields, key) for key in
                 ("schema", "seq", "epoch", "mode", "t_us", "frames"))


def parse_snapshots(text):
    """Return complete snapshots or raise ValueError on any malformed block."""
    snapshots = []
    current = None
    for line_number, line in enumerate(text.splitlines(), 1):
        try:
            if line.startswith("VP_FRAME_COST_SNAPSHOT_BEGIN "):
                if current is not None:
                    raise ValueError("nested snapshot begin")
                fields = _fields(line)
                values = {key: _int(fields, key) for key in BEGIN_FIELDS}
                if values["schema"] not in SCHEMA_CATEGORIES:
                    raise ValueError("unsupported schema")
                if values["mode"] != 1:
                    raise ValueError("snapshot is not mode 1")
                if values["depth"] != 0:
                    raise ValueError("snapshot was taken with an open span")
                names = SCHEMA_CATEGORIES[values["schema"]]
                if values["rows"] != len(names):
                    raise ValueError("unexpected category row count")
                reported = values["errors"]
                calculated = (values["mismatch"] + values["overflow"] +
                              values["underflow"] + values["clock_regress"] +
                              values["missing_time"] + values["incomplete"] +
                              values["control_error"])
                if reported != calculated:
                    raise ValueError("error fields do not reconcile")
                if values["control_error"] > 1:
                    raise ValueError("invalid control_error")
                current = dict(
                    identity=_identity(fields),
                    schema=values["schema"], seq=values["seq"],
                    epoch=values["epoch"], mode=values["mode"],
                    t_us=values["t_us"], frames=values["frames"],
                    rows=values["rows"], depth=values["depth"],
                    errors=values["errors"], mismatch=values["mismatch"],
                    overflow=values["overflow"],
                    underflow=values["underflow"],
                    clock_regress=values["clock_regress"],
                    missing_time=values["missing_time"],
                    incomplete=values["incomplete"],
                    control_error=values["control_error"],
                    clocks=values["clocks"],
                    poll_clocks=values["poll_clocks"],
                    boundaries=values["boundaries"],
                    reason=fields.get("reason", ""),
                    categories={}, details={},
                )
            elif line.startswith("VP_FRAME_COST_SNAPSHOT_ROW "):
                if current is None:
                    raise ValueError("row outside a snapshot")
                fields = _fields(line)
                if _identity(fields) != current["identity"]:
                    raise ValueError("row identity does not match snapshot")
                row = _int(fields, "row")
                if row != len(current["categories"]):
                    raise ValueError("missing, duplicate or out-of-order row")
                names = SCHEMA_CATEGORIES[current["schema"]]
                if row >= len(names) or fields.get("cat") != names[row]:
                    raise ValueError("unexpected category name/order")
                values = {key: _int(fields, key) for key in ROW_FIELDS}
                if values["self_us"] > values["incl_us"]:
                    raise ValueError(f"{CATEGORIES[row]} self exceeds inclusive")
                if values["max_us"] > values["incl_us"]:
                    raise ValueError(f"{CATEGORIES[row]} max exceeds inclusive")
                if values["root_calls"] > values["calls"]:
                    raise ValueError(f"{CATEGORIES[row]} root calls exceed calls")
                if values["root_incl_us"] > values["incl_us"]:
                    raise ValueError(f"{CATEGORIES[row]} root time exceeds inclusive")
                if values["calls"] == 0 and any(values[key] for key in
                                                ("incl_us", "self_us", "max_us",
                                                 "root_incl_us")):
                    raise ValueError(f"{CATEGORIES[row]} counters without calls")
                current["categories"][names[row]] = values
            elif line.startswith(("VP_FRAME_COST_GXM ", "VP_FRAME_COST_TEXTURE ")):
                if current is None or _identity(_fields(line)) != current["identity"]:
                    raise ValueError("detail outside its matching snapshot")
                fields = _fields(line)
                if line.startswith("VP_FRAME_COST_GXM "):
                    kind = _int(fields, "kind")
                    if kind not in (0, 1):
                        raise ValueError("unknown GXM detail kind")
                    name, keys = f"gxm-{kind}", GXM_DETAIL_FIELDS
                else:
                    name, keys = "texture", TEXTURE_DETAIL_FIELDS
                    if any(key in fields for key in RESOURCE_DETAIL_FIELDS):
                        keys = keys + RESOURCE_DETAIL_FIELDS
                if name in current["details"]:
                    raise ValueError("duplicate detail row")
                current["details"][name] = {key: _int(fields, key) for key in keys}
            elif line.startswith("VP_FRAME_COST_SNAPSHOT_END "):
                if current is None:
                    raise ValueError("end outside a snapshot")
                fields = _fields(line)
                if _identity(fields) != current["identity"]:
                    raise ValueError("end identity does not match snapshot")
                if _int(fields, "rows") != current["rows"]:
                    raise ValueError("end row count does not match")
                if _int(fields, "errors") != current["errors"]:
                    raise ValueError("end errors do not match")
                if len(current["categories"]) != current["rows"]:
                    raise ValueError("snapshot is missing rows")
                if current["schema"] >= 2 and set(current["details"]) != {"gxm-0", "gxm-1", "texture"}:
                    raise ValueError("snapshot is missing aligned detail rows")
                if not current["errors"]:
                    totals = current["categories"].values()
                    self_sum = sum(row["self_us"] for row in totals)
                    root_sum = sum(row["root_incl_us"] for row in totals)
                    if self_sum != root_sum:
                        raise ValueError("self times do not reconcile with non-overlapping roots")
                snapshots.append(current)
                current = None
        except (KeyError, ValueError) as error:
            raise ValueError(
                f"invalid frame-cost snapshot near line {line_number}: {error}"
            ) from error
    if current is not None:
        raise ValueError("truncated final frame-cost snapshot")
    return snapshots


def parse_window(text):
    match = re.fullmatch(r"(\d+):(\d+)", text.strip())
    if match is None:
        raise ValueError(f"window must be START_US:END_US: {text}")
    start, end = int(match.group(1)), int(match.group(2))
    if start >= end:
        raise ValueError(f"window start must precede end: {text}")
    return start, end


def _validate_series(selected):
    first = selected[0]
    epoch = first["epoch"]
    previous = None
    for snapshot in selected:
        if snapshot["epoch"] != epoch:
            raise ValueError("window crosses an epoch/mode switch")
        if snapshot["schema"] != first["schema"]:
            raise ValueError("window crosses a schema change")
        if snapshot["mode"] != 1:
            raise ValueError("window contains a non-mode-1 snapshot")
        if snapshot["errors"] != 0 or snapshot["control_error"] != 0:
            raise ValueError("window contains profiler/control errors")
        if snapshot["depth"] != 0:
            raise ValueError("window contains an open span")
        if previous is not None:
            if snapshot["seq"] != previous["seq"] + 1:
                raise ValueError("snapshot sequence has a gap or regressed")
            if snapshot["t_us"] <= previous["t_us"]:
                raise ValueError("snapshot time did not increase")
            if snapshot["frames"] <= previous["frames"]:
                raise ValueError("snapshot frames did not increase")
            stop_snapshot = snapshot["reason"] in (
                "mode-change", "control-error", "marker-removed")
            if (not stop_snapshot and
                    snapshot["frames"] - previous["frames"] != 32):
                raise ValueError(
                    "missing frame-cost snapshot between complete endpoints")
            for category in first["categories"]:
                now = snapshot["categories"][category]
                before = previous["categories"][category]
                if any(now[key] < before[key] for key in ROW_FIELDS):
                    raise ValueError(f"{category} cumulative counter regressed")
            for name, details in snapshot["details"].items():
                if any(value < previous["details"][name][key] for key, value in details.items()):
                    raise ValueError("aligned detail counter regressed")
        previous = snapshot


def _delta_category(before, after, frames, wall_us):
    values = {key: after[key] - before[key] for key in ROW_FIELDS}
    return dict(
        values,
        incl_us_per_frame=values["incl_us"] / frames,
        self_us_per_frame=values["self_us"] / frames,
        root_us_per_frame=values["root_incl_us"] / frames,
        root_share_percent=(values["root_incl_us"] / wall_us * 100.0
                            if wall_us else 0.0),
        max_us_epoch_endpoint=after["max_us"],
        note=("max_us_epoch_endpoint may have been set before the window; "
              "inclusive/self are exact endpoint differences"),
    )


def analyze_window(snapshots, start_us, end_us):
    selected = [s for s in snapshots if start_us <= s["t_us"] <= end_us]
    if len(selected) < 2:
        raise ValueError("window needs at least two complete snapshots")
    _validate_series(selected)
    first, last = selected[0], selected[-1]
    frames = last["frames"] - first["frames"]
    wall_us = last["t_us"] - first["t_us"]
    if frames <= 0 or wall_us <= 0:
        raise ValueError("window has no frame/time progress")
    categories = {}
    for name in first["categories"]:
        categories[name] = _delta_category(
            first["categories"][name], last["categories"][name], frames,
            wall_us)
    root_us = sum(values["root_incl_us"] for values in categories.values())
    residual_us = wall_us - root_us
    if residual_us > 0:
        residual_reason = [
            "worker idle or waiting for a new request",
            "present/driver work without a top-level cost span",
            "guest, audio and other threads are outside this single-worker profile",
            "host paths not covered by a marker",
        ]
        closed = False
    elif residual_us < 0:
        residual_reason = [
            "measured root spans exceed sampled wall (overlap or clock skew)",
            "do not clamp; window is not valid closure data",
        ]
        closed = False
    else:
        residual_reason = []
        closed = True
    details = {}
    for name, values in last["details"].items():
        delta = {key: value - first["details"][name][key] for key, value in values.items()}
        details[name] = {"delta": delta,
                         "per_frame": {key: value / frames for key, value in delta.items()}}
    return {
        "profile_schema": first["schema"],
        "requested_window_us": [start_us, end_us],
        "actual_window_us": [first["t_us"], last["t_us"]],
        "epoch": first["epoch"],
        "snapshot_seq": [first["seq"], last["seq"]],
        "frames": frames,
        "wall_us": wall_us,
        "frame_wall_us": wall_us / frames,
        "categories": categories,
        "aligned_details": details,
        "measured_root_us": root_us,
        "measured_root_share_percent": root_us / wall_us * 100.0,
        "residual_us": residual_us,
        "residual_us_per_frame": residual_us / frames,
        "residual_reason": residual_reason,
        "closure_complete": closed,
        "window_valid": residual_us >= 0,
        "notes": [
            "inclusive_us contains nested spans; never sum categories",
            "self_us subtracts direct children only",
            "root_incl_us is the only non-duplicated measured worker time",
            "wall includes idle and non-worker threads and is not CPU time",
            "notification waits are API wall time, not GPU execution time",
        ],
    }


def _existing_lines(text, prefix, start_us, end_us, keys):
    result = []
    for line in text.splitlines():
        if not line.startswith(prefix + " "):
            continue
        fields = _fields(line)
        try:
            at = int(fields["t_us"])
        except (KeyError, ValueError):
            continue
        if not start_us <= at <= end_us:
            continue
        row = {"line_prefix": prefix, "unaligned": True,
               "not_added_to_cost": True, "raw_line_sha256":
                   hashlib.sha256(line.encode("utf-8")).hexdigest()}
        for key in ("t_us", "frames"):
            if key in fields:
                try:
                    row[key] = int(fields[key])
                except ValueError:
                    pass
        for key in keys:
            if key in fields:
                try:
                    row[key] = int(fields[key])
                except ValueError:
                    row[key] = fields[key]
        result.append(row)
    return result


def _evidence_from_file(path):
    if path is None:
        return None
    raw = path.read_bytes()
    text = raw.decode("utf-8-sig", errors="replace")
    data = json.loads(text)
    if not isinstance(data, dict):
        raise ValueError("evidence JSON must be an object")
    return {"path": str(path), "sha256": hashlib.sha256(raw).hexdigest(),
            "values": data}


def check_process_evidence(evidence):
    if evidence is None:
        return False, "process evidence was not supplied"
    values = evidence["values"]
    # Use the project's lifecycle observer contract. A friendly label without
    # exit status, process identity and crash/stop checks is not evidence.
    normal = (values.get("classification") == "exited" and
              values.get("exit_code") == 0 and
              values.get("caller_stop") is False and
              values.get("crash_suspected") is False and
              values.get("pid_reused") is False and
              values.get("expected_start_time_checked") is True)
    if normal:
        return True, None
    return False, "process evidence does not prove a normal exit"


def check_save_evidence(evidence):
    if evidence is None:
        return False, "save-protection evidence was not supplied"
    values = evidence["values"]
    if (values.get("restored") is True and values.get("verified") is True and
            values.get("markers_verified") is True):
        return True, None
    return False, "save-protection evidence does not prove verified save and marker restoration"


def _present_frame_wall(log_text, start_us, end_us):
    points = []
    for line in log_text.splitlines():
        if not line.startswith("VP_GXM_PERF_PRESENT "):
            continue
        fields = _fields(line)
        try:
            at, frames = int(fields["t_us"]), int(fields["frames"])
        except (KeyError, ValueError):
            continue
        if start_us <= at <= end_us:
            points.append((at, frames))
    if len(points) < 2:
        raise ValueError("mode-0 log has fewer than two present endpoints")
    points.sort()
    wall = points[-1][0] - points[0][0]
    frames = points[-1][1] - points[0][1]
    if wall <= 0 or frames <= 0:
        raise ValueError("mode-0 present endpoints do not advance")
    return wall / frames


def analyze_mode0_interference(log_text, windows, mode1_results):
    has_marker = "VP_FRAME_COST_MARKER enabled=1" in log_text
    mode0_points = [
        line for line in log_text.splitlines()
        if line.startswith("VP_FRAME_COST_POINT ") and "mode=0" in line
    ]
    comparisons = []
    for (start, end), mode1 in zip(windows, mode1_results):
        entry = {"requested_window_us": [start, end],
                 "descriptive_only": True}
        try:
            controls = []
            for line in log_text.splitlines():
                if line.startswith(("VP_FRAME_COST_POINT ", "VP_FRAME_COST_EPOCH ")):
                    fields = _fields(line)
                    if "t_us" in fields and "mode" in fields:
                        controls.append((int(fields["t_us"]), int(fields["mode"]),
                                         int(fields.get("control_error", "0"))))
            before = [point for point in controls if point[0] <= start]
            during = [point for point in controls if start < point[0] <= end]
            if (not has_marker or not before or before[-1][1:] != (0, 0) or
                    any(mode or error for _, mode, error in during)):
                raise ValueError("requested comparison window is not proven mode 0")
            mode0_wall = _present_frame_wall(log_text, start, end)
            entry["mode0_frame_wall_us"] = mode0_wall
            entry["mode1_frame_wall_us"] = mode1["frame_wall_us"]
            entry["difference_us"] = mode1["frame_wall_us"] - mode0_wall
            entry["available"] = True
        except ValueError as error:
            entry["available"] = False
            entry["reason"] = str(error)
        comparisons.append(entry)
    return {
        "marker_enabled": has_marker,
        "mode0_points_in_log": len(mode0_points),
        "comparisons": comparisons,
        "note": ("descriptive interference check only; mode0 and mode1 "
                 "endpoints are not aligned and this is not a speedup claim"),
    }


def markdown_report(report):
    lines = [
        "# Frame-cost decomposition",
        "",
        f"- schema: {report['schema']}",
        f"- gxm log: `{report['inputs']['gxm_log']['path']}`",
        f"- gxm sha256: `{report['inputs']['gxm_log']['sha256']}`",
        f"- performance_pass: {report['performance_pass']}",
        f"- speedup_claim: {report['speedup_claim']}",
        "",
    ]
    for index, window in enumerate(report["windows"]):
        lines += [
            f"## Window {index + 1}: {window['requested_window_us'][0]}.."
            f"{window['requested_window_us'][1]} us",
            "",
            f"- actual: {window['actual_window_us'][0]}.."
            f"{window['actual_window_us'][1]} us",
            f"- epoch: {window['epoch']}",
            f"- frames: {window['frames']}",
            f"- frame wall: {window['frame_wall_us'] / 1000:.3f} ms",
            f"- measured root: "
            f"{window['measured_root_share_percent']:.2f}% of wall",
            f"- residual: {window['residual_us_per_frame'] / 1000:+.3f} ms/frame "
            f"({window['residual_us']:+d} us)",
            "",
            "| category | calls | incl ms/frame | self ms/frame | "
            "root % wall | root calls |",
            "|---|---:|---:|---:|---:|---:|",
        ]
        for name, values in window["categories"].items():
            lines.append(
                f"| {name} | {values['calls']} | "
                f"{values['incl_us_per_frame'] / 1000:.3f} | "
                f"{values['self_us_per_frame'] / 1000:.3f} | "
                f"{values['root_share_percent']:.2f} | "
                f"{values['root_calls']} |")
        lines += [
            "",
            "Residual reasons: " +
            ("; ".join(window["residual_reason"]) if window["residual_reason"]
             else "closed exactly"),
            "",
        ]
    lines += ["## Existing unaligned evidence", ""]
    for label, rows in report["existing_unaligned"].items():
        lines.append(f"- {label}: {len(rows)} line(s) in the requested "
                     "windows; own endpoints, not added to cost deltas")
    if report["mode0_interference"] is not None:
        lines += [
            "",
            "## Mode 0 interference (descriptive)",
            "",
            f"- mode-0 marker present: "
            f"{report['mode0_interference']['marker_enabled']}",
            f"- mode-0 points: "
            f"{report['mode0_interference']['mode0_points_in_log']}",
            "- comparisons are endpoint-limited and are not a speed claim",
        ]
    lines += [
        "## Limitations",
        "",
    ]
    lines += [f"- {item}" for item in report["limitations"]]
    lines.append("")
    return "\n".join(lines)


def analyze(gxm_text, windows, mode0_text=None, process_evidence=None,
            save_evidence=None, source_path=None, gxm_sha256=None,
            mode0_sha256=None, mode0_path=None):
    snapshots = parse_snapshots(gxm_text)
    results = [analyze_window(snapshots, start, end)
               for start, end in windows]
    process_ok, process_reason = check_process_evidence(process_evidence)
    save_ok, save_reason = check_save_evidence(save_evidence)
    valid = all(window["window_valid"] for window in results)
    sufficient_frames = all(window["frames"] >= 128 for window in results)
    performance_pass = valid and sufficient_frames and process_ok and save_ok
    report = {
        "schema": SCHEMA,
        "clock": "sceKernelGetProcessTimeWide microseconds (game process clock)",
        "windows": results,
        "performance_pass": performance_pass,
        "minimum_128_frames_per_window": sufficient_frames,
        "speedup_claim": False,
        "process_evidence_ok": process_ok,
        "process_evidence_reason": process_reason,
        "save_evidence_ok": save_ok,
        "save_evidence_reason": save_reason,
        "snapshot_count": len(snapshots),
        "inputs": {
            "gxm_log": {
                "path": str(source_path) if source_path else None,
                "sha256": gxm_sha256 if gxm_sha256 is not None else
                          hashlib.sha256(gxm_text.encode("utf-8")).hexdigest(),
            },
            "tool_sha256": hashlib.sha256(
                Path(__file__).read_bytes()).hexdigest(),
            "process_evidence": process_evidence,
            "save_protection": save_evidence,
            "mode0_log": None,
        },
        "existing_unaligned": {},
        "mode0_interference": None,
        "limitations": [
            "cost data is single-GXM-worker wall time, not per-thread totals",
            "nested categories are not additive; use inclusive, self and root fields",
            "frame wall includes worker idle, present/driver work and non-worker threads",
            "signed residual is preserved; it is never clamped to zero",
            "notification/finish waits are API blocking, not GPU execution time",
            "the GXM worker has no clean teardown; the last periodic or explicit "
            "mode-off snapshot is the available endpoint",
            "VP_FRAME_BUDGET/ENCODE/WAIT lines use their own endpoints and are not mixed",
            "no result declares a speedup; only measurement validity gates performance_pass",
        ],
    }
    if mode0_text is not None:
        report["mode0_interference"] = analyze_mode0_interference(
            mode0_text, windows, results)
        report["inputs"]["mode0_log"] = {
            "path": str(mode0_path) if mode0_path else None,
            "sha256": mode0_sha256 if mode0_sha256 is not None else
                      hashlib.sha256(mode0_text.encode("utf-8")).hexdigest(),
        }
    for label, prefix, keys in (
        ("vp_frame_budget", "VP_FRAME_BUDGET",
         ("wall_us", "frames", "residual_us", "decode_incl_us",
          "draw_prepare_incl_us", "gxm_call_incl_us", "command_dispatch_us",
          "batch_complete_us", "notify_block_us", "finish_block_us")),
        ("vp_gxm_batch_encode_perf", "VP_GXM_BATCH_ENCODE_PERF",
         ("draws", "prep_us", "encode_us", "draw_us", "texture_us",
          "nested_flush_us", "nested_pending_us")),
        ("vp_gpu_wait_total", "VP_GPU_WAIT_TOTAL",
         ("snap", "frames", "real", "block_us")),
    ):
        rows = []
        for start, end in windows:
            rows.extend(_existing_lines(gxm_text, prefix, start, end, keys))
        report["existing_unaligned"][label] = rows
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gxm-log", type=Path, required=True)
    parser.add_argument("--window", action="append", default=[],
                        metavar="START_US:END_US",
                        help="game-clock window; may be repeated")
    parser.add_argument("--mode0-log", type=Path,
                        help="optional mode-0 log for descriptive interference")
    parser.add_argument("--process-evidence", type=Path)
    parser.add_argument("--save-protection", type=Path)
    parser.add_argument("--output", type=Path, required=True,
                        help="directory for report.json and report.md")
    args = parser.parse_args(argv)
    try:
        if not args.window:
            raise ValueError("at least one --window START_US:END_US is required")
        windows = [parse_window(item) for item in args.window]
        gxm_raw = args.gxm_log.read_bytes()
        gxm_text = gxm_raw.decode("utf-8", errors="replace")
        gxm_sha256 = hashlib.sha256(gxm_raw).hexdigest()
        mode0_text = None
        mode0_sha256 = None
        if args.mode0_log:
            mode0_raw = args.mode0_log.read_bytes()
            mode0_text = mode0_raw.decode("utf-8", errors="replace")
            mode0_sha256 = hashlib.sha256(mode0_raw).hexdigest()
        process = _evidence_from_file(args.process_evidence)
        save = _evidence_from_file(args.save_protection)
        report = analyze(gxm_text, windows, mode0_text, process, save,
                         args.gxm_log, gxm_sha256, mode0_sha256,
                         args.mode0_log)
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / "report.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8")
        (args.output / "report.md").write_text(
            markdown_report(report), encoding="utf-8")
        summary = {
            "windows": len(report["windows"]),
            "snapshots": report["snapshot_count"],
            "performance_pass": report["performance_pass"],
            "speedup_claim": report["speedup_claim"],
            "output": str(args.output),
        }
        print(json.dumps(summary))
        return 0
    except (OSError, ValueError, KeyError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

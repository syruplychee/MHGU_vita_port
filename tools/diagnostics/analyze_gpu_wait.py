"""Attribute notification API blocking using complete cumulative snapshots."""
import argparse
import hashlib
import json
from pathlib import Path
import re

FIELDS = ("calls", "real", "polls", "failures", "block_us", "pending_sum",
          "draws", "sync", "b0", "b1", "b2", "b3", "b4")
PAIR = re.compile(r"([a-z_0-9]+)=([^\s]+)")


def parse_snapshots(text):
    snapshots = []
    current = None
    for line in text.splitlines():
        if not line.startswith(("VP_GPU_WAIT ", "VP_GPU_WAIT_TOTAL ")):
            continue
        fields = dict(PAIR.findall(line))
        try:
            snap, at, frames = (int(fields[k]) for k in ("snap", "t_us", "frames"))
            if min(snap, at, frames) < 0:
                raise ValueError("negative snapshot identity")
            if line.startswith("VP_GPU_WAIT "):
                row, rows = int(fields["row"]), int(fields["rows"])
                values = {k: int(fields[k]) for k in FIELDS}
                if any(v < 0 for v in values.values()) or rows <= 0 or not 0 <= row < rows:
                    raise ValueError("invalid row/counter")
                if current is None:
                    if row != 0:
                        raise ValueError("snapshot does not start at row zero")
                    current = dict(snap=snap, t_us=at, frames=frames, rows=rows, causes={})
                if (snap, at, frames, rows) != tuple(current[k] for k in
                                                   ("snap", "t_us", "frames", "rows")):
                    raise ValueError("incomplete or mixed snapshot")
                if row != len(current["causes"]) or fields["cause"] in current["causes"]:
                    raise ValueError("duplicate or out-of-order row")
                if values["calls"] != values["real"] + values["polls"]:
                    raise ValueError("calls do not reconcile")
                if values["real"] != sum(values[f"b{i}"] for i in range(5)):
                    raise ValueError("wait buckets do not reconcile")
                if values["failures"] > values["real"] or values["sync"] > values["calls"]:
                    raise ValueError("invalid failures/synchronous count")
                current["causes"][fields["cause"]] = values
            else:
                if current is None or len(current["causes"]) != current["rows"]:
                    raise ValueError("missing snapshot rows")
                if (snap, at, frames) != tuple(current[k] for k in ("snap", "t_us", "frames")):
                    raise ValueError("mismatched total identity")
                for key in ("real", "block_us"):
                    if sum(v[key] for v in current["causes"].values()) != int(fields[key]):
                        raise ValueError("global wait total does not reconcile")
                if snapshots:
                    previous = snapshots[-1]
                    if snap != previous["snap"] + 1 or at <= previous["t_us"] or frames <= previous["frames"]:
                        raise ValueError("snapshot identity regressed or skipped")
                    if current["causes"].keys() != previous["causes"].keys():
                        raise ValueError("cause list changed")
                    for name, values in current["causes"].items():
                        if any(values[k] < previous["causes"][name][k] for k in FIELDS):
                            raise ValueError("cumulative counter regressed")
                snapshots.append(current)
                current = None
        except (KeyError, ValueError) as exc:
            raise ValueError(f"invalid GPU wait snapshot: {exc}") from exc
    if current is not None:
        raise ValueError("truncated final snapshot")
    if len(snapshots) < 2:
        raise ValueError("need at least two complete snapshots")
    return snapshots


def analyze(snapshots, start_us, end_us):
    selected = [s for s in snapshots if start_us <= s["t_us"] <= end_us]
    if len(selected) < 2:
        raise ValueError("window needs two complete snapshots")
    first, last = selected[0], selected[-1]
    frames = last["frames"] - first["frames"]
    causes = {}
    for name, values in last["causes"].items():
        delta = {k: values[k] - first["causes"][name][k] for k in FIELDS}
        delta["ms_per_frame"] = delta["block_us"] / frames / 1000
        delta["mean_real_wait_us"] = delta["block_us"] / delta["real"] if delta["real"] else 0
        causes[name] = delta
    total = sum(v["block_us"] for v in causes.values())
    for values in causes.values():
        values["block_share_percent"] = values["block_us"] / total * 100 if total else 0
    return dict(schema=1, requested_window_us=[start_us, end_us],
                actual_window_us=[first["t_us"], last["t_us"]], frames=frames,
                frame_mean_ms=(last["t_us"] - first["t_us"]) / frames / 1000,
                notify_ms_per_frame=total / frames / 1000,
                bucket_bounds_us=[0, 100, 1000, 5000, 20000, None],
                causes=dict(sorted(causes.items(), key=lambda row: -row[1]["block_us"])),
                scope="notification API wall time, not GPU execution time; snapshots inside requested window")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--start-seconds", type=float, required=True)
    parser.add_argument("--end-seconds", type=float, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    raw = args.log.read_bytes()
    report = analyze(parse_snapshots(raw.decode("utf-8")),
                     int(args.start_seconds * 1e6), int(args.end_seconds * 1e6))
    report["input"] = dict(path=str(args.log), sha256=hashlib.sha256(raw).hexdigest())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"frames={report['frames']} frame_ms={report['frame_mean_ms']:.3f} "
          f"notify_ms={report['notify_ms_per_frame']:.3f}")
    for name, values in report["causes"].items():
        if values["calls"]:
            print(f"{name}: {values['ms_per_frame']:.3f} ms/frame "
                  f"share={values['block_share_percent']:.1f}% real={values['real']} "
                  f"polls={values['polls']} failures={values['failures']}")


if __name__ == "__main__":
    main()

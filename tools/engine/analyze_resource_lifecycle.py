"""Join bounded texture requests to original rTexture load lifetimes.

The observed native-object +0xb8 VA is a same-run association, never a cache
identity shortcut. Load observations are not complete content-write versions.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re

from analyze_frame_resources import analyze as analyze_requests

EVENT = re.compile(
    r"^RESOURCE_LIFECYCLE event=(\d+) kind=([a-z-]+) object=(0x[0-9a-f]+) "
    r"lr=(0x[0-9a-f]+) sp=(0x[0-9a-f]+) thread=(0x[0-9a-f]+) "
    r"vptr=(0x[0-9a-f]+) name=(.*?) gpu=(0x[0-9a-f]+) "
    r"width=(\d+) height=(\d+) depth=(\d+) lod=(\d+)(?: replacement=0x[0-9a-f]+)?$")
GPU = re.compile(r"^RESOURCE_GPU_OBJECT event=(\d+) gpu=(0x[0-9a-f]+) words=([0-9a-f,]+)$")
REQUEST = re.compile(r"^VP_FRAME_RESOURCE frame=(\d+) key=([^ ]+) ")


def analyze(text):
    requests = analyze_requests(text)
    selected = {tuple(row["key"]) for row in requests["resources"]
                if row["raw_bytes"] == 4194304 and row["key"][1:4] == [1024, 1024, 8]}
    valid_frames = set()
    for segment in requests["capture_segments"]:
        # The existing request analyzer excludes both capture boundaries.
        valid_frames.update(range(segment["first_frame"] + 1, segment["last_frame"]))
    events = {}
    live = defaultdict(dict)
    kinds = Counter()
    malformed = Counter()
    joined = defaultdict(Counter)
    ambiguous_objects = Counter()
    observations = []

    def invalidate(obj):
        for owners in live.values():
            owners.pop(obj, None)

    for line in text.splitlines():
        if line.startswith("RESOURCE_LIFECYCLE "):
            match = EVENT.fullmatch(line)
            if not match:
                malformed["event"] += 1
                continue
            seq, kind, obj, lr, sp, thread, vptr, name, gpu, width, height, depth, lod = match.groups()
            seq, obj, gpu = int(seq), int(obj, 16), int(gpu, 16)
            if seq in events:
                raise ValueError("duplicate lifecycle event")
            row = dict(event=seq, kind=kind, object=obj, gpu=gpu, name=name,
                       width=int(width), height=int(height), vptr=vptr)
            events[seq] = row
            kinds[kind] += 1
            invalidate(obj)
        elif line.startswith("RESOURCE_GPU_OBJECT "):
            match = GPU.fullmatch(line)
            if not match:
                malformed["gpu_object"] += 1
                continue
            seq, gpu, raw = match.groups()
            words = [int(v, 16) for v in raw.split(",")]
            if len(words) != 132:
                malformed["gpu_word_count"] += 1
                continue
            event = events.get(int(seq))
            if not event:
                malformed["gpu_without_event"] += 1
                continue
            if int(gpu, 16) != event["gpu"]:
                raise ValueError("GPU object differs from lifecycle event")
            if event["kind"] == "load-commit":
                va = words[46] | words[47] << 32
                if not va:
                    continue
                event = dict(event, va=hex(va))
                live[va][event["object"]] = event
                observations.append(event)
        elif line.startswith("VP_FRAME_RESOURCE "):
            match = REQUEST.match(line)
            if not match or int(match[1]) not in valid_frames:
                continue
            raw = match[2].split(",")
            key = (hex(int(raw[0], 16)), *(int(v) for v in raw[1:]))
            if key not in selected:
                continue
            owners = list(live[int(raw[0], 16)].values())
            owners = [o for o in owners if (o["width"], o["height"]) == key[1:3]]
            names = {o["name"] for o in owners}
            owner = next(iter(names)) if len(names) == 1 else "UNKNOWN_OR_AMBIGUOUS"
            if len(owners) != 1:
                ambiguous_objects[key] += 1
            joined[key][owner] += 1
    rows = []
    for key in sorted(selected):
        counts = joined[key]
        rows.append(dict(key=list(key), owner_requests=dict(counts),
                         unique_live_object=bool(counts) and not ambiguous_objects[key],
                         fully_associated=bool(counts) and "UNKNOWN_OR_AMBIGUOUS" not in counts))
    return dict(request_analysis=requests, events=dict(kinds), malformed_records=dict(malformed),
                complete_event_sequence=bool(events) and set(events) == set(range(1, max(events) + 1)),
                load_commit_observations=observations, repeated_large_rgba8=rows,
                all_large_associated=bool(rows) and all(r["fully_associated"] for r in rows),
                complete_content_write_coverage=False,
                limitations=["invalidation covers only the installed original-code sites; inspect patch records for coverage",
                             "GPU object +0xb8 association is checked against same-run requests",
                             "no pixels are compared by this tool; names and stable VAs do not prove immutability",
                             "this report cannot authorize skipping raw byte validation"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    payload = args.sdk.read_bytes()
    report = analyze(payload.decode(errors="replace"))
    report["sdk_sha256"] = hashlib.sha256(payload).hexdigest()
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({key: report[key] for key in
                      ("events", "malformed_records", "all_large_associated",
                       "complete_content_write_coverage")}, ensure_ascii=False))


if __name__ == "__main__":
    main()

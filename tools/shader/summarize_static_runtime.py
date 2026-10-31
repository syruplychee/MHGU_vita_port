#!/usr/bin/env python3
"""Summarise a static-shader target run from its own log (no invented fields).

The log is the Vita-side record.  This tool only extracts values that the
runtime actually printed; missing lines make the summary incomplete instead of
being filled with defaults.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

STAT = re.compile(r"^VP_GXM_STATIC_STATS\s+(.*)$", re.M)
KV = re.compile(r"([a-z_]+)=([^\s]+)")
DRAW = re.compile(r"^VP_GXM_FRAME_DRAW\s+rc=(-?\d+)", re.M)
TEST = re.compile(r"^VP_GXM_TEST\s+(.*)$", re.M)
INDEX = re.compile(r"^VP_GXM_STATIC_INDEX\s+(.*)$", re.M)
PRELOAD = re.compile(r"^VP_GXM_STATIC_PRELOAD\s+(.*)$", re.M)


def sha(path):
    if not path:
        return None
    data = Path(path).read_bytes()
    return hashlib.sha256(data).hexdigest()


def kv(text):
    out = {}
    for key, value in KV.findall(text):
        if "/" in value:
            left, right = value.split("/", 1)
            out[key] = {"used": int(left), "limit": int(right)}
        else:
            try:
                out[key] = int(value)
            except ValueError:
                out[key] = value
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--log", required=True)
    ap.add_argument("--package", required=True)
    ap.add_argument("--screenshot", default="")
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    text = Path(args.log).read_text(encoding="utf-8", errors="replace")
    stats = STAT.findall(text)
    draws = DRAW.findall(text)
    tests = TEST.findall(text)
    index = INDEX.findall(text)
    preload = PRELOAD.findall(text)
    manifest_path = Path(args.package) / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.is_file() else {}
    sidecar = Path(args.package) / "shader-index.json"
    sidecar_data = json.loads(sidecar.read_text(encoding="utf-8")) if sidecar.is_file() else {}

    report = {
        "schema": 1,
        "mode": "static-index-v1" if "VP_GXM_SHADER mode=static" in text else "unknown",
        "log": str(Path(args.log).resolve()),
        "log_sha256": sha(args.log),
        "package": str(Path(args.package).resolve()),
        "package_index_sha256": sha(Path(args.package) / "shader-index.bin"),
        "package_manifest_sha256": sha(manifest_path),
        "package_counts": {
            "stages": len(sidecar_data.get("stages", [])),
            "variants": len(sidecar_data.get("variants", [])),
            "programs": len(sidecar_data.get("programs", [])),
            "observed_programs": sum(1 for p in sidecar_data.get("programs", []) if p.get("flags", 0) & 1),
        },
        "manifest_coverage": manifest.get("coverage", {}),
        "index_lines": [kv(x) for x in index],
        "preload_lines": [kv(x) for x in preload],
        "test_lines": [kv(x) for x in tests],
        "stats_last": kv(stats[-1]) if stats else None,
        "stats_samples": len(stats),
        "frame_draw_lines": len(draws),
        "frame_draw_rc_zero": sum(1 for rc in draws if rc == "0"),
        "frame_draw_rc_nonzero": sum(1 for rc in draws if rc != "0"),
        "screenshot": str(Path(args.screenshot).resolve()) if args.screenshot else None,
        "screenshot_sha256": sha(args.screenshot) if args.screenshot else None,
    }
    Path(args.output).write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    stats_last = report["stats_last"] or {}
    print(f"static-runtime-summary: mode={report['mode']} draws={report['frame_draw_lines']} "
          f"draw_ok={stats_last.get('draw_ok')} draw_fail={stats_last.get('draw_fail')} "
          f"resident={stats_last.get('resident')} pair_hits={stats_last.get('pair_hits')}")


if __name__ == "__main__":
    main()

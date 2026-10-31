#!/usr/bin/env python3
"""Verify the S2/P3 COLOR0/COLOR1/TEXCOORD varying probe.

Each case is drawn three times (rows 0/1/2 read COLOR0, COLOR1, TEXCOORD2).
The tool checks:

  exact        finite constant values pass through bit-exactly and unclamped;
               a mismatch is a probe regression or a clamp finding;
  perspective  the value at s=0.5 follows the 1/w interpolation; a deviation
               of more than a few ulp is recorded, not hidden;
  sign-zero    -0.0 may come back as +0.0; the sign loss is recorded;
  nan          a NaN input must remain NaN; payload canonicalization is
               recorded because the varying unit may replace it with 0x7fffffff;
  inf          +inf may come back as the canonical NaN; that limitation is
               recorded rather than treated as a finite value.

COLOR0 and TEXCOORD2 are fed from the same vertex output, so their bits must
match in every case.  COLOR1 is a separate output and is checked separately.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def value_of(pattern: int) -> float:
    return struct.unpack("<f", struct.pack("<I", pattern))[0]


def is_nan(pattern: int) -> bool:
    return (pattern & 0x7F800000) == 0x7F800000 and (pattern & 0x007FFFFF) != 0


def ulp_distance(a: int, b: int) -> int:
    ia = a if a < 1 << 31 else a - (1 << 32)
    ib = b if b < 1 << 31 else b - (1 << 32)
    if ia < 0 <= ib or ib < 0 <= ia:
        return abs(ia) + abs(ib)
    return abs(ia - ib)


def perspective(pattern_l: int, pattern_r: int, wl_bits: int, wr_bits: int) -> int:
    left = value_of(pattern_l)
    right = value_of(pattern_r)
    wl = value_of(wl_bits)
    wr = value_of(wr_bits)
    numerator = 0.5 * left / wl + 0.5 * right / wr
    denominator = 0.5 / wl + 0.5 / wr
    return bits(f32(numerator / denominator))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_log(path: Path) -> dict:
    cases: dict[int, dict] = {}
    results: dict[int, dict] = {}
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    for line in lines:
        parts = line.split()
        if not parts:
            continue
        head = parts[0]
        if head.startswith("case="):
            index = int(head.split("=", 1)[1])
            entry = {"name": parts[1]}
            for item in parts[2:]:
                key, value = item.split("=", 1)
                entry[key] = int(value, 16)
            cases[index] = entry
        elif head.startswith("result="):
            index = int(head.split("=", 1)[1])
            entry = {}
            for item in parts[1:]:
                if item.startswith("row"):
                    key, value = item.split("=", 1)
                    entry[key] = int(value, 16)
            results[index] = entry
    complete = any(line.strip() == "RESULT COMPLETE" for line in lines)
    failed = any(line.strip() == "RESULT FAIL" for line in lines)
    return {"cases": cases, "results": results, "complete": complete,
            "failed": failed, "lines": len(lines)}


def verify(log_path: Path, vpk_path: Path | None) -> dict:
    parsed = parse_log(log_path)
    problems: list[str] = []
    findings: list[dict] = []
    counts = {"exact": 0, "perspective": 0, "sign-zero": 0, "nan": 0, "inf": 0}

    if not parsed["complete"] or parsed["failed"]:
        problems.append("probe did not end with RESULT COMPLETE")

    for index, case in sorted(parsed["cases"].items()):
        name = case["name"]
        if index not in parsed["results"]:
            problems.append(f"case {index} {name}: missing result")
            continue
        rows = parsed["results"][index]
        if set(rows) != {"row0", "row1", "row2"}:
            problems.append(f"case {index} {name}: incomplete result rows")
            continue
        if rows["row0"] != rows["row2"]:
            problems.append(
                f"case {index} {name}: COLOR0 {rows['row0']:08x} != "
                f"TEXCOORD2 {rows['row2']:08x}; the two outputs share a source")
        entry = {"index": index, "name": name,
                 "observed": {key: f"{value:08x}" for key, value in rows.items()}}

        if name == "wiring":
            expected = {"row0": 0x3F800000, "row1": 0x40000000, "row2": 0x3F800000}
            for row, value in expected.items():
                if rows[row] != value:
                    problems.append(
                        f"case {index} {name}: {row} {rows[row]:08x} != {value:08x}")
            counts["exact"] += 1
            continue

        if name.startswith("range_"):
            expected0 = case["v0l"]
            expected1 = case["v1l"]
            if name == "range_neg_zero":
                if rows["row0"] in (0x80000000, 0x00000000):
                    entry["sign_preserved"] = rows["row0"] == 0x80000000
                    counts["sign-zero"] += 1
                    if not entry["sign_preserved"]:
                        findings.append(entry)
                else:
                    problems.append(
                        f"case {index} {name}: {rows['row0']:08x} is not zero")
                continue
            if name == "range_nan":
                for row in ("row0", "row1", "row2"):
                    if not is_nan(rows[row]):
                        problems.append(
                            f"case {index} {name}: {row} is not NaN")
                entry["payload_preserved"] = rows["row0"] == expected0
                counts["nan"] += 1
                if not entry["payload_preserved"]:
                    findings.append(entry)
                continue
            if name == "range_inf":
                for row in ("row0", "row1", "row2"):
                    if rows[row] == 0x7F800000:
                        continue
                    if is_nan(rows[row]):
                        entry.setdefault("inf_to_nan", []).append(row)
                        continue
                    problems.append(
                        f"case {index} {name}: {row} {rows[row]:08x} is neither "
                        "inf nor NaN")
                counts["inf"] += 1
                if entry.get("inf_to_nan"):
                    findings.append(entry)
                continue
            for row, expected in (("row0", expected0), ("row1", expected1),
                                  ("row2", expected0)):
                if rows[row] != expected:
                    problems.append(
                        f"case {index} {name}: {row} {rows[row]:08x} != "
                        f"{expected:08x} (clamped or altered)")
            counts["exact"] += 1
            continue

        if name.startswith("p_"):
            ideal0 = perspective(case["v0l"], case["v0r"], case["wl"], case["wr"])
            ideal1 = perspective(case["v1l"], case["v1r"], case["wl"], case["wr"])
            entry["ideal"] = {"row0": f"{ideal0:08x}", "row1": f"{ideal1:08x}",
                              "row2": f"{ideal0:08x}"}
            for row, ideal in (("row0", ideal0), ("row1", ideal1), ("row2", ideal0)):
                distance = ulp_distance(rows[row], ideal)
                entry[f"{row}_ulp"] = distance
                if distance > 4:
                    problems.append(
                        f"case {index} {name}: {row} {rows[row]:08x} is "
                        f"{distance} ulp from the perspective ideal {ideal:08x}")
                elif distance:
                    entry["deviates"] = True
            counts["perspective"] += 1
            if entry.get("deviates"):
                findings.append(entry)
            continue

        problems.append(f"case {index} {name}: no expected entry")

    summary = {
        "probe": "gxm-s2-p3-varying",
        "log": str(log_path),
        "log_sha256": sha256(log_path),
        "vpk_sha256": None if vpk_path is None else sha256(vpk_path),
        "complete": parsed["complete"],
        "cases": len(parsed["cases"]),
        "counts": counts,
        "problems": problems,
        "findings": findings,
        "limits": {
            "negative_zero": "-0.0 arrives as +0.0 on all three varying paths",
            "nan_payload": "0x7fc12345 arrives as 0x7fffffff; payload is "
                           "canonicalized, not preserved",
            "infinity": "+inf arrives as canonical NaN on all three paths",
            "perspective": "s=0.5 follows the 1/w interpolation; finite "
                           "results can differ from the exact real value by "
                           "a few ulp",
        },
    }
    return summary


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--vpk", type=Path, default=None)
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args(argv)
    summary = verify(args.log, args.vpk)
    text = json.dumps(summary, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    sys.stdout.write(text)
    return 1 if summary["problems"] else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

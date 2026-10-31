#!/usr/bin/env python3
"""Verify the S2/P2 GXM uint/float carrier probe and classify its evidence.

The probe (src/probes/gxm_s2_uint_probe.c) draws one one-pixel quad per case
into an F32_R target and logs the input and result bits.  This tool compares
the run with the binary32 model of the A3 helpers, but keeps four kinds of
result apart:

  pass             exact expected value; a mismatch is a probe regression
  rounding         correctly-rounded ideal; a deviation is recorded, because
                   the target rounding mode is part of what is being measured
  ftz              the target may flush a subnormal input/output; either the
                   preserved bits or zero are recorded, neither is a failure
  nan              a NaN payload is read back; preservation is recorded
  observed-broken  the A3 helper as originally emitted on this target; the
                   deviation from the ideal is the finding, not a probe error
  measured-math    exp2/log/pow/int behaviour the lowering must not assume;
                   the measured target result is asserted so a change is seen
  conversion       int()/trunc() semantics used by the A3 lowering

The run this tool was written for is run7: the original helpers, the
scale-normalized decoder candidate, and the split int->float candidate.  The
two candidate helpers are emitted by tools/shader/shader_ir.py after the P2
finding; the original-helper cases stay in the probe as the differential.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path

GXM_ERROR_UNSUPPORTED_U32 = 0x805B0009
GXM_ERROR_UNSUPPORTED_S32 = GXM_ERROR_UNSUPPORTED_U32 - (1 << 32)
FLT_MIN_PATTERN = 0x00800000
FLT_MAX_PATTERN = 0x7F7FFFFF
NAN_PATTERN = 0x7FC12345


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def value_of(pattern: int) -> float:
    return struct.unpack("<f", struct.pack("<I", pattern))[0]


def is_nan(pattern: int) -> bool:
    return (pattern & 0x7F800000) == 0x7F800000 and (pattern & 0x007FFFFF) != 0


def signed(pattern: int) -> int:
    return pattern if pattern < 1 << 31 else pattern - (1 << 32)


def unsigned_from_float(pattern: int) -> int:
    return signed(pattern) & 0xFFFFFFFF


# --- expected model -------------------------------------------------------

def expected_for(name: str, x: int, y: int, corrected_helpers: bool = False):
    """Return (class, ideal_bits_or_None, allowed_bits_or_None, note)."""
    if corrected_helpers:
        kind, ideal, allowed, note = expected_for(name, x, y)
        if kind == 'observed-broken' and name.startswith(('decode_', 'uitof_')):
            return ('pass', ideal, allowed, 'current emitted P2 helper; exact result required')
        return kind, ideal, allowed, note
    if name.startswith(("exp2fix_", "log2fix_")):
        return ("skipped", None, None, "inactive candidate program")
    if name.startswith("pass_") or name.startswith("cal_"):
        if name == "pass_nan_payload":
            return ("nan", None, None, "payload read back; preservation recorded")
        return ("pass", x, None, "bit-pattern pass-through")
    if name.startswith("decode_fix_"):
        if name == "decode_fix_subnormal_min":
            return ("ftz", None, {0, bits(1.0)}, "subnormal input may be flushed")
        if name == "decode_fix_zero_neg":
            return ("pass", bits(f32(2147483648.0)), None, "negative zero pattern")
        pattern = x
        if is_nan(pattern):
            return ("nan", None, None, "NaN payload read back")
        return ("pass", bits(f32(float(pattern))), None, "scale-normalized decoder")
    if name.startswith("decode_int_"):
        pattern = x
        if is_nan(pattern):
            return ("nan", None, None, "NaN payload read back")
        if pattern < FLT_MIN_PATTERN and pattern != 0:
            return ("ftz", None, {0, bits(f32(float(pattern)))},
                    "subnormal pattern, target may flush")
        return ("observed-broken", bits(f32(float(signed(pattern)))), None,
                "original signed field decoder")
    if name.startswith("decode_"):
        pattern = x
        if is_nan(pattern):
            return ("nan", None, None, "NaN payload read back")
        if pattern < FLT_MIN_PATTERN and pattern != 0:
            return ("ftz", None, {0, bits(f32(float(pattern)))},
                    "subnormal pattern, target may flush")
        return ("observed-broken", bits(f32(float(pattern))), None,
                "original unsigned field decoder")
    if name.startswith("uitof_fix_"):
        return ("pass", bits(f32(float(int(value_of(x)) & 0xFFFFFFFF))), None,
                "split int-to-float conversion")
    if name.startswith("uitof_"):
        return ("observed-broken",
                bits(f32(float(int(value_of(x)) & 0xFFFFFFFF))), None,
                "direct int-to-float conversion")
    if name.startswith("i2f_plain_"):
        value = int(value_of(x))
        return ("observed-broken", bits(f32(float(value))), None,
                "plain function int-to-float conversion")
    if name.startswith("u2f_inline_"):
        value = int(value_of(x))
        return ("pass", bits(f32(float(value))), None,
                "inline float(int(x)) conversion")
    if name.startswith("mul_"):
        if name == "mul_subnormal_min_one":
            return ("ftz", None, {0, 0x00000001}, "subnormal input")
        if name == "mul_subnormal_max_one":
            return ("ftz", None, {0, 0x007FFFFF}, "subnormal input")
        if name == "mul_subnormal_min_half":
            return ("ftz", None, {0}, "subnormal input and underflow")
        if name == "mul_normal_min_half":
            return ("ftz", None, {0, 0x00400000}, "subnormal result")
        if name == "mul_normal_min_quarter":
            return ("ftz", None, {0, 0x00200000}, "subnormal result")
        if name == "mul_two24_one":
            return ("pass", bits(16777216.0), None, "exact product")
        if name == "mul_one_two_m24":
            return ("pass", bits(2.0 ** -24), None, "exact product")
    if name.startswith("add_"):
        if name == "add_two24_one":
            return ("rounding", bits(16777216.0), None, "2^24+1 rounds to even")
        if name == "add_two24_two":
            return ("rounding", bits(16777218.0), None, "exact")
        if name == "add_16777215_one":
            return ("rounding", bits(16777216.0), None, "exact after rounding")
        if name == "add_one_two_m24":
            return ("rounding", bits(1.0), None, "tie to even")
        if name == "add_one_three_m24":
            return ("rounding", 0x3F800002, None, "tie to even")
        if name == "add_two32_one":
            return ("rounding", bits(4294967296.0), None, "1 is below the ulp")
        if name == "add_fltmax_fltmax":
            return ("rounding", bits(float("inf")), None, "overflow")
    if name.startswith("exp2_"):
        if name == "exp2_zero":
            return ("measured-math", bits(1.0), None, "exp2(0)")
        if name == "exp2_half":
            return ("measured-math", bits(f32(math.exp(0.5))), None,
                    "target exp2(x) measured as exp(x)")
        if name in ("exp2_neg126", "exp2_neg149"):
            return ("measured-math", 0x00000000, None,
                    "exp underflow; exp2(-126) did not return FLT_MIN")
        if name in ("exp2_127", "exp2_128"):
            return ("measured-math", bits(float("inf")), None,
                    "exp overflow; exp2(127) did not return 2^127")
    if name.startswith("log2_"):
        if name == "log2_one":
            return ("measured-math", bits(0.0), None, "ln(1)")
        if name == "log2_tenth":
            return ("measured-math", bits(f32(math.log(0.1))), None,
                    "target log2(x) measured as ln(x)")
        if name == "log2_two24":
            return ("measured-math", bits(f32(math.log(2.0 ** 24))), None,
                    "target log2(x) measured as ln(x)")
        if name == "log2_subnormal_min":
            return ("ftz", None, {bits(float("-inf")), bits(-149.0)},
                    "subnormal input; -inf or -149 observe FTZ")
        if name == "log2_zero":
            return ("measured-math", bits(float("-inf")), None, "log(0)")
        if name == "log2_inf":
            return ("measured-math", bits(float("inf")), None, "log(inf)")
    if name.startswith("exp_"):
        if name == "exp_zero":
            return ("measured-math", bits(1.0), None, "exp(0)")
        if name == "exp_half":
            return ("measured-math",
                    bits(f32(math.exp(0.5 * math.log2(math.e)))), None,
                    "target exp(x) measured as the underlying exp primitive "
                    "applied to x*log2(e)")
        if name == "exp_neg126":
            return ("measured-math", 0, None,
                    "target exp underflows at this argument")
    if name.startswith("log_"):
        if name == "log_one":
            return ("measured-math", bits(0.0), None, "ln(1)")
        if name == "log_e":
            return ("measured-math", bits(f32(math.log(math.e) * math.log(2.0))), None,
                    "target log(x) measured as ln(x)*ln(2)")
        if name == "log_tenth":
            return ("measured-math",
                    bits(f32(math.log(0.1) * math.log(2.0))), None,
                    "target log(x) measured as ln(x)*ln(2)")
    if name.startswith("pow2_"):
        if name == "pow2_zero":
            return ("pass", bits(1.0), None, "pow(2,0)")
        if name == "pow2_half":
            return ("pass", bits(f32(math.sqrt(2.0))), None, "pow(2,0.5)")
        if name == "pow2_24":
            return ("pass", bits(16777216.0), None, "pow(2,24)")
        if name == "pow2_neg126":
            return ("pass", FLT_MIN_PATTERN, None, "pow(2,-126) kept FLT_MIN")
        if name == "pow2_127":
            return ("pass", 0x7F000000, None, "pow(2,127)")
    if name.startswith("int_"):
        if name == "int_two24":
            return ("conversion", bits(16777216.0), None, "int(2^24)")
        if name == "int_zero_neg":
            return ("conversion", 0x80000000, None, "int(-0) keeps negative zero")
        return ("conversion", bits(f32(float(math.floor(value_of(x))))), None,
                "target int() measured as floor")
    if name.startswith("trunc_"):
        if name == "trunc_two24":
            return ("pass", bits(16777216.0), None, "trunc(2^24)")
        return ("pass", bits(f32(float(math.trunc(value_of(x))))), None,
                "trunc() toward zero")
    return ("unknown", None, None, "no expected entry")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_log(path: Path) -> dict:
    names: dict[int, str] = {}
    inputs: dict[int, tuple[int, int]] = {}
    results: dict[int, int] = {}
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    for line in lines:
        parts = line.split()
        if not parts:
            continue
        head = parts[0]
        if head.startswith("case="):
            index = int(head.split("=", 1)[1])
            names[index] = parts[1]
            values = {}
            for item in parts[1:]:
                if "=" in item:
                    key, value = item.split("=", 1)
                    values[key] = value
            inputs[index] = (int(values.get("x", "0"), 16), int(values.get("y", "0"), 16))
        elif head.startswith("result="):
            index = int(head.split("=", 1)[1])
            results[index] = int(parts[1].split("=", 1)[1], 16)
    complete = any(line.strip() == "RESULT COMPLETE" for line in lines)
    failed = any(line.strip() == "RESULT FAIL" for line in lines)
    return {"names": names, "inputs": inputs, "results": results,
            "complete": complete, "failed": failed, "lines": len(lines)}


def verify(log_path: Path, vpk_path: Path | None, corrected_helpers: bool = False) -> dict:
    parsed = parse_log(log_path)
    problems: list[str] = []
    findings: list[dict] = []
    counts: dict[str, int] = {}
    skipped = 0

    if not parsed["complete"] or parsed["failed"]:
        problems.append("probe did not end with RESULT COMPLETE")
    if corrected_helpers and (len(parsed['names']) != 146 or len(parsed['results']) != 138):
        problems.append('corrected-helper regression requires all 146 cases / 138 results')

    for index, name in sorted(parsed["names"].items()):
        x, y = parsed["inputs"].get(index, (0, 0))
        kind, ideal, allowed, note = expected_for(name, x, y, corrected_helpers)
        if kind == "unknown":
            problems.append(f"case {index} {name}: no expected entry")
            continue
        if kind == "skipped":
            skipped += 1
            continue
        if index not in parsed["results"]:
            problems.append(f"case {index} {name}: missing result")
            continue
        observed = parsed["results"][index]
        entry = {"index": index, "name": name, "class": kind,
                 "observed_bits": f"{observed:08x}", "note": note}
        counts[kind] = counts.get(kind, 0) + 1

        if kind == "nan":
            if not is_nan(observed):
                problems.append(f"case {index} {name}: {observed:08x} is not NaN")
            else:
                entry["payload_preserved"] = observed == x
                if not entry["payload_preserved"]:
                    findings.append(entry)
        elif kind == "ftz":
            allowed = allowed or set()
            entry["allowed"] = sorted(f"{value:08x}" for value in allowed)
            if observed not in allowed:
                problems.append(
                    f"case {index} {name}: {observed:08x} is neither the "
                    f"preserved value nor zero")
            else:
                entry["preserved"] = observed != 0
        elif kind == "observed-broken":
            entry["ideal_bits"] = f"{ideal:08x}"
            entry["deviates"] = observed != ideal
            if entry["deviates"]:
                findings.append(entry)
        elif kind in ("pass", "conversion"):
            entry["ideal_bits"] = f"{ideal:08x}"
            if observed != ideal:
                problems.append(
                    f"case {index} {name}: {observed:08x} != {ideal:08x}")
        elif kind == "measured-math":
            entry["ideal_bits"] = f"{ideal:08x}"
            if observed != ideal:
                entry["deviates"] = True
                findings.append(entry)
        elif kind == "rounding":
            entry["ideal_bits"] = f"{ideal:08x}"
            if observed != ideal:
                entry["deviates"] = True
                findings.append(entry)
        else:
            problems.append(f"case {index} {name}: unknown class {kind}")

    summary = {
        "probe": "gxm-s2-p2-uint-float",
        "log": str(log_path),
        "log_sha256": sha256(log_path),
        "vpk_sha256": None if vpk_path is None else sha256(vpk_path),
        "complete": parsed["complete"],
        "cases": len(parsed["names"]),
        "counts": counts,
        "skipped": skipped,
        "problems": problems,
        "findings": findings,
        "measured": {
            "exp2": "exp(x) on this target; exp2(-126) and exp2(127) show "
                    "natural-exp range, not powers of two",
            "log2": "ln(x) on this target",
            "log": "ln(x)*ln(2) on this target",
            "pow": "pow(2,x) returned the expected powers of two, including "
                   "FLT_MIN at -126",
            "int": "int(x) floors; float(int(x)) keeps -0 and is exact at 2^24",
            "int_to_float": "direct (float)(int variable) is scaled by "
                            "65535/65536 for large values; the split 16-bit "
                            "form and inline float(int(x)) are exact",
            "ftz": "arithmetic flushes subnormal inputs and results; a "
                   "bit-pattern pass-through preserves them",
            "subnormal_decode": "the unsigned field decoder cannot recover a "
                                "subnormal pattern because the target flushes "
                                "the input before the multiply; recorded as an "
                                "unsupported range, not as a pass",
        },
        "unresolved": {
            "candidate_pow": "a decoder using pow(2,x) compiled but Vita3K's "
                             "USSE translator rejected branches in the shader "
                             "and the probe did not reach readback; the "
                             "scale-normalized form avoids pow/log2 entirely",
            "a3_corpus": "the corrected helper bodies are in shader_ir.py but "
                         "the A3 GXP corpus was built from the old bodies; the "
                         "affected programs must be regenerated and re-pinned "
                         "before a formal package",
        },
    }
    if corrected_helpers:
        summary['helper_mode'] = 'current-emitter; original decode/uitof cases require exact ideals'
        summary['corrected_helper_cases'] = sum(
            name.startswith(('decode_', 'uitof_')) and
            expected_for(name, *parsed['inputs'][index], True)[0] == 'pass'
            for index, name in parsed['names'].items())
        summary['unresolved']['a3_corpus'] = (
            'Corpus rebuild/manifest/ledger/pair identities are verified by the separate '
            'P2 rebuild audit; this probe alone does not establish their completion.')
    return summary


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--vpk", type=Path, default=None)
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument('--corrected-helpers', action='store_true',
                        help='Require current decode/uitof results to equal their ideals; keep raw compiler controls')
    args = parser.parse_args(argv)
    summary = verify(args.log, args.vpk, args.corrected_helpers)
    text = json.dumps(summary, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    sys.stdout.write(text)
    return 1 if summary["problems"] else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

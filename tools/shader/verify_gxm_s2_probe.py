#!/usr/bin/env python3
"""Verify the S2/P1 GXM sampler1D probe log and classify its evidence.

The probe (src/probes/gxm_s2_probe.c) writes one one-pixel quad per case.  This
tool parses the device log and checks the values that are decidable from GXM
semantics plus the recorded state-setter return codes.

Three classes are kept apart so an emulator limitation is never counted as a
pass:

  pass              the sampled value matches the value implied by the sampler
                    state that GXM accepted for a linear texture;
  api-rejected      sceGxmTextureSetUAddrMode rejected the mode (GXM requires a
                    swizzled texture for MIRROR and the border modes); the
                    pixel must equal the clamp fallback the probe programs
                    before applying a mode;
  unresolved-mip    the case selects a mip level via tex1Dlod/tex2Dlod.  The
                    Vita3K 0.2.1 GL renderer translates MIPMAP_POINT/LINEAR to
                    GL_NEAREST/GL_LINEAR (renderer/src/gl/texture_formats.cpp,
                    "TODO support mipmap"), so every LOD resolves to level 0.
                    The observed value is recorded, but it is not evidence
                    about hardware mip selection.

A border sample on a swizzled texture is recorded as observed-border: the GL
backend returns black, but GXM exposes no border-colour setter, so hardware
equivalence stays open.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

GXM_ERROR_UNSUPPORTED_U32 = 0x805B0009
GXM_ERROR_UNSUPPORTED_S32 = GXM_ERROR_UNSUPPORTED_U32 - (1 << 32)

# name -> (class, expected bits or None, extra)
# expected bits are the exact float32 bit patterns read back from the F32_R
# target.  "clamp" is the value expected when a rejected mode leaves the
# texture in the clamp state the probe sets first.
CASES = {
    "nearest_texel0": ("pass", 0x00000000),
    "nearest_texel1": ("pass", 0x3F800000),
    "nearest_texel7": ("pass", 0x40E00000),
    "linear_between_0_1": ("pass", 0x3F000000),
    "linear_between_3_4": ("pass", 0x40400000),
    "clamp_negative": ("pass", 0x00000000),
    "clamp_past_one": ("pass", 0x40E00000),
    "repeat_negative": ("pass", 0x40C00000),
    "repeat_past_one": ("pass", 0x40000000),
    "mirror_negative": ("api-rejected", 0x00000000),
    "mirror_past_one": ("api-rejected", 0x40E00000),
    "border_negative": ("api-rejected", 0x00000000),
    "border_past_one": ("api-rejected", 0x40E00000),
    "mip_lod1": ("unresolved-mip", None),
    "mip_lod2": ("unresolved-mip", None),
    "mip_lod3": ("unresolved-mip", None),
    "mip_linear_lod0_5": ("unresolved-mip", None),
    "v_zero": ("pass", 0x3F800000),
    "v_half": ("pass", 0x3F800000),
    "v_one": ("pass", 0x3F800000),
    "v_negative": ("pass", 0x3F800000),
    "v_past_one": ("pass", 0x3F800000),
    "v_repeat_past_one": ("pass", 0x3F800000),
    "implicit_texel1": ("pass", 0x3F800000),
    "implicit_linear": ("pass", 0x3F000000),
    "npot_nearest_texel0": ("pass", 0x00000000),
    "npot_nearest_texel5": ("pass", 0x40A00000),
    "npot_linear_0_1": ("pass", 0x3F000000),
    "npot_clamp_negative": ("pass", 0x00000000),
    "npot_clamp_past_one": ("pass", 0x40A00000),
    "npot_repeat_negative": ("pass", 0x40800000),
    "npot_repeat_past_one": ("pass", 0x3F800000),
    "npot_mirror_negative": ("api-rejected", 0x00000000),
    "npot_border_past_one": ("api-rejected", 0x40A00000),
    "swz_clamp_past_one": ("pass", 0x40A00000),
    "swz_border_past_one": ("observed-border", 0x00000000),
    "swz_border_negative": ("observed-border", 0x00000000),
    "swz_repeat_ignore_border": ("pass", 0x40A00000),
    "ctl_lod0": ("unresolved-mip", None),
    "ctl_lod1": ("unresolved-mip", None),
    "ctl_lod2": ("unresolved-mip", None),
    "ctl_lod3": ("unresolved-mip", None),
    "ctl_lod0_5_linear": ("unresolved-mip", None),
    "ctl_literal_lod2": ("unresolved-mip", None),
    "ctl2d_v_row0": ("pass", 0x3F800000),
    "ctl2d_v_row1": ("pass", 0x40000000),
}

CONST_EXPECTED = {0: 0x3E800000, 1: 0x3F000000, 2: 0x3F400000, 3: 0x3F800000}

# Mip cases are asserted to equal the base-level sample.  That is the current
# emulator behaviour this tool records as the unresolved value; if a future
# backend starts selecting levels, this table only documents what was seen.
MIP_BASE_BITS = {
    "mip_lod1": 0x40000000,
    "mip_lod2": 0x40000000,
    "mip_lod3": 0x40000000,
    "mip_linear_lod0_5": 0x3FC00000,
    "ctl_lod0": 0x3F800000,
    "ctl_lod1": 0x3F800000,
    "ctl_lod2": 0x3F800000,
    "ctl_lod3": 0x3F800000,
    "ctl_lod0_5_linear": 0x3F800000,
    "ctl_literal_lod2": 0x3F800000,
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_log(path: Path) -> dict:
    names: dict[int, str] = {}
    states: dict[int, dict[str, int]] = {}
    results: dict[int, tuple[int, float]] = {}
    consts: dict[int, tuple[int, float, int]] = {}
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    for line in lines:
        parts = line.split()
        if not parts:
            continue
        head = parts[0]
        if head.startswith("case="):
            index = int(head.split("=", 1)[1])
            names[index] = parts[1]
        elif head.startswith("case_state="):
            index = int(head.split("=", 1)[1])
            state = {}
            for item in parts[1:]:
                key, value = item.split("=", 1)
                state[key] = int(value)
            states[index] = state
        elif head.startswith("result="):
            index = int(head.split("=", 1)[1])
            bits = int(parts[1].split("=", 1)[1], 16)
            value = float(parts[2].split("=", 1)[1])
            results[index] = (bits, value)
        elif head.startswith("const_result="):
            index = int(head.split("=", 1)[1])
            bits = int(parts[1].split("=", 1)[1], 16)
            value = float(parts[2].split("=", 1)[1])
            expected = int(parts[3].split("=", 1)[1], 16)
            consts[index] = (bits, value, expected)
    complete = any(line.strip() == "RESULT COMPLETE" for line in lines)
    failed_lines = [line for line in lines if line.strip() == "RESULT FAIL"]
    return {
        "names": names,
        "states": states,
        "results": results,
        "consts": consts,
        "complete": complete,
        "failed": bool(failed_lines),
        "lines": len(lines),
    }


def verify(log_path: Path, vpk_path: Path | None) -> dict:
    parsed = parse_log(log_path)
    problems: list[str] = []
    cases: list[dict] = []
    counts = {"pass": 0, "api-rejected": 0, "unresolved-mip": 0, "observed-border": 0}

    if not parsed["complete"] or parsed["failed"]:
        problems.append("probe did not end with RESULT COMPLETE")

    seen = set()
    for index, name in sorted(parsed["names"].items()):
        expected = CASES.get(name)
        if expected is None:
            problems.append(f"case {index} {name}: no expected entry")
            continue
        kind, expected_bits = expected
        seen.add(name)
        if index not in parsed["results"]:
            problems.append(f"case {index} {name}: missing result")
            continue
        bits, value = parsed["results"][index]
        state = parsed["states"].get(index)
        entry = {
            "index": index,
            "name": name,
            "class": kind,
            "observed_bits": f"{bits:08x}",
            "observed_float": value,
        }
        if kind == "api-rejected":
            if state is None:
                problems.append(f"case {index} {name}: missing case_state")
            else:
                rc = state.get("u_rc")
                entry["u_rc"] = rc
                if rc != GXM_ERROR_UNSUPPORTED_S32:
                    problems.append(
                        f"case {index} {name}: u_rc {rc} is not "
                        f"SCE_GXM_ERROR_UNSUPPORTED ({GXM_ERROR_UNSUPPORTED_S32})"
                    )
            if expected_bits is not None and bits != expected_bits:
                problems.append(
                    f"case {index} {name}: rejected-mode fallback {bits:08x} "
                    f"!= clamp fallback {expected_bits:08x}"
                )
            counts["api-rejected"] += 1
        elif kind == "unresolved-mip":
            entry["mip_base_bits"] = f"{MIP_BASE_BITS[name]:08x}"
            if bits != MIP_BASE_BITS[name]:
                problems.append(
                    f"case {index} {name}: observed {bits:08x} is not the "
                    f"recorded base-level value {MIP_BASE_BITS[name]:08x}; "
                    "the emulator limitation note is stale, re-check before "
                    "changing the class"
                )
            counts["unresolved-mip"] += 1
        else:
            if expected_bits is not None and bits != expected_bits:
                problems.append(
                    f"case {index} {name}: {bits:08x} != {expected_bits:08x}"
                )
            counts[kind] += 1
        cases.append(entry)

    missing = set(CASES) - seen
    if missing:
        problems.append(f"missing cases: {sorted(missing)}")

    consts_ok = True
    for index, expected_bits in CONST_EXPECTED.items():
        if index not in parsed["consts"]:
            problems.append(f"const {index}: missing")
            consts_ok = False
            continue
        bits, value, logged_expected = parsed["consts"][index]
        if bits != expected_bits or logged_expected != expected_bits:
            problems.append(
                f"const {index}: {bits:08x} expected {expected_bits:08x} "
                f"(probe logged {logged_expected:08x})"
            )
            consts_ok = False

    summary = {
        "probe": "gxm-s2-p1-sampler1d",
        "log": str(log_path),
        "log_sha256": sha256(log_path),
        "vpk_sha256": None if vpk_path is None else sha256(vpk_path),
        "complete": parsed["complete"],
        "cases": len(cases),
        "counts": counts,
        "constants_exact": consts_ok,
        "problems": problems,
        "results": cases,
        "unresolved": {
            "mip_selection": (
                "Vita3K 0.2.1 GL renderer maps MIPMAP_POINT/LINEAR to "
                "GL_NEAREST/GL_LINEAR (TODO support mipmap), so tex1Dlod/"
                "tex2Dlod LOD always resolves to level 0; hardware mip "
                "selection is not evidenced by this run"
            ),
            "border_color": (
                "GXM exposes no border-colour setter; the GL backend returns "
                "black for CLAMP_FULL_BORDER. Recorded as observed-border, "
                "not as hardware equivalence"
            ),
            "v_height1": (
                "height=1 V invariance is established on the device, and the "
                "8x2 ctl2d_v_row0/1 cases prove V delivery, but whether GXM "
                "ignores V or clamps it for 1D samples is not observable with "
                "a height=1 texture"
            ),
        },
    }
    return summary


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="gxm-s2.log from the probe run")
    parser.add_argument("--vpk", type=Path, default=None, help="VPK that produced the log")
    parser.add_argument("--output", type=Path, default=None, help="write the JSON summary here")
    parser.add_argument("--require-pass", action="store_true",
                        help="exit non-zero if any case is not class pass")
    args = parser.parse_args(argv)

    summary = verify(args.log, args.vpk)
    text = json.dumps(summary, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    sys.stdout.write(text)
    if summary["problems"]:
        return 1
    if args.require_pass and summary["counts"]["pass"] != summary["cases"]:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

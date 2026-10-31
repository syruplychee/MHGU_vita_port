#!/usr/bin/env python3
"""Compare two static shader packages without trusting either manifest.

The runtime contract is "the observed programs keep resolving to the same GXP
bytes".  This verifier reads the review sidecar (shader-index.json) emitted by
pack_static_shader_package.py, checks the files it names against their recorded
SHA-256, and reports every old variant that disappeared or changed.  It is a
host check only; it does not replace the C reader or a target run.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

VIEWPORTS = ((0, 1), (1, 2))


def load_package(root):
    root = Path(root)
    sidecar = root / "shader-index.json"
    if not sidecar.is_file():
        raise SystemExit(f"missing sidecar: {sidecar}")
    data = json.loads(sidecar.read_text(encoding="utf-8"))
    if data.get("schema") != 1 or data.get("abi_version") != 1:
        raise SystemExit(f"unsupported sidecar schema/ABI in {sidecar}")
    stages = {}
    stages_by_index = {}
    for stage in data["stages"]:
        key = (stage["stage"], stage["stage_sha256"])
        if key in stages:
            raise SystemExit(f"duplicate stage content: {key}")
        if stage["index"] in stages_by_index:
            raise SystemExit(f"duplicate stage index: {stage['index']}")
        stages[key] = stage
        stages_by_index[stage["index"]] = stage
    variants = {}
    for variant in data["variants"]:
        rel = variant["gxp"]
        path = (root / rel).resolve()
        if root.resolve() not in path.parents:
            raise SystemExit(f"payload escapes package: {rel}")
        payload = path.read_bytes()
        digest = hashlib.sha256(payload).hexdigest()
        if len(payload) != variant["gxp_bytes"] or digest != variant["gxp_sha256"]:
            raise SystemExit(f"payload identity mismatch: {rel}")
        key = (variant["stage_index"], variant["key"])
        if key in variants:
            raise SystemExit(f"duplicate variant key: {key}")
        variants[key] = dict(variant, payload=payload)
    programs = {}
    for program in data["programs"]:
        token = program["token"]
        if token in programs:
            raise SystemExit(f"duplicate program token: {token}")
        programs[token] = program
    return data, stages, stages_by_index, variants, programs


def variant_for(variants, stage_index, viewport):
    bit = 1 << viewport
    found = [v for (si, _), v in variants.items()
             if si == stage_index and (v["viewport_mask"] & bit)]
    if len(found) > 1:
        raise SystemExit(f"overlapping variants for stage {stage_index} vp{viewport}")
    return found[0] if found else None


def observed_mapping(stages_by_index, variants, programs):
    result = {}
    for token, program in sorted(programs.items()):
        if not (program["flags"] & 1):
            continue
        for viewport, _ in VIEWPORTS:
            stages_here = {}
            for kind, index in (("vert", program["vertex_stage"]),
                                ("frag", program["fragment_stage"])):
                if index == 0xFFFF:
                    continue
                stage = stages_by_index.get(index)
                if stage is None or stage["stage"] != kind:
                    raise SystemExit(f"program {token} has invalid {kind} stage {index}")
                variant = variant_for(variants, stage["index"], viewport)
                stages_here[kind] = None if variant is None else {
                    "key": variant["key"],
                    "sha256": variant["gxp_sha256"],
                    "bytes": variant["gxp_bytes"],
                }
            result[(token, viewport)] = stages_here
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old", required=True)
    parser.add_argument("--new", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    old_data, old_stages, old_stages_by_index, old_variants, old_programs = load_package(args.old)
    new_data, new_stages, new_stages_by_index, new_variants, new_programs = load_package(args.new)

    old_observed = observed_mapping(old_stages_by_index, old_variants, old_programs)
    new_observed = observed_mapping(new_stages_by_index, new_variants, new_programs)

    missing_observed = []
    changed_observed = []
    unresolved = []
    for key, old_slots in old_observed.items():
        token, viewport = key
        new_slots = new_observed.get(key)
        if new_slots is None:
            missing_observed.append({"token": token, "viewport": viewport})
            continue
        for kind, old_slot in old_slots.items():
            new_slot = new_slots.get(kind)
            if old_slot is None and new_slot is None:
                continue
            if old_slot is None or new_slot is None:
                changed_observed.append({"token": token, "viewport": viewport,
                                         "stage": kind, "old": old_slot,
                                         "new": new_slot})
            elif old_slot != new_slot:
                changed_observed.append({"token": token, "viewport": viewport,
                                         "stage": kind, "old": old_slot,
                                         "new": new_slot})
            elif not any(k[1] == new_slot["key"] for k in new_variants):
                unresolved.append({"token": token, "viewport": viewport,
                                   "stage": kind, "key": new_slot["key"]})

    # Every old payload that still has the same variant key must be byte
    # identical; a changed key is reported but can be a legitimate rebuild.
    changed_keys = []
    removed_keys = []
    for key, old_variant in old_variants.items():
        new_variant = new_variants.get(key)
        if new_variant is None:
            removed_keys.append({"stage_index": key[0], "key": key[1]})
        elif old_variant["payload"] != new_variant["payload"]:
            changed_keys.append({"stage_index": key[0], "key": key[1],
                                 "old_sha256": old_variant["gxp_sha256"],
                                 "new_sha256": new_variant["gxp_sha256"]})

    added_stages = [list(k) for k in new_stages if k not in old_stages]
    added_variants = [{"stage_index": k[0], "key": k[1]}
                      for k in new_variants if k not in old_variants]
    passed = not (missing_observed or changed_observed or unresolved or changed_keys)
    report = {
        "schema": 1,
        "old": str(Path(args.old).resolve()),
        "new": str(Path(args.new).resolve()),
        "passed": passed,
        "old_counts": {
            "stages": len(old_stages), "variants": len(old_variants),
            "programs": len(old_programs),
            "observed_programs": sum(1 for p in old_programs.values() if p["flags"] & 1),
        },
        "new_counts": {
            "stages": len(new_stages), "variants": len(new_variants),
            "programs": len(new_programs),
            "observed_programs": sum(1 for p in new_programs.values() if p["flags"] & 1),
        },
        "old_observed_slots": len(old_observed),
        "missing_observed": missing_observed,
        "changed_observed": changed_observed,
        "unresolved_observed": unresolved,
        "removed_variant_keys": removed_keys,
        "changed_variant_payloads": changed_keys,
        "added_stages": added_stages[:200],
        "added_stage_count": len(added_stages),
        "added_variants": added_variants[:200],
        "added_variant_count": len(added_variants),
    }
    Path(args.output).write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print(f"package-compare: {'PASS' if passed else 'FAIL'} "
          f"old_variants={len(old_variants)} new_variants={len(new_variants)} "
          f"added={len(added_variants)} changed={len(changed_keys)} "
          f"missing_observed={len(missing_observed)}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())

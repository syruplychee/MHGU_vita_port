#!/usr/bin/env python3
"""Build a machine-readable inventory of the locked MHGU static-resource inputs.

Stage C0 of ``docs/DEEPSEEK_STATIC_RESOURCE_GUIDE.md``.

The tool streams ``manifest.jsonl`` and ``conversion-manifest.jsonl`` in
lockstep (the two files are 268,485 rows and are never loaded into memory as a
whole).  For every payload row it records:

* the source ARC, entry index/path, DTI, original extension and conversion
  status;
* the manifest SHA-256 (the extractor's hash of the whole extracted file);
* the source-ARC identity against the *three* possible roots (base game,
  update, and the locked RomFS tree), with an explicit not-checked state;
* a structure record: real TEX header/layout parsing, MOD v0xd4 header and
  mesh-stream metadata, or an explicit opaque-preserved record;
* a status of ``verified`` / ``unverified`` / ``unknown-source`` /
  ``unsupported-format`` / ``failed`` and a reason.

ARC identity hashing is deliberately timeboxed.  A precomputed identity cache
is consumed when available; sources outside it are marked ``not-checked``
rather than assumed identical.  Payload SHA-256 is re-read for a deterministic
per-extension sample and for every representative asset.  No count in the
summary is presented as proof of full byte coverage.

Outputs (all under the caller's ``--output`` directory):

    inventory.jsonl          one row per manifest payload
    source-identity.jsonl    one row per unique source ARC
    unsupported.jsonl        TEX/MOD rows whose structure is not supported
    failed.jsonl             rows that could not be parsed
    unknown-sources.jsonl    rows whose source ARC failed identity checks
    unverified-sources.jsonl unique sources without checked identity
    sha-samples.jsonl        payload SHA-256 re-verification sample
    representative.json      verified TEX/MOD selection + rejected examples
    summary.json             counts, input hashes and explicit limitations
    commands.txt             exact invocation and tool version
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, SCRIPT_DIR)
import tex_format as tf  # noqa: E402

TOOL_VERSION = "mhgu-static-inventory-1"
INVENTORY_SCHEMA = "mhgu-static-asset-inventory/1"
SUMMARY_SCHEMA = "mhgu-static-asset-inventory-summary/1"
SELECTION_SCHEMA = "mhgu-static-asset-selection/1"
SOURCE_SCHEMA = "mhgu-static-source-identity/1"

DEFAULT_EXTRACTED = os.path.normpath(os.path.join(REPO_ROOT, "..", "extracted-resources"))
DEFAULT_BASE = os.path.normpath(os.path.join(REPO_ROOT, "..", "unpacked", "game", "1"))
DEFAULT_UPDATE = os.path.normpath(os.path.join(REPO_ROOT, "..", "unpacked", "update-mhgu", "game", "1"))
DEFAULT_LOCKED = os.path.join(REPO_ROOT, "build", "vita3k-pref", "ux0", "data",
                              "mhgu-vita-port", "romfs-tree")
DEFAULT_ARC_CACHE = os.path.join(
    REPO_ROOT, "artifacts", "deepseek-static-20261022-171155", "asset-scan",
    "arc-identity.jsonl",
)

MOD_HEADER_OFFSET = 6
MOD_FIXED_PREFIX = 132
MOD_MESH_SIZE = 48
MOD_MATERIAL_NAME_SIZE = 128
MOD_GROUP_SIZE = 32
MOD_BONE_SIZE = 24
MOD_MATRIX_SIZE = 64
MOD_SUPPORTED_VERSION = 0xD4

# Formats that must be represented in the host selection when present.
TEX_SELECTION_FORMATS = [0x07, 0x13, 0x17, 0x1F, 0x19, 0x15, 0x2A]


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_row(obj) -> str:
    return json.dumps(obj, sort_keys=False, separators=(",", ":"), ensure_ascii=True)


def count_files(root: str) -> int:
    total = 0
    for _dirpath, _dirnames, filenames in os.walk(root):
        total += len(filenames)
    return total


def load_arc_cache(path: str | None) -> tuple[dict[str, dict], dict]:
    cache: dict[str, dict] = {}
    info = {"path": path, "rows_loaded": 0, "malformed_lines": 0}
    if not path or not os.path.isfile(path):
        info["present"] = False
        return cache, info
    info["present"] = True
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                info["malformed_lines"] += 1
                continue
            source = row.get("source")
            if not isinstance(source, str):
                info["malformed_lines"] += 1
                continue
            cache[source] = row
    info["rows_loaded"] = len(cache)
    return cache, info


def state_from_cache_row(row: dict | None) -> dict:
    """Classify a source ARC against base/update/locked using hashes only."""
    if row is None:
        return {
            "checked": False,
            "verified": None,
            "state": "not-checked",
            "matched_version": None,
            "reason": "source ARC identity not checked (timeboxed sampling)",
            "base_present": None,
            "update_present": None,
            "locked_present": None,
        }
    base = row.get("base")
    update = row.get("update")
    locked = row.get("locked")
    base_present = base is not None
    update_present = update is not None
    locked_present = locked is not None
    if not locked_present:
        return {
            "checked": True,
            "verified": False,
            "state": "absent-from-locked-tree",
            "matched_version": "not-in-locked-tree",
            "reason": "source ARC is absent from the locked RomFS tree",
            "base_present": base_present,
            "update_present": update_present,
            "locked_present": False,
        }
    if update_present and locked == update:
        matched = "update-mhgu/game/1"
        if base_present and base == locked:
            matched = "base game/1 and update-mhgu/game/1 (byte-identical)"
        return {
            "checked": True,
            "verified": True,
            "state": "locked-identical-update",
            "matched_version": matched,
            "reason": "locked RomFS file is byte-identical to the update source ARC",
            "base_present": base_present,
            "update_present": True,
            "locked_present": True,
            "base_differs_from_update": base_present and base != update,
        }
    if base_present and locked == base:
        return {
            "checked": True,
            "verified": True,
            "state": "locked-identical-base-only",
            "matched_version": "base game/1",
            "reason": "locked RomFS file is byte-identical to the base source ARC",
            "base_present": True,
            "update_present": update_present,
            "locked_present": True,
            "base_differs_from_update": update_present and update != base,
        }
    return {
        "checked": True,
        "verified": False,
        "state": "locked-differs-from-both",
        "matched_version": "locked-file-differs-from-manifest-sources",
        "reason": "locked RomFS file exists but matches neither base nor update source ARC",
        "base_present": base_present,
        "update_present": update_present,
        "locked_present": True,
    }


def hash_source_identity(source: str, roots: dict[str, str]) -> dict:
    hashes = {}
    for key in ("base", "update", "locked"):
        path = os.path.join(roots[key], source.replace("/", os.sep))
        hashes[key] = sha256_file(path) if os.path.isfile(path) else None
    base = hashes["base"]
    update = hashes["update"]
    locked = hashes["locked"]
    return {
        "source": source,
        "base": base,
        "update": update,
        "locked": locked,
        "locked_eq_base": locked is not None and locked == base,
        "locked_eq_update": locked is not None and locked == update,
        "base_eq_update": base is not None and base == update,
        "entries": None,
        "bytes": None,
        "on_demand": True,
    }


def best_effort_tex(path: str) -> dict | None:
    try:
        with open(path, "rb") as handle:
            head = handle.read(0x18)
    except OSError:
        return None
    if len(head) < 0x18:
        return {"file_size": os.path.getsize(path), "short_header": len(head)}
    packed = struct.unpack_from("<I", head, 0x08)[0]
    return {
        "file_size": os.path.getsize(path),
        "magic": head[:4].decode("latin-1"),
        "layout_word": struct.unpack_from("<I", head, 0x04)[0],
        "reserved_0c": head[0x0C],
        "format_code": head[0x0D],
        "resource_type": head[0x0E],
        "flags": head[0x0F],
        "packed_dimensions": packed,
        "mip_count": packed & 0x3F,
        "width": (packed >> 6) & 0x1FFF,
        "height": (packed >> 19) & 0x1FFF,
        "payload_size_field": struct.unpack_from("<I", head, 0x10)[0],
        "reserved_14": struct.unpack_from("<I", head, 0x14)[0],
    }


def inspect_tex_structure(path: str, raw: str) -> dict:
    best = best_effort_tex(path)
    try:
        header = tf.parse_tex_file(path)
    except OSError as exc:
        return {
            "kind": "tex", "status": "failed",
            "reason": f"cannot read TEX input: {exc}",
            "subresource_count": 0, "faces": None, "layers": None,
            "layout_verified": False,
        }
    except tf.TexFormatError as exc:
        best = best or {}
        lowered = os.path.basename(raw).lower()
        if exc.reason in ("bad-magic", "truncated-header"):
            status, cls = "failed", "not-a-parsable-tex-container"
        elif best.get("reserved_0c") == 6 or "_cm" in lowered:
            status, cls = "unsupported", "cube-cm-variant-unresolved"
        elif best.get("format_code") is not None and best.get("format_code") not in tf.FORMATS:
            status, cls = "unsupported", f"unsupported-format-0x{best['format_code']:02x}"
        else:
            status, cls = "unsupported", "container-variant-unresolved"
        return {
            "kind": "tex",
            "status": status,
            "reason": f"TEX header parse rejected: {exc.reason}: {exc.detail}",
            "unsupported_class": cls if status == "unsupported" else None,
            "format_code": best.get("format_code"),
            "format_name": (tf.format_name(best["format_code"])
                            if best.get("format_code") is not None else None),
            "resource_type": best.get("resource_type"),
            "width": best.get("width"),
            "height": best.get("height"),
            "mip_count": best.get("mip_count"),
            "subresource_count": 0,
            "faces": None,
            "layers": None,
            "layout_verified": False,
            "layout_reason": exc.reason,
            "raw_evidence": best,
        }
    layout = tf.compute_layout(header, require_decoder=False)
    base = {
        "kind": "tex",
        "format_code": header.format_code,
        "format_name": tf.format_name(header.format_code),
        "resource_type": header.resource_type,
        "width": header.width,
        "height": header.height,
        "mip_count": header.mip_count,
        "subresource_count": len(layout.levels),
        "layout_verified": layout.layout_ok,
        "layout_reason": layout.layout_reason,
        "block_height_gobs0": layout.block_height_gobs0,
        "payload_size": header.payload_size,
        "payload_offset": header.payload_offset,
        "file_size": header.file_size,
        "trailing_bytes": header.trailing_bytes,
        "layout_word": header.layout_word,
        "reserved_0c": header.reserved_0c,
        "flags": header.flags,
    }
    if layout.layout_ok:
        # resource_type 1 is the single-layer 2D form validated across the
        # corpus; no TEX in the locked corpus declares multiple faces/layers.
        base.update({
            "status": "verified",
            "reason": layout.layout_reason,
            "unsupported_class": None,
            "faces": 1,
            "layers": 1,
            "faces_evidence": "resource_type 1 (single-layer 2D); no face table exists",
        })
        return base
    if header.format is None:
        cls = f"unsupported-format-0x{header.format_code:02x}"
        reason = f"format 0x{header.format_code:02x} has no verified block geometry"
    elif header.resource_type != tf.RESOURCE_TYPE_2D:
        cls = f"unsupported-resource-type-0x{header.resource_type:02x}"
        reason = layout.layout_reason
    elif not header.format.decoder:
        cls = "recognized-format-without-host-decoder"
        reason = layout.layout_reason
    else:
        cls = "layout-not-reproduced"
        reason = layout.layout_reason
    base.update({
        "status": "unsupported",
        "reason": reason,
        "unsupported_class": cls,
        "faces": None,
        "layers": None,
    })
    return base


def _read_u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _read_u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def parse_mod_structure(path: str, with_names: bool = False) -> dict:
    size = os.path.getsize(path)
    with open(path, "rb") as handle:
        head = handle.read(MOD_FIXED_PREFIX)
    if len(head) < MOD_FIXED_PREFIX:
        return {"kind": "mod", "status": "failed", "reason": "MOD is shorter than its fixed header",
                "subresource_count": 0, "mesh_count": 0, "file_size": size}
    if head[:4] != b"MOD\0":
        return {"kind": "mod", "status": "failed", "reason": "MOD magic is missing",
                "subresource_count": 0, "mesh_count": 0, "file_size": size}
    version = _read_u16(head, 4)
    h = MOD_HEADER_OFFSET
    header = {
        "version": version,
        "bone_count": _read_u16(head, h + 0),
        "mesh_count": _read_u16(head, h + 2),
        "material_count": _read_u16(head, h + 4),
        "vertex_count": _read_u32(head, h + 6),
        "triangle_index_count": _read_u32(head, h + 10),
        "vertex_id_count": _read_u32(head, h + 14),
        "vertex_buffer_size": _read_u32(head, h + 18),
        "padding": _read_u32(head, h + 22),
        "group_count": _read_u32(head, h + 26),
        "bone_offset": _read_u32(head, h + 30),
        "group_offset": _read_u32(head, h + 34),
        "material_names_offset": _read_u32(head, h + 38),
        "mesh_offset": _read_u32(head, h + 42),
        "vertex_offset": _read_u32(head, h + 46),
        "triangle_offset": _read_u32(head, h + 50),
        "end_size": _read_u32(head, h + 54),
    }
    base = {
        "kind": "mod",
        "version": version,
        "header": header,
        "file_size": size,
        "mesh_count": header["mesh_count"],
        "subresource_count": header["mesh_count"],
        "material_count": header["material_count"],
        "vertex_count": header["vertex_count"],
        "attribute_semantics": "not inferred; FVF->semantic mapping requires MFX evidence",
    }
    if version != MOD_SUPPORTED_VERSION:
        base.update({"status": "unsupported", "reason":
                     f"MOD version 0x{version:04x} is not the locked v0xd4 layout",
                     "unsupported_class": f"unsupported-mod-version-0x{version:04x}"})
        return base
    problems = []

    def check_range(offset: int, length: int, label: str) -> None:
        if length == 0:
            return
        if offset > size or length > size - offset:
            problems.append(f"{label} range {offset}+{length} exceeds file size {size}")

    check_range(header["mesh_offset"], header["mesh_count"] * MOD_MESH_SIZE, "mesh table")
    check_range(header["material_names_offset"], header["material_count"] * MOD_MATERIAL_NAME_SIZE,
                "material name table")
    check_range(header["group_offset"], header["group_count"] * MOD_GROUP_SIZE, "group table")
    check_range(header["vertex_offset"], header["vertex_buffer_size"], "vertex buffer")
    check_range(header["triangle_offset"], header["triangle_index_count"] * 2, "triangle index buffer")
    if header["bone_count"]:
        bone_bytes = header["bone_count"] * MOD_BONE_SIZE
        check_range(header["bone_offset"], bone_bytes + header["bone_count"] * MOD_MATRIX_SIZE * 2 + 256,
                    "bone/matrix data")
    if problems:
        base.update({"status": "failed", "reason": "; ".join(problems[:4]),
                     "range_problems": problems[:16]})
        return base
    streams = []
    fvf_values = set()
    stride_values = set()
    with open(path, "rb") as handle:
        if header["mesh_count"]:
            handle.seek(header["mesh_offset"])
            table = handle.read(header["mesh_count"] * MOD_MESH_SIZE)
            for index in range(header["mesh_count"]):
                o = index * MOD_MESH_SIZE
                record = {
                    "index": index,
                    "mesh_type": _read_u16(table, o + 0),
                    "vertex_count": _read_u16(table, o + 2),
                    "group_index": table[o + 4],
                    "material_index": table[o + 6],
                    "lod_level": table[o + 7],
                    "vertex_stride": table[o + 10],
                    "render_mode": table[o + 11],
                    "vertex_start": _read_u32(table, o + 12),
                    "vertex_base": _read_u32(table, o + 16),
                    "fvf_info": _read_u32(table, o + 20),
                    "face_start": _read_u32(table, o + 24),
                    "face_count": _read_u32(table, o + 28),
                    "face_base": _read_u32(table, o + 32),
                    "bone_map_index": table[o + 36],
                    "weight_maps": table[o + 37],
                    "min_index": _read_u16(table, o + 40),
                    "max_index": _read_u16(table, o + 42),
                }
                fvf_values.add(record["fvf_info"])
                stride_values.add(record["vertex_stride"])
                if len(streams) < 8:
                    streams.append(record)
                if record["vertex_stride"] == 0 and record["vertex_count"] != 0:
                    problems.append(f"mesh {index}: vertices with zero stride")
                if record["face_count"] and (
                        record["face_start"] + record["face_count"] > header["triangle_index_count"]):
                    problems.append(f"mesh {index}: face range exceeds index buffer")
        names = []
        if with_names and header["material_count"]:
            handle.seek(header["material_names_offset"])
            raw_names = handle.read(header["material_count"] * MOD_MATERIAL_NAME_SIZE)
            for index in range(header["material_count"]):
                chunk = raw_names[index * MOD_MATERIAL_NAME_SIZE:
                                 (index + 1) * MOD_MATERIAL_NAME_SIZE]
                text = chunk.split(b"\0", 1)[0]
                try:
                    names.append(text.decode("ascii"))
                except UnicodeDecodeError:
                    names.append(text.decode("latin-1"))
    base.update({
        "mesh_streams": streams,
        "mesh_streams_truncated": len(streams) < header["mesh_count"],
        "fvf_info_values": sorted(fvf_values),
        "vertex_stride_values": sorted(stride_values),
    })
    if with_names:
        base["material_names"] = names[:32]
    if problems:
        base.update({"status": "failed", "reason": "; ".join(problems[:4]),
                     "range_problems": problems[:16]})
        return base
    base.update({"status": "verified", "reason": "MOD v0xd4 header and mesh table in bounds"})
    return base


def classify_status(source_state: dict, structure: dict) -> tuple[str, str]:
    if structure["status"] == "failed":
        return "failed", structure.get("reason", "structure parse failed")
    if structure["status"] == "unsupported":
        return "unsupported-format", structure.get("reason", "unsupported structure")
    if source_state["checked"] and not source_state["verified"]:
        return "unknown-source", source_state["reason"]
    if source_state["verified"]:
        return "verified", "source ARC identity matches locked RomFS tree"
    return "unverified", "source ARC identity not checked (timeboxed sampling)"


def insert_candidate(bucket: list, candidate: dict, limit: int, required_mips: int) -> None:
    rank = (
        0 if candidate["source_verified"] else 1,
        0 if candidate["mip_count"] >= required_mips else 1,
        candidate["bytes"],
    )
    bucket.append((rank, candidate))
    bucket.sort(key=lambda item: item[0])
    del bucket[limit:]


def materialize_tex(candidate: dict, extracted_root: str) -> tuple[dict | None, str]:
    path = os.path.join(extracted_root, candidate["raw"].replace("/", os.sep))
    if not os.path.isfile(path):
        return None, "missing-input"
    actual_sha = sha256_file(path)
    if actual_sha != candidate["sha256"]:
        return None, f"file sha256 {actual_sha} != manifest {candidate['sha256']}"
    try:
        header, layout = tf.inspect_file(path, require_decoder=True)
    except tf.TexFormatError as exc:
        return None, f"reparse {exc.reason}: {exc.detail}"
    if not layout.layout_ok:
        return None, f"layout {layout.layout_reason}"
    payload = tf.read_payload(path, header)
    item = {
        "asset_key": f"tex:{sha256_bytes(payload)}",
        "kind": "tex",
        "raw": candidate["raw"],
        "source": candidate["source"],
        "archive": candidate["archive"],
        "entry": candidate["entry"],
        "entry_path": candidate["entry_path"],
        "dti": candidate["dti"],
        "flags": candidate["flags"],
        "bytes": candidate["bytes"],
        "sha256": actual_sha,
        "payload_sha256": sha256_bytes(payload),
        "payload_sha256_verified": True,
        "file_sha256_verified": True,
        "conversion_status": candidate["conversion_status"],
        "format_code": header.format_code,
        "format_name": tf.format_name(header.format_code),
        "resource_type": header.resource_type,
        "width": header.width,
        "height": header.height,
        "mip_count": header.mip_count,
        "subresource_count": len(layout.levels),
        "faces": 1,
        "layers": 1,
        "layout_verified": True,
        "layout_reason": layout.layout_reason,
        "block_height_gobs0": layout.block_height_gobs0,
        "levels": [
            {
                "mip": level.mip,
                "width": level.width,
                "height": level.height,
                "offset": level.offset,
                "stored_size": level.stored_size,
                "logical_size": level.logical_size,
                "padded_size": level.padded_size,
                "alignment": level.alignment,
            }
            for level in layout.levels
        ],
        "notes": [
            "All mips present in the source are preserved; no mip is synthesized.",
            "resource_type 1 is single-layer 2D: one face and one layer, no face table.",
            "Channel swizzle is TIC sampling state, not TEX payload data, and is not guessed.",
        ],
    }
    return item, "ok"


def materialize_mod(candidate: dict, extracted_root: str) -> tuple[dict | None, str]:
    path = os.path.join(extracted_root, candidate["raw"].replace("/", os.sep))
    if not os.path.isfile(path):
        return None, "missing-input"
    actual_sha = sha256_file(path)
    if actual_sha != candidate["sha256"]:
        return None, f"file sha256 {actual_sha} != manifest {candidate['sha256']}"
    structure = parse_mod_structure(path, with_names=True)
    if structure["status"] != "verified":
        return None, structure.get("reason", "MOD reparse failed")
    item = {
        "asset_key": f"mod:{actual_sha}",
        "kind": "mod",
        "raw": candidate["raw"],
        "source": candidate["source"],
        "archive": candidate["archive"],
        "entry": candidate["entry"],
        "entry_path": candidate["entry_path"],
        "dti": candidate["dti"],
        "flags": candidate["flags"],
        "bytes": candidate["bytes"],
        "sha256": actual_sha,
        "file_sha256_verified": True,
        "conversion_status": candidate["conversion_status"],
        "header": structure["header"],
        "mesh_count": structure["mesh_count"],
        "subresource_count": structure["mesh_count"],
        "mesh_streams": structure["mesh_streams"],
        "mesh_streams_truncated": structure["mesh_streams_truncated"],
        "fvf_info_values": structure["fvf_info_values"],
        "vertex_stride_values": structure["vertex_stride_values"],
        "material_names": structure.get("material_names", []),
        "attribute_semantics": structure["attribute_semantics"],
        "notes": [
            "Header and mesh descriptors are parsed from explicit byte fields.",
            "FVF values and vertex strides are recorded as raw evidence, not semantic attribute names.",
            "Skinning/animation conversion is C3 scope and is not performed.",
        ],
    }
    return item, "ok"


def write_jsonl(handle, obj: dict) -> None:
    handle.write(canonical_row(obj))
    handle.write("\n")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", default=os.path.join(DEFAULT_EXTRACTED, "manifest.jsonl"))
    parser.add_argument("--conversion-manifest",
                        default=os.path.join(DEFAULT_EXTRACTED, "conversion-manifest.jsonl"))
    parser.add_argument("--extracted-root", default=DEFAULT_EXTRACTED)
    parser.add_argument("--base-root", default=DEFAULT_BASE)
    parser.add_argument("--update-root", default=DEFAULT_UPDATE)
    parser.add_argument("--locked-root", default=DEFAULT_LOCKED)
    parser.add_argument("--arc-cache", default=DEFAULT_ARC_CACHE)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)

    os.makedirs(args.output, exist_ok=True)
    out = lambda name: os.path.join(args.output, name)  # noqa: E731
    roots = {"base": args.base_root, "update": args.update_root, "locked": args.locked_root}
    cache, cache_info = load_arc_cache(args.arc_cache)
    states: dict[str, dict] = {}
    sources: dict[str, dict] = {}

    status_counts = collections.Counter()
    ext_counts = collections.Counter()
    conversion_counts = collections.Counter()
    structure_counts = collections.Counter()
    source_state_counts = collections.Counter()
    tex_total = tex_layout_ok = 0
    tex_format_counts = collections.Counter()
    tex_rtype_counts = collections.Counter()
    tex_unsupported_classes = collections.Counter()
    tex_mip_hist = collections.Counter()
    mod_total = mod_verified = mod_unsupported = mod_failed = 0
    mod_mesh_total = 0
    mod_stream_descriptors = 0
    mod_fvf_values = collections.Counter()
    mod_stride_values = collections.Counter()
    unsupported_written = failed_written = unknown_written = 0
    alignment_mismatches = 0
    rows = 0

    tex_candidates: dict[int, list] = {code: [] for code in TEX_SELECTION_FORMATS}
    mod_candidates: list = []
    rejected_samples: list[dict] = []
    cm_samples = 0
    fmt_samples: dict[int, int] = collections.Counter()
    volume_samples = 0
    sha_samples: dict[str, list] = collections.defaultdict(list)
    global_samples: list = []

    def get_source_state(source: str) -> dict:
        state = states.get(source)
        if state is None:
            state = state_from_cache_row(cache.get(source))
            states[source] = state
        return state

    with open(args.manifest, "r", encoding="utf-8") as manifest_handle, \
            open(args.conversion_manifest, "r", encoding="utf-8") as conversion_handle, \
            open(out("inventory.jsonl"), "w", encoding="utf-8") as inventory_handle, \
            open(out("unsupported.jsonl"), "w", encoding="utf-8") as unsupported_handle, \
            open(out("failed.jsonl"), "w", encoding="utf-8") as failed_handle, \
            open(out("unknown-sources.jsonl"), "w", encoding="utf-8") as unknown_handle:
        while True:
            manifest_line = manifest_handle.readline()
            conversion_line = conversion_handle.readline()
            if not manifest_line and not conversion_line:
                break
            if not manifest_line or not conversion_line:
                alignment_mismatches += 1
                break
            rows += 1
            record = json.loads(manifest_line)
            conversion = json.loads(conversion_line)
            if record.get("raw") != conversion.get("raw"):
                alignment_mismatches += 1
            raw = record.get("raw") or ""
            extension = (record.get("extension") or "").lower()
            source = record.get("source") or ""
            source_state = get_source_state(source)
            source_info = sources.setdefault(source, {"entries": 0, "bytes": 0})
            source_info["entries"] += 1
            source_info["bytes"] += int(record.get("bytes") or 0)
            ext_counts[extension] += 1
            conversion_counts[conversion.get("status")] += 1
            source_state_counts[source_state["state"]] += 1

            path = os.path.join(args.extracted_root, raw.replace("/", os.sep))
            if extension == "tex":
                tex_total += 1
                structure = inspect_tex_structure(path, raw)
                tex_format_counts[structure.get("format_code")] += 1
                tex_rtype_counts[structure.get("resource_type")] += 1
                tex_mip_hist[structure.get("mip_count")] += 1
                if structure["status"] == "verified":
                    tex_layout_ok += 1
                elif structure["status"] == "unsupported":
                    tex_unsupported_classes[structure.get("unsupported_class")] += 1
            elif extension == "mod":
                mod_total += 1
                try:
                    structure = parse_mod_structure(path)
                except OSError as exc:
                    structure = {"kind": "mod", "status": "failed",
                                 "reason": f"cannot read MOD: {exc}", "subresource_count": 0,
                                 "mesh_count": 0}
                if structure["status"] == "verified":
                    mod_verified += 1
                    mod_mesh_total += structure.get("mesh_count", 0)
                    for stream in structure.get("mesh_streams", []):
                        mod_stream_descriptors += 1
                        mod_fvf_values[stream["fvf_info"]] += 1
                        mod_stride_values[stream["vertex_stride"]] += 1
                elif structure["status"] == "unsupported":
                    mod_unsupported += 1
                elif structure["status"] == "failed":
                    mod_failed += 1
            else:
                structure = {
                    "kind": "opaque",
                    "status": "opaque",
                    "reason": "preserved as extracted bytes; no offline structure parser in C0",
                    "format": extension,
                }

            status, status_reason = classify_status(source_state, structure)
            status_counts[status] += 1
            structure_counts[structure["status"]] += 1

            row_out = {
                "schema": INVENTORY_SCHEMA,
                "row": rows,
                "raw": raw,
                "extension": extension,
                "source": source,
                "archive": record.get("archive"),
                "entry": record.get("entry"),
                "entry_path": record.get("entry_path"),
                "dti": record.get("dti"),
                "flags": record.get("flags"),
                "bytes": record.get("bytes"),
                "compressed_bytes": record.get("compressed_bytes"),
                "detected_type": record.get("detected_type"),
                "sha256_manifest": record.get("sha256"),
                "sha256_actual": None,
                "sha256_verified": None,
                "conversion_status": conversion.get("status"),
                "conversion_outputs": conversion.get("outputs") or [],
                "source_identity": {
                    "checked": source_state["checked"],
                    "verified": source_state["verified"],
                    "state": source_state["state"],
                    "matched_version": source_state["matched_version"],
                    "reason": source_state["reason"],
                    "table": "source-identity.jsonl",
                },
                "original_type": ("TEX" if extension == "tex" else
                                  "MOD" if extension == "mod" else f"opaque:{extension}"),
                "structure": structure,
                "status": status,
                "status_reason": status_reason,
            }
            write_jsonl(inventory_handle, row_out)

            if status in ("unsupported-format", "failed"):
                listing = {
                    "schema": "mhgu-static-unsupported/1",
                    "raw": raw,
                    "source": source,
                    "entry": record.get("entry"),
                    "dti": record.get("dti"),
                    "original_type": row_out["original_type"],
                    "status": status,
                    "reason": status_reason,
                    "unsupported_class": structure.get("unsupported_class"),
                    "format_code": structure.get("format_code"),
                    "resource_type": structure.get("resource_type"),
                    "width": structure.get("width"),
                    "height": structure.get("height"),
                    "mip_count": structure.get("mip_count"),
                    "source_identity": row_out["source_identity"],
                }
                if status == "failed":
                    write_jsonl(failed_handle, listing)
                    failed_written += 1
                else:
                    write_jsonl(unsupported_handle, listing)
                    unsupported_written += 1
            if status == "unknown-source":
                write_jsonl(unknown_handle, {
                    "raw": raw, "source": source, "entry": record.get("entry"),
                    "status": status, "reason": status_reason,
                })
                unknown_written += 1

            # Deterministic SHA re-verification sample: first two small rows of
            # every extension, plus every 50,000th row.
            if len(extension) and len(sha_samples[extension]) < 2 and \
                    int(record.get("bytes") or 0) <= 2_000_000:
                sha_samples[extension].append({
                    "raw": raw, "source": source, "extension": extension,
                    "manifest_sha256": record.get("sha256"), "bytes": record.get("bytes"),
                    "source_state": source_state["state"],
                })
            if rows % 50_000 == 0:
                global_samples.append({
                    "raw": raw, "source": source, "extension": extension,
                    "manifest_sha256": record.get("sha256"), "bytes": record.get("bytes"),
                    "source_state": source_state["state"],
                })

            # Collect representative candidates.
            if structure["status"] == "verified" and extension == "tex" and \
                    structure["format_code"] in tex_candidates:
                candidate = {
                    "kind": "tex", "raw": raw, "source": source,
                    "archive": record.get("archive"), "entry": record.get("entry"),
                    "entry_path": record.get("entry_path"), "dti": record.get("dti"),
                    "flags": record.get("flags"), "bytes": int(record.get("bytes") or 0),
                    "sha256": record.get("sha256"),
                    "conversion_status": conversion.get("status"),
                    "format_code": structure["format_code"],
                    "mip_count": structure["mip_count"],
                    "source_verified": bool(source_state["verified"]),
                }
                required = 2 if structure["format_code"] == 0x07 else (
                    4 if structure["format_code"] in (0x13, 0x17) else 1)
                insert_candidate(tex_candidates[structure["format_code"]], candidate, 2, required)
            if structure["status"] == "unsupported" and extension == "tex":
                cls = structure.get("unsupported_class") or ""
                if cls == "cube-cm-variant-unresolved" and cm_samples < 3 and source_state["verified"]:
                    cm_samples += 1
                    rejected_samples.append({
                        "kind": "tex", "raw": raw, "source": source,
                        "entry": record.get("entry"), "dti": record.get("dti"),
                        "unsupported_class": cls, "reason": structure["reason"],
                        "format_code": structure.get("format_code"),
                        "resource_type": structure.get("resource_type"),
                        "width": structure.get("width"), "height": structure.get("height"),
                        "mip_count": structure.get("mip_count"),
                        "reserved_0c": structure.get("raw_evidence", {}).get("reserved_0c"),
                        "payload_size_field": structure.get("raw_evidence", {}).get("payload_size_field"),
                        "source_state": source_state["state"],
                    })
                if cls.startswith("unsupported-format-0x") and fmt_samples[structure["format_code"]] < 2:
                    fmt_samples[structure["format_code"]] += 1
                    rejected_samples.append({
                        "kind": "tex", "raw": raw, "source": source,
                        "entry": record.get("entry"), "dti": record.get("dti"),
                        "unsupported_class": cls, "reason": structure["reason"],
                        "format_code": structure.get("format_code"),
                        "resource_type": structure.get("resource_type"),
                        "width": structure.get("width"), "height": structure.get("height"),
                        "mip_count": structure.get("mip_count"),
                        "source_state": source_state["state"],
                    })
                if cls.startswith("unsupported-resource-type") and volume_samples < 1:
                    volume_samples += 1
                    rejected_samples.append({
                        "kind": "tex", "raw": raw, "source": source,
                        "entry": record.get("entry"), "dti": record.get("dti"),
                        "unsupported_class": cls, "reason": structure["reason"],
                        "format_code": structure.get("format_code"),
                        "resource_type": structure.get("resource_type"),
                        "width": structure.get("width"), "height": structure.get("height"),
                        "mip_count": structure.get("mip_count"),
                        "source_state": source_state["state"],
                    })
            if structure["status"] == "verified" and extension == "mod" and \
                    structure.get("mesh_count", 0) > 0:
                streams = structure.get("mesh_streams", [])
                has_stream_evidence = any(s["vertex_stride"] > 0 and s["fvf_info"] != 0
                                          for s in streams)
                if has_stream_evidence:
                    candidate = {
                        "kind": "mod", "raw": raw, "source": source,
                        "archive": record.get("archive"), "entry": record.get("entry"),
                        "entry_path": record.get("entry_path"), "dti": record.get("dti"),
                        "flags": record.get("flags"), "bytes": int(record.get("bytes") or 0),
                        "sha256": record.get("sha256"),
                        "conversion_status": conversion.get("status"),
                        "mesh_count": structure["mesh_count"],
                        "material_count": structure.get("material_count", 0),
                        "source_verified": bool(source_state["verified"]),
                    }
                    mod_candidates.append(candidate)
                    mod_candidates.sort(key=lambda c: (
                        0 if c["source_verified"] else 1,
                        0 if c["mesh_count"] >= 2 else 1,
                        c["bytes"],
                    ))
                    del mod_candidates[20:]

    # ------------------------------------------------------------------
    # Re-verify the SHA sample.
    # ------------------------------------------------------------------
    sample_total = sample_verified = sample_failed = 0
    with open(out("sha-samples.jsonl"), "w", encoding="utf-8") as handle:
        sample_rows = []
        for extension in sorted(sha_samples):
            sample_rows.extend(sha_samples[extension])
        sample_rows.extend(global_samples)
        seen = set()
        for sample in sample_rows:
            if sample["raw"] in seen:
                continue
            seen.add(sample["raw"])
            path = os.path.join(args.extracted_root, sample["raw"].replace("/", os.sep))
            actual = sha256_file(path) if os.path.isfile(path) else None
            verified = actual is not None and actual == sample["manifest_sha256"]
            sample_total += 1
            if verified:
                sample_verified += 1
            else:
                sample_failed += 1
            write_jsonl(handle, {
                "raw": sample["raw"], "source": sample["source"],
                "extension": sample["extension"],
                "manifest_sha256": sample["manifest_sha256"],
                "actual_sha256": actual,
                "verified": verified,
                "source_state": sample["source_state"],
            })

    # ------------------------------------------------------------------
    # Representative selection, with on-demand source identity for the few
    # selected sources that fall outside the timeboxed cache.
    # ------------------------------------------------------------------
    def resolve_source(source: str) -> dict:
        get_source_state(source)
        state = states[source]
        if state["checked"]:
            return state
        row = hash_source_identity(source, roots)
        cache[source] = row
        state = state_from_cache_row(row)
        states[source] = state
        return state

    selected_tex: list[dict] = []
    selected_mod: list[dict] = []
    selection_failures: list[dict] = []
    for code in TEX_SELECTION_FORMATS:
        for _rank, candidate in tex_candidates.get(code, []):
            item, reason = materialize_tex(candidate, args.extracted_root)
            if item is None:
                selection_failures.append({"raw": candidate["raw"], "reason": reason})
                continue
            state = resolve_source(candidate["source"])
            item["source_status"] = ("verified" if state["verified"] else
                                     "unknown-source" if state["checked"] else "unverified")
            item["source_status_reason"] = state["reason"]
            item["source_identity_state"] = state["state"]
            selected_tex.append(item)
            break
    for candidate in mod_candidates:
        if len(selected_mod) >= 3:
            break
        item, reason = materialize_mod(candidate, args.extracted_root)
        if item is None:
            selection_failures.append({"raw": candidate["raw"], "reason": reason})
            continue
        state = resolve_source(candidate["source"])
        item["source_status"] = ("verified" if state["verified"] else
                                 "unknown-source" if state["checked"] else "unverified")
        item["source_status_reason"] = state["reason"]
        item["source_identity_state"] = state["state"]
        if any(existing["sha256"] == item["sha256"] for existing in selected_mod):
            continue
        selected_mod.append(item)

    rejected_samples.append({
        "kind": "aggregate",
        "unsupported_class": "bc7-not-present",
        "reason": "formats 0x30/0x31 have no corpus instances and no host decoder; not claimed",
    })
    selection_obj = {
        "schema": SELECTION_SCHEMA,
        "tool": "build_asset_inventory.py",
        "tool_version": TOOL_VERSION,
        "selection": selected_tex,
        "models": selected_mod,
        "rejected": rejected_samples,
        "selection_failures": selection_failures,
        "counts": {
            "textures": len(selected_tex),
            "models": len(selected_mod),
            "rejected_examples": len(rejected_samples),
            "selection_failures": len(selection_failures),
        },
        "notes": [
            "Only TEX entries with a validated block-linear mip chain are selected.",
            "No cube-map TEX is selected: 124 _CM rows use an unresolved container variant; "
            "byte 0x0c=6 is recorded only as raw evidence.",
            "MOD entries are metadata-only; skinning/animation (C3) is not converted.",
        ],
    }
    with open(out("representative.json"), "w", encoding="utf-8") as handle:
        json.dump(selection_obj, handle, indent=2)
        handle.write("\n")

    # ------------------------------------------------------------------
    # Full source-identity table.
    # ------------------------------------------------------------------
    source_counts = collections.Counter()
    base_differs = 0
    with open(out("source-identity.jsonl"), "w", encoding="utf-8") as handle, \
            open(out("unverified-sources.jsonl"), "w", encoding="utf-8") as unverified_handle:
        for source in sorted(sources):
            state = states.get(source) or state_from_cache_row(cache.get(source))
            row = cache.get(source)
            entry = {
                "schema": SOURCE_SCHEMA,
                "source": source,
                "manifest_entries": sources[source]["entries"],
                "manifest_bytes": sources[source]["bytes"],
                "status": ("verified" if state["verified"] else
                           "unknown-source" if state["checked"] else "unverified"),
                "checked": state["checked"],
                "state": state["state"],
                "matched_version": state["matched_version"],
                "reason": state["reason"],
                "base_present": state.get("base_present"),
                "update_present": state.get("update_present"),
                "locked_present": state.get("locked_present"),
                "base_sha256": (row or {}).get("base"),
                "update_sha256": (row or {}).get("update"),
                "locked_sha256": (row or {}).get("locked"),
                "locked_eq_base": (row or {}).get("locked_eq_base"),
                "locked_eq_update": (row or {}).get("locked_eq_update"),
                "base_eq_update": (row or {}).get("base_eq_update"),
            }
            write_jsonl(handle, entry)
            if entry["status"] == "unverified":
                write_jsonl(unverified_handle, {
                    "source": source,
                    "manifest_entries": sources[source]["entries"],
                    "reason": state["reason"],
                })
            source_counts[entry["status"]] += 1
            if state.get("base_differs_from_update"):
                base_differs += 1

    # ------------------------------------------------------------------
    # Summary.
    # ------------------------------------------------------------------
    input_manifest_sha = sha256_file(args.manifest)
    input_conversion_sha = sha256_file(args.conversion_manifest)
    root_file_counts = {}
    for key, root in roots.items():
        root_file_counts[key] = count_files(root) if os.path.isdir(root) else None
    summary = {
        "schema": SUMMARY_SCHEMA,
        "tool": "build_asset_inventory.py",
        "tool_version": TOOL_VERSION,
        "inputs": {
            "manifest": {"path": args.manifest, "bytes": os.path.getsize(args.manifest),
                         "sha256": input_manifest_sha},
            "conversion_manifest": {"path": args.conversion_manifest,
                                    "bytes": os.path.getsize(args.conversion_manifest),
                                    "sha256": input_conversion_sha},
            "extracted_root": args.extracted_root,
            "base_root": {"path": args.base_root, "file_count": root_file_counts["base"]},
            "update_root": {"path": args.update_root, "file_count": root_file_counts["update"]},
            "locked_root": {"path": args.locked_root, "file_count": root_file_counts["locked"]},
            "arc_identity_cache": {
                **cache_info,
                "coverage_note": "Timeboxed partial precomputed hash cache; sources outside it "
                                 "are marked not-checked, never assumed identical.",
            },
        },
        "row_count": rows,
        "manifest_conversion": {
            "pair_mismatches": alignment_mismatches,
            "aligned": alignment_mismatches == 0,
        },
        "status_counts": dict(sorted(status_counts.items())),
        "structure_status_counts": dict(sorted(structure_counts.items())),
        "extension_counts": dict(sorted(ext_counts.items())),
        "conversion_status_counts": dict(sorted(conversion_counts.items())),
        "source_identity": {
            "unique_sources": len(sources),
            "status_counts": dict(sorted(source_counts.items())),
            "row_state_counts": dict(sorted(source_state_counts.items())),
            "sources_where_update_differs_from_base": base_differs,
            "cache_rows_loaded": cache_info["rows_loaded"],
            "cache_coverage_fraction": (round(min(1.0, cache_info["rows_loaded"] / len(sources)), 6)
                                        if sources else None),
        },
        "file_sha_samples": {
            "rows": sample_total,
            "verified": sample_verified,
            "failed": sample_failed,
            "note": "Deterministic per-extension sample plus every 50,000th row; "
                    "each sampled file's SHA-256 is re-read and compared with the manifest. "
                    "This is not a full-corpus byte audit and not a payload hash for every row.",
        },
        "tex": {
            "total": tex_total,
            "layout_verified": tex_layout_ok,
            "unsupported": sum(tex_unsupported_classes.values()),
            "format_counts": {f"0x{code:02x}" if code is not None else "unknown":
                              count for code, count in sorted(tex_format_counts.items(),
                                                              key=lambda kv: (kv[0] is None, kv[0]))},
            "resource_type_counts": {str(code): count for code, count in
                                     sorted(tex_rtype_counts.items(),
                                            key=lambda kv: (kv[0] is None, kv[0]))},
            "unsupported_classes": dict(sorted(tex_unsupported_classes.items(),
                                               key=lambda kv: str(kv[0]))),
            "mip_count_histogram": {str(k): v for k, v in sorted(tex_mip_hist.items(),
                                                                 key=lambda kv: (kv[0] is None, kv[0]))},
        },
        "mod": {
            "total": mod_total,
            "header_verified": mod_verified,
            "unsupported": mod_unsupported,
            "failed": mod_failed,
            "mesh_count_total": mod_mesh_total,
            "stream_descriptors_recorded": mod_stream_descriptors,
            "fvf_info_top": [[k, v] for k, v in mod_fvf_values.most_common(20)],
            "vertex_stride_top": [[k, v] for k, v in mod_stride_values.most_common(20)],
            "attribute_semantics": "not inferred; raw FVF/stride evidence only",
        },
        "representative": {
            "textures": len(selected_tex),
            "models": len(selected_mod),
            "texture_formats": [item["format_name"] for item in selected_tex],
            "texture_sha_verified": all(item["payload_sha256_verified"] for item in selected_tex),
            "model_sha_verified": all(item["file_sha256_verified"] for item in selected_mod),
        },
        "unsupported_and_gaps": {
            "unsupported_rows_written": unsupported_written,
            "failed_rows_written": failed_written,
            "unknown_source_rows_written": unknown_written,
            "cube_cm_variant": "124 _CM rows have byte 0x0c=6 and a non-default offset/size "
                               "layout; no face/mip table was proven, so cube support is not claimed.",
            "bc7": "formats 0x30/0x31 are recognized by name but have no corpus instance and no "
                   "host decoder; not converted.",
            "c3": "MOD skinning/animation conversion is out of C0-C2 scope.",
            "c4_to_c6": "GXM upload/runtime integration and provenance are out of scope for this tool.",
        },
        "coverage_statement": (
            "Counts are manifest payload-row counts, not proof of byte coverage. "
            f"ARC identity is checked for {cache_info['rows_loaded']} of {len(sources)} unique "
            "sources from a timeboxed cache; the remainder is explicitly unverified. "
            f"Whole-file SHA-256 was re-read for {sample_verified}/{sample_total} sampled "
            "manifest rows, not their embedded payloads. Only the representative TEX/MOD "
            "entries had both file and TEX payload SHA-256 re-hashed and were structurally "
            "verified end to end."
        ),
    }
    with open(out("summary.json"), "w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2, sort_keys=True)
        handle.write("\n")
    with open(out("commands.txt"), "w", encoding="utf-8") as handle:
        handle.write(f"{TOOL_VERSION}\n")
        handle.write("argv: " + " ".join(sys.argv) + "\n")
        handle.write("python: -I (isolated mode requested by the caller)\n")
    print(json.dumps({
        "rows": rows,
        "status_counts": summary["status_counts"],
        "tex_layout_verified": tex_layout_ok,
        "mod_verified": mod_verified,
        "representative_textures": len(selected_tex),
        "representative_models": len(selected_mod),
    }, sort_keys=True))
    return 0 if alignment_mismatches == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

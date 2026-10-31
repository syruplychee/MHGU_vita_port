"""Convert a small, verified set of MHGU TEX resources into a host test pack.

The pack is intentionally *not* a PNG export and does not force any format to
RGBA8.  For every selected texture it stores:

* the exact original pixel payload (all mips, tier/format preserved);
* a lossless block-linear -> linear-block-raster conversion of every mip
  (compressed formats stay compressed; there is no decode/re-encode);
* every mip's original offset/stored size/padded size/logical size/alignment,
  plus identity hashes and independent decode verification hashes.

Layout and decode are verified by tools/assets/tex_verify.c in the test suite;
this converter is not its own oracle.

Pack layout ("VTXP", little endian, no native C structs)::

    0x00  4  magic "VTXP"
    0x04  u32 version (1)
    0x08  u32 header_size (64)
    0x0c  u32 record_count
    0x10  u32 flags (0)
    0x14  u32 converter_version_length
    0x18  u64 index_offset
    0x20  u64 index_size
    0x28  u64 data_offset
    0x30  u64 data_size
    0x38  u32 index_crc32 (zlib CRC-32 over index bytes)
    0x3c  u32 data_crc32
    [header bytes 0x40..0x3f are zero]
    0x40  converter version (UTF-8)
    ...   index JSON (UTF-8, sorted keys, no floats)
    ...   data blob (16-byte aligned)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tex_format as tf  # noqa: E402

PACK_MAGIC = b"VTXP"
PACK_VERSION = 1
PACK_HEADER_SIZE = 0x40
CONVERTER_VERSION = "mhgu-tex-host-1"
DATA_ALIGNMENT = 16


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_json(obj) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


class BlobWriter:
    def __init__(self) -> None:
        self.parts: list[bytes] = []
        self.offset = 0

    def align(self, alignment: int) -> None:
        if alignment < 1:
            raise ValueError("alignment")
        pad = (-self.offset) % alignment
        if pad:
            self.parts.append(b"\0" * pad)
            self.offset += pad

    def append(self, data: bytes, alignment: int = DATA_ALIGNMENT) -> tuple[int, int]:
        self.align(alignment)
        offset = self.offset
        self.parts.append(data)
        self.offset += len(data)
        return offset, len(data)

    def build(self) -> bytes:
        return b"".join(self.parts)


def alpha_stats(rgba: bytes) -> dict:
    if len(rgba) % 4:
        raise ValueError("rgba size")
    alpha = rgba[3::4]
    return {
        "alpha_min": min(alpha),
        "alpha_max": max(alpha),
        "alpha_nonzero_pixels": sum(1 for value in alpha if value != 0),
        "pixels": len(alpha),
    }


def convert_one(record: dict, extracted_root: str) -> tuple[dict, bytes, bytes]:
    raw_relative = record["raw"]
    path = os.path.join(extracted_root, raw_relative)
    if not os.path.isfile(path):
        raise tf.TexFormatError("missing-input", raw_relative)
    actual_sha = sha256_file(path)
    expected_file_sha = record.get("sha256") or record.get("file_sha256")
    if expected_file_sha and actual_sha != expected_file_sha:
        raise tf.TexFormatError(
            "file-sha-mismatch", f"{raw_relative}: {actual_sha} != {expected_file_sha}"
        )
    header, layout = tf.inspect_file(path, require_decoder=True)
    if not layout.layout_ok:
        raise tf.TexFormatError("layout-unverified", layout.layout_reason)
    payload = tf.read_payload(path, header)
    source_sha = sha256_bytes(payload)
    expected_payload_sha = record.get("payload_sha256")
    if expected_payload_sha and source_sha != expected_payload_sha:
        raise tf.TexFormatError(
            "payload-sha-mismatch", f"payload {source_sha} != {expected_payload_sha}"
        )
    fmt = header.format
    assert fmt is not None
    blob = BlobWriter()
    source_offset, source_size = blob.append(payload)
    levels_json = []
    decoded_hashes = {}
    for level in layout.levels:
        linear = tf.untile_level(payload, level, fmt)
        rgba = tf.decode_level_rgba(linear, level, fmt)
        target_offset, target_size = blob.append(linear)
        stats = alpha_stats(rgba)
        levels_json.append(
            {
                "mip": level.mip,
                "width": level.width,
                "height": level.height,
                "block_columns": level.block_columns,
                "block_rows": level.block_rows,
                "block_height_gobs": level.block_height_gobs,
                "gob_columns": level.gob_columns,
                "source_offset": level.offset,
                "source_stored_size": level.stored_size,
                "logical_size": level.logical_size,
                "padded_size": level.padded_size,
                "alignment": level.alignment,
                "source_sha256": sha256_bytes(payload[level.offset:level.offset + level.stored_size]),
                "target_offset": target_offset,
                "target_size": target_size,
                "target_sha256": sha256_bytes(linear),
                "decoded_rgba_sha256": sha256_bytes(rgba),
                **stats,
            }
        )
        decoded_hashes[level.mip] = {
            "linear_sha256": sha256_bytes(linear),
            "rgba_sha256": sha256_bytes(rgba),
        }
    # Round-trip: retile each target mip and compare with the original level.
    for level, info in zip(layout.levels, levels_json):
        linear = tf.untile_level(payload, level, fmt)
        retiled = tf.retile_level(linear, level, fmt)
        original = payload[level.offset:level.offset + level.stored_size]
        if retiled != original:
            raise tf.TexFormatError("roundtrip-mismatch", f"mip {level.mip}")
    out = {
        "asset_key": record.get("asset_key"),
        "identity": {
            "source_arc": record.get("source"),
            "archive_relative": record.get("archive"),
            "entry_index": record.get("entry"),
            "entry_path": record.get("entry_path"),
            "dti": record.get("dti"),
            "raw_relative": raw_relative,
            "payload_sha256": source_sha,
            "file_sha256": actual_sha,
            "manifest_file_sha256": expected_file_sha,
            "manifest_payload_sha256": expected_payload_sha,
            "source_status": record.get("source_status", "unknown"),
            "source_status_reason": record.get("source_status_reason", ""),
        },
        "container": {
            "magic": "TEX\\0",
            "layout_word": header.layout_word,
            "reserved_0c": header.reserved_0c,
            "format_code": header.format_code,
            "format_name": fmt.name,
            "format_family": fmt.family,
            "resource_type": header.resource_type,
            "flags": header.flags,
            "width": header.width,
            "height": header.height,
            "mip_count": header.mip_count,
            "payload_offset": header.payload_offset,
            "payload_size": header.payload_size,
            "file_size": header.file_size,
            "trailing_bytes": header.trailing_bytes,
            "layout_verified": layout.layout_ok,
            "layout_reason": layout.layout_reason,
            "block_height_gobs0": layout.block_height_gobs0,
            "layout_model": "NVN block-linear; block height validated against every stored mip offset",
        },
        "conversion": {
            "converter": "convert_tex.py",
            "converter_version": CONVERTER_VERSION,
            "transformation": "lossless block-linear -> linear block raster per mip; no decode/re-encode",
            "target_layout": "linear blocks, format-preserving, 16-byte aligned in pack",
            "compressed_preserved": fmt.compressed,
            "notes": [
                "All original mips are preserved; no mip is synthesized.",
                "No face/layer data exists in the selected resource (resource_type 1).",
                "TIC component swizzle is sampling state and is not stored in TEX; it is not guessed.",
                "This is a host test pack, not a GXM upload-ready swizzle.",
            ],
        },
        "source_blob": {
            "offset": source_offset,
            "size": source_size,
            "sha256": source_sha,
            "alignment": DATA_ALIGNMENT,
        },
        "levels": levels_json,
        "decoded_rgba_sha256_by_mip": decoded_hashes,
        "subresource_count": len(levels_json),
    }
    return out, payload, blob.build()


def write_pack(records: list[dict], output_dir: str) -> dict:
    if os.path.exists(output_dir) and os.listdir(output_dir):
        raise SystemExit(f"output directory is not empty: {output_dir}")
    os.makedirs(output_dir, exist_ok=True)
    blob = BlobWriter()
    index_records = []
    for record, payload, payload_blob in records:
        data_offset, data_size = blob.append(payload_blob)
        item = dict(record)
        item["pack_data"] = {
            "offset": data_offset,
            "size": data_size,
            "sha256": sha256_bytes(payload_blob),
            "alignment": DATA_ALIGNMENT,
            "source_offset_within_data": record["source_blob"]["offset"],
        }
        index_records.append(item)
    data = blob.build()
    version = CONVERTER_VERSION.encode("utf-8")
    index_obj = {
        "schema": "mhgu-tex-host-pack-index",
        "schema_version": 1,
        "converter": "convert_tex.py",
        "converter_version": CONVERTER_VERSION,
        "pack_magic": PACK_MAGIC.decode("ascii"),
        "pack_version": PACK_VERSION,
        "data_alignment": DATA_ALIGNMENT,
        "record_count": len(index_records),
        "records": index_records,
        "limitations": [
            "Host verification pack only; not a runtime GXM asset pack.",
            "Cube maps and multi-layer textures are not included: no such TEX exists in the locked corpus.",
            "3D resource type 0x20 and formats 0x1e/0x20 are rejected, not converted.",
        ],
    }
    index_data = canonical_json(index_obj)
    header = bytearray(PACK_HEADER_SIZE)
    struct.pack_into("<4sIIIII", header, 0, PACK_MAGIC, PACK_VERSION, PACK_HEADER_SIZE,
                     len(index_records), 0, len(version))
    index_offset = PACK_HEADER_SIZE + len(version)
    data_offset = index_offset + len(index_data)
    data_offset = (data_offset + DATA_ALIGNMENT - 1) // DATA_ALIGNMENT * DATA_ALIGNMENT
    struct.pack_into("<QQQQ", header, 0x18, index_offset, len(index_data), data_offset, len(data))
    struct.pack_into("<II", header, 0x38, zlib.crc32(index_data) & 0xFFFFFFFF,
                     zlib.crc32(data) & 0xFFFFFFFF)
    with open(os.path.join(output_dir, "textures.vtxp"), "wb") as handle:
        handle.write(bytes(header))
        handle.write(version)
        handle.write(index_data)
        handle.write(b"\0" * (data_offset - index_offset - len(index_data)))
        handle.write(data)
    with open(os.path.join(output_dir, "textures.index.json"), "w", encoding="utf-8") as handle:
        json.dump(index_obj, handle, indent=2, sort_keys=True)
        handle.write("\n")
    pack_path = os.path.join(output_dir, "textures.vtxp")
    summary = {
        "schema": "mhgu-tex-host-pack-summary",
        "schema_version": 1,
        "converter_version": CONVERTER_VERSION,
        "pack_path": os.path.basename(pack_path),
        "pack_sha256": sha256_file(pack_path),
        "pack_bytes": os.path.getsize(pack_path),
        "data_bytes": len(data),
        "index_sha256": sha256_bytes(index_data),
        "record_count": len(index_records),
        "records": [
            {
                "asset_key": item.get("asset_key"),
                "format": item["container"]["format_name"],
                "format_code": item["container"]["format_code"],
                "width": item["container"]["width"],
                "height": item["container"]["height"],
                "mip_count": item["container"]["mip_count"],
                "payload_sha256": item["identity"]["payload_sha256"],
                "source_status": item["identity"]["source_status"],
                "target_sha256_by_mip": [level["target_sha256"] for level in item["levels"]],
            }
            for item in index_records
        ],
    }
    with open(os.path.join(output_dir, "summary.json"), "w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2, sort_keys=True)
        handle.write("\n")
    return summary


def read_pack(pack_path: str) -> tuple[bytes, dict, bytes]:
    with open(pack_path, "rb") as handle:
        raw = handle.read()
    if len(raw) < PACK_HEADER_SIZE:
        raise tf.TexFormatError("pack-truncated", str(len(raw)))
    magic, version, header_size, record_count, flags, version_len = struct.unpack_from(
        "<4sIIIII", raw, 0)
    if magic != PACK_MAGIC:
        raise tf.TexFormatError("pack-magic", magic.hex())
    if version != PACK_VERSION:
        raise tf.TexFormatError("pack-version", str(version))
    if header_size != PACK_HEADER_SIZE:
        raise tf.TexFormatError("pack-header-size", str(header_size))
    index_offset, index_size, data_offset, data_size = struct.unpack_from("<QQQQ", raw, 0x18)
    index_crc, data_crc = struct.unpack_from("<II", raw, 0x38)
    if index_offset + index_size > len(raw) or data_offset + data_size > len(raw):
        raise tf.TexFormatError("pack-range", "index/data outside file")
    if index_offset < PACK_HEADER_SIZE + version_len:
        raise tf.TexFormatError("pack-range", "index overlaps header")
    index_data = raw[index_offset:index_offset + index_size]
    data = raw[data_offset:data_offset + data_size]
    if zlib.crc32(index_data) & 0xFFFFFFFF != index_crc:
        raise tf.TexFormatError("pack-crc", "index")
    if zlib.crc32(data) & 0xFFFFFFFF != data_crc:
        raise tf.TexFormatError("pack-crc", "data")
    index = json.loads(index_data.decode("utf-8"))
    if index.get("record_count") != record_count:
        raise tf.TexFormatError("pack-count", f"{index.get('record_count')} != {record_count}")
    version_text = raw[PACK_HEADER_SIZE:PACK_HEADER_SIZE + version_len].decode("utf-8")
    return data, index, raw


def verify_pack(output_dir: str) -> dict:
    pack_path = os.path.join(output_dir, "textures.vtxp")
    data, index, raw = read_pack(pack_path)
    records = index["records"]
    keys = set()
    problems = []
    for item in records:
        key = item.get("asset_key")
        if key in keys:
            problems.append(f"duplicate asset_key {key}")
        keys.add(key)
        pdata = item["pack_data"]
        if pdata["offset"] + pdata["size"] > len(data):
            problems.append(f"record {key}: data out of range")
            continue
        chunk = data[pdata["offset"]:pdata["offset"] + pdata["size"]]
        if sha256_bytes(chunk) != pdata["sha256"]:
            problems.append(f"record {key}: data sha mismatch")
        src = item["source_blob"]
        payload = chunk[src["offset"]:src["offset"] + src["size"]]
        if sha256_bytes(payload) != src["sha256"]:
            problems.append(f"record {key}: source payload sha mismatch")
        try:
            fmt = tf.require_format(item["container"]["format_code"], require_decoder=True)
        except tf.TexFormatError as exc:
            problems.append(f"record {key}: {exc.reason}")
            continue
        if item.get("subresource_count") != len(item["levels"]):
            problems.append(f"record {key}: subresource count mismatch")
        cursor = 0
        for info in item["levels"]:
            level = tf.LevelLayout(
                mip=info["mip"], width=info["width"], height=info["height"],
                block_columns=info["block_columns"], block_rows=info["block_rows"],
                gob_columns=info["gob_columns"],
                block_height_gobs=info["block_height_gobs"],
                offset=info["source_offset"], stored_size=info["source_stored_size"],
                logical_size=info["logical_size"], padded_size=info["padded_size"],
                alignment=info["alignment"])
            if level.offset != cursor:
                problems.append(f"record {key} mip {level.mip}: non-contiguous source offset")
            cursor += level.stored_size
            linear = tf.untile_level(payload, level, fmt)
            target = chunk[info["target_offset"]:info["target_offset"] + info["target_size"]]
            if sha256_bytes(linear) != info["target_sha256"]:
                problems.append(f"record {key} mip {level.mip}: target sha mismatch")
            if linear != target:
                problems.append(f"record {key} mip {level.mip}: target bytes mismatch")
            retiled = tf.retile_level(linear, level, fmt)
            if retiled != payload[level.offset:level.offset + level.stored_size]:
                problems.append(f"record {key} mip {level.mip}: round trip mismatch")
        if cursor != len(payload):
            problems.append(f"record {key}: level chain does not cover source payload")
    return {
        "pack_sha256": sha256_file(pack_path),
        "record_count": len(records),
        "problems": problems,
        "passed": not problems,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selection", required=False,
                        help="representative.json produced by build_asset_inventory.py")
    parser.add_argument("--extracted-root", required=False)
    parser.add_argument("--output", required=True)
    parser.add_argument("--verify", action="store_true",
                        help="verify an existing --output pack instead of writing")
    args = parser.parse_args(argv)
    if args.verify:
        report = verify_pack(args.output)
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0 if report["passed"] else 1
    if not args.selection or not args.extracted_root:
        parser.error("--selection and --extracted-root are required unless --verify")
    with open(args.selection, "r", encoding="utf-8") as handle:
        selection = json.load(handle)
    records = []
    converted = []
    for item in selection["selection"]:
        record, payload, payload_blob = convert_one(item, args.extracted_root)
        records.append((record, payload, payload_blob))
        converted.append(record)
        print(f"converted {record['identity']['raw_relative']} "
              f"{record['container']['format_name']} "
              f"{record['container']['width']}x{record['container']['height']} "
              f"mips={record['container']['mip_count']}", flush=True)
    summary = write_pack(records, args.output)
    verify = verify_pack(args.output)
    summary["verification"] = verify
    with open(os.path.join(args.output, "summary.json"), "w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(json.dumps({"pack_sha256": summary["pack_sha256"], "passed": verify["passed"]},
                     sort_keys=True))
    return 0 if verify["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())

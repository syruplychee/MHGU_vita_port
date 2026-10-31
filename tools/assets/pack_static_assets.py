#!/usr/bin/env python3
"""Build the Stage C1 static-asset pack from verified C0/C2 artifacts.

The pack is an explicit little-endian container.  No native C struct is ever
written or read: every field is packed/unpacked with ``struct`` at documented
offsets, and every table is checksummed.

Container layout (all integers little endian)::

    header (0x80 bytes)
      0x00  4s  magic "MHSP"
      0x04  u32 version (1)
      0x08  u32 header_size (0x80)
      0x0c  u32 flags (0)
      0x10  u32 record_count
      0x14  u32 gap_count
      0x18  u32 record_size (0x60)
      0x1c  u32 gap_size (0x20)
      0x20  u64 record_table_offset
      0x28  u64 record_table_bytes
      0x30  u64 gap_table_offset
      0x38  u64 gap_table_bytes
      0x40  u64 index_json_offset
      0x48  u64 index_json_bytes
      0x50  u64 payload_offset
      0x58  u64 payload_bytes
      0x60  u32 payload_alignment (64)
      0x64  u32 index_schema_version (1)
      0x68  u32 record_table_crc32
      0x6c  u32 gap_table_crc32
      0x70  u32 index_json_crc32
      0x74  u32 payload_crc32
      0x78  u32 header_crc32 (over 0x80 header bytes with this field zero)
      0x7c  u32 reserved (0)
    record table   record_count * 0x60
    gap table      gap_count * 0x20
    index JSON     UTF-8, canonical separators
    padding        zeroes to 64-byte payload alignment
    payload blob

Record fields (0x60 bytes)::

    0x00 u32 record_size         0x08 u32 asset_key_crc32
    0x04 u32 kind_code           0x0c u32 record_index
    0x10 u64 payload_offset      0x18 u64 payload_size
    0x20 u64 source_size         0x28 u32 payload_crc32
    0x2c u32 source_crc32        0x30 u32 format_code
    0x34 u32 width               0x38 u32 height
    0x3c u32 mip_count           0x40 u32 flags
    0x44 u32 asset_key_length    0x48 u64 reserved
    0x50 u64 reserved            0x58 u64 reserved

Gap fields (0x20 bytes)::

    0x00 u32 gap_size            0x04 u32 kind_code
    0x08 u32 count               0x0c u32 summary_crc32
    0x10 u64 source_bytes        0x18 u32 reserved
    0x1c u32 reserved

The payload blob embeds each converted TEX host blob from ``textures.vtxp``
(original payload plus every lossless linear-mip target).  MOD entries are
metadata-only: their source bytes are not copied into the pack.  Gap records
carry unsupported/cube/3D/C3/runtime/coverage limitations supplied by the C0
inventory; they are data, not claims of support.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
DEFAULT_EXTRACTED = os.path.normpath(os.path.join(REPO_ROOT, "..", "extracted-resources"))
sys.path.insert(0, SCRIPT_DIR)
import tex_format as tf  # noqa: E402
import convert_tex as ct  # noqa: E402

TOOL_VERSION = "mhgu-static-pack-1"
PACK_MAGIC = b"MHSP"
PACK_VERSION = 1
HEADER_SIZE = 0x80
RECORD_SIZE = 0x60
GAP_SIZE = 0x20
INDEX_SCHEMA = "mhgu-static-asset-pack-index"
INDEX_SCHEMA_VERSION = 1
PAYLOAD_ALIGNMENT = 64

KIND_TO_CODE = {
    "texture-test-blob": 1,
    "model-metadata": 2,
    "opaque-preserved": 3,
}
CODE_TO_KIND = {value: key for key, value in KIND_TO_CODE.items()}

GAP_TO_CODE = {
    "unsupported-format": 1,
    "unsupported-resource-type": 2,
    "unknown-source": 3,
    "unverified-source": 4,
    "not-converted-c3": 5,
    "runtime-out-of-scope": 6,
    "verification-limit": 7,
    "host-test-only": 8,
}
CODE_TO_GAP = {value: key for key, value in GAP_TO_CODE.items()}

RECORD_FLAG_IMMUTABLE = 1 << 0
RECORD_FLAG_HOST_TEST_ONLY = 1 << 1
RECORD_FLAG_ALL_MIPS_PRESERVED = 1 << 2
RECORD_FLAG_METADATA_ONLY = 1 << 3
KNOWN_RECORD_FLAGS = (RECORD_FLAG_IMMUTABLE | RECORD_FLAG_HOST_TEST_ONLY |
                      RECORD_FLAG_ALL_MIPS_PRESERVED | RECORD_FLAG_METADATA_ONLY)


class StaticPackError(ValueError):
    def __init__(self, reason: str, detail: str = "") -> None:
        self.reason = reason
        self.detail = detail
        super().__init__(f"{reason}: {detail}" if detail else reason)


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_json(obj) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode("utf-8")


def align(value: int, alignment: int) -> int:
    if alignment < 1 or alignment & (alignment - 1):
        raise StaticPackError("pack-alignment", str(alignment))
    return (value + alignment - 1) // alignment * alignment


class BlobWriter:
    def __init__(self, alignment: int) -> None:
        self.alignment = alignment
        self.parts: list[bytes] = []
        self.offset = 0

    def append(self, data: bytes) -> tuple[int, int]:
        pad = (-self.offset) % self.alignment
        if pad:
            self.parts.append(b"\0" * pad)
            self.offset += pad
        offset = self.offset
        self.parts.append(data)
        self.offset += len(data)
        return offset, len(data)

    def build(self) -> bytes:
        return b"".join(self.parts)


def build_gaps(selection: dict, inventory_summary: dict) -> list[dict]:
    gaps: list[dict] = []
    unsupported_classes = inventory_summary.get("tex", {}).get("unsupported_classes", {})
    rejected = selection.get("rejected", [])
    samples_by_class: dict[str, list[str]] = {}
    for item in rejected:
        cls = item.get("unsupported_class")
        if cls and item.get("raw"):
            samples_by_class.setdefault(cls, []).append(item["raw"])

    for cls, count in sorted(unsupported_classes.items(), key=lambda kv: str(kv[0])):
        if cls is None or cls == "None":
            kind = "unsupported-format"
            summary = "unsupported TEX structure with no classified reason"
        elif cls.startswith("unsupported-resource-type"):
            kind = "unsupported-resource-type"
            summary = ("TEX resource type is not single-layer 2D; face/layer semantics are "
                       "not proven, so the item is not converted")
        elif cls == "cube-cm-variant-unresolved":
            kind = "unsupported-format"
            summary = ("_CM TEX container variant unresolved: byte 0x0c=6 is raw evidence only; "
                       "no verified face/mip offset table exists, so cube support is not claimed")
        elif cls == "recognized-format-without-host-decoder":
            kind = "unsupported-format"
            summary = "format is recognized but this host converter has no independent decoder"
        else:
            kind = "unsupported-format"
            summary = f"unsupported TEX class {cls}"
        gaps.append({
            "kind": kind,
            "count": int(count),
            "class": cls,
            "summary": summary,
            "samples": samples_by_class.get(cls, [])[:4],
            "source_bytes": 0,
        })

    if not any(g.get("class") == "bc7-not-present" for g in gaps):
        gaps.append({
            "kind": "unsupported-format",
            "count": 0,
            "class": "bc7-not-present",
            "summary": "formats 0x30/0x31 have no corpus instance and no host decoder; not claimed",
            "samples": [],
            "source_bytes": 0,
        })

    status_counts = inventory_summary.get("status_counts", {})
    source_identity = inventory_summary.get("source_identity", {})
    unknown_rows = int(status_counts.get("unknown-source", 0))
    unverified_sources = int(source_identity.get("status_counts", {}).get("unverified", 0))
    gaps.append({
        "kind": "unknown-source",
        "count": unknown_rows,
        "class": "unknown-source-rows",
        "summary": "manifest payload rows whose source ARC is absent from, or differs from, "
                   "the locked RomFS tree; not treated as verified inputs",
        "samples": [],
        "source_bytes": 0,
    })
    gaps.append({
        "kind": "unverified-source",
        "count": unverified_sources,
        "class": "timeboxed-source-identity",
        "summary": "unique source ARCs outside the timeboxed hash cache; identity explicitly "
                   "unverified, not assumed identical",
        "samples": [],
        "source_bytes": 0,
    })
    gaps.append({
        "kind": "not-converted-c3",
        "count": int(inventory_summary.get("mod", {}).get("header_verified", 0)),
        "class": "mod-skinning-animation",
        "summary": "MOD header/mesh-stream metadata is recorded, but skinning, bone "
                   "deformation and animation conversion are C3 scope and are not performed",
        "samples": [],
        "source_bytes": 0,
    })
    gaps.append({
        "kind": "runtime-out-of-scope",
        "count": 0,
        "class": "c4-to-c6-runtime-provenance",
        "summary": "GXM upload, runtime integration and provenance are C4-C6 scope; this pack "
                   "is a host-verifiable static artifact only",
        "samples": [],
        "source_bytes": 0,
    })
    gaps.append({
        "kind": "verification-limit",
        "count": int(source_identity.get("unique_sources", 0)),
        "class": "timeboxed-arc-hashing",
        "summary": "ARC identity hashing was timeboxed; see source-identity.jsonl for explicit "
                   "verified/unverified rows. Counts are not coverage proof",
        "samples": [],
        "source_bytes": 0,
    })
    gaps.append({
        "kind": "host-test-only",
        "count": 0,
        "class": "not-gxm-upload-ready",
        "summary": "TEX payload targets are lossless linear-block rasters for host verification, "
                   "not GXM tiled/swizzled uploads; no alpha/mip/face data was dropped",
        "samples": [],
        "source_bytes": 0,
    })
    for index, gap in enumerate(gaps):
        gap["gap_index"] = index
    return gaps


def assemble_pack(record_table: bytes, gap_table: bytes, index_data: bytes,
                  payload: bytes, record_count: int, gap_count: int) -> bytes:
    if len(record_table) != record_count * RECORD_SIZE:
        raise StaticPackError("pack-record-table-size", str(len(record_table)))
    if len(gap_table) != gap_count * GAP_SIZE:
        raise StaticPackError("pack-gap-table-size", str(len(gap_table)))
    record_offset = HEADER_SIZE
    gap_offset = record_offset + len(record_table)
    index_offset = gap_offset + len(gap_table)
    payload_offset = align(index_offset + len(index_data), PAYLOAD_ALIGNMENT)
    total = payload_offset + len(payload)
    header = bytearray(HEADER_SIZE)
    struct.pack_into("<4sIII", header, 0x00, PACK_MAGIC, PACK_VERSION, HEADER_SIZE, 0)
    struct.pack_into("<IIII", header, 0x10, record_count, gap_count, RECORD_SIZE, GAP_SIZE)
    struct.pack_into("<QQQQQQQQ", header, 0x20, record_offset, len(record_table),
                     gap_offset, len(gap_table), index_offset, len(index_data),
                     payload_offset, len(payload))
    struct.pack_into("<II", header, 0x60, PAYLOAD_ALIGNMENT, INDEX_SCHEMA_VERSION)
    struct.pack_into("<IIII", header, 0x68,
                     zlib.crc32(record_table) & 0xFFFFFFFF,
                     zlib.crc32(gap_table) & 0xFFFFFFFF,
                     zlib.crc32(index_data) & 0xFFFFFFFF,
                     zlib.crc32(payload) & 0xFFFFFFFF)
    # Header CRC is computed with its own field zeroed.
    header[0x78:0x7C] = b"\0\0\0\0"
    struct.pack_into("<I", header, 0x78, zlib.crc32(bytes(header)) & 0xFFFFFFFF)
    out = bytearray(total)
    out[0:HEADER_SIZE] = header
    out[record_offset:record_offset + len(record_table)] = record_table
    out[gap_offset:gap_offset + len(gap_table)] = gap_table
    out[index_offset:index_offset + len(index_data)] = index_data
    # Padding bytes remain zero.
    out[payload_offset:payload_offset + len(payload)] = payload
    return bytes(out)


def read_pack_header(raw: bytes) -> dict:
    if len(raw) < HEADER_SIZE:
        raise StaticPackError("pack-truncated", f"{len(raw)} < {HEADER_SIZE}")
    magic, version, header_size, flags = struct.unpack_from("<4sIII", raw, 0)
    if magic != PACK_MAGIC:
        raise StaticPackError("pack-magic", magic.hex())
    if version != PACK_VERSION:
        raise StaticPackError("pack-version", str(version))
    if header_size != HEADER_SIZE:
        raise StaticPackError("pack-header-size", str(header_size))
    record_count, gap_count, record_size, gap_size = struct.unpack_from("<IIII", raw, 0x10)
    if record_size != RECORD_SIZE:
        raise StaticPackError("pack-record-size", str(record_size))
    if gap_size != GAP_SIZE:
        raise StaticPackError("pack-gap-size", str(gap_size))
    record_offset, record_bytes, gap_offset, gap_bytes, index_offset, index_bytes, \
        payload_offset, payload_bytes = struct.unpack_from("<QQQQQQQQ", raw, 0x20)
    payload_alignment, index_version = struct.unpack_from("<II", raw, 0x60)
    record_crc, gap_crc, index_crc, payload_crc, header_crc, reserved = \
        struct.unpack_from("<IIIIII", raw, 0x68)
    if payload_alignment < 1 or payload_alignment & (payload_alignment - 1):
        raise StaticPackError("pack-alignment", str(payload_alignment))
    if index_version != INDEX_SCHEMA_VERSION:
        raise StaticPackError("pack-index-version", str(index_version))
    expected_payload_offset = align(index_offset + index_bytes, payload_alignment)
    if record_offset != HEADER_SIZE:
        raise StaticPackError("pack-record-offset", str(record_offset))
    if gap_offset != record_offset + record_bytes:
        raise StaticPackError("pack-gap-offset", str(gap_offset))
    if index_offset != gap_offset + gap_bytes:
        raise StaticPackError("pack-index-offset", str(index_offset))
    if payload_offset != expected_payload_offset:
        raise StaticPackError("pack-payload-offset", str(payload_offset))
    if record_bytes != record_count * RECORD_SIZE:
        raise StaticPackError("pack-record-count", f"{record_bytes} != {record_count}*{RECORD_SIZE}")
    if gap_bytes != gap_count * GAP_SIZE:
        raise StaticPackError("pack-gap-count", f"{gap_bytes} != {gap_count}*{GAP_SIZE}")
    if payload_offset + payload_bytes != len(raw):
        raise StaticPackError("pack-range", "payload end does not equal file size")
    header_copy = bytearray(raw[:HEADER_SIZE])
    header_copy[0x78:0x7C] = b"\0\0\0\0"
    if zlib.crc32(bytes(header_copy)) & 0xFFFFFFFF != header_crc:
        raise StaticPackError("pack-header-crc", "header")
    record_table = raw[record_offset:record_offset + record_bytes]
    gap_table = raw[gap_offset:gap_offset + gap_bytes]
    index_data = raw[index_offset:index_offset + index_bytes]
    payload = raw[payload_offset:payload_offset + payload_bytes]
    if zlib.crc32(record_table) & 0xFFFFFFFF != record_crc:
        raise StaticPackError("pack-crc", "record table")
    if zlib.crc32(gap_table) & 0xFFFFFFFF != gap_crc:
        raise StaticPackError("pack-crc", "gap table")
    if zlib.crc32(index_data) & 0xFFFFFFFF != index_crc:
        raise StaticPackError("pack-crc", "index json")
    if zlib.crc32(payload) & 0xFFFFFFFF != payload_crc:
        raise StaticPackError("pack-crc", "payload")
    return {
        "version": version, "flags": flags, "record_count": record_count,
        "gap_count": gap_count, "record_table": record_table,
        "gap_table": gap_table, "index_data": index_data, "payload": payload,
        "payload_alignment": payload_alignment, "reserved": reserved,
        "header_crc32": header_crc,
    }


def validate_index_records(index: dict, record_count: int) -> None:
    if index.get("schema") != INDEX_SCHEMA:
        raise StaticPackError("pack-index-schema", str(index.get("schema")))
    if index.get("schema_version") != INDEX_SCHEMA_VERSION:
        raise StaticPackError("pack-index-schema-version", str(index.get("schema_version")))
    records = index.get("records")
    gaps = index.get("gaps")
    if not isinstance(records, list) or not isinstance(gaps, list):
        raise StaticPackError("pack-index-shape", "records/gaps must be lists")
    if len(records) != record_count:
        raise StaticPackError("pack-index-count", f"{len(records)} != {record_count}")
    seen = set()
    for position, record in enumerate(records):
        key = record.get("asset_key")
        if not isinstance(key, str) or not key:
            raise StaticPackError("pack-asset-key", f"record {position}")
        if key in seen:
            raise StaticPackError("pack-duplicate-key", key)
        seen.add(key)
        if record.get("record_index") != position:
            raise StaticPackError("pack-record-index", f"{record.get('record_index')} != {position}")
        if record.get("kind") not in KIND_TO_CODE:
            raise StaticPackError("pack-record-kind", str(record.get("kind")))
        target = record.get("target") or {}
        for field in ("payload_offset", "payload_size", "payload_crc32", "source_size",
                      "source_crc32"):
            if not isinstance(target.get(field), int):
                raise StaticPackError("pack-target-field", f"record {position}: {field}")


def read_static_pack(path: str) -> dict:
    with open(path, "rb") as handle:
        raw = handle.read()
    header = read_pack_header(raw)
    try:
        index = json.loads(header["index_data"].decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise StaticPackError("pack-index-json", str(exc))
    validate_index_records(index, header["record_count"])
    gaps = index.get("gaps")
    if len(gaps) != header["gap_count"]:
        raise StaticPackError("pack-index-gap-count", f"{len(gaps)} != {header['gap_count']}")
    payload = header["payload"]
    record_table = header["record_table"]
    keys = set()
    for position, record in enumerate(index["records"]):
        offset = position * RECORD_SIZE
        (record_size, kind_code, key_crc, record_index, payload_offset, payload_size,
         source_size, payload_crc, source_crc, format_code, width, height, mip_count,
         flags, key_length, reserved0, reserved1, reserved2) = struct.unpack_from(
            "<IIIIQQQIIIIIIIIQQQ", record_table, offset)
        if record_size != RECORD_SIZE:
            raise StaticPackError("pack-record-size-field", f"record {position}")
        if record_index != position:
            raise StaticPackError("pack-record-index-field", f"{record_index} != {position}")
        if kind_code != KIND_TO_CODE[record["kind"]]:
            raise StaticPackError("pack-record-kind-field", f"record {position}")
        key = record["asset_key"]
        if key in keys:
            raise StaticPackError("pack-duplicate-key", key)
        keys.add(key)
        key_bytes = key.encode("utf-8")
        if key_length != len(key_bytes):
            raise StaticPackError("pack-key-length", f"record {position}")
        if key_crc != zlib.crc32(key_bytes) & 0xFFFFFFFF:
            raise StaticPackError("pack-key-crc", key)
        if flags & ~KNOWN_RECORD_FLAGS:
            raise StaticPackError("pack-record-flags", f"record {position}: 0x{flags:x}")
        if payload_offset % header["payload_alignment"]:
            raise StaticPackError("pack-payload-align", f"record {position}")
        if payload_offset > len(payload) or payload_size > len(payload) - payload_offset:
            raise StaticPackError("pack-payload-range", f"record {position}")
        chunk = payload[payload_offset:payload_offset + payload_size]
        if zlib.crc32(chunk) & 0xFFFFFFFF != payload_crc:
            raise StaticPackError("pack-record-crc", f"record {position}")
        target = record["target"]
        expected = {
            "payload_offset": payload_offset,
            "payload_size": payload_size,
            "payload_crc32": payload_crc,
            "source_size": source_size,
            "source_crc32": source_crc,
            "format_code": format_code,
            "width": width,
            "height": height,
            "mip_count": mip_count,
            "flags": flags,
        }
        for field, value in expected.items():
            if target.get(field) != value:
                raise StaticPackError("pack-target-mismatch", f"record {position}: {field}")
        if record["kind"] == "texture-test-blob":
            source_off = target.get("source_blob_offset_within_payload")
            source_len = target.get("source_blob_size")
            if not isinstance(source_off, int) or not isinstance(source_len, int):
                raise StaticPackError("pack-source-blob", f"record {position}")
            if source_off > len(chunk) or source_len > len(chunk) - source_off:
                raise StaticPackError("pack-source-range", f"record {position}")
            if zlib.crc32(chunk[source_off:source_off + source_len]) & 0xFFFFFFFF != source_crc:
                raise StaticPackError("pack-source-crc", f"record {position}")
    gap_table = header["gap_table"]
    for position, gap in enumerate(gaps):
        if gap.get("gap_index") != position:
            raise StaticPackError("pack-gap-index", f"{gap.get('gap_index')} != {position}")
        offset = position * GAP_SIZE
        gap_size, kind_code, count, summary_crc, source_bytes, res0, res1 = struct.unpack_from(
            "<IIIIQII", gap_table, offset)
        if gap_size != GAP_SIZE:
            raise StaticPackError("pack-gap-size-field", f"gap {position}")
        if kind_code != GAP_TO_CODE.get(gap.get("kind")):
            raise StaticPackError("pack-gap-kind", f"gap {position}")
        if count != gap.get("count"):
            raise StaticPackError("pack-gap-count-field", f"gap {position}")
        expected_crc = zlib.crc32(gap.get("summary", "").encode("utf-8")) & 0xFFFFFFFF
        if summary_crc != expected_crc:
            raise StaticPackError("pack-gap-crc", f"gap {position}")
        if source_bytes != gap.get("source_bytes", 0):
            raise StaticPackError("pack-gap-bytes", f"gap {position}")
    return {"header": header, "index": index, "payload": payload}


def verify_pack_artifacts(path: str) -> dict:
    parsed = read_static_pack(path)
    index = parsed["index"]
    payload = parsed["payload"]
    problems: list[str] = []
    tex_verified = 0
    tex_levels = 0
    mod_verified = 0
    for record in index["records"]:
        target = record["target"]
        if record["kind"] == "texture-test-blob":
            chunk = payload[target["payload_offset"]:
                            target["payload_offset"] + target["payload_size"]]
            source = chunk[target["source_blob_offset_within_payload"]:
                           target["source_blob_offset_within_payload"] + target["source_blob_size"]]
            try:
                fmt = tf.require_format(target["format_code"], require_decoder=True)
                levels = []
                cursor = 0
                for level in record["layout"]["levels"]:
                    layout = tf.LevelLayout(
                        mip=level["mip"], width=level["width"], height=level["height"],
                        block_columns=level["block_columns"], block_rows=level["block_rows"],
                        gob_columns=level["gob_columns"],
                        block_height_gobs=level["block_height_gobs"],
                        offset=level["source_offset"], stored_size=level["source_stored_size"],
                        logical_size=level["logical_size"], padded_size=level["padded_size"],
                        alignment=level["alignment"])
                    if layout.offset != cursor:
                        problems.append(f"{record['asset_key']}: mip {layout.mip} non-contiguous")
                    cursor += layout.stored_size
                    linear = tf.untile_level(source, layout, fmt)
                    slice_ = chunk[level["target_offset"]:
                                   level["target_offset"] + level["target_size"]]
                    if linear != slice_:
                        problems.append(f"{record['asset_key']}: mip {layout.mip} target mismatch")
                    if tf.retile_level(linear, layout, fmt) != source[
                            layout.offset:layout.offset + layout.stored_size]:
                        problems.append(f"{record['asset_key']}: mip {layout.mip} retile mismatch")
                    if sha256_bytes(linear) != level["target_sha256"]:
                        problems.append(f"{record['asset_key']}: mip {layout.mip} target sha")
                    rgba = tf.decode_level_rgba(linear, layout, fmt)
                    if sha256_bytes(rgba) != level["decoded_rgba_sha256"]:
                        problems.append(f"{record['asset_key']}: mip {layout.mip} decode sha")
                    tex_levels += 1
                if cursor != len(source):
                    problems.append(f"{record['asset_key']}: level chain does not cover source")
                if len(record["layout"]["levels"]) != target["mip_count"]:
                    problems.append(f"{record['asset_key']}: mip count mismatch")
                tex_verified += 1
            except tf.TexFormatError as exc:
                problems.append(f"{record['asset_key']}: {exc.reason}: {exc.detail}")
        elif record["kind"] == "model-metadata":
            if target["payload_size"] != 0:
                problems.append(f"{record['asset_key']}: metadata record has payload")
            if record["layout"]["header"]["version"] != 0xD4:
                problems.append(f"{record['asset_key']}: MOD version")
            mod_verified += 1
        else:
            problems.append(f"{record['asset_key']}: unsupported record kind")
    return {
        "pack_sha256": sha256_file(path),
        "record_count": len(index["records"]),
        "gap_count": len(index["gaps"]),
        "textures_verified": tex_verified,
        "texture_levels_verified": tex_levels,
        "models_verified": mod_verified,
        "problems": problems,
        "passed": not problems,
    }


def build_pack(selection_path: str, tex_pack_dir: str, inventory_summary_path: str,
               output_dir: str, extracted_root: str) -> dict:
    with open(selection_path, "r", encoding="utf-8") as handle:
        selection = json.load(handle)
    with open(inventory_summary_path, "r", encoding="utf-8") as handle:
        inventory_summary = json.load(handle)
    tex_pack_path = os.path.join(tex_pack_dir, "textures.vtxp")
    tex_index_path = os.path.join(tex_pack_dir, "textures.index.json")
    data, tex_index, _raw = ct.read_pack(tex_pack_path)
    vtxp_by_key = {record["asset_key"]: record for record in tex_index["records"]}
    selected_tex = selection.get("selection", [])
    missing = [item["asset_key"] for item in selected_tex if item["asset_key"] not in vtxp_by_key]
    if missing:
        raise StaticPackError("pack-missing-vtxp-record", ",".join(missing[:4]))

    blob = BlobWriter(PAYLOAD_ALIGNMENT)
    record_specs: list[dict] = []
    index_records: list[dict] = []
    for item in selected_tex:
        vtxp = vtxp_by_key[item["asset_key"]]
        pdata = vtxp["pack_data"]
        chunk = data[pdata["offset"]:pdata["offset"] + pdata["size"]]
        payload_offset, payload_size = blob.append(chunk)
        source_blob = vtxp["source_blob"]
        source_bytes = chunk[source_blob["offset"]:source_blob["offset"] + source_blob["size"]]
        flags = RECORD_FLAG_IMMUTABLE | RECORD_FLAG_HOST_TEST_ONLY | RECORD_FLAG_ALL_MIPS_PRESERVED
        target = {
            "payload_offset": payload_offset,
            "payload_size": payload_size,
            "payload_crc32": zlib.crc32(chunk) & 0xFFFFFFFF,
            "payload_alignment": PAYLOAD_ALIGNMENT,
            "source_size": len(source_bytes),
            "source_crc32": zlib.crc32(source_bytes) & 0xFFFFFFFF,
            "source_blob_offset_within_payload": source_blob["offset"],
            "source_blob_size": source_blob["size"],
            "vtxp_record_index": vtxp.get("record_index"),
            "vtxp_pack_data_sha256": pdata["sha256"],
            "immutable": True,
            "host_test_only": True,
            "gxm_upload_ready": False,
            "loading_constraints": [
                "read-only; payload offsets are fixed at pack build time",
                "host verification target; requires a separate GXM upload conversion",
                "all source mips preserved; no mip/face is synthesized",
            ],
            "format_code": vtxp["container"]["format_code"],
            "width": vtxp["container"]["width"],
            "height": vtxp["container"]["height"],
            "mip_count": vtxp["container"]["mip_count"],
            "flags": flags,
        }
        layout = {
            "format_code": vtxp["container"]["format_code"],
            "format_name": vtxp["container"]["format_name"],
            "resource_type": vtxp["container"]["resource_type"],
            "width": vtxp["container"]["width"],
            "height": vtxp["container"]["height"],
            "mip_count": vtxp["container"]["mip_count"],
            "faces": 1,
            "layers": 1,
            "layout_model": vtxp["container"]["layout_model"],
            "layout_verified": vtxp["container"]["layout_verified"],
            "levels": vtxp["levels"],
        }
        index_record = {
            "record_index": len(index_records),
            "asset_key": item["asset_key"],
            "kind": "texture-test-blob",
            "identity": {
                "source": item.get("source"),
                "archive_relative": item.get("archive"),
                "entry_index": item.get("entry"),
                "entry_path": item.get("entry_path"),
                "dti": item.get("dti"),
                "raw_relative": item.get("raw"),
                "source_file_sha256": item.get("sha256"),
                "source_payload_sha256": item.get("payload_sha256"),
                "source_status": item.get("source_status"),
                "source_identity_state": item.get("source_identity_state"),
            },
            "layout": layout,
            "target": target,
            "dependencies": [],
            "dependencies_note": "No dependency edges are asserted offline; material/MFX "
                                 "linkage is not resolved in C0-C2.",
            "notes": item.get("notes", []) + [
                "Embedded from textures.vtxp; source payload and all linear-mip targets preserved.",
            ],
        }
        index_records.append(index_record)
        record_specs.append({"kind": "texture-test-blob", "key": item["asset_key"],
                             "target": target, "format_code": target["format_code"],
                             "width": target["width"], "height": target["height"],
                             "mip_count": target["mip_count"], "flags": flags,
                             "source_size": len(source_bytes), "source_crc32": target["source_crc32"]})

    for item in selection.get("models", []):
        raw_path = os.path.join(extracted_root, item["raw"].replace("/", os.sep))
        if not os.path.isfile(raw_path):
            raise StaticPackError("pack-missing-mod", item["raw"])
        with open(raw_path, "rb") as handle:
            source_bytes = handle.read()
        if sha256_bytes(source_bytes) != item["sha256"]:
            raise StaticPackError("pack-mod-sha", item["raw"])
        flags = RECORD_FLAG_IMMUTABLE | RECORD_FLAG_HOST_TEST_ONLY | RECORD_FLAG_METADATA_ONLY
        target = {
            "payload_offset": 0, "payload_size": 0, "payload_crc32": 0,
            "payload_alignment": 0, "source_size": len(source_bytes),
            "source_crc32": zlib.crc32(source_bytes) & 0xFFFFFFFF,
            "source_blob_offset_within_payload": 0, "source_blob_size": 0,
            "immutable": True, "host_test_only": True, "gxm_upload_ready": False,
            "metadata_only": True,
            "loading_constraints": [
                "metadata only; no geometry bytes are stored",
                "not loadable as a model without a C3 conversion",
            ],
            "format_code": 0, "width": 0, "height": 0, "mip_count": 0, "flags": flags,
        }
        index_record = {
            "record_index": len(index_records),
            "asset_key": item["asset_key"],
            "kind": "model-metadata",
            "identity": {
                "source": item.get("source"),
                "archive_relative": item.get("archive"),
                "entry_index": item.get("entry"),
                "entry_path": item.get("entry_path"),
                "dti": item.get("dti"),
                "raw_relative": item.get("raw"),
                "source_file_sha256": item.get("sha256"),
                "source_status": item.get("source_status"),
                "source_identity_state": item.get("source_identity_state"),
            },
            "layout": {
                "header": item["header"],
                "mesh_count": item["mesh_count"],
                "mesh_streams": item["mesh_streams"],
                "mesh_streams_truncated": item["mesh_streams_truncated"],
                "fvf_info_values": item["fvf_info_values"],
                "vertex_stride_values": item["vertex_stride_values"],
                "material_names": item["material_names"],
                "attribute_semantics": item["attribute_semantics"],
            },
            "target": target,
            "dependencies": [],
            "dependencies_note": "Material names are in-file evidence; no external dependency "
                                 "edges are asserted (C3/C4 gap).",
            "notes": item.get("notes", []),
        }
        index_records.append(index_record)
        record_specs.append({"kind": "model-metadata", "key": item["asset_key"],
                             "target": target, "format_code": 0, "width": 0, "height": 0,
                             "mip_count": 0, "flags": flags, "source_size": len(source_bytes),
                             "source_crc32": target["source_crc32"]})

    if not index_records:
        raise StaticPackError("pack-empty", "no selection records")
    gaps = build_gaps(selection, inventory_summary)
    payload = blob.build()
    record_table = bytearray(len(index_records) * RECORD_SIZE)
    for position, spec in enumerate(record_specs):
        target = spec["target"]
        key_bytes = spec["key"].encode("utf-8")
        struct.pack_into(
            "<IIIIQQQIIIIIIIIQQQ", record_table, position * RECORD_SIZE,
            RECORD_SIZE, KIND_TO_CODE[spec["kind"]],
            zlib.crc32(key_bytes) & 0xFFFFFFFF, position,
            target["payload_offset"], target["payload_size"], target["source_size"],
            target["payload_crc32"], target["source_crc32"],
            spec["format_code"], spec["width"], spec["height"], spec["mip_count"],
            spec["flags"], len(key_bytes), 0, 0, 0)
    gap_table = bytearray(len(gaps) * GAP_SIZE)
    for position, gap in enumerate(gaps):
        summary_crc = zlib.crc32(gap["summary"].encode("utf-8")) & 0xFFFFFFFF
        struct.pack_into("<IIIIQII", gap_table, position * GAP_SIZE, GAP_SIZE,
                         GAP_TO_CODE[gap["kind"]], gap["count"], summary_crc,
                         gap.get("source_bytes", 0), 0, 0)
    index_obj = {
        "schema": INDEX_SCHEMA,
        "schema_version": INDEX_SCHEMA_VERSION,
        "tool": "pack_static_assets.py",
        "tool_version": TOOL_VERSION,
        "pack": {
            "magic": PACK_MAGIC.decode("ascii"),
            "version": PACK_VERSION,
            "header_size": HEADER_SIZE,
            "record_size": RECORD_SIZE,
            "gap_size": GAP_SIZE,
            "payload_alignment": PAYLOAD_ALIGNMENT,
            "record_count": len(index_records),
            "gap_count": len(gaps),
        },
        "inputs": {
            "selection_sha256": sha256_file(selection_path),
            "textures_pack_sha256": sha256_file(tex_pack_path),
            "textures_index_sha256": sha256_file(tex_index_path),
            "inventory_summary_sha256": sha256_file(inventory_summary_path),
        },
        "records": index_records,
        "gaps": gaps,
        "counts": {
            "textures": sum(1 for record in index_records
                            if record["kind"] == "texture-test-blob"),
            "models": sum(1 for record in index_records if record["kind"] == "model-metadata"),
            "gap_records": len(gaps),
            "payload_bytes": len(payload),
        },
        "limitations": [
            "Host-verifiable static pack only; not GXM upload-ready.",
            "No cube/array TEX is included: the _CM variant is unresolved and recorded as a gap.",
            "MOD records are metadata-only; skinning/animation conversion is C3 scope.",
            "Dependency edges are not invented; only evidenced in-file metadata is recorded.",
        ],
    }
    index_data = canonical_json(index_obj)
    pack_bytes = assemble_pack(bytes(record_table), bytes(gap_table), index_data,
                               payload, len(index_records), len(gaps))
    os.makedirs(output_dir, exist_ok=True)
    pack_path = os.path.join(output_dir, "static-assets.mhsp")
    with open(pack_path, "wb") as handle:
        handle.write(pack_bytes)
    with open(os.path.join(output_dir, "static-assets.index.json"), "w",
              encoding="utf-8") as handle:
        json.dump(index_obj, handle, indent=2, sort_keys=True)
        handle.write("\n")
    verification = verify_pack_artifacts(pack_path)
    summary = {
        "schema": "mhgu-static-asset-pack-summary/1",
        "tool": "pack_static_assets.py",
        "tool_version": TOOL_VERSION,
        "pack_path": os.path.basename(pack_path),
        "pack_sha256": sha256_file(pack_path),
        "pack_bytes": os.path.getsize(pack_path),
        "payload_bytes": len(payload),
        "record_count": len(index_records),
        "record_kinds": {
            "textures": index_obj["counts"]["textures"],
            "models": index_obj["counts"]["models"],
        },
        "gap_count": len(gaps),
        "gap_kinds": {gap["kind"]: gap["count"] for gap in gaps},
        "verification": verification,
    }
    with open(os.path.join(output_dir, "summary.json"), "w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2, sort_keys=True)
        handle.write("\n")
    return summary


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selection")
    parser.add_argument("--tex-pack")
    parser.add_argument("--inventory-summary")
    parser.add_argument("--extracted-root", default=DEFAULT_EXTRACTED)
    parser.add_argument("--output")
    parser.add_argument("--verify-pack", help="read and verify an existing .mhsp file")
    args = parser.parse_args(argv)
    if args.verify_pack:
        report = verify_pack_artifacts(args.verify_pack)
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0 if report["passed"] else 1
    if not (args.selection and args.tex_pack and args.inventory_summary and args.output):
        parser.error("--selection, --tex-pack, --inventory-summary and --output are required")
    summary = build_pack(args.selection, args.tex_pack, args.inventory_summary, args.output,
                         args.extracted_root)
    print(json.dumps({"pack_sha256": summary["pack_sha256"],
                      "passed": summary["verification"]["passed"],
                      "record_count": summary["record_count"]}, sort_keys=True))
    return 0 if summary["verification"]["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())

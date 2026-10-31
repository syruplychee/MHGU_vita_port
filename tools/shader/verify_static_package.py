#!/usr/bin/env python3
"""Verify a static shader package without requiring game inputs.

Checks the package-level manifest, the VPSHIX1 index header/CRC, every
referenced GXP (size + SHA-256 + CRC32) and the VPSRES1 resource sidecar. This
is an integrity check only; it is not a GXM link, pixel or runtime claim.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

INDEX_HEADER = 64
STAGE_RECORD = 48
VARIANT_RECORD = 40
PROGRAM_RECORD = 16


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--allow-legacy-abi", action="store_true",
                        help="accept ABI 1 packages (diagnostics only)")
    args = parser.parse_args()
    package = args.package.resolve()
    if not package.is_dir():
        fail(f"package directory missing: {package}")

    manifest_path = package / "manifest.json"
    index_path = package / "shader-index.bin"
    index_json_path = package / "shader-index.json"
    sidecar_path = package / "shader-resources.bin"
    sidecar_manifest_path = package / "shader-resources.manifest.json"
    for path in (manifest_path, index_path, index_json_path, sidecar_path,
                 sidecar_manifest_path):
        if not path.is_file():
            fail(f"missing {path.name}")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    abi = int(manifest.get("abi_version", 0))
    if abi not in (1, 2) or (abi == 1 and not args.allow_legacy_abi):
        fail(f"unsupported manifest ABI {abi}; ABI 2 is required for play")
    if sha256(index_path) != manifest.get("index_sha256"):
        fail("shader-index.bin does not match manifest.index_sha256")
    if sha256(index_json_path) != manifest.get("index_json_sha256"):
        fail("shader-index.json does not match manifest.index_json_sha256")

    data = index_path.read_bytes()
    if len(data) < INDEX_HEADER or data[:8] != b"VPSHIX1\x00":
        fail("shader-index.bin magic/header mismatch")
    version, header_size, flags = struct.unpack_from("<HHI", data, 8)
    stages, variants, programs, stage_size, variant_size, program_size, \
        body_bytes, body_crc, index_abi = struct.unpack_from("<IIIIIIIII", data, 16)
    if version != 1 or header_size != INDEX_HEADER or flags != 1:
        fail("unsupported VPSHIX1 header")
    if (stage_size, variant_size, program_size) != (
            STAGE_RECORD, VARIANT_RECORD, PROGRAM_RECORD):
        fail("unexpected VPSHIX1 record sizes")
    if index_abi != abi:
        fail(f"index ABI {index_abi} != manifest ABI {abi}")
    if body_bytes != len(data) - INDEX_HEADER:
        fail("VPSHIX1 body length mismatch")
    if stages * STAGE_RECORD + variants * VARIANT_RECORD + programs * PROGRAM_RECORD != body_bytes:
        fail("VPSHIX1 record counts do not match body length")
    if zlib.crc32(data[INDEX_HEADER:]) & 0xFFFFFFFF != body_crc:
        fail("VPSHIX1 body CRC mismatch")

    index_json = json.loads(index_json_path.read_text(encoding="utf-8-sig"))
    if len(index_json["stages"]) != stages or len(index_json["variants"]) != variants \
            or len(index_json["programs"]) != programs:
        fail("shader-index.json counts do not match the binary index")
    if int(index_json["abi_version"]) != abi:
        fail("shader-index.json ABI does not match the binary index")

    referenced = set()
    for variant in index_json["variants"]:
        relative = str(variant["gxp"]).replace("\\", "/")
        if relative.startswith("/") or ".." in relative.split("/"):
            fail(f"variant path escapes the package: {relative}")
        gxp = package / relative
        if not gxp.is_file():
            fail(f"missing GXP: {relative}")
        payload = gxp.read_bytes()
        if len(payload) != int(variant["gxp_bytes"]):
            fail(f"GXP size mismatch: {relative}")
        if hashlib.sha256(payload).hexdigest() != variant["gxp_sha256"]:
            fail(f"GXP SHA-256 mismatch: {relative}")
        if (zlib.crc32(payload) & 0xFFFFFFFF) != int(variant["gxp_crc32"]):
            fail(f"GXP CRC32 mismatch: {relative}")
        referenced.add(relative)

    shader_files = {p.relative_to(package).as_posix()
                    for p in (package / "shaders").glob("*.gxp")}
    if shader_files != referenced:
        missing = sorted(referenced - shader_files)
        extra = sorted(shader_files - referenced)
        fail(f"shader directory mismatch; missing={missing[:4]} extra={extra[:4]}")

    sidecar_manifest = json.loads(sidecar_manifest_path.read_text(encoding="utf-8-sig"))
    sidecar_info = sidecar_manifest.get("sidecar", {})
    if sha256(sidecar_path) != sidecar_info.get("sha256"):
        fail("shader-resources.bin does not match its manifest")
    if sidecar_path.stat().st_size != int(sidecar_info.get("bytes", -1)):
        fail("shader-resources.bin size does not match its manifest")
    if sidecar_manifest.get("index", {}).get("sha256") != manifest.get("index_sha256"):
        fail("resource manifest and package manifest disagree on the index identity")

    print(f"PASS static package: abi={abi} stages={stages} variants={variants} "
          f"programs={programs} gxp={len(referenced)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

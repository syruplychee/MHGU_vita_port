"""Read-only NSO layout/linkage inventory for planning; never decodes or executes code."""
import argparse
from collections import Counter
import hashlib
import importlib.metadata
import json
from pathlib import Path
import struct
import sys


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def inventory(path, expected, decompress):
    raw = path.read_bytes()
    if len(raw) != expected["size"] or hashlib.sha256(raw).hexdigest() != expected["sha256"]:
        raise ValueError(f"{path.name}: input hash/size mismatch")
    if raw[:4] != b"NSO0" or raw[0x40:0x60].hex() != expected["build_id"]:
        raise ValueError(f"{path.name}: NSO/build ID mismatch")
    flags = u32(raw, 12)
    segments, payloads = [], []
    for index, name in enumerate(("text", "rodata", "data")):
        offset, address, size, extra = struct.unpack_from("<4I", raw, 16 + index * 16)
        stored = u32(raw, 0x60 + index * 4) if flags & (1 << index) else size
        if offset + stored > len(raw) or address + size > 128 * 1024 * 1024:
            raise ValueError("NSO segment out of range")
        block = raw[offset:offset + stored]
        decoded = decompress(block, uncompressed_size=size) if flags & (1 << index) else block
        digest = hashlib.sha256(decoded).hexdigest()
        if len(decoded) != size:
            raise ValueError("NSO decoded size mismatch")
        if flags & (1 << (index + 3)) and bytes.fromhex(digest) != raw[0xA0 + index * 32:0xC0 + index * 32]:
            raise ValueError("NSO decoded segment hash mismatch")
        segments.append({"name": name, "offset": address, "size": size,
                         "sha256": digest, "stored_bytes": stored})
        payloads.append((address, decoded))
    image = bytearray(max(address + len(data) for address, data in payloads))
    for address, data in payloads:
        image[address:address + len(data)] = data
    mod = u32(image, 4)
    tags, dynamic = {}, []
    if image[mod:mod + 4] == b"MOD0":
        position = mod + u32(image, mod + 4)
        for offset in range(position, min(position + 65536, len(image) - 7), 8):
            tag, value = struct.unpack_from("<2I", image, offset)
            if not tag:
                break
            dynamic.append((tag, value))
            tags[tag] = value
        else:
            raise ValueError("Unterminated dynamic table")
    else:
        raise ValueError("MOD0 header missing")
    strings, string_size = tags.get(5, 0), tags.get(10, 0)

    def string_at(offset):
        if not 0 <= offset < string_size:
            raise ValueError("String offset out of range")
        end = image.find(b"\0", strings + offset, strings + string_size)
        if end < 0:
            raise ValueError("Unterminated dynamic string")
        return image[strings + offset:end].decode("utf-8")

    symbols = []
    # NSO header explicitly declares the dynsym extent relative to rodata.
    sym_offset, sym_size = struct.unpack_from("<2I", raw, 0x98)
    sym_begin = segments[1]["offset"] + sym_offset
    if sym_size % 16 or sym_begin + sym_size > len(image):
        raise ValueError("Invalid dynamic symbol extent")
    for position in range(sym_begin, sym_begin + sym_size, 16):
        name_offset, value, size, info, other, section = struct.unpack_from("<3IBBH", image, position)
        name = string_at(name_offset)
        if name:
            symbols.append({"name": name, "defined": section != 0,
                            "type": info & 15, "value": value, "size": size})
    relocations = {}
    for label, address_tag, size_tag in (("rel", 17, 18), ("plt", 23, 2)):
        size, address = tags.get(size_tag, 0), tags.get(address_tag, 0)
        if size and ((label == "plt" and tags.get(20) != 17) or size % 8 or address + size > len(image)):
            raise ValueError("Unsupported relocation table")
        counts = Counter(u32(image, p + 4) & 255 for p in range(address, address + size, 8))
        relocations[label] = {str(kind): count for kind, count in sorted(counts.items())}
    imports = [s["name"] for s in symbols if not s["defined"]]
    named = [name for name in imports if any(term in name.lower() for term in
             ("nvn", "graphics", "tls", "errno", "audiorenderer"))]
    return {"name": path.name, "sha256": expected["sha256"], "build_id": expected["build_id"],
            "segments": segments, "bss_size": u32(raw, 0x3C),
            "image_span_with_bss": len(image) + u32(raw, 0x3C),
            "dynamic_symbol_count_including_null": sym_size // 16,
            "named_imports": len(imports), "named_exports": sum(s["defined"] for s in symbols),
            "needed": [string_at(value) for tag, value in dynamic if tag == 1],
            "relocation_type_counts": relocations, "selected_import_names": named}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exefs", type=Path, required=True)
    parser.add_argument("--analysis-libraries", type=Path, required=True,
                        help="Existing local Python packages containing lz4; no install performed")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if args.output.resolve().is_relative_to(args.exefs.resolve()):
        parser.error("Write the report outside input ExeFS")
    sys.path.insert(0, str(args.analysis_libraries.resolve()))
    import lz4.block
    lock = json.loads((root / "config/target-1.4.0.json").read_text(encoding="utf-8"))
    result = {"profile": lock["profile"], "method": "Read-only NSO header, decoded segment hashes, MOD0 dynamic tables",
              "limitations": "No instruction decoding, reachability, ABI, device execution or relocation correctness proof",
              "lz4_distribution": importlib.metadata.version("lz4"),
              "modules": [inventory(args.exefs / m["name"], m, lz4.block.decompress) for m in lock["modules"]]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()

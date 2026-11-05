"""Verify the resource experiment's production guards against locked main.

Read-only. Reports source identity, patch bytes and the reviewed PIC roots.
No game payload is written to the repository or output file.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

import lz4.block

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "input"))
from nso_reloc_model import decode


def inspect(image, header):
    def word(at):
        return struct.unpack_from("<I", image, at)[0]

    def guard(at, size, expected):
        actual = hashlib.sha256(image[at:at + size]).hexdigest()
        if actual != expected:
            raise ValueError(f"immutable range mismatch at {at:x}")
        return {"offset": hex(at), "bytes": size, "sha256": actual}

    def macro(name):
        return re.search(r"#define " + name + r" (\S+)", header)[1].strip('"u')

    heap_guard = guard(int(macro("VP_TITLE_HEAP_GUARD_OFFSET"), 16),
                       int(macro("VP_TITLE_HEAP_GUARD_BYTES"), 16),
                       macro("VP_TITLE_HEAP_GUARD_SHA"))
    replacements = []
    for row in re.findall(r"\{(0x[0-9a-f]+)u, (0x[0-9a-f]+)u, "
                          r"(0x[0-9a-f]+)u, (0x[0-9a-f]+)u\}", header):
        at, old, small, medium = (int(v, 16) for v in row)
        if word(at) != old:
            raise ValueError(f"patch word mismatch at {at:x}")
        register = (old >> 12) & 15
        for instruction, mib in [(old, 850), (small, 256), (medium, 512)]:
            immediate = ((instruction >> 4) & 0xf000) | (instruction & 0xfff)
            if (instruction & 0xfff00000 != 0xe3400000 or
                    (instruction >> 12) & 15 != register or
                    immediate << 16 != mib * 1024 * 1024):
                raise ValueError("replacement is not the reviewed MOVT capacity")
        replacements.append({"offset": hex(at), "original": f"{old:08x}",
                             "capacity256": f"{small:08x}",
                             "capacity512": f"{medium:08x}", "register": register})
    probes = []
    pattern = (r'\{(0x[0-9a-f]+)u, (0x[0-9a-f]+)u, (0x[0-9a-f]+)u, '
               r'(0x[0-9a-f]+)u, "([^"]+)",\s*"([0-9a-f]{64})"\}')
    for at, first, second, service, name, sha in re.findall(pattern, header):
        at, first, second, service = (int(v, 16) for v in (at, first, second, service))
        if (word(at), word(at + 4)) != (first, second):
            raise ValueError(f"probe displacement mismatch at {at:x}")
        row = guard(at, 64, sha)
        row.update(name=name, service=hex(service), overwritten_bytes=8,
                   original_words=[f"{first:08x}", f"{second:08x}"])
        probes.append(row)
    if len(replacements) != 2 or len(probes) != 6:
        raise ValueError("incomplete production contract")
    roots = []
    for load, add, target, name in [
            (0xb51cc0, 0xb51ccc, 0x1579c57, "rTexture"),
            (0x151dd8, 0x151ddc, 0x1556d50, "Resource"),
            (0x151e20, 0x151e2c, 0x1556d5f, "NVNTexture"),
            (0x151e5c, 0x151e60, 0x1556d6a, "NVNBuffer"),
            (0x151e88, 0x151e8c, 0x1556d74, "Shader")]:
        instruction = word(load)
        register = (instruction >> 12) & 15
        literal = load + 8 + ((instruction & 0xfff) if instruction & 0x800000
                              else -(instruction & 0xfff))
        if (instruction & 0x0f7f0000 != 0x051f0000 or
                word(add) & 0x0fff0ff0 != 0x008f0000 or
                word(add) & 15 != register or
                (word(literal) + add + 8) & 0xffffffff != target or
                image[target:target + len(name) + 1] != name.encode() + b"\0"):
            raise ValueError(f"PIC root mismatch: {name}")
        roots.append({"name": name, "load": hex(load), "literal": hex(literal),
                      "add_pc": hex(add), "string": hex(target)})
    return {"heap_guard": heap_guard, "capacity_words": replacements,
            "lifecycle_probes": probes, "reviewed_pic_roots": roots,
            "limitations": ["guards are finite reviewed ranges, not all engine code",
                            "a load/replace/release probe does not cover all content writers",
                            "runtime GPU field associations require same-run resource evidence"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exefs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    lock = json.loads((root / "config/target-1.4.0.json").read_text())
    expected = next(m for m in lock["modules"] if m["name"] == "main")
    module = decode(args.exefs / "main", expected, lz4.block.decompress)
    report = inspect(module.image, (root / "src/loader/resource_experiment.h").read_text())
    report.update(main_nso_sha256=expected["sha256"],
                  decoded_sha256=hashlib.sha256(module.image).hexdigest())
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print("PASS locked main, 2 capacity words, 6 lifecycle guards and 5 PIC roots")


if __name__ == "__main__":
    main()

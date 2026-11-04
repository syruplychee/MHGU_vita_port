"""Read-only A32 render audit from the project's locked NSO inputs.

Disassembly and decoded images are private artifacts, never source fixtures.
Requires the existing offline analysis dependencies lz4 and capstone.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import capstone
import lz4.block

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "input"))
from nso_reloc_model import decode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exefs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--range", action="append", default=[],
                        help="START:END in hexadecimal module offsets")
    parser.add_argument("--compare-elf", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    lock = json.loads((root / "config/target-1.4.0.json").read_text())
    expected = next(m for m in lock["modules"] if m["name"] == "main")
    image = decode(args.exefs / "main", expected, lz4.block.decompress)
    args.output.mkdir(parents=True, exist_ok=True)
    ranges = [tuple(int(v, 16) for v in r.split(":")) for r in args.range]
    text = image.segments[0]
    report = {"main_nso_sha256": expected["sha256"],
              "decoded_sha256": hashlib.sha256(image.image).hexdigest(),
              "ranges": [], "limitations": [
                  "A32 direct BL inventory excludes indirect and Thumb calls",
                  "static call counts are not runtime frequency or duration"]}
    if args.compare_elf:
        elf = args.compare_elf.read_bytes()
        if elf[:7] != b"\x7fELF\x01\x01\x01":
            raise ValueError("expected little-endian ELF32")
        phoff = struct.unpack_from("<I", elf, 28)[0]
        phsize, phcount = struct.unpack_from("<HH", elf, 42)
        compared = []
        for i in range(phcount):
            kind, off, va, _, size, _, flags, _ = struct.unpack_from(
                "<8I", elf, phoff + i * phsize)
            if kind != 1 or not size:
                continue
            if elf[off:off + size] != image.image[va:va + size]:
                raise ValueError(f"ELF source mismatch at module offset {va:x}")
            compared.append({"module_offset": hex(va), "bytes": size,
                             "flags": flags})
        report["analysis_elf"] = {"sha256": hashlib.sha256(elf).hexdigest(),
                                   "compared_load_segments": compared}
    cs = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    callers = {start: [] for start, _ in ranges}
    for at in range(text.offset, text.offset + text.size - 3, 4):
        word = struct.unpack_from("<I", image.image, at)[0]
        if word & 0x0f000000 != 0x0b000000 or word >> 28 == 15:
            continue
        delta = (word & 0xffffff) << 2
        if delta & 0x2000000:
            delta -= 0x4000000
        target = (at + 8 + delta) & 0xffffffff
        if target in callers:
            callers[target].append(hex(at))
    for start, end in ranges:
        if start % 4 or end <= start or end > text.offset + text.size:
            raise ValueError("invalid text range")
        data = image.image[start:end]
        instructions = list(cs.disasm(data, start))
        name = f"main-{start:08x}-{end:08x}.txt"
        (args.output / name).write_text("\n".join(
            f"{i.address:08x} {i.bytes.hex()} {i.mnemonic} {i.op_str}"
            for i in instructions) + "\n", encoding="utf-8")
        report["ranges"].append({"start": hex(start), "end_exclusive": hex(end),
            "sha256": hashlib.sha256(data).hexdigest(),
            "first_16_bytes": data[:16].hex(), "direct_a32_bl_callers": callers[start],
            "disassembly": name})
    (args.output / "identity.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

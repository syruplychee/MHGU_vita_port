"""Strict read-only NSO/MOD0/ELF32 metadata model; no instruction execution or writes."""
from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct


def require(condition, message):
    if not condition: raise ValueError(message)


def u32(data, offset): return struct.unpack_from("<I", data, offset)[0]
def i32(data, offset): return struct.unpack_from("<i", data, offset)[0]


@dataclass(frozen=True)
class Segment:
    name: str
    offset: int
    size: int
    sha256: str


@dataclass(frozen=True)
class Symbol:
    index: int
    name: str
    value: int
    size: int
    binding: int
    type: int
    visibility: int
    section: int


@dataclass(frozen=True)
class Relocation:
    table: str
    index: int
    offset: int
    type: int
    symbol: int


@dataclass
class Module:
    name: str
    identity: dict
    segments: list
    image: bytes
    span: int
    bss_begin: int
    bss_end: int
    mod0: int
    tags: dict
    dynamic_entries: list
    symbols: list
    relocations: list
    metadata: list
    lifecycle: dict

    def region(self, offset, size=1):
        if size == 0:
            if offset == self.span: return "one_past_image"
            size = 1
        for s in self.segments:
            if s.offset <= offset and offset + size <= s.offset + s.size: return s.name
        if len(self.image) <= offset and offset + size <= self.span: return "bss"
        return "gap" if 0 <= offset and offset + size <= self.span else "outside"

    def read(self, offset, size):
        require(self.region(offset, size) in ("text", "rodata", "data"), f"{self.name}: metadata not file-backed at {offset:#x}")
        return self.image[offset:offset + size]

    def target_word(self, offset):
        if self.region(offset, 4) not in ("text", "rodata", "data", "bss"): return None
        return int.from_bytes(self.image[offset:offset + 4].ljust(4, b"\0"), "little")

    def string(self, offset):
        address, size = self.tags[5], self.tags[10]
        require(0 <= offset < size, f"{self.name}: string index outside table")
        end = self.image.find(b"\0", address + offset, address + size)
        require(end >= 0, f"{self.name}: unterminated string")
        return self.image[address + offset:end].decode("utf-8")


def decode(path: Path, expected, decompress):
    raw = path.read_bytes()
    require(len(raw) == expected["size"] and hashlib.sha256(raw).hexdigest() == expected["sha256"], f"{path.name}: wrong size/hash")
    require(len(raw) >= 256 and raw[:4] == b"NSO0" and raw[0x40:0x60].hex() == expected["build_id"], f"{path.name}: wrong NSO/build ID")
    flags = u32(raw, 12)
    require(u32(raw, 4) == 0 and not flags & ~0x3f, f"{path.name}: unsupported NSO version/flags")
    segments, payloads, file_ranges = [], [], []
    previous = 0
    for i, name in enumerate(("text", "rodata", "data")):
        source, offset, size, _ = struct.unpack_from("<4I", raw, 16 + 16*i)
        stored = u32(raw, 0x60 + 4*i) if flags & (1 << i) else size
        require(source >= 256 and stored > 0 and source + stored <= len(raw), f"{path.name}: stored segment out of range")
        require(size > 0 and offset % 4096 == 0 and previous <= offset and offset + size <= 128*1024*1024,
                f"{path.name}: decoded segment out of range/overlap")
        require(i != 0 or offset == 0, f"{path.name}: text must start at module offset zero")
        require(all(source + stored <= a or b <= source for a, b in file_ranges), f"{path.name}: stored segments overlap")
        file_ranges.append((source, source + stored)); previous = offset + size
        payload = raw[source:source + stored]
        if flags & (1 << i): payload = decompress(payload, uncompressed_size=size)
        sha = hashlib.sha256(payload).hexdigest()
        require(len(payload) == size and flags & (1 << (i+3)) and bytes.fromhex(sha) == raw[0xa0+i*32:0xc0+i*32],
                f"{path.name}: missing/mismatched decoded segment hash")
        segments.append(Segment(name, offset, size, sha)); payloads.append(payload)
    span = previous + u32(raw, 0x3c)
    require(span <= 128*1024*1024, f"{path.name}: image/BSS exceeds audit cap")
    image = bytearray(previous)
    for s, data in zip(segments, payloads): image[s.offset:s.offset+s.size] = data
    m = Module(expected.get("name", path.name), dict(expected), segments, bytes(image), span, 0, 0, 0, {}, [], [], [], [], {})
    m.read(0, 8); m.mod0 = u32(m.image, 4)
    require(m.mod0 % 4 == 0 and m.read(m.mod0, 28)[:4] == b"MOD0", f"{m.name}: invalid MOD0")
    dynamic = m.mod0 + i32(m.image, m.mod0 + 4)
    require(dynamic % 4 == 0, f"{m.name}: unaligned dynamic table")
    for i in range(8192):
        p = dynamic + i*8; tag, value = struct.unpack("<II", m.read(p, 8))
        m.dynamic_entries.append((tag, value))
        if not tag: break
        require(tag == 1 or tag not in m.tags, f"{m.name}: duplicate dynamic tag {tag:#x}")
        if tag != 1: m.tags[tag] = value
    else: raise ValueError(f"{m.name}: unterminated dynamic table")
    m.metadata = [("mod0", m.mod0, 28), ("dynamic", dynamic, len(m.dynamic_entries)*8)]
    m.bss_begin = m.mod0 + i32(m.image, m.mod0 + 8)
    m.bss_end = m.mod0 + i32(m.image, m.mod0 + 12)
    require(segments[2].offset <= m.bss_begin <= m.bss_end <= span, f"{m.name}: MOD0 BSS outside RW span")
    require(not any(m.image[m.bss_begin:min(len(m.image), m.bss_end)]), f"{m.name}: nonzero logical BSS file overlap")
    string_offset, string_size, symbol_offset, symbol_size = struct.unpack_from("<4I", raw, 0x90)
    ro = segments[1]
    require(string_offset + string_size <= ro.size and symbol_offset + symbol_size <= ro.size
            and symbol_size > 0 and symbol_size % 16 == 0, f"{m.name}: NSO dynamic tables outside RO")
    require(m.tags.get(5) == ro.offset + string_offset and m.tags.get(10) == string_size
            and m.tags.get(6) == ro.offset + symbol_offset and m.tags.get(11) == 16, f"{m.name}: NSO/dynamic table disagreement")
    m.read(m.tags[5], string_size); m.read(m.tags[6], symbol_size)
    m.metadata += [("dynstr", m.tags[5], string_size), ("dynsym", m.tags[6], symbol_size)]
    for index, pos in enumerate(range(m.tags[6], m.tags[6] + symbol_size, 16)):
        n, value, size, info, other, section = struct.unpack_from("<IIIBBH", m.image, pos)
        require(other & ~3 == 0, f"{m.name}: unsupported symbol visibility bits")
        m.symbols.append(Symbol(index, m.string(n), value, size, info >> 4, info & 15, other, section))
    require(m.symbols[0] == Symbol(0,"",0,0,0,0,0,0), f"{m.name}: malformed null symbol")
    count = len(m.symbols)
    if 4 in m.tags:
        address = m.tags[4]; buckets, chains = struct.unpack("<II", m.read(address, 8))
        require(chains == count and 0 < buckets <= count, f"{m.name}: SysV hash count disagreement")
        table = m.read(address + 8, (buckets+chains)*4)
        require(all(v[0] < count for v in struct.iter_unpack("<I", table)), f"{m.name}: SysV hash index out of range")
        m.metadata.append(("sysv_hash", address, 8+len(table)))
    if 0x6ffffef5 in m.tags:
        address = m.tags[0x6ffffef5]; buckets, first, bloom, _ = struct.unpack("<4I", m.read(address, 16))
        require(0 < buckets <= count and first <= count and 0 < bloom <= count, f"{m.name}: bad GNU hash header")
        length = 16 + bloom*4 + buckets*4 + (count-first)*4
        m.read(address, length)
        m.metadata.append(("gnu_hash", address, length))
    for table, addr_tag, size_tag in (("rel",17,18),("plt",23,2)):
        address, size = m.tags.get(addr_tag,0), m.tags.get(size_tag,0)
        require(size % 8 == 0 and address % 4 == 0, f"{m.name}: malformed REL extent")
        require(not size or m.tags.get(19) == 8, f"{m.name}: wrong RELENT")
        require(not size or table != "plt" or m.tags.get(20) == 17, f"{m.name}: PLT is not REL")
        if size:
            data = m.read(address, size); m.metadata.append((table, address, size))
            for index, (offset, info) in enumerate(struct.iter_unpack("<II", data)):
                m.relocations.append(Relocation(table,index,offset,info&255,info>>8))
    relative_count = m.tags.get(0x6ffffffa,0)
    rels = [r for r in m.relocations if r.table == "rel"]
    require(relative_count <= len(rels) and all(r.type == 23 and r.symbol == 0 for r in rels[:relative_count]),
            f"{m.name}: invalid RELCOUNT prefix")
    for label, address_tag, size_tag in (("init_array",25,27),("fini_array",26,28)):
        address, size = m.tags.get(address_tag,0), m.tags.get(size_tag,0)
        require(size % 4 == 0 and address % 4 == 0, f"{m.name}: malformed lifecycle array")
        if size: m.read(address,size)
        m.lifecycle[label] = {"offset": address, "bytes": size, "entries": size//4}
    for label, tag in (("init",12),("fini",13)):
        value = m.tags.get(tag,0)
        require(not value or m.region(value&~1,2) == "text", f"{m.name}: {label} outside text")
        m.lifecycle[label] = value
    m.lifecycle["needed"] = [m.string(value) for tag,value in m.dynamic_entries if tag == 1]
    return m

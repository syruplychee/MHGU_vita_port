#!/usr/bin/env python3
"""Build a private Unifont/R8 derivative of all mapped text fonts.

The original RomFS tree is read only. No game data belongs in Git. Layout
metrics stay unchanged; glyph masks are made on a 3/4 native pixel grid.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import gzip
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib

from PIL import Image, ImageDraw
import tex_format as tf

RESIDENT_SHA = "66b9b2e5cb201bc1cde3ed89a68e515d42dbbd97d8ab8f687a96101725a4261d"
UNIFONT_SHA = "e66385c79a0b8b24a466f3129930e08a966a935b4bf3b28c6bb17a9df9bf791d"
TEX_DTI = 0x241F5DEB
GFD_DTI = 0x2D462600
SIZE = 1024
SYSTEM_FONTS = [("sysfont_AM_NOMIP",(8,16)), ("detail_sysfont_AM_NOMIP",(16,32))]


def sha(data):
    return hashlib.sha256(data).hexdigest()


@dataclass
class Gfd:
    header: bytes
    prefix: str
    pages: int
    glyphs: list[tuple]

    @classmethod
    def parse(cls, data):
        if len(data) < 60 or data[:8] != b"GFD\0\x06\x0f\x01\0":
            raise ValueError("unsupported GFD magic/version")
        fields = struct.unpack_from("<10I", data)
        pos = 56 + fields[9] * 4
        length, = struct.unpack_from("<I", data, pos)
        end = pos + 4 + length
        if end >= len(data) or data[end] or end+1+(fields[7]+fields[8])*20 != len(data):
            raise ValueError("invalid GFD prefix/record length")
        prefix = data[pos+4:end].decode("ascii")
        records = [struct.unpack_from("<5I", data, end+1+i*20)
                   for i in range(fields[7]+fields[8])]
        return cls(data[:end+1], prefix, fields[6], records)

    def encode(self, glyphs, pages):
        if len(glyphs) != len(self.glyphs) or not 1 <= pages <= 255:
            raise ValueError("GFD record/page count")
        header = bytearray(self.header)
        struct.pack_into("<I", header, 24, pages)
        return bytes(header) + b"".join(struct.pack("<5I", *g) for g in glyphs)


@dataclass
class Entry:
    name: bytes
    type: int
    flags: int
    payload: bytes

    def raw(self):
        result = zlib.decompress(self.payload) if self.flags & 0x40000000 else self.payload
        if len(result) != self.flags & 0x3fffffff:
            raise ValueError("ARC uncompressed size mismatch")
        return result

    def replace(self, data):
        return Entry(self.name, self.type, (self.flags & 0xc0000000) | len(data),
                     zlib.compress(data, 9) if self.flags & 0x40000000 else data)


def parse_arc(data):
    if data[:4] != b"ARC\0" or struct.unpack_from("<H", data, 4)[0] != 17:
        raise ValueError("unsupported ARC version")
    count, = struct.unpack_from("<H", data, 6)
    entries = []
    for i in range(count):
        pos = 12 + i*80
        name = data[pos:pos+64]
        kind, size, flags, offset = struct.unpack_from("<4I", data, pos+64)
        if len(name) != 64 or offset < 12+80*count or offset+size > len(data):
            raise ValueError("invalid ARC entry range")
        entries.append(Entry(name, kind, flags, data[offset:offset+size]))
    return entries


def encode_arc(entries, reserved=b"\0"*4):
    if len(entries) > 65535:
        raise ValueError("too many ARC entries")
    start = (12 + len(entries)*80 + 0x7fff) & ~0x7fff
    header = bytearray(struct.pack("<4sHH", b"ARC\0", 17, len(entries)) + reserved)
    payload = bytearray()
    for entry in entries:
        header.extend(entry.name)
        header.extend(struct.pack("<4I", entry.type, len(entry.payload), entry.flags, start+len(payload)))
        payload.extend(entry.payload)
    header.extend(bytes(start-len(header)))
    return bytes(header+payload)


def entry_name(entry):
    return entry.name.split(b"\0")[0].decode("ascii")


def load_unifont(path):
    result = {}
    with gzip.open(path, "rt", encoding="ascii") as source:
        for line in source:
            cp, value = line.strip().split(":")
            raw = bytes.fromhex(value)
            width = len(raw)//2
            if width not in (8, 16):
                raise ValueError("unsupported Unifont glyph width")
            pixels = bytes(255 if raw[y*(width//8)+x//8] & (128 >> (x%8)) else 0
                           for y in range(16) for x in range(width))
            result[int(cp, 16)] = Image.frombytes("L", (width, 16), pixels)
    return result


def make_tex(template, mask, code=0x2c):
    width, height = mask.size
    header = bytearray(template[:24])
    struct.pack_into("<I", header, 8, 1 | (width << 6) | (height << 19))
    header[13] = code
    header[14] = 1
    stored = tf.level_size(width,height,tf.FORMATS[code],tf.default_block_height_gobs(height))
    struct.pack_into("<I", header, 16, stored)
    data = bytes(header) + bytes(stored)
    layout = tf.compute_layout(tf.parse_tex_header(data), require_decoder=True)
    result = bytes(header) + tf.retile_level(mask.tobytes(), layout.levels[0], tf.FORMATS[code])
    if tf.untile_level(result[24:], layout.levels[0], tf.FORMATS[code]) != mask.tobytes():
        raise ValueError("R8 tile roundtrip failed")
    return result


def alpha_page(data):
    header = tf.parse_tex_header(data)
    layout = tf.compute_layout(header, require_decoder=True)
    if not layout.layout_ok:
        raise ValueError("font TEX layout mismatch")
    if header.format_code != 7:
        return None, header
    level = layout.levels[0]
    linear = tf.untile_level(data[header.payload_offset:header.payload_offset+level.padded_size], level, header.format)
    image = Image.frombytes("RGBA", (header.width, header.height), linear)
    if any(channel.getextrema() != (255, 255) for channel in image.split()[:3]):
        return None, header
    return image.getchannel("A"), header


def convert_family(gfd, pages, font):
    masks = []
    old_decoded = old_payload = 0
    for data in pages:
        mask, header = alpha_page(data)
        if mask is None:
            return None
        masks.append(mask)
        old_decoded += header.width*header.height*4
        old_payload += header.payload_size
    atlas = Image.new("L", (SIZE, SIZE))
    atlases = [atlas]
    glyphs = []
    x = y = row_height = 1
    zero_area = []
    examples = []
    for original in gfd.glyphs:
        cp, coords, rect, metrics, bearings = original
        ow, oh = rect & 4095, (rect >> 12) & 4095
        width, height = metrics & 4095, (metrics >> 12) & 4095
        if not ow or not oh or not width or not height:
            glyphs.append(original)
            zero_area.append(cp)
            continue
        if cp not in font:
            raise ValueError(f"missing Unifont glyph U+{cp:04X}")
        if width+2 > SIZE or height+2 > SIZE:
            raise ValueError(f"oversized glyph U+{cp:04X}: {width}x{height}")
        if x+width+1 > SIZE:
            x, y, row_height = 1, y+row_height+1, 1
        if y+height+1 > SIZE:
            atlas = Image.new("L", (SIZE, SIZE))
            atlases.append(atlas)
            x = y = row_height = 1
        native = (max(1, (3*width+2)//4), max(1, (3*height+2)//4))
        glyph = font[cp].resize(native, Image.Resampling.NEAREST).resize((width, height), Image.Resampling.NEAREST)
        atlas.paste(glyph, (x, y))
        glyphs.append((cp, len(atlases)-1 | (x << 8) | (y << 20),
                       (rect & 0xff000000) | width | (height << 12), metrics, bearings & 0xffff0000))
        if cp in {65, 97, 0x4e00, 0x4e2d, 0x76ee, 0x56fd, 0x6587, 0x72e9, 0x730e, 0x6751, 0x957f, 0x3002}:
            page, ox, oy = coords & 255, (coords >> 8) & 4095, coords >> 20
            before = Image.new("L", (width, height))
            bx, by = bearings & 255, (bearings >> 8) & 255
            bx = bx-256 if bx >= 128 else bx
            by = by-256 if by >= 128 else by
            before.paste(masks[page].crop((ox, oy, ox+ow, oy+oh)), (bx, by))
            examples.append((cp, before, glyph))
        x += width+1
        row_height = max(row_height, height)
    new_pages = [make_tex(pages[0], mask) for mask in atlases]
    new_gfd = gfd.encode(glyphs, len(new_pages))
    # Only UV rectangles, bearings and page count may change.
    parsed = Gfd.parse(new_gfd)
    for old, new in zip(gfd.glyphs, parsed.glyphs):
        if old[0] != new[0] or old[3] != new[3] or old[4] >> 16 != new[4] >> 16:
            raise ValueError("font layout metrics changed")
    summary = dict(prefix=gfd.prefix, glyphs=len(glyphs), old_pages=len(pages), new_pages=len(new_pages),
                   old_payload_bytes=old_payload, new_payload_bytes=len(new_pages)*SIZE*SIZE,
                   old_decoded_bytes=old_decoded, new_decoded_bytes=len(new_pages)*SIZE*SIZE,
                   preserved_zero_area_codepoints=sorted(set(zero_area)))
    summary.update(source_gfd_sha256=sha(gfd.encode(gfd.glyphs,gfd.pages)),
                   source_tex_sha256=[sha(page) for page in pages],
                   output_gfd_sha256=sha(new_gfd),output_tex_sha256=[sha(page) for page in new_pages])
    return new_gfd, new_pages, summary, examples


def page_entries(entries, gfd):
    prefix = re.escape(gfd.prefix)
    matched = {}
    suffixes = set()
    for i, entry in enumerate(entries):
        match = re.fullmatch(prefix+r"_(\d+)(_HQ|_ID)", entry_name(entry))
        if match and entry.type == TEX_DTI:
            page = int(match[1])
            if page in matched:
                raise ValueError("duplicate atlas page")
            matched[page] = i
            suffixes.add(match[2])
    if len(suffixes) != 1 or sorted(matched) != list(range(gfd.pages)):
        raise ValueError(f"missing/ambiguous atlas pages for {gfd.prefix}")
    return [matched[i] for i in range(gfd.pages)], suffixes.pop()


def build(tree, font_path, output):
    if sha((tree/"nativeNX/loc/arc/resident.arc").read_bytes()) != RESIDENT_SHA:
        raise ValueError("resident.arc input identity mismatch")
    if sha(font_path.read_bytes()) != UNIFONT_SHA:
        raise ValueError("Unifont input identity mismatch")
    for name in ["OFL-1.1.txt","COPYRIGHT.txt"]:
        if not (font_path.parent/name).is_file():
            raise ValueError(f"font attribution missing: {name}; run fetch_pixel_font.py")
    if output.exists():
        raise ValueError("output already exists; use a fresh output directory")
    font = load_unifont(font_path)
    cache, loose_map, families, skipped, changes = {}, {}, [], [], []
    previews = []
    counts = dict(arcs_scanned=0, embedded_gfd=0, loose_gfd=0, converted_embedded_gfd=0)
    output.mkdir(parents=True)

    def write(path, old, new):
        rel = path.relative_to(tree)
        target = output/"overlay"/rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(new)
        changes.append(dict(path=rel.as_posix(), input_sha256=sha(old), output_sha256=sha(new),
                            input_bytes=len(old), output_bytes=len(new)))

    for path in sorted(tree.rglob("*.arc")):
        counts["arcs_scanned"] += 1
        with path.open("rb") as stream:
            head = stream.read(12)
            if head[:4] != b"ARC\0":
                raise ValueError(f"unexpected archive magic: {path}")
            count = struct.unpack_from("<H", head, 6)[0]
            table = stream.read(count*80)
        if not any(struct.unpack_from("<I", table, i*80+64)[0] == GFD_DTI for i in range(count)):
            continue
        original = path.read_bytes()
        entries = parse_arc(original)
        replacement, remove, additions = {}, set(), {}
        for i, entry in enumerate(entries):
            if entry.type != GFD_DTI:
                continue
            raw = entry.raw()
            gfd = Gfd.parse(raw)
            counts["embedded_gfd"] += 1
            indices, suffix = page_entries(entries, gfd)
            pages = [entries[j].raw() for j in indices]
            key = (sha(raw), tuple(sha(page) for page in pages))
            if key not in cache:
                cache[key] = convert_family(gfd, pages, font)
                if cache[key]:
                    families.append(cache[key][2])
                    if "font_festa" in gfd.prefix:
                        previews.extend(cache[key][3])
            converted = cache[key]
            if converted is None:
                skipped.append(dict(path=path.relative_to(tree).as_posix(), entry=entry_name(entry), reason="color-or-non-RGBA-text-atlas"))
                continue
            new_gfd, new_pages, _, _ = converted
            if sha(raw) in loose_map and loose_map[sha(raw)] != new_gfd:
                raise ValueError("ambiguous loose GFD version")
            loose_map[sha(raw)] = new_gfd
            counts["converted_embedded_gfd"] += 1
            replacement[i] = entry.replace(new_gfd)
            for page, idx in enumerate(indices):
                if page < len(new_pages):
                    replacement[idx] = entries[idx].replace(new_pages[page])
                else:
                    remove.add(idx)
            extra = []
            for page in range(len(indices), len(new_pages)):
                name = (gfd.prefix+f"_{page:02d}"+suffix).encode("ascii")
                if len(name) >= 64:
                    raise ValueError("atlas entry name too long")
                extra.append(Entry(name.ljust(64, b"\0"), TEX_DTI, entries[indices[0]].flags, b"").replace(new_pages[page]))
            additions[indices[-1]] = extra
        if replacement:
            changed = []
            for i, entry in enumerate(entries):
                if i not in remove:
                    changed.append(replacement.get(i, entry))
                changed.extend(additions.get(i, []))
            encoded = encode_arc(changed, original[8:12])
            decoded = parse_arc(encoded)
            if [(e.name, e.type, e.flags, e.payload) for e in decoded] != [(e.name, e.type, e.flags, e.payload) for e in changed]:
                raise ValueError("ARC rebuild verification failed")
            write(path, original, encoded)
    for path in sorted(tree.rglob("*.gfd")):
        raw = path.read_bytes()
        Gfd.parse(raw)
        counts["loose_gfd"] += 1
        if sha(raw) not in loose_map:
            raise ValueError(f"loose GFD has no unique converted embedded counterpart: {path}")
        write(path, raw, loose_map[sha(raw)])
    system_fonts = []
    for name, cell in SYSTEM_FONTS:
        path = tree/"nativeNX/system/texture"/(name+".tex")
        original = path.read_bytes()
        header = tf.parse_tex_header(original)
        layout = tf.compute_layout(header,require_decoder=True)
        if header.format_code!=0x19 or not layout.layout_ok or header.mip_count!=1:
            raise ValueError(f"unreviewed system font layout: {path}")
        level = layout.levels[0]
        linear = tf.untile_level(original[header.payload_offset:],level,header.format)
        rgba = tf.decode_level_rgba(linear,level,header.format)
        # The proven fixed grids start at ASCII space. Preserve the additional
        # game symbols outside U+0020..U+007E as binary masks, without guessing
        # their Unicode meaning or replacing them with arbitrary glyphs.
        mask = Image.frombytes("RGBA",(header.width,header.height),rgba).getchannel("R").point(lambda v:255 if v>=128 else 0)
        columns = header.width//cell[0]
        for cp in range(32,127):
            glyph = font[cp].resize((max(1,(3*cell[0]+2)//4),max(1,(3*cell[1]+2)//4)),Image.Resampling.NEAREST).resize(cell,Image.Resampling.NEAREST)
            index = cp-32
            mask.paste(glyph,((index%columns)*cell[0],(index//columns)*cell[1]))
        converted = make_tex(original,mask,code=0x12)
        write(path,original,converted)
        system_fonts.append(dict(path=path.relative_to(tree).as_posix(),cell=list(cell),ascii_first=32,ascii_last=126,
                                 native_format=0x12,decoded_bytes=header.width*header.height,
                                 preserved_custom_slots=True))
    # Non-GFD icon resources are explicit exclusions, never alpha-only guesses.
    for path in sorted(tree.rglob("*")):
        if (path.is_file() and "font" in path.name.lower() and path.suffix.lower() not in {".arc", ".gfd"}
                and path.relative_to(tree).as_posix() not in {item["path"] for item in system_fonts}):
            skipped.append(dict(path=path.relative_to(tree).as_posix(), reason="unmapped-resource-review-required"))
    manifest = dict(version=1, scan_complete=True, counts=counts, families=families, system_fonts=system_fonts, changes=changes, excluded=skipped,
                    unifont=dict(version="18.0.01", sha256=sha(font_path.read_bytes()),
                                 source="https://unifoundry.com/pub/unifont/unifont-18.0.01/font-builds/unifont-18.0.01.hex.gz",
                                 license="OFL-1.1",derivative_name="MHGU Vita Pixel"),
                    native_scale=[3, 4], preserve_metrics=True)
    (output/"manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    license_path = font_path.parent/"OFL-1.1.txt"
    (output/"OFL-1.1.txt").write_bytes(license_path.read_bytes())
    for name in ["COPYING","README","COPYRIGHT.txt"]:
        source = font_path.parent/name
        if source.exists():
            (output/("Unifont-"+name)).write_bytes(source.read_bytes())
    if previews:
        preview = Image.new("RGB", (720, 70*len(previews)+35), "#303030")
        draw = ImageDraw.Draw(preview)
        draw.text((10, 8), "Original / Native-grid Unifont (same logical cell)", fill="white")
        for i, (cp, before, after) in enumerate(previews):
            top = 35+i*70
            draw.text((10, top), f"U+{cp:04X}", fill="white")
            for x, mask in [(140, before), (360, after)]:
                enlarged = mask.resize((mask.width*2, mask.height*2), Image.Resampling.NEAREST)
                preview.paste("white", (x, top, x+enlarged.width, top+enlarged.height), enlarged)
        preview.save(output/"preview.png")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", required=True, type=Path)
    parser.add_argument("--unifont", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = build(args.tree.resolve(), args.unifont.resolve(), args.output.resolve())
    print(json.dumps(result["counts"], ensure_ascii=False))


if __name__ == "__main__":
    main()

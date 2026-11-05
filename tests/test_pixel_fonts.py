import gzip
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/"tools/assets"))
sys.path.insert(0, str(ROOT/"tools/romfs"))
import build_pixel_fonts as pf
import build_romfs_index as romfs
import tex_format as tf


def gfd_bytes(glyphs=None, pages=1, secondary=1):
    header = bytearray(56)
    prefix = b"GUI\\02_common\\font\\font_festa"
    glyph = (65, 0, 8 | (16 << 12) | (0x41 << 24), 24 | (24 << 12) | (0x14 << 24), 0xabcd0201)
    if glyphs is None:
        glyphs = [glyph]*14
    struct.pack_into("<10I", header, 0, 0x444647, 0x10f06, 0, 2, 2, 24, pages, len(glyphs)-secondary, secondary, 2)
    header.extend(struct.pack("<2fI", 3.0, 2.375, len(prefix))+prefix+b"\0")
    return bytes(header)+b"".join(struct.pack("<5I",*row) for row in glyphs)


def rgba_tex(size=64):
    header = bytearray(24)
    header[:4] = b"TEX\0"
    struct.pack_into("<I", header, 8, 1 | (size << 6) | (size << 19))
    header[13], header[14] = 7, 1
    stored = tf.level_size(size, size, tf.FORMATS[7], tf.default_block_height_gobs(size))
    struct.pack_into("<I", header, 16, stored)
    return bytes(header)+b"\xff"*stored


class FontTests(unittest.TestCase):
    def test_zero_area_private_slots_remain_empty(self):
        glyph = (0xe810, 0, 0, 6, 0xffff0000)
        parsed = pf.Gfd.parse(gfd_bytes([glyph],secondary=0))
        with patch.object(pf,"SIZE",64):
            result, pages, summary, _ = pf.convert_family(parsed,[rgba_tex()],{})
        self.assertEqual(pf.Gfd.parse(result).glyphs,[glyph])
        self.assertEqual(summary["preserved_zero_area_codepoints"],[0xe810])
        self.assertEqual(len(pages),1)

    def test_missing_printable_glyph_is_explicit_failure(self):
        with self.assertRaisesRegex(ValueError,"missing Unifont glyph U\\+0041"):
            pf.convert_family(pf.Gfd.parse(gfd_bytes()),[rgba_tex()],{})

    def test_r8_canonical_and_tiling(self):
        with patch.object(pf, "SIZE", 64):
            mask = pf.Image.frombytes("L", (64, 64), bytes([0, 255])*2048)
            data = pf.make_tex(rgba_tex(), mask)
        header = tf.parse_tex_header(data)
        layout = tf.compute_layout(header)
        self.assertTrue(layout.layout_ok)
        linear = tf.untile_level(data[24:], layout.levels[0], tf.FORMATS[0x12])
        self.assertEqual(linear, mask.tobytes())
        rgba = tf.decode_level_rgba(linear, layout.levels[0], tf.FORMATS[0x12])
        self.assertEqual(rgba[:8], b"\0\0\0\xff\xff\0\0\xff")

    def test_gfd_preserves_unknown_header_secondary_and_order(self):
        original = gfd_bytes()
        parsed = pf.Gfd.parse(original)
        self.assertEqual(parsed.encode(parsed.glyphs, parsed.pages), original)
        result = parsed.encode(parsed.glyphs, 3)
        self.assertEqual(result[:24], original[:24])
        self.assertEqual(result[28:], original[28:])
        self.assertEqual(len(parsed.glyphs), 14)
        with self.assertRaises(ValueError):
            pf.Gfd.parse(original[:-1])

    def test_full_build_arc_payload_and_layout(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(pf, "SIZE", 64), patch.object(pf,"SYSTEM_FONTS",[]):
            root = Path(directory)
            tree = root/"tree"
            resident = tree/"nativeNX/loc/arc/resident.arc"
            resident.parent.mkdir(parents=True)
            unrelated = pf.Entry(b"unrelated".ljust(64, b"\0"), 123, 0x40000000, b"").replace(b"original-data"*77)
            atlas = pf.Entry(b"GUI\\02_common\\font\\font_festa_00_HQ".ljust(64, b"\0"), pf.TEX_DTI, 0x40000000, b"").replace(rgba_tex())
            gfd = pf.Entry(b"GUI\\02_common\\font\\font_festa".ljust(64, b"\0"), pf.GFD_DTI, 0x40000000, b"").replace(gfd_bytes())
            source = pf.encode_arc([unrelated, atlas, gfd], b"ABCD")
            resident.write_bytes(source)
            (tree/"font.gfd").write_bytes(gfd_bytes())
            font = root/"unifont.hex.gz"
            with gzip.open(font, "wt") as stream:
                stream.write("0041:"+"18"*16+"\n")
            (root/"OFL-1.1.txt").write_text("synthetic license fixture")
            (root/"COPYRIGHT.txt").write_text("synthetic copyright fixture")
            with patch.object(pf, "RESIDENT_SHA", pf.sha(source)), patch.object(pf,"UNIFONT_SHA",pf.sha(font.read_bytes())):
                manifest = pf.build(tree, font, root/"result")
            self.assertEqual(resident.read_bytes(), source)
            self.assertEqual(manifest["counts"]["converted_embedded_gfd"], 1)
            entries = pf.parse_arc((root/"result/overlay/nativeNX/loc/arc/resident.arc").read_bytes())
            self.assertEqual(entries[0], unrelated)
            self.assertEqual(len(entries), 6)  # Four pages, GFD, unrelated payload.
            parsed = pf.Gfd.parse(entries[-1].raw())
            self.assertEqual(parsed.pages, 4)
            for old, new in zip(pf.Gfd.parse(gfd_bytes()).glyphs, parsed.glyphs):
                self.assertEqual(old[0], new[0])
                self.assertEqual(old[3], new[3])
                self.assertEqual(old[2] >> 24, new[2] >> 24)
                self.assertEqual(old[4] >> 16, new[4] >> 16)
                self.assertEqual(new[4] & 65535, 0)
            # The virtual name and metadata use replacement sizes while physical
            # paths point into a separate pack; unchanged content stays in base.
            index = root/"index"
            index.mkdir()
            romfs.build(tree, index, 256, overlay=root/"result/overlay",
                        base_prefix=b"romfs-tree/", overlay_prefix=b"font-pixel/overlay/")
            mapping = (index/"romfs.map").read_bytes()
            self.assertIn(b"font-pixel/overlay/nativeNX/loc/arc/resident.arc\0", mapping)
            self.assertEqual(json.loads((root/"result/manifest.json").read_text())["scan_complete"], True)
            # More original pages than the new shelf packing needs: remove
            # only old atlas entries, preserving unrelated compressed bytes.
            extra = []
            for page in range(8):
                name = f"GUI\\02_common\\font\\font_festa_{page:02d}_HQ".encode().ljust(64,b"\0")
                extra.append(pf.Entry(name,atlas.type,atlas.flags,atlas.payload))
            many_gfd = gfd_bytes(pages=8)
            many_source = pf.encode_arc([unrelated]+extra+[gfd.replace(many_gfd)])
            resident.write_bytes(many_source)
            (tree/"font.gfd").write_bytes(many_gfd)
            with patch.object(pf,"RESIDENT_SHA",pf.sha(many_source)), patch.object(pf,"UNIFONT_SHA",pf.sha(font.read_bytes())):
                pf.build(tree,font,root/"removed")
            removed = pf.parse_arc((root/"removed/overlay/nativeNX/loc/arc/resident.arc").read_bytes())
            self.assertEqual(len(removed),6)
            self.assertEqual(removed[0],unrelated)


if __name__ == "__main__":
    unittest.main()

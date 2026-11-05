import gzip
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/assets"))
import native_font_strikes as nfs


def bdf_glyph(encoding, width, height, xoff, yoff, rows,
              advance=None, name="glyph"):
    if advance is None:
        advance = width
    return "\n".join([
        f"STARTCHAR {name}",
        f"ENCODING {encoding}",
        f"DWIDTH {advance} 0",
        f"BBX {width} {height} {xoff} {yoff}",
        "BITMAP",
        *rows,
        "ENDCHAR",
    ])


def bdf_text(glyphs, ascent=12, descent=4, pixel_size=None,
             version="2.1", chars=None):
    props = [f"FONT_ASCENT {ascent}", f"FONT_DESCENT {descent}"]
    if pixel_size is not None:
        props.append(f"PIXEL_SIZE {pixel_size}")
    count = len(glyphs) if chars is None else chars
    return "\n".join([
        f"STARTFONT {version}",
        "FONT -mhgu-test-Medium-R-Normal--16-160-75-75-C-80-ISO10646-1",
        "SIZE 16 75 75",
        "FONTBOUNDINGBOX 16 16 0 -4",
        f"STARTPROPERTIES {len(props)}",
        *props,
        "ENDPROPERTIES",
        f"CHARS {count}",
        *glyphs,
        "ENDFONT",
        "",
    ])


BDF_A = bdf_glyph(65, 8, 10, 0, -2,
                  ["18", "24", "42", "81", "FF",
                   "81", "81", "81", "00", "00"], name="A")
BDF_NINE = bdf_glyph(66, 9, 2, -1, -3, ["FFFF", "8080"], name="nine")
BDF_SPACE = bdf_glyph(32, 0, 0, 0, 0, [], advance=4, name="space")


class BdfTests(unittest.TestCase):
    def test_bitmap_bytes_padding_and_baseline(self):
        strike = nfs.parse_bdf(bdf_text([BDF_A, BDF_NINE, BDF_SPACE]), "a" * 64)
        self.assertEqual(strike.pixel_height, 16)  # ascent + descent fallback
        self.assertEqual(set(strike.glyphs), {32, 65, 66})

        nine = strike.glyphs[66]
        self.assertEqual((nine.width, nine.height, nine.advance), (9, 2, 9))
        self.assertEqual(nine.bearing_x, -1)
        self.assertEqual(nine.bearing_y, 12 - (-3) - 2)
        # Row padding bits beyond the declared 9 pixels are discarded.
        self.assertEqual(nine.pixels,
                         bytes([255] * 9 + [255] + [0] * 7 + [255]))

        letter = strike.glyphs[65]
        self.assertEqual((letter.width, letter.height), (8, 10))
        self.assertEqual(letter.bearing_y, 12 - (-2) - 10)
        self.assertEqual(len(letter.pixels), 80)
        self.assertEqual(letter.pixels[0 * 8 + 3], 255)
        self.assertEqual(letter.pixels[0 * 8 + 4], 255)
        self.assertEqual(letter.pixels[0 * 8 + 2], 0)

        space = strike.glyphs[32]
        self.assertEqual((space.width, space.height, space.pixels), (0, 0, b""))
        self.assertEqual(space.advance, 4)
        self.assertEqual((space.bearing_x, space.bearing_y), (0, 12))

    def test_pixel_size_and_bdf_22(self):
        strike = nfs.parse_bdf(
            bdf_text([BDF_A], pixel_size=20, version="2.2"), "b" * 64)
        self.assertEqual(strike.pixel_height, 20)
        self.assertEqual(strike.glyphs[65].bearing_y, 4)

    def test_duplicate_encoding_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate codepoint U\\+0041"):
            nfs.parse_bdf(bdf_text([BDF_A, BDF_A]), "c" * 64)

    def test_bad_bitmap_width_is_rejected(self):
        broken = bdf_glyph(65, 9, 1, 0, 0, ["FF"])
        with self.assertRaisesRegex(ValueError, "bitmap row width mismatch"):
            nfs.parse_bdf(bdf_text([broken]), "d" * 64)

    def test_chars_count_mismatch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "CHARS 2 != parsed 1"):
            nfs.parse_bdf(bdf_text([BDF_A], chars=2), "e" * 64)

    def test_non_bdf_suffix_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "font.ttf"
            path.write_bytes(b"")
            with self.assertRaisesRegex(ValueError, "unsupported font suffix"):
                nfs.load_font(path)


class UnifontTests(unittest.TestCase):
    def test_hex_8x16_and_16x16_exact_bytes(self):
        text = "0041:" + "80" * 16 + "\n" + "0042:" + "8001" * 16 + "\n"
        strike = nfs.parse_unifont(text, "f" * 64)
        self.assertEqual(strike.pixel_height, 16)

        narrow = strike.glyphs[0x41]
        self.assertEqual(
            (narrow.width, narrow.height, narrow.advance,
             narrow.bearing_x, narrow.bearing_y),
            (8, 16, 8, 0, 0))
        self.assertEqual(narrow.pixels,
                         bytes(255 if x == 0 else 0
                               for _ in range(16) for x in range(8)))

        wide = strike.glyphs[0x42]
        self.assertEqual((wide.width, wide.height, wide.advance), (16, 16, 16))
        self.assertEqual(wide.pixels,
                         bytes(255 if x in (0, 15) else 0
                               for _ in range(16) for x in range(16)))
        self.assertEqual(len(wide.pixels), 256)

    def test_hex_gz_loader_hashes_on_disk_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "font.hex.gz"
            with gzip.open(path, "wt", encoding="ascii") as stream:
                stream.write("0041:" + "FF" * 16 + "\n")
            strike = nfs.load_font(path)
            self.assertEqual(strike.glyphs[0x41].pixels, b"\xff" * 128)
            self.assertEqual(strike.source_sha256,
                             hashlib.sha256(path.read_bytes()).hexdigest())

    def test_hex_loader_hashes_on_disk_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "font.hex"
            path.write_text("0041:" + "80" * 16 + "\n", encoding="ascii")
            strike = nfs.load_font(path)
            self.assertEqual(strike.glyphs[0x41].width, 8)
            self.assertEqual(strike.source_sha256,
                             hashlib.sha256(path.read_bytes()).hexdigest())

    def test_bad_hex_is_explicit(self):
        with self.assertRaisesRegex(ValueError, "unsupported bitmap length"):
            nfs.parse_unifont("0041:FF\n", "g" * 64)
        with self.assertRaisesRegex(ValueError, "duplicate codepoint U\\+0041"):
            nfs.parse_unifont(
                "0041:" + "00" * 16 + "\n0041:" + "00" * 16 + "\n",
                "g" * 64)


def native(codepoint, width, height, value=255, advance=None):
    return nfs.NativeGlyph(
        codepoint, width, height, width if advance is None else advance,
        0, 0, bytes([value]) * (width * height))


class PackTests(unittest.TestCase):
    def test_optional_strike_omits_missing_records(self):
        strike = nfs.FontStrike(12, {65: native(65, 3, 5)}, "partial", 10)
        atlas = nfs.pack_strikes([strike], [65, 66], page_size=16, allow_missing=True)
        self.assertEqual([glyph.codepoint for glyph in atlas.glyphs], [65])
        self.assertEqual(nfs.coverage([65, 66], [strike]), {12: {66}})

    def test_empty_glyph_keeps_advance_and_draws_nothing(self):
        glyph = nfs.NativeGlyph(32, 0, 0, 4, 1, 12, b"")
        strike = nfs.FontStrike(16, {32: glyph}, "1" * 64)
        atlas = nfs.pack_strikes([strike], [32], page_size=8)
        self.assertEqual(len(atlas.pages), 1)
        self.assertEqual(atlas.pages[0], bytes(64))
        packed = atlas.glyphs[0]
        self.assertEqual(
            (packed.codepoint, packed.width, packed.height, packed.advance,
             packed.page, packed.x, packed.y),
            (32, 0, 0, 4, 0, 1, 1))

    def test_missing_glyph_is_explicit_failure(self):
        strike = nfs.FontStrike(16, {65: native(65, 8, 8)}, "2" * 64)
        self.assertEqual(nfs.coverage([65, 66], [strike]), {16: {66}})
        with self.assertRaisesRegex(
                ValueError, r"missing glyphs: 16px: U\+0042"):
            nfs.pack_strikes([strike], [65, 66], page_size=32)

    def test_duplicate_strike_height_is_rejected(self):
        strike = nfs.FontStrike(16, {65: native(65, 8, 8)}, "3" * 64)
        with self.assertRaisesRegex(ValueError, "duplicate strike pixel height"):
            nfs.coverage([65], [strike, strike])
        with self.assertRaisesRegex(ValueError, "duplicate strike pixel height"):
            nfs.pack_strikes([strike, strike], [65], page_size=32)

    def test_no_strikes_is_explicit_failure(self):
        self.assertEqual(nfs.coverage([65], []), {})
        with self.assertRaisesRegex(ValueError, "no strikes supplied"):
            nfs.pack_strikes([], [65], page_size=32)

    def test_shelf_boundaries_and_multiple_pages(self):
        codepoints = [1, 2, 3, 4, 5]
        strike = nfs.FontStrike(
            16, {cp: native(cp, 6, 6) for cp in codepoints}, "4" * 64)
        atlas = nfs.pack_strikes([strike], codepoints, page_size=16)
        self.assertEqual(len(atlas.pages), 2)
        self.assertEqual(
            [(glyph.page, glyph.x, glyph.y) for glyph in atlas.glyphs],
            [(0, 1, 1), (0, 8, 1), (0, 1, 8), (0, 8, 8), (1, 1, 1)])
        for page in atlas.pages:
            self.assertEqual(len(page), 256)
            self.assertEqual(page[:16], bytes(16))
            self.assertEqual(page[-16:], bytes(16))
            for row in range(16):
                self.assertEqual(page[row * 16], 0)
                self.assertEqual(page[row * 16 + 15], 0)
        # One transparent column separates the two glyphs on the first shelf.
        self.assertEqual(atlas.pages[0][1 * 16 + 7], 0)
        for glyph in atlas.glyphs:
            page = atlas.pages[glyph.page]
            for row in range(glyph.height):
                start = (glyph.y + row) * 16 + glyph.x
                self.assertEqual(page[start:start + glyph.width],
                                 bytes([255]) * glyph.width)

    def test_page_index_limit_is_255(self):
        codepoints = list(range(256))
        strike = nfs.FontStrike(
            16, {cp: native(cp, 6, 6, 0) for cp in codepoints}, "5" * 64)
        atlas = nfs.pack_strikes([strike], codepoints, page_size=8)
        self.assertEqual(len(atlas.pages), 256)
        self.assertEqual(atlas.glyphs[-1].page, 255)

        codepoints.append(256)
        strike = nfs.FontStrike(
            16, {cp: native(cp, 6, 6, 0) for cp in codepoints}, "5" * 64)
        with self.assertRaisesRegex(ValueError, "exceed 255"):
            nfs.pack_strikes([strike], codepoints, page_size=8)

    def test_packing_is_deterministic(self):
        low = nfs.FontStrike(
            10, {1: native(1, 4, 4), 2: native(2, 4, 4)}, "6" * 64)
        high = nfs.FontStrike(
            20, {1: native(1, 8, 8), 2: native(2, 8, 8)}, "7" * 64)
        first = nfs.pack_strikes([high, low], [2, 1], page_size=32)
        second = nfs.pack_strikes([low, high], [1, 2, 2], page_size=32)
        self.assertEqual(first.pages, second.pages)
        self.assertEqual(first.glyphs, second.glyphs)
        self.assertEqual([glyph.strike_pixel_height for glyph in first.glyphs],
                         [10, 10, 20, 20])
        self.assertEqual([glyph.codepoint for glyph in first.glyphs],
                         [1, 2, 1, 2])


class BuildTests(unittest.TestCase):
    def test_build_writes_pages_manifest_and_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            font = root / "test.bdf"
            font.write_text(bdf_text([BDF_A, BDF_SPACE]), encoding="ascii")
            codepoints = root / "codepoints.json"
            codepoints.write_text(json.dumps([32, 65]), encoding="ascii")
            output = root / "out"

            manifest = nfs.build([font], codepoints, output)
            self.assertTrue((output / "page-000.alpha8").is_file())
            manifest_on_disk = json.loads(
                (output / "native-font.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest_on_disk, manifest)
            report = json.loads(
                (output / "coverage.json").read_text(encoding="utf-8"))
            self.assertTrue(report["complete"])
            self.assertEqual(manifest["fonts"][0]["sha256"],
                             hashlib.sha256(font.read_bytes()).hexdigest())

            page = (output / "page-000.alpha8").read_bytes()
            self.assertEqual(len(page), 1024 * 1024)
            self.assertEqual(page[0], 0)  # transparent page border
            self.assertEqual([glyph["codepoint"] for glyph in manifest["glyphs"]],
                             [32, 65])
            self.assertEqual(manifest["glyphs"][0]["advance"], 4)
            self.assertEqual(manifest["glyphs"][1]["bearing_y"], 4)
            glyph = manifest["glyphs"][1]
            self.assertEqual((glyph["width"], glyph["height"]), (8, 10))
            start = glyph["y"] * 1024 + glyph["x"] + 3
            self.assertEqual(page[start], 255)
            self.assertEqual(page[start + 1], 255)

    def test_build_reports_missing_glyph_without_pages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            font = root / "test.bdf"
            font.write_text(bdf_text([BDF_A]), encoding="ascii")
            codepoints = root / "codepoints.json"
            codepoints.write_text(json.dumps([65, 66]), encoding="ascii")
            output = root / "out"
            with self.assertRaisesRegex(ValueError, r"U\+0042"):
                nfs.build([font], codepoints, output)
            report = json.loads(
                (output / "coverage.json").read_text(encoding="utf-8"))
            self.assertFalse(report["complete"])
            self.assertEqual(
                report["strikes"][0]["missing_codepoints"], [66])
            self.assertEqual(report["strikes"][0]["missing_hex"], ["U+0042"])
            self.assertEqual(report["fonts"][0]["sha256"],
                             hashlib.sha256(font.read_bytes()).hexdigest())
            self.assertFalse((output / "native-font.json").exists())
            self.assertEqual(list(output.glob("page-*.alpha8")), [])


if __name__ == "__main__":
    unittest.main()

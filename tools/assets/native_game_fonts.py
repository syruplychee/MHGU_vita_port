"""Native strike catalogue carried by the static GFD secondary-record array.

Primary layout metrics remain the original game's. The guarded quad emitters
consume the catalogue; stock glyph lookup never indexes these new records.
"""
import struct

from PIL import Image

from native_font_strikes import load_font, pack_strikes

MAGIC = 0x31465056  # VPF1, little endian.


class NativeGameFonts:
    def __init__(self, paths):
        self.strikes = sorted((load_font(path) for path in paths),
                              key=lambda font: font.pixel_height)
        heights = [font.pixel_height for font in self.strikes]
        if len(set(heights)) != len(heights) or 16 not in heights:
            raise ValueError("native fonts require unique strikes and complete 16px coverage")
        if any(not 0 < font.ascent < 128 for font in self.strikes):
            raise ValueError("native strike ascent must fit the GFD catalogue")

    def convert(self, gfd, pages, unused_font):
        # Import lazily: the existing packager owns TEX/ARC identity checks.
        from build_pixel_fonts import alpha_page, make_tex, sha
        old_decoded = old_payload = 0
        for page in pages:
            mask, header = alpha_page(page)
            if mask is None:
                return None
            old_decoded += header.width * header.height * 4
            old_payload += header.payload_size
        if struct.unpack_from("<I", gfd.header, 32)[0] != 0:
            raise ValueError("native catalogue requires the audited zero-secondary GFD")
        active = {g[0] for g in gfd.glyphs if (g[2] & 4095) and ((g[2] >> 12) & 4095)}
        complete = next(font for font in self.strikes if font.pixel_height == 16)
        if active - complete.glyphs.keys():
            raise ValueError("16px font does not cover every active game glyph")
        # Missing small strikes are omitted, never painted as boxes. The
        # complete 16px source above makes every active character drawable.
        atlas = pack_strikes(self.strikes, active, allow_missing=True)
        images = [Image.frombytes("L", (1024, 1024), page) for page in atlas.pages]
        placements = {(g.codepoint,g.strike_pixel_height):(g,g.page) for g in atlas.glyphs}
        missing = {str(f.pixel_height):sorted(active-f.glyphs.keys()) for f in self.strikes}
        if not 1 <= len(images) <= 255:
            raise ValueError("native font page count does not fit GFD")
        primary, variants = [], []
        for original in gfd.glyphs:
            cp = original[0]
            default = placements.get((cp, 16))
            coordinates = 0 if default is None else self._record(cp, complete, default)[1]
            primary.append((cp, coordinates, *original[2:]))
            for strike in self.strikes:
                placed = placements.get((cp, strike.pixel_height))
                variants.append(self._record(cp, strike, placed))
        secondary = [(MAGIC, 1, len(primary), len(self.strikes), 0)] + variants
        header = bytearray(gfd.header)
        struct.pack_into("<I", header, 24, len(images))
        struct.pack_into("<I", header, 32, len(secondary))
        data = bytes(header) + b"".join(struct.pack("<5I", *r) for r in primary + secondary)
        output_pages = [make_tex(pages[0], mask) for mask in images]
        summary = dict(prefix=gfd.prefix, glyphs=len(primary), old_pages=len(pages),
                       new_pages=len(images), old_payload_bytes=old_payload,
                       new_payload_bytes=len(images)*1024*1024, old_decoded_bytes=old_decoded,
                       new_decoded_bytes=len(images)*1024*1024,
                       native_strikes=[f.pixel_height for f in self.strikes],
                       native_missing_codepoints=missing,
                       source_gfd_sha256=sha(gfd.encode(gfd.glyphs,gfd.pages)),
                       source_tex_sha256=[sha(page) for page in pages],
                       output_gfd_sha256=sha(data),output_tex_sha256=[sha(page) for page in output_pages])
        return data, output_pages, summary, []

    @staticmethod
    def _record(cp, strike, placed):
        if placed is None:
            return cp, 0, 0, strike.pixel_height << 12, strike.ascent << 16
        glyph, page = placed
        if not (-128 <= glyph.bearing_x <= 127 and -128 <= glyph.bearing_y <= 127):
            raise ValueError("native bearing does not fit GFD")
        return (cp, page | glyph.x << 8 | glyph.y << 20,
                glyph.width | glyph.height << 12,
                glyph.advance | strike.pixel_height << 12,
                (glyph.bearing_x & 255) | (glyph.bearing_y & 255) << 8 | strike.ascent << 16)

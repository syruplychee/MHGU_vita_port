#!/usr/bin/env python3
"""Load native bitmap font strikes and pack them into unscaled Alpha8 pages.

This is an offline asset tool. It accepts GNU Unifont .hex/.hex.gz and BDF
2.1/2.2 files, keeps every bitmap at its authored size, and never rasterises
or resizes. It does not write game GFD/ARC data and it never downloads fonts.

Coordinates follow BDF/SFNT conventions where noted:

* NativeGlyph.bearing_x is the BBX x offset from the pen origin.
* NativeGlyph.bearing_y is measured downward from the top of the font cell:
  FONT_ASCENT - BBX yoff - height.
* FontStrike.pixel_height is the BDF PIXEL_SIZE, or FONT_ASCENT +
  FONT_DESCENT when PIXEL_SIZE is absent.

Pages are single-channel bytes; each byte is 0 or 255.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import gzip
import hashlib
import json
from pathlib import Path

MAX_PAGES = 256  # Valid page indices are 0..255.
_UNIFONT_WIDTH = {32: 8, 64: 16}


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True)
class NativeGlyph:
    """One bitmap glyph in font-cell coordinates.

    width and height are the original bitmap dimensions (BBX for BDF, the
    fixed cell for Unifont). pixels is row-major, exactly width * height
    bytes, each 0 or 255. Empty glyphs have no pixels but still carry their
    advance and bearings.
    """
    codepoint: int
    width: int
    height: int
    advance: int
    bearing_x: int
    bearing_y: int
    pixels: bytes

    def __post_init__(self) -> None:
        if isinstance(self.codepoint, bool) or not isinstance(self.codepoint, int):
            raise ValueError("codepoint must be an integer")
        if not 0 <= self.codepoint <= 0x10FFFF:
            raise ValueError(f"codepoint out of range: {self.codepoint}")
        if self.width < 0 or self.height < 0:
            raise ValueError(f"negative glyph size for U+{self.codepoint:04X}")
        if len(self.pixels) != self.width * self.height:
            raise ValueError(
                f"U+{self.codepoint:04X}: pixel count {len(self.pixels)} != "
                f"{self.width}x{self.height}")
        if any(value not in (0, 255) for value in self.pixels):
            raise ValueError(f"U+{self.codepoint:04X}: pixels must be 0 or 255")


@dataclass(frozen=True)
class FontStrike:
    """One font size: pixel_height, glyphs keyed by codepoint, source hash."""
    pixel_height: int
    glyphs: dict[int, NativeGlyph]
    source_sha256: str
    ascent: int = 0

    def __post_init__(self) -> None:
        if self.pixel_height <= 0:
            raise ValueError("strike pixel_height must be positive")
        if not self.source_sha256:
            raise ValueError("strike source_sha256 is required")
        for codepoint, glyph in self.glyphs.items():
            if codepoint != glyph.codepoint:
                raise ValueError("glyph key/codepoint mismatch")


def _read_text(path: Path) -> tuple[str, str]:
    """Return decoded ASCII text and the SHA256 of the on-disk bytes."""
    path = Path(path)
    raw = path.read_bytes()
    payload = gzip.decompress(raw) if path.name.lower().endswith(".gz") else raw
    try:
        text = payload.decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError(f"{path}: font file is not ASCII") from error
    return text, _sha256(raw)


def parse_unifont(text: str, source_sha256: str) -> FontStrike:
    """Parse GNU Unifont hex lines (CP:HEX), accepting only 8x16 and 16x16."""
    glyphs: dict[int, NativeGlyph] = {}
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if line.count(":") != 1:
            raise ValueError(f"Unifont line {number}: expected CP:HEX")
        cp_text, hex_text = line.split(":")
        try:
            codepoint = int(cp_text, 16)
        except ValueError as error:
            raise ValueError(f"Unifont line {number}: invalid codepoint") from error
        width = _UNIFONT_WIDTH.get(len(hex_text))
        if width is None:
            raise ValueError(
                f"Unifont line {number}: unsupported bitmap length {len(hex_text)}")
        try:
            raw = bytes.fromhex(hex_text)
        except ValueError as error:
            raise ValueError(f"Unifont line {number}: invalid hex") from error
        if len(raw) * 2 != len(hex_text):
            raise ValueError(f"Unifont line {number}: invalid hex spacing")
        if codepoint in glyphs:
            raise ValueError(
                f"Unifont line {number}: duplicate codepoint U+{codepoint:04X}")
        row_bytes = width // 8
        pixels = bytearray(width * 16)
        for y in range(16):
            row = raw[y * row_bytes:(y + 1) * row_bytes]
            for x in range(width):
                if row[x // 8] & (0x80 >> (x % 8)):
                    pixels[y * width + x] = 255
        glyphs[codepoint] = NativeGlyph(
            codepoint, width, 16, width, 0, 0, bytes(pixels))
    return FontStrike(16, glyphs, source_sha256, 14)


def load_unifont(path: Path) -> FontStrike:
    text, digest = _read_text(path)
    return parse_unifont(text, digest)


def _parse_bdf_glyph(lines: list[str], pos: int):
    """Parse one STARTCHAR body, returning a raw glyph spec and the new pos."""
    encoding = advance = bbx = None
    saw_bitmap = False
    while True:
        if pos >= len(lines):
            raise ValueError("BDF: unterminated STARTCHAR")
        line = lines[pos].strip()
        pos += 1
        if not line:
            continue
        parts = line.split()
        keyword = parts[0]
        if keyword == "ENCODING":
            if encoding is not None:
                raise ValueError("BDF: duplicate ENCODING")
            if len(parts) < 2:
                raise ValueError("BDF: ENCODING needs a codepoint")
            encoding = int(parts[1])
        elif keyword == "DWIDTH":
            if advance is not None:
                raise ValueError("BDF: duplicate DWIDTH")
            if len(parts) != 3:
                raise ValueError("BDF: DWIDTH needs two integers")
            advance = int(parts[1])
            int(parts[2])
        elif keyword == "BBX":
            if bbx is not None:
                raise ValueError("BDF: duplicate BBX")
            if len(parts) != 5:
                raise ValueError("BDF: BBX needs four integers")
            width, height, xoff, yoff = (int(value) for value in parts[1:])
            if width < 0 or height < 0:
                raise ValueError("BDF: negative BBX size")
            bbx = (width, height, xoff, yoff)
        elif keyword == "BITMAP":
            saw_bitmap = True
            break
        elif keyword == "ENDCHAR":
            break
    if encoding is None:
        raise ValueError("BDF: glyph missing ENCODING")
    if advance is None:
        raise ValueError("BDF: glyph missing DWIDTH")
    if bbx is None:
        raise ValueError("BDF: glyph missing BBX")
    width, height, xoff, yoff = bbx
    rows = []
    if saw_bitmap:
        bytes_per_row = (width + 7) // 8
        for _ in range(height):
            if pos >= len(lines):
                raise ValueError("BDF: truncated BITMAP")
            row = lines[pos].strip()
            pos += 1
            if len(row) != bytes_per_row * 2:
                raise ValueError("BDF: bitmap row width mismatch")
            try:
                data = bytes.fromhex(row)
            except ValueError as error:
                raise ValueError("BDF: invalid bitmap hex") from error
            # BDF rows are byte-padded; drop bits beyond the declared width.
            if width % 8 and data:
                data = data[:-1] + bytes([data[-1] & (0xFF << (8 - width % 8))])
            rows.append(data)
        if pos >= len(lines):
            raise ValueError("BDF: unterminated BITMAP")
        if lines[pos].strip() != "ENDCHAR":
            raise ValueError("BDF: expected ENDCHAR after BITMAP")
        pos += 1
    elif height != 0:
        raise ValueError("BDF: non-empty glyph without BITMAP")
    pixels = bytearray(width * height)
    for y, data in enumerate(rows):
        for x in range(width):
            if data[x // 8] & (0x80 >> (x % 8)):
                pixels[y * width + x] = 255
    return (encoding, advance, width, height, xoff, yoff, bytes(pixels)), pos


def parse_bdf(text: str, source_sha256: str) -> FontStrike:
    """Parse a BDF 2.1/2.2 font without rasterising or scaling anything.

    ENCODING -1 glyphs are validated but omitted (they have no Unicode
    codepoint). Duplicate encoded codepoints are rejected.
    """
    lines = text.splitlines()
    pos = 0
    while pos < len(lines) and not lines[pos].strip():
        pos += 1
    if pos >= len(lines):
        raise ValueError("BDF: empty file")
    header = lines[pos].split()
    pos += 1
    if len(header) != 2 or header[0] != "STARTFONT" or header[1] not in ("2.1", "2.2"):
        raise ValueError("BDF: expected STARTFONT 2.1 or 2.2")

    ascent = descent = pixel_size = declared_chars = None
    specs = []
    parsed_chars = 0
    ended = False

    def set_once(current, value, name):
        if current is not None and current != value:
            raise ValueError(f"BDF: conflicting {name}")
        return value

    while pos < len(lines):
        line = lines[pos].strip()
        pos += 1
        if not line:
            continue
        parts = line.split()
        keyword = parts[0]
        if keyword == "ENDFONT":
            ended = True
            break
        if keyword == "STARTCHAR":
            spec, pos = _parse_bdf_glyph(lines, pos)
            specs.append(spec)
            parsed_chars += 1
            continue
        if keyword in ("FONT_ASCENT", "FONT_DESCENT", "PIXEL_SIZE", "CHARS"):
            if len(parts) != 2:
                raise ValueError(f"BDF: {keyword} needs one integer")
            value = int(parts[1])
            if keyword == "FONT_ASCENT":
                ascent = set_once(ascent, value, keyword)
            elif keyword == "FONT_DESCENT":
                descent = set_once(descent, value, keyword)
            elif keyword == "PIXEL_SIZE":
                pixel_size = set_once(pixel_size, value, keyword)
            else:
                declared_chars = set_once(declared_chars, value, keyword)

    if not ended:
        raise ValueError("BDF: missing ENDFONT")
    if declared_chars is None:
        raise ValueError("BDF: missing CHARS")
    if declared_chars != parsed_chars:
        raise ValueError(f"BDF: CHARS {declared_chars} != parsed {parsed_chars}")
    if ascent is None:
        raise ValueError("BDF: missing FONT_ASCENT")
    if ascent < 0 or (descent is not None and descent < 0):
        raise ValueError("BDF: negative FONT_ASCENT/FONT_DESCENT")
    if pixel_size is None:
        if descent is None:
            raise ValueError("BDF: missing PIXEL_SIZE and FONT_DESCENT")
        pixel_size = ascent + descent
    if pixel_size <= 0:
        raise ValueError("BDF: non-positive pixel height")

    glyphs: dict[int, NativeGlyph] = {}
    for encoding, advance, width, height, xoff, yoff, pixels in specs:
        if encoding == -1:
            continue
        if not 0 <= encoding <= 0x10FFFF:
            raise ValueError(f"BDF: codepoint out of range: {encoding}")
        if encoding in glyphs:
            raise ValueError(f"BDF: duplicate codepoint U+{encoding:04X}")
        glyphs[encoding] = NativeGlyph(
            encoding, width, height, advance, xoff,
            ascent - yoff - height, pixels)
    return FontStrike(pixel_size, glyphs, source_sha256, ascent)


def load_bdf(path: Path) -> FontStrike:
    text, digest = _read_text(path)
    return parse_bdf(text, digest)


def load_font(path: Path) -> FontStrike:
    path = Path(path)
    name = path.name.lower()
    if name.endswith((".hex", ".hex.gz")):
        return load_unifont(path)
    if name.endswith((".bdf", ".bdf.gz")):
        return load_bdf(path)
    raise ValueError(f"{path}: unsupported font suffix (want .hex[.gz] or .bdf[.gz])")


@dataclass(frozen=True)
class PackedGlyph:
    """Placement metadata for one glyph in one strike."""
    codepoint: int
    strike_pixel_height: int
    page: int
    x: int
    y: int
    width: int
    height: int
    advance: int
    bearing_x: int
    bearing_y: int


@dataclass(frozen=True)
class PackedAtlas:
    """Alpha8 pages plus the deterministic glyph placement list."""
    page_size: int
    padding: int
    pages: tuple[bytes, ...]
    glyphs: tuple[PackedGlyph, ...]


def _normalise_codepoints(codepoints) -> list[int]:
    result = set()
    for codepoint in codepoints:
        if isinstance(codepoint, bool) or not isinstance(codepoint, int):
            raise ValueError(f"codepoint must be an integer: {codepoint!r}")
        if not 0 <= codepoint <= 0x10FFFF:
            raise ValueError(f"codepoint out of range: {codepoint}")
        result.add(codepoint)
    return sorted(result)


def _ordered_strikes(strikes) -> list[FontStrike]:
    ordered = sorted(strikes, key=lambda strike: strike.pixel_height)
    for previous, current in zip(ordered, ordered[1:]):
        if previous.pixel_height == current.pixel_height:
            raise ValueError(
                f"duplicate strike pixel height {current.pixel_height}; "
                "strike selection by screen height would be ambiguous")
    return ordered


def _missing_message(missing: dict[int, set[int]]) -> str:
    parts = []
    for pixel_height, codepoints in missing.items():
        if codepoints:
            names = ", ".join(f"U+{codepoint:04X}" for codepoint in sorted(codepoints))
            parts.append(f"{pixel_height}px: {names}")
    return "; ".join(parts)


def coverage(codepoints, strikes) -> dict[int, set[int]]:
    """Return missing codepoints per strike height; never falls back to another."""
    requested = _normalise_codepoints(codepoints)
    ordered = _ordered_strikes(strikes)
    return {
        strike.pixel_height: set(requested).difference(strike.glyphs)
        for strike in ordered
    }


def pack_strikes(strikes, codepoints, page_size: int = 1024,
                 padding: int = 1, *, allow_missing: bool = False) -> PackedAtlas:
    """Shelf-pack glyphs by (strike pixel height, codepoint) without scaling.

    Each non-empty glyph is copied byte-for-byte into a zeroed Alpha8 page.
    At least one transparent pixel surrounds every glyph and the page border.
    Empty glyphs keep their advance/bearings metadata but draw nothing.
    allow_missing omits absent records for a separately audited multi-strike
    catalogue; the default rejects any missing character.
    """
    if page_size <= 0:
        raise ValueError("page_size must be positive")
    if padding < 1:
        raise ValueError("padding must be at least one pixel")
    if page_size <= 2 * padding:
        raise ValueError("page_size leaves no room for a glyph")
    requested = _normalise_codepoints(codepoints)
    ordered = _ordered_strikes(strikes)
    if requested and not ordered:
        raise ValueError("missing glyphs: no strikes supplied")
    missing = coverage(requested, ordered)
    if not allow_missing and any(missing.values()):
        raise ValueError("missing glyphs: " + _missing_message(missing))

    pages: list[bytearray] = []
    packed: list[PackedGlyph] = []
    x = y = row_height = padding

    def new_page():
        if len(pages) >= MAX_PAGES:
            raise ValueError("page index would exceed 255")
        pages.append(bytearray(page_size * page_size))
        return padding, padding, 0

    for strike in ordered:
        for codepoint in requested:
            if codepoint not in strike.glyphs:
                continue
            glyph = strike.glyphs[codepoint]
            if glyph.width == 0 or glyph.height == 0:
                if not pages:
                    x, y, row_height = new_page()
                packed.append(PackedGlyph(
                    codepoint, strike.pixel_height, len(pages) - 1,
                    padding, padding, glyph.width, glyph.height,
                    glyph.advance, glyph.bearing_x, glyph.bearing_y))
                continue
            if (glyph.width + 2 * padding > page_size
                    or glyph.height + 2 * padding > page_size):
                raise ValueError(
                    f"glyph U+{codepoint:04X} ({glyph.width}x{glyph.height}) "
                    f"does not fit in {page_size}x{page_size} with padding")
            if not pages:
                x, y, row_height = new_page()
            if x + glyph.width + padding > page_size:
                x = padding
                y += row_height + padding
                row_height = 0
            if y + glyph.height + padding > page_size:
                x, y, row_height = new_page()
            page = pages[-1]
            for row in range(glyph.height):
                start = (y + row) * page_size + x
                page[start:start + glyph.width] = glyph.pixels[
                    row * glyph.width:(row + 1) * glyph.width]
            packed.append(PackedGlyph(
                codepoint, strike.pixel_height, len(pages) - 1, x, y,
                glyph.width, glyph.height, glyph.advance,
                glyph.bearing_x, glyph.bearing_y))
            x += glyph.width + padding
            row_height = max(row_height, glyph.height)
    return PackedAtlas(
        page_size, padding, tuple(bytes(page) for page in pages), tuple(packed))


def _read_codepoints(path: Path) -> list[int]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"{path}: invalid JSON: {error}") from error
    if not isinstance(value, list):
        raise ValueError(f"{path}: expected a JSON list of integers")
    return _normalise_codepoints(value)


def _prepare_output(path: Path) -> Path:
    if path.exists():
        if not path.is_dir():
            raise ValueError(f"{path}: output is not a directory")
        if any(path.iterdir()):
            raise ValueError(f"{path}: output directory must be empty")
    else:
        path.mkdir(parents=True)
    return path


def _coverage_report(requested: list[int], strikes) -> dict:
    missing = coverage(requested, strikes)
    return {
        "requested_codepoints": requested,
        "complete": not any(missing.values()),
        "strikes": [
            {
                "pixel_height": pixel_height,
                "missing_codepoints": sorted(codepoints),
                "missing_hex": [f"U+{codepoint:04X}"
                                for codepoint in sorted(codepoints)],
            }
            for pixel_height, codepoints in missing.items()
        ],
    }


def build(font_paths, codepoint_path, output) -> dict:
    """Build pages and metadata for the explicitly requested codepoints."""
    font_paths = [Path(path) for path in font_paths]
    requested = _read_codepoints(Path(codepoint_path))
    strikes = [load_font(path) for path in font_paths]
    ordered = _ordered_strikes(strikes)
    output = _prepare_output(Path(output))

    report = _coverage_report(requested, ordered)
    report["fonts"] = [
        {
            "path": str(path),
            "sha256": strike.source_sha256,
            "pixel_height": strike.pixel_height,
        }
        for path, strike in zip(font_paths, strikes)
    ]
    (output / "coverage.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not report["complete"]:
        detail = "; ".join(
            f"{item['pixel_height']}px: {', '.join(item['missing_hex'])}"
            for item in report["strikes"] if item["missing_codepoints"])
        raise ValueError("missing glyphs: " + detail)

    atlas = pack_strikes(ordered, requested)
    page_entries = []
    for index, page in enumerate(atlas.pages):
        filename = f"page-{index:03d}.alpha8"
        (output / filename).write_bytes(page)
        page_entries.append(
            {"file": filename, "sha256": _sha256(page), "bytes": len(page)})
    manifest = {
        "version": 1,
        "format": "alpha8",
        "page_size": atlas.page_size,
        "padding": atlas.padding,
        "codepoints": requested,
        "fonts": [
            {
                "path": str(path),
                "sha256": strike.source_sha256,
                "pixel_height": strike.pixel_height,
                "ascent": strike.ascent,
                "glyphs": len(strike.glyphs),
            }
            for path, strike in zip(font_paths, strikes)
        ],
        "pages": page_entries,
        "glyphs": [
            {
                "codepoint": glyph.codepoint,
                "strike_pixel_height": glyph.strike_pixel_height,
                "page": glyph.page,
                "x": glyph.x,
                "y": glyph.y,
                "width": glyph.width,
                "height": glyph.height,
                "advance": glyph.advance,
                "bearing_x": glyph.bearing_x,
                "bearing_y": glyph.bearing_y,
            }
            for glyph in atlas.glyphs
        ],
        "coverage": {"complete": True, "requested": len(requested), "missing": []},
    }
    (output / "native-font.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    return manifest


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", action="append", required=True, type=Path,
                        help="BDF 2.1/2.2 or Unifont .hex/.hex.gz; repeatable")
    parser.add_argument("--codepoints", required=True, type=Path,
                        help="JSON list of integer codepoints")
    parser.add_argument("--output", required=True, type=Path,
                        help="new or empty output directory")
    args = parser.parse_args(argv)
    try:
        manifest = build(args.font, args.codepoints, args.output)
    except (OSError, ValueError) as error:
        raise SystemExit(f"error: {error}")
    print(json.dumps({
        "output": str(args.output),
        "pages": len(manifest["pages"]),
        "glyphs": len(manifest["glyphs"]),
    }, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

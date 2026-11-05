"""Parser and layout model for MHGU/NVN ``TEX`` textures.

This module is deliberately independent of ``src/platform/gpu_texture.c``.  It
re-derives the container layout from bytes and from the documented NVN
block-linear layout (reference_projs/SuperMarioWonderRecomp/docs/nvn/TILING.md,
which agrees with the address formula already used by gpu_texture.c).

Container layout (little endian)::

    0x00  4  magic "TEX\\0"
    0x04  4  layout word (purpose not proven; preserved verbatim)
    0x08  4  packed:
               bits  0..5   mip count        (1..64)
               bits  6..18  width            (1..16384)
               bits 19..31  height           (1..16384)
    0x0c  1  unknown (preserved)
    0x0d  1  format code (see FORMATS)
    0x0e  1  resource type (1 = single-layer 2D; 0x20 observed for a 3D volume)
    0x0f  1  flags (not yet given a meaning; preserved)
    0x10  4  total pixel payload size in bytes
    0x14  4  reserved / unknown (preserved)
    0x18  4*(mip_count-1)  absolute offsets of mips 1..N-1 inside the payload
    ...   payload starts immediately after the offset table (mip 0 offset 0)

Payload levels are NVN block-linear.  For every level::

    row_bytes   = ceil(width / block_w) * bytes_per_block
    block_rows  = ceil(height / block_h)
    gob_columns = ceil(row_bytes / 64)
    block_height_gobs(level) = min(block_height_gobs(0),
                                   pow2ceil(block_rows(level) / 8))
    level_size  = gob_columns * ceil(block_rows / (8*block_height_gobs))
                  * 512 * block_height_gobs

Levels are stored back to back with no alignment padding.  The block height of
level 0 is *not* stored in a field this project has proven; the observed data
matches the NVN default (smallest power of two >= block_rows/8, capped at 16).
Every conversion therefore *validates* the complete offset table against the
computed chain and refuses to proceed if any level disagrees.

The module never guesses faces/layers: resource types other than 1 are
rejected with an explicit reason.  All 55,256 corpus TEX rows report resource
type 1 except one resource type 0x20 3D volume, but 124 ``_CM`` rows do not
match the single-layer layout chain and are marked unsupported (their face/mip
table was never proven).  Cube/array support is therefore not claimed.
"""

from __future__ import annotations

import hashlib
import os
import struct
from dataclasses import dataclass, field
from typing import Optional, Sequence

MAGIC = b"TEX\0"
BASE_HEADER_SIZE = 0x18
MAX_DIMENSION = 16384
MAX_MIPS = 64
GOB_BYTES = 512
GOB_BYTES_WIDE = 64
GOB_ROWS = 8
MAX_BLOCK_HEIGHT_GOBS = 16

RESOURCE_TYPE_2D = 1
RESOURCE_TYPE_3D_VOLUME = 0x20


class TexFormatError(ValueError):
    """Raised when a TEX container is malformed or an unsupported feature is hit."""

    def __init__(self, reason: str, detail: str = "") -> None:
        self.reason = reason
        self.detail = detail
        super().__init__(f"{reason}: {detail}" if detail else reason)


@dataclass(frozen=True)
class TexFormat:
    code: int
    name: str
    block_w: int
    block_h: int
    bytes_per_block: int
    family: str
    decoder: bool
    note: str = ""

    @property
    def compressed(self) -> bool:
        return self.block_w != 1 or self.block_h != 1


FORMATS: dict[int, TexFormat] = {
    0x07: TexFormat(0x07, "RGBA8_UNORM", 1, 1, 4, "rgba8", True),
    # Engine format entry 18 (0x12) maps to NVN format 1 (R8_UNORM); verified
    # statically from main+B0AE10.  The canonical host decoder expands it to
    # (R, 0, 0, 255); the runtime TIC swizzle is sampling state and is not
    # part of the TEX payload.
    0x12: TexFormat(0x12, "R8_UNORM", 1, 1, 1, "r8", True),
    # Same NVN physical format as 0x12, without its engine channel-remap flag.
    # The mapped text-font experiment requires this native constructor path.
    0x2C: TexFormat(0x2C, "R8_UNORM_FONT", 1, 1, 1, "r8", True),
    0x13: TexFormat(0x13, "BC1_UNORM", 4, 4, 8, "bc1", True),
    0x15: TexFormat(0x15, "BC2_UNORM", 4, 4, 16, "bc2", True),
    0x17: TexFormat(0x17, "BC3_UNORM", 4, 4, 16, "bc3", True),
    0x19: TexFormat(0x19, "BC4_UNORM", 4, 4, 8, "bc4", True),
    0x1F: TexFormat(0x1F, "BC5_UNORM", 4, 4, 16, "bc5", True),
    0x2A: TexFormat(0x2A, "BC3_UNORM_ALT", 4, 4, 16, "bc3", True),
    # BC7 is recognised so the inventory can name it, but this host converter
    # has no independently written BC7 decoder.  Layout-only handling is not
    # offered because that would silently drop the decode verification step.
    0x30: TexFormat(0x30, "BC7_UNORM", 4, 4, 16, "bc7", False,
                    "BC7 decoder not implemented in this host converter"),
    0x31: TexFormat(0x31, "BC7_UNORM_SRGB", 4, 4, 16, "bc7", False,
                    "BC7 decoder not implemented in this host converter"),
}

# Codes observed in the locked corpus whose block geometry is not established.
KNOWN_UNSUPPORTED_FORMAT_NAMES: dict[int, str] = {
    0x1E: "unknown-0x1e (block geometry/semantics not established)",
    0x20: "unknown-0x20 (corrupt/unsupported payload; extractor also rejects it)",
}


def format_name(code: int) -> str:
    fmt = FORMATS.get(code)
    if fmt is not None:
        return fmt.name
    return KNOWN_UNSUPPORTED_FORMAT_NAMES.get(code, f"unknown-0x{code:02x}")


def require_format(code: int, *, require_decoder: bool = True) -> TexFormat:
    fmt = FORMATS.get(code)
    if fmt is None:
        raise TexFormatError(
            "unsupported-format",
            f"format 0x{code:02x} ({format_name(code)}) has no verified block geometry",
        )
    if require_decoder and not fmt.decoder:
        raise TexFormatError("unsupported-format", f"format 0x{code:02x} {fmt.name}: {fmt.note}")
    return fmt


def pow2_ceil(value: int) -> int:
    """Smallest power of two >= value, with value >= 1."""
    if value < 1:
        raise TexFormatError("bad-geometry", f"pow2_ceil({value})")
    result = 1
    while result < value:
        result <<= 1
    return result


def default_block_height_gobs(block_rows: int) -> int:
    """NVN default block height: smallest pow2 >= block_rows/8, capped at 16."""
    if block_rows < 1:
        raise TexFormatError("bad-geometry", "block_rows must be positive")
    bh = pow2_ceil((block_rows + 7) // 8)
    return max(1, min(MAX_BLOCK_HEIGHT_GOBS, bh))


def level_block_height(block_rows: int, level0_bh: int) -> int:
    if level0_bh < 1 or level0_bh > MAX_BLOCK_HEIGHT_GOBS:
        raise TexFormatError("bad-block-height", str(level0_bh))
    return max(1, min(level0_bh, pow2_ceil((block_rows + 7) // 8)))


def level_geometry(width: int, height: int, fmt: TexFormat, bh: int) -> tuple[int, int, int, int]:
    """Return (block_columns, block_rows, gob_columns, level_size)."""
    if width < 1 or height < 1 or width > MAX_DIMENSION or height > MAX_DIMENSION:
        raise TexFormatError("bad-geometry", f"{width}x{height}")
    bw = (width + fmt.block_w - 1) // fmt.block_w
    bh_rows = (height + fmt.block_h - 1) // fmt.block_h
    row_bytes = bw * fmt.bytes_per_block
    gobs = (row_bytes + GOB_BYTES_WIDE - 1) // GOB_BYTES_WIDE
    blocks_per_gob_height = GOB_ROWS * bh
    size = gobs * ((bh_rows + blocks_per_gob_height - 1) // blocks_per_gob_height) * GOB_BYTES * bh
    return bw, bh_rows, gobs, size


def level_size(width: int, height: int, fmt: TexFormat, bh: int) -> int:
    return level_geometry(width, height, fmt, bh)[3]


def logical_level_bytes(width: int, height: int, fmt: TexFormat) -> int:
    """Bytes of *useful* block data before GOB/block-height padding."""
    bw = (width + fmt.block_w - 1) // fmt.block_w
    bh = (height + fmt.block_h - 1) // fmt.block_h
    return bw * bh * fmt.bytes_per_block


def mip_dimensions(width: int, height: int, mip_count: int) -> list[tuple[int, int]]:
    if mip_count < 1 or mip_count > MAX_MIPS:
        raise TexFormatError("bad-mip-count", str(mip_count))
    dims: list[tuple[int, int]] = []
    w, h = width, height
    for _ in range(mip_count):
        dims.append((w, h))
        w = max(1, w // 2)
        h = max(1, h // 2)
    return dims


@dataclass
class TexHeader:
    layout_word: int
    reserved_0c: int
    format_code: int
    format: Optional[TexFormat]
    resource_type: int
    flags: int
    payload_size: int
    reserved_14: int
    mip_count: int
    width: int
    height: int
    mip_offsets: list[int]  # includes the implicit leading 0
    payload_offset: int
    file_size: int
    trailing_bytes: int
    raw_header_sha256: str = ""

    @property
    def dimensions(self) -> list[tuple[int, int]]:
        return mip_dimensions(self.width, self.height, self.mip_count)


@dataclass
class LevelLayout:
    mip: int
    width: int
    height: int
    block_columns: int
    block_rows: int
    gob_columns: int
    block_height_gobs: int
    offset: int          # inside payload
    stored_size: int     # bytes between this and next mip offset (or payload end)
    logical_size: int    # unpadded block bytes (block_columns*block_rows*bpb)
    padded_size: int     # computed block-linear level size (512-byte GOBs)
    alignment: int       # 512 * block_height_gobs


@dataclass
class TexLayout:
    header: TexHeader
    levels: list[LevelLayout]
    layout_ok: bool
    layout_reason: str
    block_height_gobs0: Optional[int] = None

    @property
    def total_stored(self) -> int:
        return sum(level.stored_size for level in self.levels)

    @property
    def total_padded(self) -> int:
        return sum(level.padded_size for level in self.levels)


def _u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise TexFormatError("truncated-header", f"u32 at 0x{offset:x}")
    return struct.unpack_from("<I", data, offset)[0]


def header_bytes_needed(mip_count: int) -> int:
    if mip_count < 1 or mip_count > MAX_MIPS:
        raise TexFormatError("bad-mip-count", str(mip_count))
    return BASE_HEADER_SIZE + 4 * (mip_count - 1)


def parse_tex_header(data: bytes, *, file_size: Optional[int] = None) -> TexHeader:
    """Parse the header/offset table from ``data``.

    ``data`` may be only the header plus offset table; ``file_size`` defaults to
    ``len(data)`` and is used for the containment check.
    """
    if len(data) < BASE_HEADER_SIZE:
        raise TexFormatError("truncated-header", f"{len(data)} bytes < 0x18")
    if data[:4] != MAGIC:
        raise TexFormatError("bad-magic", data[:4].hex())
    packed = _u32(data, 0x08)
    mip_count = packed & 0x3F
    width = (packed >> 6) & 0x1FFF
    height = (packed >> 19) & 0x1FFF
    # Explicit bit-level assertions: the three fields must reconstruct the
    # packed word exactly (no unaccounted bits besides the 32-bit container).
    if ((packed & 0x3F) | ((packed >> 6) & 0x1FFF) << 6 | ((packed >> 19) & 0x1FFF) << 19) != packed:
        raise TexFormatError("bit-assert", f"packed word 0x{packed:08x} does not round trip")
    if mip_count < 1 or mip_count > MAX_MIPS:
        raise TexFormatError("bad-mip-count", str(mip_count))
    if width < 1 or height < 1 or width > MAX_DIMENSION or height > MAX_DIMENSION:
        raise TexFormatError("bad-geometry", f"{width}x{height} mips={mip_count}")
    needed = header_bytes_needed(mip_count)
    if len(data) < needed:
        raise TexFormatError("truncated-mip-table", f"need {needed}, have {len(data)}")
    payload_size = _u32(data, 0x10)
    offsets = [0]
    for index in range(mip_count - 1):
        value = _u32(data, BASE_HEADER_SIZE + 4 * index)
        if value <= offsets[-1]:
            raise TexFormatError(
                "bad-mip-offsets",
                f"mip {index + 1} offset {value} is not greater than {offsets[-1]}",
            )
        offsets.append(value)
    if payload_size < offsets[-1]:
        raise TexFormatError(
            "bad-payload-size",
            f"payload {payload_size} smaller than last mip offset {offsets[-1]}",
        )
    if payload_size == 0:
        raise TexFormatError("bad-payload-size", "zero")
    actual_size = len(data) if file_size is None else file_size
    if needed + payload_size > actual_size:
        raise TexFormatError(
            "truncated-payload",
            f"payload ends at {needed + payload_size}, file is {actual_size}",
        )
    format_code = data[0x0D]
    header = TexHeader(
        layout_word=_u32(data, 0x04),
        reserved_0c=data[0x0C],
        format_code=format_code,
        format=FORMATS.get(format_code),
        resource_type=data[0x0E],
        flags=data[0x0F],
        payload_size=payload_size,
        reserved_14=_u32(data, 0x14),
        mip_count=mip_count,
        width=width,
        height=height,
        mip_offsets=offsets,
        payload_offset=needed,
        file_size=actual_size,
        trailing_bytes=actual_size - (needed + payload_size),
        raw_header_sha256=hashlib.sha256(data[:needed]).hexdigest(),
    )
    return header


def parse_tex_file(path: str, *, header_only: bool = True) -> TexHeader:
    with open(path, "rb") as handle:
        base = handle.read(BASE_HEADER_SIZE)
        if len(base) < BASE_HEADER_SIZE:
            raise TexFormatError("truncated-header", f"{len(base)} bytes")
        if base[:4] != MAGIC:
            raise TexFormatError("bad-magic", base[:4].hex())
        packed = struct.unpack_from("<I", base, 0x08)[0]
        mips = packed & 0x3F
        needed = header_bytes_needed(mips)
        head = base + handle.read(needed - BASE_HEADER_SIZE)
        size = os.path.getsize(path)
    return parse_tex_header(head, file_size=size)


def compute_layout(header: TexHeader, *, require_decoder: bool = False) -> TexLayout:
    """Compute/validate the block-linear chain.  Never guesses a block height.

    The only block-height hypothesis is the NVN default; it must reproduce every
    stored mip offset and the exact payload size, otherwise layout_ok is False.
    """
    fmt = header.format
    if fmt is None:
        return TexLayout(header, [], False, f"unsupported format 0x{header.format_code:02x}")
    if require_decoder and not fmt.decoder:
        return TexLayout(header, [], False, f"{fmt.name}: {fmt.note}")
    if header.resource_type != RESOURCE_TYPE_2D:
        return TexLayout(
            header, [], False,
            f"resource_type 0x{header.resource_type:02x} is not single-layer 2D",
        )
    dims = header.dimensions
    block_rows0 = (header.height + fmt.block_h - 1) // fmt.block_h
    bh0 = default_block_height_gobs(block_rows0)
    levels: list[LevelLayout] = []
    cursor = 0
    ok = True
    reason = "ok"
    for mip, (width, height) in enumerate(dims):
        blocks_h = (height + fmt.block_h - 1) // fmt.block_h
        bh = level_block_height(blocks_h, bh0)
        bw, br, gobs, padded = level_geometry(width, height, fmt, bh)
        stored = (
            header.mip_offsets[mip + 1] - header.mip_offsets[mip]
            if mip + 1 < header.mip_count
            else header.payload_size - header.mip_offsets[mip]
        )
        expected_offset = cursor
        if header.mip_offsets[mip] != expected_offset:
            ok = False
            reason = (
                f"mip {mip}: header offset {header.mip_offsets[mip]} != computed {expected_offset}"
            )
            break
        if stored != padded:
            ok = False
            reason = f"mip {mip}: stored size {stored} != computed block-linear size {padded}"
            break
        levels.append(
            LevelLayout(
                mip=mip,
                width=width,
                height=height,
                block_columns=bw,
                block_rows=br,
                gob_columns=gobs,
                block_height_gobs=bh,
                offset=expected_offset,
                stored_size=stored,
                logical_size=logical_level_bytes(width, height, fmt),
                padded_size=padded,
                alignment=GOB_BYTES * bh,
            )
        )
        cursor += padded
    if ok and cursor != header.payload_size:
        ok = False
        reason = f"computed chain ends at {cursor}, payload size is {header.payload_size}"
    if ok and header.trailing_bytes != 0:
        reason = f"ok (trailing {header.trailing_bytes} bytes after payload)"
    return TexLayout(header, levels, ok, reason, bh0)


# ---------------------------------------------------------------------------
# Block-linear address mapping
# ---------------------------------------------------------------------------

def gob_byte_offset(byte_x: int, row_y: int) -> int:
    """Byte offset inside a 64x8 GOB (TILING.md formula), with assertions."""
    if not (0 <= byte_x < 64 and 0 <= row_y < 8):
        raise TexFormatError("gob-range", f"x={byte_x} y={row_y}")
    offset = (
        (byte_x // 32) * 256
        + (row_y // 2) * 64
        + ((byte_x // 16) % 2) * 32
        + (row_y % 2) * 16
        + (byte_x % 16)
    )
    if not (0 <= offset < GOB_BYTES):
        raise TexFormatError("gob-assert", str(offset))
    return offset


def tiled_offset(block_x: int, block_y: int, blocks_w: int, bytes_per_block: int,
                 block_height_gobs: int) -> int:
    """Address of a block inside one block-linear level."""
    if block_x < 0 or block_y < 0 or blocks_w < 1:
        raise TexFormatError("block-range", f"{block_x},{block_y}/{blocks_w}")
    bytes_x = block_x * bytes_per_block
    gob_columns = (blocks_w * bytes_per_block + GOB_BYTES_WIDE - 1) // GOB_BYTES_WIDE
    offset = (
        (block_y // (GOB_ROWS * block_height_gobs)) * gob_columns * GOB_BYTES * block_height_gobs
        + (bytes_x // GOB_BYTES_WIDE) * GOB_BYTES * block_height_gobs
        + ((block_y // GOB_ROWS) % block_height_gobs) * GOB_BYTES
        + gob_byte_offset(bytes_x % GOB_BYTES_WIDE, block_y % GOB_ROWS)
    )
    return offset


def untile_level(payload: bytes, level: LevelLayout, fmt: TexFormat) -> bytes:
    """Return blocks in linear raster order (bytes_per_block chunks).

    The level must have been produced by compute_layout so stored_size equals
    the padded size; bounds are still checked explicitly.
    """
    source = memoryview(payload)[level.offset:level.offset + level.stored_size]
    if len(source) != level.stored_size:
        raise TexFormatError("truncated-level", f"mip {level.mip}")
    if level.stored_size < level.padded_size:
        raise TexFormatError("short-level", f"mip {level.mip}")
    out = bytearray(level.block_columns * level.block_rows * fmt.bytes_per_block)
    bpb = fmt.bytes_per_block
    for by in range(level.block_rows):
        for bx in range(level.block_columns):
            src = tiled_offset(bx, by, level.block_columns, bpb, level.block_height_gobs)
            if src + bpb > len(source):
                raise TexFormatError(
                    "level-out-of-range",
                    f"mip {level.mip} block {bx},{by} at {src}+{bpb} > {len(source)}",
                )
            dst = (by * level.block_columns + bx) * bpb
            out[dst:dst + bpb] = source[src:src + bpb]
    return bytes(out)


def retile_level(linear: bytes, level: LevelLayout, fmt: TexFormat) -> bytes:
    """Inverse of untile_level: linear blocks -> padded block-linear level."""
    expected = level.block_columns * level.block_rows * fmt.bytes_per_block
    if len(linear) != expected:
        raise TexFormatError("linear-size", f"{len(linear)} != {expected}")
    out = bytearray(level.padded_size)
    bpb = fmt.bytes_per_block
    for by in range(level.block_rows):
        for bx in range(level.block_columns):
            src = (by * level.block_columns + bx) * bpb
            dst = tiled_offset(bx, by, level.block_columns, bpb, level.block_height_gobs)
            if dst + bpb > len(out):
                raise TexFormatError("retile-range", f"mip {level.mip} {bx},{by}")
            out[dst:dst + bpb] = linear[src:src + bpb]
    return bytes(out)


# ---------------------------------------------------------------------------
# Verification decoders (canonical RGBA8; NOT the runtime channel swizzle)
# ---------------------------------------------------------------------------

def _rgb565_to_rgb(color: int) -> tuple[int, int, int]:
    r = (color >> 11) & 0x1F
    g = (color >> 5) & 0x3F
    b = color & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def _bc1_colors(block: bytes) -> list[tuple[int, int, int, int]]:
    c0, c1 = struct.unpack_from("<HH", block, 0)
    r0, g0, b0 = _rgb565_to_rgb(c0)
    r1, g1, b1 = _rgb565_to_rgb(c1)
    colors = [
        (r0, g0, b0, 255),
        (r1, g1, b1, 255),
    ]
    if c0 > c1:
        colors.append(((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 255))
        colors.append(((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 255))
    else:
        colors.append(((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255))
        colors.append((0, 0, 0, 0))
    return colors


def _bc_alpha_palette(block: bytes, offset: int) -> list[int]:
    a0, a1 = block[offset], block[offset + 1]
    palette = [a0, a1]
    if a0 > a1:
        for i in range(1, 7):
            palette.append(((7 - i) * a0 + i * a1) // 7)
    else:
        for i in range(1, 5):
            palette.append(((5 - i) * a0 + i * a1) // 5)
        palette.extend([0, 255])
    return palette


def decode_level_rgba(linear: bytes, level: LevelLayout, fmt: TexFormat) -> bytes:
    """Decode a linear block-order level to RGBA8 for independent verification.

    Channel semantics for BC4/BC5 are canonical (BC4 -> R, BC5 -> R,G,B=0,
    A=255).  The game's TIC component swizzle is sampling state, not part of the
    TEX payload, and is not invented here.
    """
    width, height = level.width, level.height
    out = bytearray(width * height * 4)
    if fmt.family == "rgba8":
        if len(linear) < width * height * 4:
            raise TexFormatError("linear-size", "RGBA8")
        out[:] = linear[:width * height * 4]
        return bytes(out)
    if fmt.family == "r8":
        if len(linear) < width * height:
            raise TexFormatError("linear-size", "R8")
        for i in range(width * height):
            o = i * 4
            out[o] = linear[i]
            out[o + 3] = 255
        return bytes(out)
    bw = (width + 3) // 4
    bh = (height + 3) // 4
    expected = bw * bh * fmt.bytes_per_block
    if len(linear) != expected:
        raise TexFormatError("linear-size", f"{len(linear)} != {expected}")
    for by in range(bh):
        for bx in range(bw):
            block = linear[(by * bw + bx) * fmt.bytes_per_block:
                           (by * bw + bx + 1) * fmt.bytes_per_block]
            if fmt.family in ("bc1", "bc2", "bc3"):
                if fmt.family == "bc3":
                    color_block = block[8:16]
                    alpha_palette = _bc_alpha_palette(block, 0)
                    alpha_bits = int.from_bytes(block[2:8], "little")
                    alpha_offset = 0
                elif fmt.family == "bc2":
                    color_block = block[8:16]
                    alpha_palette = None
                    alpha_bits = int.from_bytes(block[0:8], "little")
                    alpha_offset = 0
                else:
                    color_block = block
                    alpha_palette = None
                    alpha_bits = None
                colors = _bc1_colors(color_block)
                indices = int.from_bytes(color_block[4:8], "little")
                for py in range(4):
                    for px in range(4):
                        i = py * 4 + px
                        color = colors[(indices >> (2 * i)) & 3]
                        x = bx * 4 + px
                        y = by * 4 + py
                        if x >= width or y >= height:
                            continue
                        o = (y * width + x) * 4
                        out[o:o + 4] = bytes(color)
                        if alpha_palette is not None:
                            ai = (alpha_bits >> (3 * i)) & 7
                            out[o + 3] = alpha_palette[ai]
                        elif alpha_bits is not None:
                            ai = (alpha_bits >> (4 * i)) & 0xF
                            out[o + 3] = ai * 17
                continue
            # BC4/BC5: red/red+green channels, 8-byte endpoint pairs.
            if fmt.family == "bc4":
                pairs = [(block, 0)]
            elif fmt.family == "bc5":
                pairs = [(block, 0), (block, 8)]
            else:
                raise TexFormatError("unsupported-decode", fmt.name)
            decoded: list[list[int]] = []
            for pair, offset in pairs:
                palette = _bc_alpha_palette(pair, offset)
                bits = int.from_bytes(pair[offset + 2:offset + 8], "little")
                decoded.append([palette[(bits >> (3 * i)) & 7] for i in range(16)])
            for py in range(4):
                for px in range(4):
                    i = py * 4 + px
                    x = bx * 4 + px
                    y = by * 4 + py
                    if x >= width or y >= height:
                        continue
                    o = (y * width + x) * 4
                    out[o] = decoded[0][i]
                    out[o + 1] = decoded[1][i] if len(decoded) > 1 else 0
                    out[o + 2] = 0
                    out[o + 3] = 255
    return bytes(out)


def decode_tex_levels(payload: bytes, layout: TexLayout) -> list[bytes]:
    """Untile and decode every level; returns RGBA8 per mip."""
    if not layout.layout_ok or layout.header.format is None:
        raise TexFormatError("layout-unverified", layout.layout_reason)
    rgba: list[bytes] = []
    for level in layout.levels:
        linear = untile_level(payload, level, layout.header.format)
        rgba.append(decode_level_rgba(linear, level, layout.header.format))
    return rgba


def read_payload(path: str, header: TexHeader) -> bytes:
    with open(path, "rb") as handle:
        handle.seek(header.payload_offset)
        payload = handle.read(header.payload_size)
    if len(payload) != header.payload_size:
        raise TexFormatError("truncated-payload", f"{len(payload)} != {header.payload_size}")
    return payload


def inspect_file(path: str, *, require_decoder: bool = False) -> tuple[TexHeader, TexLayout]:
    header = parse_tex_file(path)
    return header, compute_layout(header, require_decoder=require_decoder)

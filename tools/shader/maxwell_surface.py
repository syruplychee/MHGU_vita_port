"""Decode Maxwell texture descriptors and untiled surfaces.

The reference runtime stores the guest's TIC/TSC tables and the surface bytes; this
module turns those bytes into a linear image a GXM replay can upload. Every field
offset here follows the locked reference tree's `video_core/textures/texture.h`
(`TICEntry`, `TSCEntry`); nothing is guessed from a name.
"""
import struct

TIC_ENTRY_SIZE = 0x20
TSC_ENTRY_SIZE = 0x20

# TICEntry.header_version
HEADER_BUFFER = 0
HEADER_PITCH_COLOR_KEY = 1
HEADER_PITCH = 2
HEADER_BLOCK_LINEAR = 3
HEADER_BLOCK_LINEAR_COLOR_KEY = 4

# TICEntry.format (TextureFormat), only the layouts this port has to read.
FORMATS = {
    0x07: ('X8B8G8R8', 4, 'rgba'),
    0x08: ('A8B8G8R8', 4, 'rgba'),
    0x09: ('A2B10G10R10', 4, 'rgba'),
    0x0C: ('R16G16', 4, 'rg'),
    0x0F: ('R32', 4, 'r'),
    0x12: ('A4B4G4R4', 2, 'rgba'),
    0x18: ('G8R8', 2, 'rg'),
    0x1B: ('R16', 2, 'r'),
    0x1D: ('R8', 1, 'r'),
}
# DXT1 is decoded: it is what the captured UI atlas is.
DXT1 = 0x24
# Compressed formats this port does not decode yet. Refused, never approximated.
COMPRESSED = {0x06, 0x0A, 0x0B, 0x10, 0x11, 0x17, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28}
COMPRESSED |= set(range(0x40, 0x60))
DEPTH = {0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
         0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E}

WRAP = {0: 'wrap', 1: 'mirror', 2: 'clamp_to_edge', 3: 'border', 4: 'clamp',
        5: 'mirror_once_clamp', 6: 'mirror_once_border', 7: 'mirror_once_clamp_ogl'}
FILTER = {0: 'none', 1: 'nearest', 2: 'linear'}


class Unsupported(ValueError):
    """A surface this tool cannot decode. Never silently approximated."""


def _u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


class TicEntry:
    """One texture/image control entry."""

    def __init__(self, data):
        if len(data) < TIC_ENTRY_SIZE:
            raise Unsupported('short TIC entry')
        word0, address_low, word2, word3 = _u32(data, 0), _u32(data, 4), _u32(data, 8), _u32(data, 12)
        word4, word5, word6, word7 = (_u32(data, 16), _u32(data, 20), _u32(data, 24), _u32(data, 28))
        self.raw = bytes(data[:TIC_ENTRY_SIZE])
        self.format = word0 & 0x7F
        self.component_types = [(word0 >> 7) & 7, (word0 >> 10) & 7, (word0 >> 13) & 7, (word0 >> 16) & 7]
        self.address = ((word2 & 0xFFFF) << 32) | address_low
        self.header_version = (word2 >> 21) & 7
        self.block_width = word3 & 7
        self.block_height = (word3 >> 3) & 7
        self.block_depth = (word3 >> 6) & 7
        self.pitch = (word3 & 0xFFFF) << 5
        self.depth_texture = (word3 >> 27) & 1
        self.max_mip_level = (word3 >> 28) & 0xF
        self.width = (word4 & 0xFFFF) + 1
        self.srgb_conversion = (word4 >> 22) & 1
        self.texture_type = (word4 >> 23) & 0xF
        self.height = (word5 & 0xFFFF) + 1
        self.depth = ((word5 >> 16) & 0x3FFF) + 1
        self.is_sparse = (word5 >> 30) & 1
        self.normalized_coords = (word5 >> 31) & 1
        self.msaa_mode = (word7 >> 8) & 0xF

    @property
    def block_linear(self):
        return self.header_version in (HEADER_BLOCK_LINEAR, HEADER_BLOCK_LINEAR_COLOR_KEY)

    @property
    def pitch_linear(self):
        return self.header_version in (HEADER_PITCH, HEADER_PITCH_COLOR_KEY)

    def describe(self):
        return {'format': self.format, 'address': '0x%x' % self.address,
                'header_version': self.header_version, 'width': self.width, 'height': self.height,
                'depth': self.depth, 'block': [self.block_width, self.block_height, self.block_depth],
                'pitch': self.pitch, 'max_mip_level': self.max_mip_level,
                'depth_texture': self.depth_texture, 'srgb': self.srgb_conversion,
                'texture_type': self.texture_type, 'msaa_mode': self.msaa_mode,
                'normalized': self.normalized_coords}


class TscEntry:
    """One sampler control entry."""

    def __init__(self, data):
        if len(data) < TSC_ENTRY_SIZE:
            raise Unsupported('short TSC entry')
        word0, word1, word2 = _u32(data, 0), _u32(data, 4), _u32(data, 8)
        self.raw = bytes(data[:TSC_ENTRY_SIZE])
        self.wrap_u = WRAP.get(word0 & 7, 'unknown_%d' % (word0 & 7))
        self.wrap_v = WRAP.get((word0 >> 3) & 7, 'unknown')
        self.wrap_p = WRAP.get((word0 >> 6) & 7, 'unknown')
        self.srgb_conversion = (word0 >> 13) & 1
        self.max_anisotropy = (word0 >> 20) & 7
        self.mag_filter = FILTER.get(word1 & 3, 'unknown_%d' % (word1 & 3))
        self.min_filter = FILTER.get((word1 >> 4) & 3, 'unknown')
        self.mipmap_filter = FILTER.get((word1 >> 6) & 3, 'unknown')
        self.min_lod_clamp = word2 & 0xFFF
        self.max_lod_clamp = (word2 >> 12) & 0xFFF

    def describe(self):
        return {'wrap_u': self.wrap_u, 'wrap_v': self.wrap_v, 'wrap_p': self.wrap_p,
                'mag_filter': self.mag_filter, 'min_filter': self.min_filter,
                'mipmap_filter': self.mipmap_filter, 'srgb': self.srgb_conversion,
                'max_anisotropy': self.max_anisotropy,
                'min_lod_clamp': self.min_lod_clamp, 'max_lod_clamp': self.max_lod_clamp}


def texture_handle(word, via_header_index):
    """TIC index and TSC index a shader's texture word selects.

    `sampler_binding == ViaHeaderBinding` makes the whole word the TIC index; the
    independent form packs both indices into one 32-bit handle.
    """
    if via_header_index:
        return word, word
    return word & 0xFFFFF, (word >> 20) & 0xFFF


def _layout(tic):
    if tic.msaa_mode:
        raise Unsupported('multisampled surfaces are not decoded')
    if tic.depth_texture:
        raise Unsupported('depth TIC entries are not decoded')
    if tic.format in COMPRESSED:
        raise Unsupported('compressed format 0x%02x' % tic.format)
    if tic.format in DEPTH:
        raise Unsupported('depth format 0x%02x has no colour decoder' % tic.format)
    if tic.format not in FORMATS:
        raise Unsupported('unhandled TIC format 0x%02x' % tic.format)
    name, bpp, channels = FORMATS[tic.format]
    if tic.depth != 1:
        raise Unsupported('3D surfaces are not decoded')
    return name, bpp, channels


def _elements(tic):
    """(bytes per element, element width, element height) of the surface grid.

    An element is one texel for the uncompressed formats and one 4x4 block for
    DXT1, which is the granularity the GOB layout and the swizzle work on.
    """
    if tic.format == DXT1:
        return 8, 4, 4
    _name, bpp, _channels = _layout(tic)
    return bpp, 1, 1


def _gob_swizzle(row_in_gob, byte_in_gob):
    """The intra-GOB swizzle: 32-byte halves, 16-byte sectors, 2-row pairs.

    Taken from the locked reference tree's own tables
    (`video_core/textures/decoders.h`, `MakeSwizzleTable`, "Tegra X1 Technical
    Reference Manual pages 1187-1188"); that is the function the reference
    renderer itself uses to read these surfaces.
    """
    return ((byte_in_gob % 64) // 32) * 256 + ((row_in_gob % 8) // 2) * 64 + \
           ((byte_in_gob % 32) // 16) * 32 + (row_in_gob % 2) * 16 + (byte_in_gob % 16)


def _block_linear_origin(tic):
    """Per-descriptor constants of the block-linear grid, in element units."""
    bpp, ew, eh = _elements(tic)
    tiles_x = (tic.width + ew - 1) // ew
    tiles_y = (tic.height + eh - 1) // eh
    elements_per_gob_row = 64 // bpp
    # StrideAlignment for a surface wider than one GOB is the GOB width, so the
    # tile row is exactly tiles_x elements wide and gobs_in_x is its byte width
    # in GOB units (reference: texture_cache/accelerated_swizzle.cpp).
    block_h = tic.block_height
    block_depth = tic.block_depth
    gobs_in_x = (tiles_x * bpp + 63) // 64
    block_size = gobs_in_x << (9 + block_h + block_depth)
    return {'bpp': bpp, 'ew': ew, 'eh': eh, 'tiles_x': tiles_x, 'tiles_y': tiles_y,
            'elements_per_gob_row': elements_per_gob_row, 'block_h': block_h,
            'block_depth': block_depth, 'block_size': block_size,
            'x_shift': 9 + block_h + block_depth}


def _tile_offset(tile_x, tile_y, tic):
    """Byte offset of element (tile_x, tile_y) inside a block-linear surface."""
    grid = _block_linear_origin(tic)
    bpp = grid['bpp']
    row_in_gob = tile_y % 8
    byte_in_gob = (tile_x % grid['elements_per_gob_row']) * bpp
    gob_x, gob_y = tile_x // grid['elements_per_gob_row'], tile_y // 8
    mask = (1 << grid['block_h']) - 1
    return ((gob_y >> grid['block_h']) * grid['block_size'] + ((gob_y & mask) << 9) +
            (gob_x << grid['x_shift']) + _gob_swizzle(row_in_gob, byte_in_gob))


def surface_size(tic):
    """Bytes the surface occupies in guest memory, from its descriptor alone."""
    bpp, ew, eh = _elements(tic)
    if tic.pitch_linear:
        row_bytes = max(tic.width, tic.pitch) * bpp // ew
        return ((row_bytes + 0xFF) // 0x100) * 0x100 * tic.height
    if tic.header_version == HEADER_BUFFER:
        return tic.width * bpp * tic.height
    if not tic.block_linear:
        raise Unsupported('header version %d has no known layout' % tic.header_version)
    grid = _block_linear_origin(tic)
    gobs_in_x = (grid['tiles_x'] * bpp + 63) // 64
    gobs_in_y = (grid['tiles_y'] + 7) // 8
    block_h = 1 << grid['block_h']
    block_depth = 1 << grid['block_depth']
    blocks_y = (gobs_in_y + block_h - 1) // block_h
    return gobs_in_x * blocks_y * block_h * block_depth * 512


def _decode_pixel(raw, name):
    if name in ('A8B8G8R8', 'X8B8G8R8'):
        return raw[0], raw[1], raw[2], (raw[3] if name == 'A8B8G8R8' else 255)
    if name == 'R8G8':
        return raw[0], raw[1], 0, 255
    if name == 'G8R8':
        return raw[1], raw[0], 0, 255
    if name == 'R8':
        return raw[0], 0, 0, 255
    if name == 'R16G16':
        r, g = struct.unpack_from('<2H', raw, 0)
        return r >> 8, g >> 8, 0, 255
    if name == 'R16':
        return struct.unpack_from('<H', raw, 0)[0] >> 8, 0, 0, 255
    if name == 'R32':
        return struct.unpack_from('<I', raw, 0)[0] >> 24, 0, 0, 255
    if name == 'A4B4G4R4':
        value = struct.unpack_from('<H', raw, 0)[0]
        return (((value >> 12) & 0xF) * 17, ((value >> 8) & 0xF) * 17,
                ((value >> 4) & 0xF) * 17, (value & 0xF) * 17)
    raise Unsupported('no decoder for %s' % name)


def _dxt1_texels(block):
    """The 16 texels of one DXT1 block, row major, as RGBA bytes."""
    color0, color1, indices = struct.unpack_from('<HHI', block, 0)

    def expand(value):
        r5, g6, b5 = (value >> 11) & 0x1F, (value >> 5) & 0x3F, value & 0x1F
        return [(r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)]

    palette = [expand(color0), expand(color1)]
    if color0 > color1:
        palette.append([(2 * palette[0][k] + palette[1][k]) // 3 for k in range(3)])
        palette.append([(palette[0][k] + 2 * palette[1][k]) // 3 for k in range(3)])
    else:
        palette.append([(palette[0][k] + palette[1][k]) // 2 for k in range(3)])
        palette.append([0, 0, 0])
    out = bytearray(64)
    for index in range(16):
        selector = (indices >> (2 * index)) & 3
        colour = palette[selector]
        alpha = 0 if (color0 <= color1 and selector == 3) else 255
        out[index * 4:index * 4 + 4] = bytes((colour[0], colour[1], colour[2], alpha))
    return out


def untile(descriptor, data, rows=None):
    """Decode a surface into linear RGBA bytes, `width * 4` per row.

    `rows` limits how many scanlines are decoded, which lets a replay check that a
    bounded capture window covers the rows it needs before using any of them.
    """
    bpp, ew, eh = _elements(descriptor)
    width, full_height = descriptor.width, descriptor.height
    height = full_height if rows is None else min(rows, full_height)
    out = bytearray(width * 4 * height)
    if descriptor.pitch_linear or descriptor.header_version == HEADER_BUFFER:
        name, _bpp, _channels = _layout(descriptor)
        if descriptor.pitch_linear:
            stride = max(descriptor.pitch, width * bpp) if ew == 1 else                 ((max(descriptor.pitch, width * bpp // ew) + 0xFF) // 0x100) * 0x100
        else:
            stride = width * bpp
        for y in range(height):
            row = y * width * 4
            for x in range(width):
                offset = y * stride + x * bpp
                if offset + bpp > len(data):
                    raise Unsupported('surface window is short of row %d' % y)
                out[row + x * 4:row + x * 4 + 4] = bytes(_decode_pixel(data[offset:offset + bpp], name))
        return bytes(out)
    if not descriptor.block_linear:
        raise Unsupported('header version %d has no accessor' % descriptor.header_version)
    if descriptor.format == DXT1:
        tiles_x = (width + 3) // 4
        for tile_y in range((height + 3) // 4):
            for tile_x in range(tiles_x):
                offset = _tile_offset(tile_x, tile_y, descriptor)
                if offset + 8 > len(data):
                    raise Unsupported('surface window is short of block row %d' % tile_y)
                texels = _dxt1_texels(data[offset:offset + 8])
                for y in range(4):
                    sy = tile_y * 4 + y
                    if sy >= height:
                        break
                    for x in range(4):
                        sx = tile_x * 4 + x
                        if sx >= width:
                            break
                        source = (y * 4 + x) * 4
                        target = (sy * width + sx) * 4
                        out[target:target + 4] = texels[source:source + 4]
        return bytes(out)
    name, _bpp, _channels = _layout(descriptor)
    for y in range(height):
        row = y * width * 4
        for x in range(width):
            offset = _tile_offset(x, y, descriptor)
            if offset + bpp > len(data):
                raise Unsupported('surface window is short of row %d' % y)
            out[row + x * 4:row + x * 4 + 4] = bytes(_decode_pixel(data[offset:offset + bpp], name))
    return bytes(out)


def read_entry(blob, index, entry_size=TIC_ENTRY_SIZE, cls=None):
    start = index * entry_size
    if start + entry_size > len(blob):
        raise Unsupported('entry %d is outside the captured table window' % index)
    return (cls or TicEntry)(blob[start:start + entry_size])

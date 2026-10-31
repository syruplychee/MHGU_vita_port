/*
 * Independent host verifier for MHGU TEX containers.
 *
 * This file is intentionally written from the container description and the
 * NVN block-linear layout notes; it does not include or link the Python
 * converter.  It is used by tests/test_static_asset_tools.py under WSL with
 * AddressSanitizer/UndefinedBehaviorSanitizer.
 *
 * Modes:
 *   tex_verify --self-test
 *   tex_verify --negative
 *   tex_verify --file <path> [--mip N]
 *
 * Exit codes: 0 success, 1 verification failure, 2 usage/IO error.
 *
 * Output is line oriented "key=value" and is parsed by the Python test.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEX_MAX_MIPS 64u
#define TEX_MAX_DIM 16384u
#define TEX_MAX_BH 16u
#define TEX_HEADER_BASE 0x18u

typedef struct {
    int code;
    const char *name;
    unsigned block_w, block_h, bytes_per_block;
    int decoder;
} Format;

static const Format kFormats[] = {
    {0x07, "RGBA8_UNORM", 1, 1, 4, 1},
    {0x13, "BC1_UNORM", 4, 4, 8, 1},
    {0x15, "BC2_UNORM", 4, 4, 16, 1},
    {0x17, "BC3_UNORM", 4, 4, 16, 1},
    {0x19, "BC4_UNORM", 4, 4, 8, 1},
    {0x1f, "BC5_UNORM", 4, 4, 16, 1},
    {0x2a, "BC3_UNORM_ALT", 4, 4, 16, 1},
    {0x30, "BC7_UNORM", 4, 4, 16, 0},
    {0x31, "BC7_UNORM_SRGB", 4, 4, 16, 0},
};
#define FORMAT_COUNT (sizeof(kFormats) / sizeof(kFormats[0]))

typedef struct {
    uint32_t mip_count, width, height, payload_size, payload_offset;
    uint8_t format_code, resource_type, flags, reserved_0c;
    uint32_t layout_word, reserved_14;
    uint32_t offsets[TEX_MAX_MIPS];
    uint64_t file_size;
    uint64_t trailing;
    const Format *format;
} Header;

typedef struct {
    unsigned width, height, block_columns, block_rows, gob_columns, bh;
    uint32_t offset, stored_size, logical_size, padded_size, alignment;
} Level;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static const Format *find_format(uint8_t code) {
    for (size_t i = 0; i < FORMAT_COUNT; ++i)
        if (kFormats[i].code == code) return &kFormats[i];
    return NULL;
}

static uint64_t fnv1a64(const uint8_t *data, size_t size) {
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static unsigned pow2_ceil(unsigned v) {
    unsigned p = 1;
    while (p < v) p <<= 1;
    return p;
}

static const char *parse_header(const uint8_t *data, size_t size, Header *h,
                                char *why, size_t why_size) {
    memset(h, 0, sizeof(*h));
    h->file_size = size;
    if (size < TEX_HEADER_BASE) {
        snprintf(why, why_size, "truncated-header");
        return "reject";
    }
    if (memcmp(data, "TEX\0", 4) != 0) {
        snprintf(why, why_size, "bad-magic");
        return "reject";
    }
    uint32_t packed = rd32(data + 8);
    h->mip_count = packed & 0x3fu;
    h->width = (packed >> 6) & 0x1fffu;
    h->height = (packed >> 19) & 0x1fffu;
    if (((packed & 0x3fu) | (((packed >> 6) & 0x1fffu) << 6) |
         (((packed >> 19) & 0x1fffu) << 19)) != packed) {
        snprintf(why, why_size, "packed-bit-assert");
        return "reject";
    }
    if (h->mip_count < 1 || h->mip_count > TEX_MAX_MIPS) {
        snprintf(why, why_size, "bad-mip-count");
        return "reject";
    }
    if (h->width < 1 || h->height < 1 || h->width > TEX_MAX_DIM ||
        h->height > TEX_MAX_DIM) {
        snprintf(why, why_size, "bad-geometry");
        return "reject";
    }
    uint64_t needed = TEX_HEADER_BASE + 4ull * (h->mip_count - 1u);
    if (needed > size) {
        snprintf(why, why_size, "truncated-mip-table");
        return "reject";
    }
    h->layout_word = rd32(data + 4);
    h->reserved_0c = data[0x0c];
    h->format_code = data[0x0d];
    h->resource_type = data[0x0e];
    h->flags = data[0x0f];
    h->payload_size = rd32(data + 0x10);
    h->reserved_14 = rd32(data + 0x14);
    h->payload_offset = (uint32_t)needed;
    h->offsets[0] = 0;
    for (uint32_t i = 0; i + 1 < h->mip_count; ++i) {
        uint32_t value = rd32(data + TEX_HEADER_BASE + 4u * i);
        if (value <= h->offsets[i]) {
            snprintf(why, why_size, "bad-mip-offsets(mip=%u)", i + 1u);
            return "reject";
        }
        h->offsets[i + 1] = value;
    }
    if (h->payload_size == 0 || h->payload_size < h->offsets[h->mip_count - 1]) {
        snprintf(why, why_size, "bad-payload-size");
        return "reject";
    }
    if ((uint64_t)h->payload_offset + h->payload_size > size) {
        snprintf(why, why_size, "truncated-payload");
        return "reject";
    }
    h->trailing = size - ((uint64_t)h->payload_offset + h->payload_size);
    h->format = find_format(h->format_code);
    return "ok";
}

static unsigned level_bh(unsigned block_rows, unsigned level0_bh) {
    unsigned need = pow2_ceil((block_rows + 7u) / 8u);
    unsigned bh = need < level0_bh ? need : level0_bh;
    return bh ? bh : 1u;
}

static void level_geometry(unsigned w, unsigned h, const Format *f, unsigned bh,
                           unsigned *bw, unsigned *br, unsigned *gobs,
                           unsigned *size) {
    *bw = (w + f->block_w - 1u) / f->block_w;
    *br = (h + f->block_h - 1u) / f->block_h;
    unsigned row_bytes = *bw * f->bytes_per_block;
    *gobs = (row_bytes + 63u) / 64u;
    unsigned span = 8u * bh;
    *size = *gobs * ((*br + span - 1u) / span) * 512u * bh;
}

static const char *compute_layout(const Header *h, Level *levels, char *why,
                                  size_t why_size) {
    const Format *f = h->format;
    if (!f) {
        snprintf(why, why_size, "unsupported-format(0x%02x)", h->format_code);
        return "reject";
    }
    if (!f->decoder) {
        snprintf(why, why_size, "unsupported-format(%s)", f->name);
        return "reject";
    }
    if (h->resource_type != 1u) {
        snprintf(why, why_size, "unsupported-resource-type(0x%02x)", h->resource_type);
        return "reject";
    }
    unsigned block_rows0 = (h->height + f->block_h - 1u) / f->block_h;
    unsigned bh0 = pow2_ceil((block_rows0 + 7u) / 8u);
    if (bh0 > TEX_MAX_BH) bh0 = TEX_MAX_BH;
    if (bh0 < 1u) bh0 = 1u;
    uint64_t cursor = 0;
    unsigned w = h->width, hh = h->height;
    for (uint32_t mip = 0; mip < h->mip_count; ++mip) {
        unsigned bw, br, gobs, padded;
        unsigned bh = level_bh((hh + f->block_h - 1u) / f->block_h, bh0);
        level_geometry(w, hh, f, bh, &bw, &br, &gobs, &padded);
        uint32_t stored = (mip + 1u < h->mip_count)
                              ? h->offsets[mip + 1] - h->offsets[mip]
                              : h->payload_size - h->offsets[mip];
        if (h->offsets[mip] != cursor) {
            snprintf(why, why_size, "layout-offset-mismatch(mip=%u)", mip);
            return "layout-fail";
        }
        if (stored != padded) {
            snprintf(why, why_size, "layout-size-mismatch(mip=%u,%u!=%u)", mip,
                     stored, padded);
            return "layout-fail";
        }
        Level *lv = &levels[mip];
        lv->width = w;
        lv->height = hh;
        lv->block_columns = bw;
        lv->block_rows = br;
        lv->gob_columns = gobs;
        lv->bh = bh;
        lv->offset = h->offsets[mip];
        lv->stored_size = stored;
        lv->logical_size = bw * br * f->bytes_per_block;
        lv->padded_size = padded;
        lv->alignment = 512u * bh;
        cursor += padded;
        w = w > 1u ? w / 2u : 1u;
        hh = hh > 1u ? hh / 2u : 1u;
    }
    if (cursor != h->payload_size) {
        snprintf(why, why_size, "layout-total-mismatch(%llu!=%u)",
                 (unsigned long long)cursor, h->payload_size);
        return "layout-fail";
    }
    snprintf(why, why_size, "ok");
    return "ok";
}

static unsigned gob_byte_offset(unsigned x, unsigned y) {
    return (x / 32u) * 256u + (y / 2u) * 64u + ((x / 16u) % 2u) * 32u +
           (y % 2u) * 16u + (x % 16u);
}

static uint32_t tiled_offset(unsigned bx, unsigned by, unsigned blocks_w,
                             unsigned bpb, unsigned bh) {
    unsigned bytes_x = bx * bpb;
    unsigned gobs = (blocks_w * bpb + 63u) / 64u;
    return (by / (8u * bh)) * gobs * 512u * bh + (bytes_x / 64u) * 512u * bh +
           ((by / 8u) % bh) * 512u + gob_byte_offset(bytes_x % 64u, by % 8u);
}

static int untile_level(const uint8_t *payload, size_t payload_size,
                        const Level *lv, const Format *f, uint8_t *out,
                        size_t out_size) {
    size_t need = (size_t)lv->block_columns * lv->block_rows * f->bytes_per_block;
    if (out_size != need) return -1;
    if ((uint64_t)lv->offset + lv->stored_size > payload_size) return -1;
    const uint8_t *src = payload + lv->offset;
    for (unsigned by = 0; by < lv->block_rows; ++by) {
        for (unsigned bx = 0; bx < lv->block_columns; ++bx) {
            uint32_t at = tiled_offset(bx, by, lv->block_columns,
                                       f->bytes_per_block, lv->bh);
            if ((uint64_t)at + f->bytes_per_block > lv->stored_size) return -1;
            memcpy(out + ((size_t)by * lv->block_columns + bx) * f->bytes_per_block,
                   src + at, f->bytes_per_block);
        }
    }
    return 0;
}

static int retile_level(const uint8_t *linear, size_t linear_size, const Level *lv,
                        const Format *f, uint8_t *out, size_t out_size) {
    size_t need = (size_t)lv->block_columns * lv->block_rows * f->bytes_per_block;
    if (linear_size != need || out_size != lv->padded_size) return -1;
    memset(out, 0, out_size);
    for (unsigned by = 0; by < lv->block_rows; ++by) {
        for (unsigned bx = 0; bx < lv->block_columns; ++bx) {
            uint32_t at = tiled_offset(bx, by, lv->block_columns,
                                       f->bytes_per_block, lv->bh);
            if ((uint64_t)at + f->bytes_per_block > out_size) return -1;
            memcpy(out + at,
                   linear + ((size_t)by * lv->block_columns + bx) * f->bytes_per_block,
                   f->bytes_per_block);
        }
    }
    return 0;
}

static void rgb565(unsigned c, uint8_t out[4]) {
    unsigned r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
    out[3] = 255u;
}

static void bc1_colors(const uint8_t *block, uint8_t colors[4][4]) {
    unsigned c0 = block[0] | ((unsigned)block[1] << 8);
    unsigned c1 = block[2] | ((unsigned)block[3] << 8);
    rgb565(c0, colors[0]);
    rgb565(c1, colors[1]);
    if (c0 > c1) {
        for (unsigned c = 0; c < 3; ++c) {
            colors[2][c] = (uint8_t)((2u * colors[0][c] + colors[1][c]) / 3u);
            colors[3][c] = (uint8_t)((colors[0][c] + 2u * colors[1][c]) / 3u);
        }
        colors[2][3] = 255u;
        colors[3][3] = 255u;
    } else {
        for (unsigned c = 0; c < 3; ++c) {
            colors[2][c] = (uint8_t)((colors[0][c] + colors[1][c]) / 2u);
            colors[3][c] = 0u;
        }
        colors[2][3] = 255u;
        colors[3][3] = 0u;
    }
}

static void alpha_palette(const uint8_t *block, unsigned off, uint8_t palette[8]) {
    unsigned a0 = block[off], a1 = block[off + 1];
    palette[0] = (uint8_t)a0;
    palette[1] = (uint8_t)a1;
    if (a0 > a1) {
        for (unsigned i = 1; i <= 6u; ++i)
            palette[i + 1] = (uint8_t)(((7u - i) * a0 + i * a1) / 7u);
    } else {
        for (unsigned i = 1; i <= 4u; ++i)
            palette[i + 1] = (uint8_t)(((5u - i) * a0 + i * a1) / 5u);
        palette[6] = 0u;
        palette[7] = 255u;
    }
}

static int decode_level(const uint8_t *linear, size_t linear_size, const Level *lv,
                        const Format *f, uint8_t *rgba, size_t rgba_size,
                        unsigned *alpha_min, unsigned *alpha_max) {
    if (rgba_size != (size_t)lv->width * lv->height * 4u) return -1;
    *alpha_min = 255u;
    *alpha_max = 0u;
    if (!strcmp(f->name, "RGBA8_UNORM")) {
        if (linear_size < rgba_size) return -1;
        memcpy(rgba, linear, rgba_size);
    } else {
        unsigned bw = (lv->width + 3u) / 4u;
        unsigned bh = (lv->height + 3u) / 4u;
        if (linear_size != (size_t)bw * bh * f->bytes_per_block) return -1;
        for (unsigned by = 0; by < bh; ++by) {
            for (unsigned bx = 0; bx < bw; ++bx) {
                const uint8_t *block =
                    linear + ((size_t)by * bw + bx) * f->bytes_per_block;
                if (!strcmp(f->name, "BC4_UNORM") || !strcmp(f->name, "BC5_UNORM")) {
                    unsigned pairs = f->code == 0x1f ? 2u : 1u;
                    uint8_t chan[2][16];
                    for (unsigned p = 0; p < pairs; ++p) {
                        uint8_t pal[8];
                        alpha_palette(block, p * 8u, pal);
                        uint64_t bits = 0;
                        for (unsigned i = 0; i < 6u; ++i)
                            bits |= (uint64_t)block[p * 8u + 2u + i] << (8u * i);
                        for (unsigned i = 0; i < 16u; ++i)
                            chan[p][i] = pal[(bits >> (3u * i)) & 7u];
                    }
                    for (unsigned py = 0; py < 4u; ++py) {
                        for (unsigned px = 0; px < 4u; ++px) {
                            unsigned x = bx * 4u + px, y = by * 4u + py;
                            if (x >= lv->width || y >= lv->height) continue;
                            unsigned i = py * 4u + px;
                            uint8_t *o = rgba + ((size_t)y * lv->width + x) * 4u;
                            o[0] = chan[0][i];
                            o[1] = pairs > 1u ? chan[1][i] : 0u;
                            o[2] = 0u;
                            o[3] = 255u;
                        }
                    }
                } else {
                    const uint8_t *color_block;
                    uint8_t colors[4][4];
                    uint8_t pal[8];
                    uint64_t alpha_bits = 0;
                    int have_alpha_palette = 0, have_alpha_nibbles = 0;
                    if (!strcmp(f->name, "BC3_UNORM") || !strcmp(f->name, "BC3_UNORM_ALT")) {
                        color_block = block + 8;
                        alpha_palette(block, 0, pal);
                        have_alpha_palette = 1;
                        for (unsigned i = 0; i < 6u; ++i)
                            alpha_bits |= (uint64_t)block[2u + i] << (8u * i);
                    } else if (!strcmp(f->name, "BC2_UNORM")) {
                        color_block = block + 8;
                        have_alpha_nibbles = 1;
                        for (unsigned i = 0; i < 8u; ++i)
                            alpha_bits |= (uint64_t)block[i] << (8u * i);
                    } else {
                        color_block = block;
                    }
                    bc1_colors(color_block, colors);
                    uint32_t indices = (uint32_t)color_block[4] |
                                       ((uint32_t)color_block[5] << 8) |
                                       ((uint32_t)color_block[6] << 16) |
                                       ((uint32_t)color_block[7] << 24);
                    for (unsigned py = 0; py < 4u; ++py) {
                        for (unsigned px = 0; px < 4u; ++px) {
                            unsigned x = bx * 4u + px, y = by * 4u + py;
                            if (x >= lv->width || y >= lv->height) continue;
                            unsigned i = py * 4u + px;
                            uint8_t *o = rgba + ((size_t)y * lv->width + x) * 4u;
                            memcpy(o, colors[(indices >> (2u * i)) & 3u], 4u);
                            if (have_alpha_palette)
                                o[3] = pal[(alpha_bits >> (3u * i)) & 7u];
                            else if (have_alpha_nibbles)
                                o[3] = (uint8_t)(((alpha_bits >> (4u * i)) & 0xfu) * 17u);
                        }
                    }
                }
            }
        }
    }
    for (size_t i = 0; i < rgba_size; i += 4) {
        unsigned a = rgba[i + 3];
        if (a < *alpha_min) *alpha_min = a;
        if (a > *alpha_max) *alpha_max = a;
    }
    return 0;
}

static uint8_t *read_file(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *data = (uint8_t *)malloc((size_t)n ? (size_t)n : 1u);
    if (!data) { fclose(f); return NULL; }
    size_t got = fread(data, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(data); return NULL; }
    *size_out = got;
    return data;
}

static int check(int condition, const char *name) {
    printf("selftest.%s=%s\n", name, condition ? "ok" : "FAIL");
    return condition ? 0 : 1;
}

/* Build a synthetic TEX blob: mip_count levels, each level produced by retile. */
static uint8_t *build_synthetic(unsigned w, unsigned h, unsigned mip_count,
                                const Format *f, uint32_t *size_out) {
    Header hd;
    memset(&hd, 0, sizeof(hd));
    hd.mip_count = mip_count;
    hd.width = w;
    hd.height = h;
    hd.format = f;
    hd.format_code = (uint8_t)f->code;
    hd.resource_type = 1;
    unsigned block_rows0 = (h + f->block_h - 1u) / f->block_h;
    unsigned bh0 = pow2_ceil((block_rows0 + 7u) / 8u);
    if (bh0 > 16u) bh0 = 16u;
    uint64_t payload = 0;
    unsigned cw = w, ch = h;
    for (unsigned m = 0; m < mip_count; ++m) {
        unsigned bw, br, gobs, padded;
        unsigned bh = level_bh((ch + f->block_h - 1u) / f->block_h, bh0);
        level_geometry(cw, ch, f, bh, &bw, &br, &gobs, &padded);
        if (m > 0) hd.offsets[m] = (uint32_t)payload;
        payload += padded;
        cw = cw > 1u ? cw / 2u : 1u;
        ch = ch > 1u ? ch / 2u : 1u;
    }
    uint64_t need = TEX_HEADER_BASE + 4ull * (mip_count - 1u) + payload;
    uint8_t *blob = (uint8_t *)calloc(1, (size_t)need);
    if (!blob) return NULL;
    memcpy(blob, "TEX\0", 4);
    uint32_t packed = (mip_count & 0x3fu) | ((w & 0x1fffu) << 6) |
                      ((h & 0x1fffu) << 19);
    blob[4] = 0xa0; blob[5] = 0x80; blob[6] = 0x00; blob[7] = 0x20;
    memcpy(blob + 8, &packed, 4);
    blob[0x0c] = 1;
    blob[0x0d] = (uint8_t)f->code;
    blob[0x0e] = 1;
    blob[0x0f] = 0;
    uint32_t payload32 = (uint32_t)payload;
    memcpy(blob + 0x10, &payload32, 4);
    for (unsigned m = 1; m < mip_count; ++m) {
        uint32_t off = hd.offsets[m];
        memcpy(blob + TEX_HEADER_BASE + 4u * (m - 1u), &off, 4);
    }
    /* Fill each level with deterministic bytes via retile of a linear pattern. */
    cw = w; ch = h;
    unsigned level_off = 0;
    for (unsigned m = 0; m < mip_count; ++m) {
        Level lv;
        memset(&lv, 0, sizeof(lv));
        unsigned bw, br, gobs, padded;
        unsigned bh = level_bh((ch + f->block_h - 1u) / f->block_h, bh0);
        level_geometry(cw, ch, f, bh, &bw, &br, &gobs, &padded);
        lv.width = cw; lv.height = ch; lv.block_columns = bw; lv.block_rows = br;
        lv.bh = bh; lv.padded_size = padded;
        size_t lin_size = (size_t)bw * br * f->bytes_per_block;
        uint8_t *lin = (uint8_t *)malloc(lin_size);
        if (!lin) { free(blob); return NULL; }
        for (size_t i = 0; i < lin_size; ++i)
            lin[i] = (uint8_t)((i * 37u + m * 11u + 3u) & 0xffu);
        if (retile_level(lin, lin_size, &lv, f, blob + TEX_HEADER_BASE +
                         4u * (mip_count - 1u) + level_off, padded) != 0) {
            free(lin); free(blob); return NULL;
        }
        free(lin);
        level_off += padded;
        cw = cw > 1u ? cw / 2u : 1u;
        ch = ch > 1u ? ch / 2u : 1u;
    }
    *size_out = (uint32_t)need;
    return blob;
}

static int self_test(void) {
    int failures = 0;
    /* GOB address formula must be a bijection 0..511. */
    unsigned seen[512];
    memset(seen, 0, sizeof(seen));
    for (unsigned y = 0; y < 8u; ++y)
        for (unsigned x = 0; x < 64u; ++x) {
            unsigned at = gob_byte_offset(x, y);
            if (at >= 512u || seen[at]) failures++;
            seen[at] = 1;
        }
    failures += check(failures == 0, "gob_bijection");
    for (unsigned i = 0; i < 512u; ++i)
        if (!seen[i]) { failures++; break; }

    /* Known level sizes from the locked corpus. */
    const Format *bc1 = find_format(0x13), *bc5 = find_format(0x1f);
    const Format *rgba8 = find_format(0x07);
    unsigned bw, br, gobs, sz;
    level_geometry(1024, 1024, bc5, 16, &bw, &br, &gobs, &sz);
    failures += check(sz == 1048576u, "bc5_1024_bh16");
    level_geometry(512, 512, bc5, 16, &bw, &br, &gobs, &sz);
    failures += check(sz == 262144u, "bc5_512_bh16");
    level_geometry(32, 32, bc5, 1, &bw, &br, &gobs, &sz);
    failures += check(sz == 1024u, "bc5_32_bh1");
    level_geometry(16, 16, bc5, 1, &bw, &br, &gobs, &sz);
    failures += check(sz == 512u, "bc5_16_bh1");
    level_geometry(1, 1, bc1, 1, &bw, &br, &gobs, &sz);
    failures += check(sz == 512u, "bc1_1x1_min_tile");
    level_geometry(5, 3, bc1, 1, &bw, &br, &gobs, &sz);
    failures += check(sz == 512u, "bc1_odd_5x3");
    level_geometry(33, 17, bc1, 1, &bw, &br, &gobs, &sz);
    failures += check(sz == 1024u, "bc1_odd_33x17");

    /* Synthetic containers: retile/untile round trip through parse+layout. */
    struct { unsigned w, h, mips; const Format *f; const char *name; } cases[] = {
        {5, 3, 1, bc1, "roundtrip_odd_bc1"},
        {1, 1, 1, rgba8, "roundtrip_1x1_rgba8"},
        {64, 64, 7, bc1, "roundtrip_mips_bc1"},
        {16, 9, 4, bc5, "roundtrip_odd_bc5"},
        {3, 1, 2, rgba8, "roundtrip_tiny_rgba8"},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        uint32_t blob_size = 0;
        uint8_t *blob = build_synthetic(cases[c].w, cases[c].h, cases[c].mips,
                                        cases[c].f, &blob_size);
        int ok = blob != NULL;
        if (ok) {
            Header hd;
            char why[128];
            if (strcmp(parse_header(blob, blob_size, &hd, why, sizeof(why)), "ok") != 0)
                ok = 0;
            else {
                Level levels[TEX_MAX_MIPS];
                if (strcmp(compute_layout(&hd, levels, why, sizeof(why)), "ok") != 0)
                    ok = 0;
                else {
                    for (unsigned m = 0; m < hd.mip_count && ok; ++m) {
                        size_t lin_size = (size_t)levels[m].block_columns *
                                          levels[m].block_rows *
                                          cases[c].f->bytes_per_block;
                        uint8_t *lin = (uint8_t *)malloc(lin_size);
                        uint8_t *re = (uint8_t *)malloc(levels[m].padded_size);
                        if (!lin || !re ||
                            untile_level(blob + hd.payload_offset, hd.payload_size,
                                         &levels[m], cases[c].f, lin, lin_size) != 0 ||
                            retile_level(lin, lin_size, &levels[m], cases[c].f, re,
                                         levels[m].padded_size) != 0 ||
                            memcmp(re, blob + hd.payload_offset + levels[m].offset,
                                   levels[m].padded_size) != 0)
                            ok = 0;
                        free(lin); free(re);
                    }
                }
            }
            free(blob);
        }
        failures += check(ok, cases[c].name);
    }

    /* BC1 known block: c0 red > c1 blue. */
    {
        uint8_t block[8] = {0x00, 0xf8, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00};
        uint8_t colors[4][4];
        bc1_colors(block, colors);
        failures += check(colors[0][0] == 255 && colors[0][1] == 0 && colors[0][2] == 0,
                          "bc1_red");
        failures += check(colors[1][2] == 255 && colors[1][0] == 0, "bc1_blue");
        failures += check(colors[2][3] == 255 && colors[3][3] == 255, "bc1_four_color_alpha");
    }
    /* BC1 three-color mode (c0 <= c1): index 3 is transparent. */
    {
        uint8_t block[8] = {0x1f, 0x00, 0x00, 0xf8, 0xff, 0xff, 0xff, 0xff};
        uint8_t colors[4][4];
        bc1_colors(block, colors);
        failures += check(colors[3][3] == 0, "bc1_transparent_index");
        failures += check(colors[0][3] == 255, "bc1_opaque_index");
    }
    /* BC3 alpha palette: a0=255,a1=0; index 1 -> 0, index 0 -> 255. */
    {
        uint8_t block[16];
        memset(block, 0, sizeof(block));
        block[0] = 255; block[1] = 0;
        /* alpha indices: pixel0=0, pixel1=1, rest 0. */
        block[2] = 0x08;
        uint8_t pal[8];
        alpha_palette(block, 0, pal);
        failures += check(pal[0] == 255 && pal[1] == 0, "bc3_alpha_palette");
    }
    /* BC5 channels: endpoints R=10,G=20 -> RGBA R=10,G=20,A=255. */
    {
        uint8_t block[16];
        memset(block, 0, sizeof(block));
        block[0] = 10; block[1] = 10;   /* R endpoints */
        block[8] = 20; block[9] = 20;   /* G endpoints */
        Header hd;
        memset(&hd, 0, sizeof(hd));
        hd.width = 4; hd.height = 4; hd.mip_count = 1;
        hd.format = bc5; hd.format_code = 0x1f; hd.resource_type = 1;
        Level lv;
        memset(&lv, 0, sizeof(lv));
        lv.width = 4; lv.height = 4; lv.block_columns = 1; lv.block_rows = 1;
        lv.bh = 1; lv.padded_size = 512; lv.stored_size = 512;
        uint8_t rgba[64];
        unsigned amin = 0, amax = 0;
        failures += check(decode_level(block, 16, &lv, bc5, rgba, sizeof(rgba),
                                       &amin, &amax) == 0, "bc5_decode_call");
        failures += check(rgba[0] == 10 && rgba[1] == 20 && rgba[2] == 0 &&
                          rgba[3] == 255 && amin == 255 && amax == 255,
                          "bc5_channels");
    }
    /* Negative cases: explicit reject reasons. */
    {
        uint8_t small[0x40];
        memset(small, 0, sizeof(small));
        memcpy(small, "TEX\0", 4);
        Header hd;
        char why[128];
        small[0] = 'X';
        failures += check(strcmp(parse_header(small, sizeof(small), &hd, why,
                                              sizeof(why)), "reject") == 0,
                          "negative_bad_magic");
        small[0] = 'T'; small[1] = 'E'; small[2] = 'X'; small[3] = 0;
        /* mip count 0 */
        failures += check(strcmp(parse_header(small, sizeof(small), &hd, why,
                                              sizeof(why)), "reject") == 0,
                          "negative_mip_zero");
        /* A 4x4 BC1 one-mip container has a 512-byte payload. */
        uint8_t big[0x18 + 0x400];
        memset(big, 0, sizeof(big));
        memcpy(big, "TEX\0", 4);
        uint32_t packed = 1u | (4u << 6) | (4u << 19);
        memcpy(big + 8, &packed, 4);
        big[0x0c] = 1;
        big[0x0d] = 0x1e; big[0x0e] = 1; big[0x0f] = 0;
        uint32_t payload = 0x200;
        memcpy(big + 0x10, &payload, 4);
        Level levels[TEX_MAX_MIPS];
        failures += check(strcmp(parse_header(big, sizeof(big), &hd, why,
                                              sizeof(why)), "ok") == 0,
                          "negative_parse_ok");
        failures += check(strcmp(compute_layout(&hd, levels, why, sizeof(why)),
                                 "reject") == 0 &&
                          strstr(why, "unsupported-format") != NULL,
                          "negative_unsupported_format");
        /* cube resource type 2 */
        big[0x0d] = 0x13;
        big[0x0e] = 2;
        failures += check(strcmp(parse_header(big, sizeof(big), &hd, why,
                                              sizeof(why)), "ok") == 0 &&
                          strcmp(compute_layout(&hd, levels, why, sizeof(why)),
                                 "reject") == 0 &&
                          strstr(why, "resource-type") != NULL,
                          "negative_cube");
        /* 3D resource type 0x20 */
        big[0x0e] = 0x20;
        failures += check(strcmp(parse_header(big, sizeof(big), &hd, why,
                                              sizeof(why)), "ok") == 0 &&
                          strcmp(compute_layout(&hd, levels, why, sizeof(why)),
                                 "reject") == 0 &&
                          strstr(why, "resource-type") != NULL,
                          "negative_3d");
        /* layout mismatch: BC1 4x4 with payload 512 is exact; 1024 is not. */
        big[0x0e] = 1;
        big[0x0d] = 0x13;
        payload = 0x400;
        memcpy(big + 0x10, &payload, 4);
        failures += check(strcmp(parse_header(big, sizeof(big), &hd, why,
                                              sizeof(why)), "ok") == 0,
                          "negative_layout_parse");
        failures += check(strcmp(compute_layout(&hd, levels, why, sizeof(why)),
                                 "layout-fail") == 0, "negative_layout_mismatch");
        /* truncated payload */
        payload = 0x800;
        memcpy(big + 0x10, &payload, 4);
        failures += check(strcmp(parse_header(big, sizeof(big), &hd, why,
                                              sizeof(why)), "reject") == 0 &&
                          strstr(why, "truncated-payload") != NULL,
                          "negative_truncated_payload");
    }
    printf("selftest.result=%s\n", failures ? "FAIL" : "ok");
    return failures ? 1 : 0;
}

static int negative_mode(void) { return self_test(); }

static int file_mode(const char *path, unsigned want_mip, int have_mip) {
    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    if (!data) {
        printf("result=io-error\n");
        return 2;
    }
    Header hd;
    char why[256];
    const char *parsed = parse_header(data, size, &hd, why, sizeof(why));
    printf("parse=%s\n", parsed);
    printf("parse_reason=%s\n", why);
    if (strcmp(parsed, "ok") != 0) {
        free(data);
        return 1;
    }
    printf("mip_count=%u\nwidth=%u\nheight=%u\n", hd.mip_count, hd.width, hd.height);
    printf("format=0x%02x\nresource_type=0x%02x\n", hd.format_code, hd.resource_type);
    printf("payload_offset=%u\npayload_size=%u\ntrailing=%llu\n",
           hd.payload_offset, hd.payload_size, (unsigned long long)hd.trailing);
    Level levels[TEX_MAX_MIPS];
    const char *layout = compute_layout(&hd, levels, why, sizeof(why));
    printf("layout=%s\nlayout_reason=%s\n", layout, why);
    if (strcmp(layout, "ok") != 0) {
        free(data);
        return 1;
    }
    if (!have_mip) want_mip = 0;
    if (want_mip >= hd.mip_count) {
        printf("result=bad-mip\n");
        free(data);
        return 1;
    }
    const Level *lv = &levels[want_mip];
    printf("mip=%u\nlevel_width=%u\nlevel_height=%u\n", want_mip, lv->width, lv->height);
    printf("level_offset=%u\nlevel_stored=%u\nlevel_logical=%u\nlevel_padded=%u\n",
           lv->offset, lv->stored_size, lv->logical_size, lv->padded_size);
    printf("level_bh=%u\nlevel_alignment=%u\n", lv->bh, lv->alignment);
    size_t lin_size = (size_t)lv->block_columns * lv->block_rows *
                      hd.format->bytes_per_block;
    uint8_t *linear = (uint8_t *)malloc(lin_size);
    uint8_t *rgba = (uint8_t *)malloc((size_t)lv->width * lv->height * 4u);
    if (!linear || !rgba ||
        untile_level(data + hd.payload_offset, hd.payload_size, lv, hd.format,
                     linear, lin_size) != 0) {
        printf("result=untile-error\n");
        free(linear); free(rgba); free(data);
        return 1;
    }
    unsigned amin = 0, amax = 0;
    if (decode_level(linear, lin_size, lv, hd.format, rgba,
                     (size_t)lv->width * lv->height * 4u, &amin, &amax) != 0) {
        printf("result=decode-error\n");
        free(linear);
        free(rgba); free(data);
        return 1;
    }
    printf("linear_fnv1a64=%016llx\n", (unsigned long long)fnv1a64(linear, lin_size));
    printf("rgba_fnv1a64=%016llx\n",
           (unsigned long long)fnv1a64(rgba, (size_t)lv->width * lv->height * 4u));
    printf("alpha_min=%u\nalpha_max=%u\n", amin, amax);
    printf("result=ok\n");
    free(linear);
    free(rgba);
    free(data);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--self-test")) return self_test();
    if (argc >= 2 && !strcmp(argv[1], "--negative")) return negative_mode();
    if (argc >= 5 && !strcmp(argv[1], "--file") && !strcmp(argv[3], "--mip")) {
        char *end = NULL;
        unsigned mip = (unsigned)strtoul(argv[4], &end, 10);
        if (!end || *end) return 2;
        return file_mode(argv[2], mip, 1);
    }
    if (argc >= 3 && !strcmp(argv[1], "--file"))
        return file_mode(argv[2], 0, 0);
    fprintf(stderr,
            "usage: %s --self-test | --negative | --file <path> [--mip N]\n",
            argv[0]);
    return 2;
}

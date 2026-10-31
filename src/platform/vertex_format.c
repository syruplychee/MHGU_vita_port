#include "vertex_format.h"

#include <string.h>

#define VP_ATTR_BUFFER_MASK 31u
#define VP_ATTR_CONSTANT_BIT 64u
#define VP_ATTR_OFFSET_SHIFT 7u
#define VP_ATTR_OFFSET_MASK 0x3fffu
#define VP_ATTR_SIZE_SHIFT 21u
#define VP_ATTR_SIZE_MASK 63u
#define VP_ATTR_TYPE_SHIFT 27u
#define VP_ATTR_TYPE_MASK 7u
#define VP_ATTR_BGRA_SHIFT 31u

enum {
    VP_DK_SIZE_4X32 = 0x01,
    VP_DK_SIZE_3X32 = 0x02,
    VP_DK_SIZE_2X32 = 0x04,
    VP_DK_SIZE_1X32 = 0x12,
    VP_DK_SIZE_4X16 = 0x03,
    VP_DK_SIZE_3X16 = 0x05,
    VP_DK_SIZE_2X16 = 0x0f,
    VP_DK_SIZE_1X16 = 0x1b,
    VP_DK_SIZE_4X8 = 0x0a,
    VP_DK_SIZE_10_10_10_2 = 0x30
};

enum {
    VP_DK_TYPE_SNORM = 1,
    VP_DK_TYPE_UNORM = 2,
    VP_DK_TYPE_UINT = 4,
    VP_DK_TYPE_FLOAT = 7
};

static unsigned components_32(unsigned size) {
    return size == VP_DK_SIZE_4X32 ? 4u :
           size == VP_DK_SIZE_3X32 ? 3u :
           size == VP_DK_SIZE_2X32 ? 2u : 1u;
}

static unsigned components_16(unsigned size) {
    return size == VP_DK_SIZE_4X16 ? 4u :
           size == VP_DK_SIZE_3X16 ? 3u :
           size == VP_DK_SIZE_2X16 ? 2u : 1u;
}

int vp_vertex_format_decode(uint32_t descriptor, unsigned stride,
                            VpVertexFormat *out) {
    unsigned offset, size, type;
    VpVertexFormat value;

    if (out == NULL || stride == 0u) return -1;
    if ((descriptor & VP_ATTR_BUFFER_MASK) != 0u ||
        (descriptor & VP_ATTR_CONSTANT_BIT) != 0u ||
        (descriptor >> VP_ATTR_BGRA_SHIFT) != 0u)
        return -1;
    offset = (descriptor >> VP_ATTR_OFFSET_SHIFT) & VP_ATTR_OFFSET_MASK;
    size = (descriptor >> VP_ATTR_SIZE_SHIFT) & VP_ATTR_SIZE_MASK;
    type = (descriptor >> VP_ATTR_TYPE_SHIFT) & VP_ATTR_TYPE_MASK;

    memset(&value, 0, sizeof(value));
    value.source_offset = offset;
    if (type == VP_DK_TYPE_FLOAT) {
        if (size == VP_DK_SIZE_4X32 || size == VP_DK_SIZE_3X32 ||
            size == VP_DK_SIZE_2X32 || size == VP_DK_SIZE_1X32) {
            value.kind = VP_VERTEX_FORMAT_F32;
            value.components = components_32(size);
            value.source_bytes = value.components * 4u;
        } else if (size == VP_DK_SIZE_4X16 || size == VP_DK_SIZE_3X16 ||
                   size == VP_DK_SIZE_2X16 || size == VP_DK_SIZE_1X16) {
            value.kind = VP_VERTEX_FORMAT_F16;
            value.components = components_16(size);
            value.source_bytes = value.components * 2u;
        } else {
            return -1;
        }
    } else if (type == VP_DK_TYPE_UNORM) {
        if (size == VP_DK_SIZE_4X8) {
            value.kind = VP_VERTEX_FORMAT_U8N;
            value.components = 4u;
            value.source_bytes = 4u;
        } else if (size == VP_DK_SIZE_10_10_10_2) {
            value.kind = VP_VERTEX_FORMAT_RGB10A2_UNORM;
            value.components = 4u;
            value.source_bytes = 4u;
        } else {
            return -1;
        }
    } else if (type == VP_DK_TYPE_SNORM) {
        if (size == VP_DK_SIZE_4X16) {
            /* Already used by the captured character draws. */
            value.kind = VP_VERTEX_FORMAT_S16N;
            value.components = 4u;
            value.source_bytes = 8u;
        } else if (size == VP_DK_SIZE_4X8) {
            value.kind = VP_VERTEX_FORMAT_S8N;
            value.components = 4u;
            value.source_bytes = 4u;
        } else {
            return -1;
        }
    } else if (type == VP_DK_TYPE_UINT) {
        if (size == VP_DK_SIZE_4X8) {
            value.kind = VP_VERTEX_FORMAT_U8;
            value.components = 4u;
            value.source_bytes = 4u;
        } else {
            return -1;
        }
    } else {
        return -1;
    }
    if (offset > stride || value.source_bytes > stride - offset) return -1;
    *out = value;
    return 0;
}

int vp_vertex_format_check_input(uint32_t descriptor, int has_float, int has_uint) {
    unsigned type = (descriptor >> VP_ATTR_TYPE_SHIFT) & VP_ATTR_TYPE_MASK;
    if (type == VP_DK_TYPE_UINT)
        return has_uint && !has_float ? 0 : -1;
    return has_float && !has_uint ? 0 : -1;
}

void vp_vertex_rgb10a2_to_float4(uint32_t packed, float out[4]) {
    if (out == NULL) return;
    out[0] = (float)(packed & 0x3ffu) / 1023.0f;
    out[1] = (float)((packed >> 10) & 0x3ffu) / 1023.0f;
    out[2] = (float)((packed >> 20) & 0x3ffu) / 1023.0f;
    out[3] = (float)((packed >> 30) & 3u) / 3.0f;
}

int vp_vertex_expand_rgb10a2(const void *vertices, unsigned vertex_count,
                             unsigned stride, const uint32_t descriptors[16],
                             uint32_t location_mask, void *out, size_t capacity,
                             unsigned *out_stride) {
    unsigned offsets[VP_VERTEX_PACK_SLOTS];
    unsigned count = 0, location;
    size_t needed;

    if (vertices == NULL || descriptors == NULL || out == NULL ||
        out_stride == NULL || stride == 0u || vertex_count == 0u ||
        location_mask == 0u || (location_mask & ~0xffffu) != 0u)
        return -1;
    for (location = 0; location < 16u; ++location) {
        VpVertexFormat format;
        if ((location_mask & (1u << location)) == 0u) continue;
        if (vp_vertex_format_decode(descriptors[location], stride, &format) < 0 ||
            format.kind != VP_VERTEX_FORMAT_RGB10A2_UNORM)
            return -1;
        if (count == VP_VERTEX_PACK_SLOTS) return -1;
        offsets[count] = format.source_offset;
        ++count;
    }
    if (count == 0u) return -1;
    if ((size_t)vertex_count > capacity / (16u * (size_t)count)) return -1;
    needed = (size_t)vertex_count * 16u * (size_t)count;
    if (needed > capacity) return -1;
    for (unsigned vertex = 0; vertex < vertex_count; ++vertex) {
        const uint8_t *source = (const uint8_t *)vertices + (size_t)vertex * stride;
        uint8_t *destination = (uint8_t *)out + (size_t)vertex * 16u * count;
        for (unsigned slot = 0; slot < count; ++slot) {
            uint32_t packed;
            float values[4];
            memcpy(&packed, source + offsets[slot], sizeof(packed));
            vp_vertex_rgb10a2_to_float4(packed, values);
            memcpy(destination + slot * 16u, values, sizeof(values));
        }
    }
    (void)needed;
    *out_stride = 16u * count;
    return (int)count;
}

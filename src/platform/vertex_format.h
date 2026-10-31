/* Maxwell vertex-attribute descriptor decoding shared by the legacy and static
 * GXM paths.
 *
 * The bit layout is the one already used by the reviewed draw path:
 *   buffer [4:0], constant [6], offset [20:7], size [26:21], type [29:27],
 *   BGRA [31]. `size` is the packed DkVtxAttribSize code (deko3d's primary
 * source naming), not a component count. Unknown combinations are rejected;
 * they are never reinterpreted as a different format.
 */
#ifndef VP_VERTEX_FORMAT_H
#define VP_VERTEX_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define VP_VERTEX_PACK_SLOTS 4u

typedef enum {
    VP_VERTEX_FORMAT_F32 = 0,
    VP_VERTEX_FORMAT_U8N,
    VP_VERTEX_FORMAT_S16N,
    VP_VERTEX_FORMAT_S8N,
    VP_VERTEX_FORMAT_U8,
    VP_VERTEX_FORMAT_F16,
    /* RGB10A2 UNORM has no direct GXM vertex format; the runtime expands it
     * into four F32 components on a second vertex stream. */
    VP_VERTEX_FORMAT_RGB10A2_UNORM
} VpVertexFormatKind;

typedef struct {
    VpVertexFormatKind kind;
    unsigned components;      /* components exposed to the shader */
    unsigned source_offset;   /* byte offset in the guest vertex stream */
    unsigned source_bytes;    /* bytes consumed in the guest vertex stream */
} VpVertexFormat;

/* Decode one descriptor. Returns 0 on success and -1 for any malformed or
 * unsupported descriptor (including offset/size overruns). */
int vp_vertex_format_decode(uint32_t descriptor, unsigned stride,
                            VpVertexFormat *out);

/* Check the descriptor type against the GXP reflection inputs at one location.
 * A UINT descriptor needs int4 in_attrN_uint; every other accepted type needs
 * the float in_attrN. Declaring both is ambiguous and rejected. */
int vp_vertex_format_check_input(uint32_t descriptor, int has_float, int has_uint);

/* Convert one RGB10A2 UNORM word (R in bits 0..9, G 10..19, B 20..29, A 30..31)
 * to float4 in the same component order. */
void vp_vertex_rgb10a2_to_float4(uint32_t packed, float out[4]);

/* Expand the packed attributes selected by `location_mask` into a tightly
 * packed float4 stream. `descriptors` is the 16-entry Maxwell array in
 * attribute-location order. Returns the number of expanded attributes and sets
 * out_stride to 16 * count, or -1 when the selection, source range or capacity
 * is invalid. */
int vp_vertex_expand_rgb10a2(const void *vertices, unsigned vertex_count,
                             unsigned stride, const uint32_t descriptors[16],
                             uint32_t location_mask, void *out, size_t capacity,
                             unsigned *out_stride);

#endif

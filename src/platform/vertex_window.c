#include "vertex_window.h"
#include "gpu_indices.h"

#include <string.h>

_Static_assert(VP_VERTEX_WINDOW_MAX_STREAM_BYTES <= 512u * 1024u,
               "two vertex streams must stay within 512 KiB");
_Static_assert(VP_VERTEX_WINDOW_MAX_INDEX_BYTES == 65536u * 4u,
               "index staging stays at 65,536 32-bit indices");

static uint32_t index_mask(unsigned index_size) {
    return index_size == 1u ? 0xffu : index_size == 2u ? 0xffffu : 0xffffffffu;
}

static uint32_t out_mask(unsigned out_size) {
    return out_size == 2u ? 0xffffu : 0xffffffffu;
}

int vp_vertex_window_plan(const void *indices, unsigned index_count,
                          unsigned index_size, int restart_enabled,
                          uint32_t restart_index, int32_t base_vertex,
                          unsigned stride, int index_identity_required,
                          VpVertexWindowPlan *out) {
    VpGpuIndexRange range;
    int64_t first_vertex, last_vertex;
    uint64_t span;
    int scanned;

    if (out == NULL) return VP_VERTEX_WINDOW_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->base_vertex = base_vertex;
    if (indices == NULL || (index_size != 1u && index_size != 2u && index_size != 4u))
        return VP_VERTEX_WINDOW_INVALID_ARGUMENT;
    if (stride == 0u) return VP_VERTEX_WINDOW_INVALID_ARGUMENT;
    if (index_count > VP_VERTEX_WINDOW_MAX_INDEX_COUNT)
        return VP_VERTEX_WINDOW_INDEX_LIMIT;

    scanned = vp_gpu_index_range(indices, index_size, index_count,
                                 restart_enabled, restart_index, &range);
    if (scanned < 0) return VP_VERTEX_WINDOW_INVALID_ARGUMENT;
    out->first_index = range.minimum;
    out->min_index = range.minimum;
    out->max_index = range.maximum;
    out->real_indices = range.real_indices;
    out->restart_indices = range.restart_indices;
    if (scanned == 0) return VP_VERTEX_WINDOW_EMPTY;

    first_vertex = (int64_t)range.minimum + (int64_t)base_vertex;
    last_vertex = (int64_t)range.maximum + (int64_t)base_vertex;
    if (first_vertex < 0) return VP_VERTEX_WINDOW_BASE_UNDERFLOW;
    if (last_vertex > (int64_t)UINT32_MAX) return VP_VERTEX_WINDOW_BASE_OVERFLOW;
    span = (uint64_t)last_vertex - (uint64_t)first_vertex + 1u;
    if (span > (uint64_t)UINT32_MAX) return VP_VERTEX_WINDOW_SPAN_OVERFLOW;

    out->first_vertex = (uint32_t)first_vertex;
    out->vertex_count = (uint32_t)span;
    /* Both operands are 32-bit, so the 64-bit products cannot wrap. */
    out->source_offset = (uint64_t)out->first_vertex * stride;
    out->source_bytes = span * stride;

    /* VertexIdCopy exposes the original vertex identity as an attribute. That
     * copy is not emulated by the GXM path, so refuse every such draw even
     * when the window starts at zero. */
    if (index_identity_required)
        return VP_VERTEX_WINDOW_IDENTITY_REQUIRED;
    if (span > VP_VERTEX_WINDOW_MAX_VERTICES)
        return VP_VERTEX_WINDOW_COUNT_LIMIT;
    if (out->source_bytes > VP_VERTEX_WINDOW_MAX_BYTES)
        return VP_VERTEX_WINDOW_BYTE_LIMIT;
    return VP_VERTEX_WINDOW_OK;
}

int vp_vertex_window_remap(const void *indices, unsigned index_count,
                           unsigned index_size, int restart_enabled,
                           uint32_t restart_index, uint32_t window_first,
                           void *out, size_t out_capacity, unsigned out_size,
                           uint32_t canonical_restart) {
    uint32_t marker, limit, value;
    unsigned i;

    if (indices == NULL || out == NULL ||
        (index_size != 1u && index_size != 2u && index_size != 4u) ||
        (out_size != 2u && out_size != 4u))
        return -1;
    if (index_count != 0u && (size_t)index_count > out_capacity / out_size)
        return -1;
    marker = restart_index & index_mask(index_size);
    limit = out_mask(out_size);
    if (restart_enabled && canonical_restart > limit) return -1;
    /* Same width, unshifted and marker-free: every real index is already its
     * final value, so a bulk copy is exact even for unaligned staging buffers.
     * U8 widening, shifted windows and restart remapping keep the scalar path
     * where the underflow, collision and width checks are observable. */
    if (index_size == out_size && window_first == 0u && !restart_enabled) {
        memcpy(out, indices, (size_t)index_count * out_size);
        return 0;
    }
    for (i = 0; i < index_count; i++) {
        uint32_t raw = vp_gpu_index_read(indices, index_size, i);
        if (restart_enabled && raw == marker) {
            value = canonical_restart;
        } else {
            if (raw < window_first) return -1;
            value = raw - window_first;
            /* A remapped real index must never collide with the marker or the
             * splitter would create a primitive boundary that the source did
             * not have. Windows are bounded well below the canonical marker,
             * but reject any pathological contract explicitly. */
            if (restart_enabled && value == canonical_restart) return -1;
            if (value > limit) return -1;
        }
        if (out_size == 2u) {
            uint16_t narrow = (uint16_t)value;
            memcpy((uint8_t *)out + (size_t)i * 2u, &narrow, 2u);
        } else memcpy((uint8_t *)out + (size_t)i * 4u, &value, 4u);
    }
    return 0;
}

const char *vp_vertex_window_result_name(int result) {
    switch (result) {
    case VP_VERTEX_WINDOW_OK: return "ok";
    case VP_VERTEX_WINDOW_EMPTY: return "all-restart-or-empty";
    case VP_VERTEX_WINDOW_INVALID_ARGUMENT: return "invalid-argument";
    case VP_VERTEX_WINDOW_INVALID_FORMAT: return "invalid-format";
    case VP_VERTEX_WINDOW_BASE_UNDERFLOW: return "base-underflow";
    case VP_VERTEX_WINDOW_BASE_OVERFLOW: return "base-overflow";
    case VP_VERTEX_WINDOW_SPAN_OVERFLOW: return "span-overflow";
    case VP_VERTEX_WINDOW_IDENTITY_REQUIRED: return "vertex-identity-required";
    case VP_VERTEX_WINDOW_COUNT_LIMIT: return "count-limit";
    case VP_VERTEX_WINDOW_BYTE_LIMIT: return "byte-limit";
    case VP_VERTEX_WINDOW_INDEX_LIMIT: return "index-count-limit";
    default: return "unknown";
    }
}

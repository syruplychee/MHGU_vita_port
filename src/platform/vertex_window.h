/* Bounded indexed-vertex window planning shared by the full-frame draw path
 * and the host tests.
 *
 * Maxwell indexed draws address a vertex stream through the index buffer.
 * `global_base_vertex_index` (method 0x50d, NVN 0x1434) is added to every
 * index before the vertex fetch, exactly like glDrawElementsBaseVertex. The
 * old runtime used max(index)+1 as a dense vertex count and always started at
 * the stream base; large shared vertex buffers therefore looked like huge
 * draws even when the indices referenced a small window.
 *
 * This module computes the real source window, applies the base vertex once
 * with signed range checks, and remaps indices into that window. The full-frame
 * staging budget is a measured finite 8192 vertices / 256 KiB per stream. A
 * window that still exceeds it is refused explicitly; the budget is not
 * unbounded and larger observed windows remain uncovered.
 */
#ifndef VP_VERTEX_WINDOW_H
#define VP_VERTEX_WINDOW_H

#include <stddef.h>
#include <stdint.h>

/* Full-frame staging limits. They are measured budgets, not claims that every
 * observed window fits. Both are checked before any copy or allocation. */
#define VP_VERTEX_WINDOW_MAX_VERTICES 8192u
#define VP_VERTEX_WINDOW_MAX_BYTES (256u * 1024u)
/* Two vertex streams (source + RGB10A2 expansion) are fixed at <= 512 KiB. */
#define VP_VERTEX_WINDOW_MAX_STREAM_BYTES (2u * VP_VERTEX_WINDOW_MAX_BYTES)
/* Index staging is unchanged: 65,536 indices, widened to at most 32 bits. */
#define VP_VERTEX_WINDOW_MAX_INDEX_COUNT 65536u
#define VP_VERTEX_WINDOW_MAX_INDEX_BYTES (VP_VERTEX_WINDOW_MAX_INDEX_COUNT * 4u)

typedef enum {
    VP_VERTEX_WINDOW_OK = 0,
    /* Every index is a primitive-restart marker (or the buffer is empty).
     * The draw produces no primitives and must be skipped explicitly. */
    VP_VERTEX_WINDOW_EMPTY = 1,
    VP_VERTEX_WINDOW_INVALID_ARGUMENT = -1,
    VP_VERTEX_WINDOW_INVALID_FORMAT = -2,
    VP_VERTEX_WINDOW_BASE_UNDERFLOW = -3,
    VP_VERTEX_WINDOW_BASE_OVERFLOW = -4,
    VP_VERTEX_WINDOW_SPAN_OVERFLOW = -5,
    /* The draw exposes vertex identity through the unimplemented VertexIdCopy
     * attribute path. GXM has no vertex-id input and the copy is not emulated,
     * so no window (shifted or not) can be replayed faithfully. */
    VP_VERTEX_WINDOW_IDENTITY_REQUIRED = -6,
    VP_VERTEX_WINDOW_COUNT_LIMIT = -7,
    VP_VERTEX_WINDOW_BYTE_LIMIT = -8,
    VP_VERTEX_WINDOW_INDEX_LIMIT = -9
} VpVertexWindowResult;

typedef struct {
    uint32_t first_index;    /* raw minimum index (restart markers excluded) */
    uint32_t first_vertex;   /* effective first source vertex: min + base */
    uint32_t vertex_count;   /* source window length, not max(index)+1 */
    uint32_t min_index;      /* raw minimum, kept for diagnostics */
    uint32_t max_index;      /* raw maximum, kept for diagnostics */
    uint32_t real_indices;   /* non-restart index count */
    uint32_t restart_indices;/* restart marker count */
    int32_t base_vertex;     /* applied exactly once to first_vertex */
    uint64_t source_offset;  /* first_vertex * stride bytes */
    uint64_t source_bytes;   /* vertex_count * stride bytes */
} VpVertexWindowPlan;

/* Plan a bounded source window. `restart_index` is the raw Maxwell marker;
 * it is masked to the index width. `index_identity_required` must be nonzero
 * when the draw exposes the vertex id as a shader input (VertexIdCopy, method
 * 0x587). Such a draw is refused unconditionally because the copy attribute is
 * not emulated; a zero shift would not make it faithful. Returns
 * VP_VERTEX_WINDOW_OK, VP_VERTEX_WINDOW_EMPTY, or a negative result. On any
 * return the plan is filled with every field computed before the failure so
 * the caller can log the measured span. */
int vp_vertex_window_plan(const void *indices, unsigned index_count,
                          unsigned index_size, int restart_enabled,
                          uint32_t restart_index, int32_t base_vertex,
                          unsigned stride, int index_identity_required,
                          VpVertexWindowPlan *out);

/* Copy/remap indices into a staging buffer. Real indices are
 * remapped as `raw - window_first`; restart markers are replaced by
 * `canonical_restart` so the GXM splitter can compare a single marker.
 * Same-width, window-zero, restart-free requests are copied in bulk (unaligned
 * buffers included); the scalar path keeps every shift/restart/width check.
 * `out_size` must be 2 or 4. Returns 0 on success, -1 for an invalid argument,
 * a real index below window_first, a value that cannot be represented in
 * out_size, or an output buffer overflow. */
int vp_vertex_window_remap(const void *indices, unsigned index_count,
                           unsigned index_size, int restart_enabled,
                           uint32_t restart_index, uint32_t window_first,
                           void *out, size_t out_capacity, unsigned out_size,
                           uint32_t canonical_restart);

const char *vp_vertex_window_result_name(int result);

#endif

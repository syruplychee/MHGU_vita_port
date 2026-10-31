/* B111: pure clear-fusion enable policy and per-request clear ordering.
 *
 * The runtime owns the GXM surfaces and the actual clear/blit calls. This
 * header only decides:
 *   - whether full-surface colour clear fusion is enabled (explicit markers,
 *     the bounded-batch default, and the legacy/diagnostic fallback);
 *   - whether one representable clear request defers its full RGBA colour
 *     value, must materialize an older pending value first, or issues the
 *     actual surface clear;
 *   - whether the same request's depth/stencil bookkeeping must run.
 *
 * It is free of Vita SDK types so the ordering can be exercised on the host.
 * Callers must finish every depth/stencil validation and refusal before
 * consulting the plan: a refused request must have no colour side effect.
 */
#ifndef VP_CLEAR_FUSION_H
#define VP_CLEAR_FUSION_H

#include <stdint.h>

/* Guest clear mask: bits 0/1 are depth/stencil, bits 2..5 are R/G/B/A. */
#define VP_CLEAR_FUSION_DEPTH_MASK 0x03u
#define VP_CLEAR_FUSION_COLOR_MASK 0x3cu

typedef enum {
    VP_CLEAR_FUSION_SOURCE_DISABLE = 0, /* clear-fusion.disable marker */
    VP_CLEAR_FUSION_SOURCE_ENABLE = 1,  /* clear-fusion.enable marker */
    VP_CLEAR_FUSION_SOURCE_LEGACY = 2,  /* no marker; standalone path */
    VP_CLEAR_FUSION_SOURCE_DEFAULT = 3  /* no marker; bounded-batch default */
} VPClearFusionSource;

typedef struct {
    int enabled;
    VPClearFusionSource source;
} VPClearFusionSetting;

/* The disable marker wins over the enable marker and over the default. An
 * explicit enable stays effective for old tools, direct diagnostics and the
 * batch self test. The default requires ordinary bounded-batch gameplay: the
 * static shader path is active, the batch is enabled, no auxiliary diagnostic
 * marker is present and the self-test override is not active. */
static inline VPClearFusionSetting vp_clear_fusion_resolve(
        int disable_marker, int enable_marker, int static_mode,
        int bounded_batch_enabled, int aux_disabled, int test_mode) {
    VPClearFusionSetting out;
    if (disable_marker) {
        out.enabled = 0;
        out.source = VP_CLEAR_FUSION_SOURCE_DISABLE;
        return out;
    }
    if (enable_marker) {
        out.enabled = 1;
        out.source = VP_CLEAR_FUSION_SOURCE_ENABLE;
        return out;
    }
    if (!static_mode || test_mode >= 0 || !bounded_batch_enabled ||
        aux_disabled) {
        out.enabled = 0;
        out.source = VP_CLEAR_FUSION_SOURCE_LEGACY;
        return out;
    }
    out.enabled = 1;
    out.source = VP_CLEAR_FUSION_SOURCE_DEFAULT;
    return out;
}

static inline int vp_clear_fusion_writes_colour(uint32_t mask) {
    return (mask & VP_CLEAR_FUSION_COLOR_MASK) != 0u;
}

static inline int vp_clear_fusion_full_colour(uint32_t mask) {
    return (mask & VP_CLEAR_FUSION_COLOR_MASK) == VP_CLEAR_FUSION_COLOR_MASK;
}

typedef struct {
    int refused;             /* an earlier validation refused the request */
    int materialize_pending; /* commit an older pending colour before writing */
    int defer_colour;        /* fold this full RGBA clear into the next scene */
    int surface_clear;       /* issue the actual surface clear now */
    int apply_depth;         /* depth/stencil bookkeeping runs exactly once */
} VPClearFusionPlan;

/* Plan one clear after all depth/stencil validation. `representable` is 0 when
 * an earlier rule refused the request; every effect flag then stays 0.
 *
 * Ordering rules:
 *   - a later full RGBA clear coalesces with an older pending full clear: the
 *     last colour wins and the older value is not materialized;
 *   - a later colour write that cannot coalesce (partial channels, or a full
 *     clear that cannot defer because a B34 dump is due) materializes the old
 *     pending value first, so materializing the old value can never overwrite
 *     the newer clear;
 *   - a depth-only clear writes no colour and leaves the pending colour
 *     untouched;
 *   - depth/stencil bookkeeping is independent of the colour decision, so a
 *     combined colour+depth clear still applies/records its depth exactly once
 *     while the colour part is deferred. */
static inline VPClearFusionPlan vp_clear_fusion_plan(
        int representable, int fusion_enabled, int has_color_mem,
        int color_pending, uint32_t mask, int dump_due,
        unsigned depth_apply_mask) {
    VPClearFusionPlan out;
    out.refused = 0;
    out.materialize_pending = 0;
    out.defer_colour = 0;
    out.surface_clear = 0;
    out.apply_depth = 0;
    if (!representable) {
        out.refused = 1;
        return out;
    }
    out.defer_colour = fusion_enabled && has_color_mem &&
                       vp_clear_fusion_full_colour(mask) && !dump_due;
    out.materialize_pending = color_pending &&
                              vp_clear_fusion_writes_colour(mask) &&
                              !out.defer_colour;
    out.surface_clear = !out.defer_colour;
    out.apply_depth = depth_apply_mask != 0u;
    return out;
}

#endif /* VP_CLEAR_FUSION_H */

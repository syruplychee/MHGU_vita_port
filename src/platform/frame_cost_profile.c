#include "frame_cost_profile.h"

#include <string.h>

const char *const vp_frame_cost_category_names[VP_FRAME_COST_CATEGORY_COUNT] = {
    "entry",
    "dispatch",
    "mme",
    "draw-submit",
    "clear-submit",
    "tex-lookup",
    "tex-store",
    "tex-decode",
    "tex-tail",
    "gxm-draw",
    "gxm-clear",
    "gxm-flush",
    "wait-notify",
    "wait-finish",
    "draw-state",
    "draw-index-window",
    "draw-cbuf-resolve",
    "draw-resources",
    "draw-final",
    "gxm-request",
    "worker-tail",
    "log"
};

void vp_frame_cost_profile_reset(VPFrameCostProfile *profile) {
    if (profile == NULL) return;
    memset(profile, 0, sizeof(*profile));
}

static int vp_frame_cost_valid_category(unsigned category) {
    return category < VP_FRAME_COST_CATEGORY_COUNT;
}

int vp_frame_cost_span_begin(VPFrameCostProfile *profile, unsigned category,
                             uint64_t now_us) {
    VPFrameCostSpan *span;
    if (profile == NULL) return 0;
    /* Zero is the runtime's "timing inactive" sentinel. It must not create a
     * span or an accounting error: a disabled run performs no clock work. */
    if (now_us == 0u) return 0;
    if (!vp_frame_cost_valid_category(category)) {
        profile->mismatch++;
        return 0;
    }
    if (profile->depth >= VP_FRAME_COST_STACK) {
        profile->overflow++;
        return 0;
    }
    span = &profile->stack[profile->depth++];
    span->category = category;
    span->start_us = now_us;
    span->child_us = 0u;
    return 1;
}

int vp_frame_cost_span_end(VPFrameCostProfile *profile, unsigned category,
                           uint64_t now_us) {
    VPFrameCostSpan *span;
    VPFrameCostCategory *stats;
    uint64_t elapsed, self_us;
    unsigned depth;

    if (profile == NULL) return -1;
    if (now_us == 0u) {
        profile->missing_time++;
        return -1;
    }
    if (profile->depth == 0u) {
        profile->underflow++;
        return -1;
    }
    depth = profile->depth - 1u;
    span = &profile->stack[depth];
    if (!vp_frame_cost_valid_category(category) || span->category != category) {
        /* Keep the stack intact so the caller's unmatched end remains visible
         * through underflow/mismatch instead of silently closing a different
         * span. */
        profile->mismatch++;
        return -1;
    }
    if (now_us < span->start_us) {
        profile->clock_regression++;
        elapsed = 0u;
    } else {
        elapsed = now_us - span->start_us;
    }
    if (span->child_us > elapsed) {
        profile->underflow++;
        self_us = 0u;
    } else {
        self_us = elapsed - span->child_us;
    }
    stats = &profile->categories[category];
    stats->calls++;
    stats->inclusive_us += elapsed;
    stats->self_us += self_us;
    if (elapsed > stats->max_us) stats->max_us = elapsed;
    if (profile->depth == 1u) {
        stats->root_calls++;
        stats->root_inclusive_us += elapsed;
    }
    profile->depth = depth;
    if (profile->depth != 0u) {
        VPFrameCostSpan *parent = &profile->stack[profile->depth - 1u];
        parent->child_us += elapsed;
    }
    return 0;
}

uint64_t vp_frame_cost_error_count(const VPFrameCostProfile *profile) {
    if (profile == NULL) return 0u;
    return profile->mismatch + profile->overflow + profile->underflow +
           profile->clock_regression + profile->missing_time +
           profile->incomplete_snapshot;
}

int vp_frame_cost_parse_mode(const char *text, size_t length, unsigned *mode) {
    size_t i;
    if (text == NULL || mode == NULL || length == 0u) return -1;
    if (text[0] != '0' && text[0] != '1') return -1;
    for (i = 1u; i < length; ++i) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == '\v' || c == '\f') continue;
        return -1;
    }
    *mode = (unsigned)(text[0] - '0');
    return 0;
}

/* Pure bounded-batch admission and lifetime policy.
 *
 * The runtime owns GPU resources; this header only decides whether one draw may
 * join the currently open batch and tracks the bounded pending set. It is kept
 * free of Vita SDK types so the same invariants can be exhausted on the host.
 *
 * A caller must flush the current batch before mutating any resource that its
 * pending draws reference (pair/resident cache, depth pool, surface storage,
 * texture backing). The policy refuses a new identity instead of silently
 * reinterpreting it.
 */
#ifndef VP_BATCH_POLICY_H
#define VP_BATCH_POLICY_H

#include <stddef.h>
#include <stdint.h>

/* Entry capacity is independent of staging and per-scene uniform capacity. */
#define VP_BATCH_MAX_DRAWS 128u
/* Two lazily allocated 8 MiB pages can serve one Scene. Actual page reuse is
 * controlled by the runtime's arena completion notifications. */
#define VP_BATCH_STAGING_BUDGET (16u * 1024u * 1024u)

enum {
    VP_BATCH_ACCEPT = 0,
    VP_BATCH_FLUSH_SCOPE = 1,
    VP_BATCH_FLUSH_RT = 2,
    VP_BATCH_FLUSH_DEPTH = 3,
    VP_BATCH_FLUSH_MODE = 4,
    VP_BATCH_FLUSH_LIMIT = 5,
    VP_BATCH_FLUSH_BUDGET = 6,
    VP_BATCH_REFUSE_TOO_LARGE = 7
};

enum {
    VP_BATCH_BOUNDARY_END_SUBMIT = 0,
    VP_BATCH_BOUNDARY_CLEAR = 1,
    VP_BATCH_BOUNDARY_READBACK = 2,
    VP_BATCH_BOUNDARY_PRESENT = 3,
    VP_BATCH_BOUNDARY_RESOURCE = 4,
    VP_BATCH_BOUNDARY_DEADLINE = 5,
    VP_BATCH_BOUNDARY_FAILURE = 6
};

typedef struct {
    int open;
    int depth_only;
    uint64_t scope;
    uint64_t target_va;
    unsigned target_width;
    unsigned target_height;
    uint32_t target_format;
    uint64_t depth_va;
    uint32_t depth_format;
    unsigned depth_width;
    unsigned depth_height;
    int depth_attachment;
    unsigned draws;
    size_t bytes_used;
    unsigned resources_pinned;
    unsigned max_draws_seen;
} VPBatchPolicy;

typedef struct {
    unsigned encoded;
    unsigned completed;
    unsigned failed;
    unsigned flushes;
    unsigned sync_fallbacks;
    unsigned refused;
    unsigned max_batch;
    unsigned max_bytes;
    unsigned commits_dropped;
} VPBatchPolicyStats;

static inline void vp_batch_policy_init(VPBatchPolicy *policy,
                                        VPBatchPolicyStats *stats) {
    if (policy != NULL) {
        size_t bytes = sizeof(*policy);
        unsigned char *p = (unsigned char *)policy;
        for (size_t i = 0; i < bytes; ++i) p[i] = 0;
    }
    if (stats != NULL) {
        size_t bytes = sizeof(*stats);
        unsigned char *p = (unsigned char *)stats;
        for (size_t i = 0; i < bytes; ++i) p[i] = 0;
    }
}

static inline int vp_batch_policy_identity_matches(const VPBatchPolicy *p,
                                                   int depth_only,
                                                   uint64_t target_va,
                                                   unsigned target_width,
                                                   unsigned target_height,
                                                   uint32_t target_format,
                                                   int depth_attachment,
                                                   uint64_t depth_va,
                                                   uint32_t depth_format,
                                                   unsigned depth_width,
                                                   unsigned depth_height,
                                                   unsigned *reason) {
    if (p->depth_only != depth_only) {
        if (reason != NULL) *reason = VP_BATCH_FLUSH_MODE;
        return 0;
    }
    if (!depth_only &&
        (p->target_va != target_va || p->target_width != target_width ||
         p->target_height != target_height ||
         p->target_format != target_format)) {
        if (reason != NULL) *reason = VP_BATCH_FLUSH_RT;
        return 0;
    }
    if (p->depth_attachment != depth_attachment ||
        (depth_attachment &&
         (p->depth_va != depth_va || p->depth_format != depth_format ||
          p->depth_width != depth_width || p->depth_height != depth_height))) {
        if (reason != NULL) *reason = VP_BATCH_FLUSH_DEPTH;
        return 0;
    }
    return 1;
}

/* Returns VP_BATCH_ACCEPT when the draw may join. Otherwise the caller must
 * flush the open batch (unless the reason is REFUSE_TOO_LARGE), then call again
 * for the now-empty policy. */
static inline unsigned vp_batch_policy_admit(const VPBatchPolicy *p,
                                             uint64_t scope, int depth_only,
                                             uint64_t target_va,
                                             unsigned target_width,
                                             unsigned target_height,
                                             uint32_t target_format,
                                             int depth_attachment,
                                             uint64_t depth_va,
                                             uint32_t depth_format,
                                             unsigned depth_width,
                                             unsigned depth_height,
                                             size_t reserve_bytes,
                                             unsigned max_draws,
                                             size_t budget_bytes) {
    unsigned reason = VP_BATCH_ACCEPT;
    if (max_draws == 0u || max_draws > VP_BATCH_MAX_DRAWS)
        max_draws = VP_BATCH_MAX_DRAWS;
    if (reserve_bytes > budget_bytes) return VP_BATCH_REFUSE_TOO_LARGE;
    if (!p->open) return VP_BATCH_ACCEPT;
    if (p->scope != scope) return VP_BATCH_FLUSH_SCOPE;
    if (!vp_batch_policy_identity_matches(p, depth_only, target_va,
                                          target_width, target_height,
                                          target_format, depth_attachment,
                                          depth_va, depth_format, depth_width,
                                          depth_height, &reason))
        return reason;
    if (p->draws >= max_draws) return VP_BATCH_FLUSH_LIMIT;
    /* A corrupted/overflowed byte count must flush, not wrap the subtraction. */
    if (p->bytes_used > budget_bytes) return VP_BATCH_FLUSH_BUDGET;
    if (reserve_bytes > budget_bytes - p->bytes_used)
        return VP_BATCH_FLUSH_BUDGET;
    return VP_BATCH_ACCEPT;
}

/* Records one admitted draw. Returns 0 on success, -1 if the caller violated
 * an invariant (slot/byte limit). */
static inline int vp_batch_policy_push(VPBatchPolicy *p,
                                       VPBatchPolicyStats *stats,
                                       uint64_t scope, int depth_only,
                                       uint64_t target_va,
                                       unsigned target_width,
                                       unsigned target_height,
                                       uint32_t target_format,
                                       int depth_attachment,
                                       uint64_t depth_va,
                                       uint32_t depth_format,
                                       unsigned depth_width,
                                       unsigned depth_height,
                                       size_t reserve_bytes,
                                       unsigned max_draws,
                                       size_t budget_bytes) {
    if (p == NULL || stats == NULL) return -1;
    if (max_draws == 0u || max_draws > VP_BATCH_MAX_DRAWS)
        max_draws = VP_BATCH_MAX_DRAWS;
    if (reserve_bytes > budget_bytes) {
        stats->refused++;
        return -1;
    }
    if (!p->open) {
        p->open = 1;
        p->depth_only = depth_only;
        p->scope = scope;
        p->target_va = depth_only ? 0u : target_va;
        p->target_width = depth_only ? 0u : target_width;
        p->target_height = depth_only ? 0u : target_height;
        p->target_format = depth_only ? 0u : target_format;
        p->depth_va = depth_va;
        p->depth_format = depth_format;
        p->depth_width = depth_width;
        p->depth_height = depth_height;
        p->depth_attachment = depth_attachment;
    }
    if (p->draws >= max_draws ||
        p->bytes_used > budget_bytes ||
        reserve_bytes > budget_bytes - p->bytes_used) {
        stats->refused++;
        return -1;
    }
    p->draws++;
    p->bytes_used += reserve_bytes;
    p->resources_pinned = p->draws;
    if (p->draws > stats->max_batch) stats->max_batch = p->draws;
    if (p->bytes_used > stats->max_bytes) stats->max_bytes = (unsigned)p->bytes_used;
    stats->encoded++;
    return 0;
}

/* Successful scene flush: publish all pending commits and release pins. */
static inline unsigned vp_batch_policy_commit(VPBatchPolicy *p,
                                              VPBatchPolicyStats *stats) {
    unsigned pending;
    if (p == NULL || stats == NULL) return 0u;
    pending = p->open ? p->draws : 0u;
    stats->completed += pending;
    if (pending != 0u) stats->flushes++;
    vp_batch_policy_init(p, NULL);
    return pending;
}

/* Failed scene flush: never publishes pending work as successful. */
static inline unsigned vp_batch_policy_abort(VPBatchPolicy *p,
                                             VPBatchPolicyStats *stats) {
    unsigned pending;
    if (p == NULL || stats == NULL) return 0u;
    pending = p->open ? p->draws : 0u;
    if (pending != 0u) {
        stats->failed += pending;
        stats->commits_dropped += pending;
        stats->flushes++;
    }
    vp_batch_policy_init(p, NULL);
    return pending;
}

/* End-submit, clear, readback, present and deadline are unconditional
 * boundaries. The returned pending count is committed by the caller only after
 * the GPU scene actually finished. */
static inline unsigned vp_batch_policy_boundary(VPBatchPolicy *p,
                                                unsigned boundary,
                                                VPBatchPolicyStats *stats) {
    unsigned pending;
    if (p == NULL || stats == NULL) return 0u;
    pending = p->open ? p->draws : 0u;
    if (boundary == VP_BATCH_BOUNDARY_FAILURE) {
        /* The caller is ending a scene whose GPU completion is uncertain. */
        vp_batch_policy_init(p, NULL);
        stats->failed += pending;
        stats->commits_dropped += pending;
        return pending;
    }
    if (boundary == VP_BATCH_BOUNDARY_END_SUBMIT ||
        boundary == VP_BATCH_BOUNDARY_CLEAR ||
        boundary == VP_BATCH_BOUNDARY_READBACK ||
        boundary == VP_BATCH_BOUNDARY_PRESENT ||
        boundary == VP_BATCH_BOUNDARY_RESOURCE ||
        boundary == VP_BATCH_BOUNDARY_DEADLINE) {
        stats->sync_fallbacks++;
        return pending;
    }
    return 0u;
}

#endif /* VP_BATCH_POLICY_H */

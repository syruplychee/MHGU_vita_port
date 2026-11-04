/* Pure shader-resource planning shared by the GXM runtime and host tests.
 *
 * guest_svc used to query, per declared texture slot, the fragment/vertex
 * sampler declaration, the static-resource state and the sidecar descriptor.
 * This module turns the same three original operations into one plan per
 * stage: declared slots, resources_active, each slot's original layout return
 * code and the value-only VPResourceTexture copy. No pointer from a resident
 * program is exposed.
 *
 * The bounded cache is keyed by the full (token, gpu_stage, viewport) tuple and
 * the resident/resource generation supplied by the caller. A generation bump
 * (resident registration, eviction or index reload) makes every old entry
 * stale; a hash collision only evicts. The plan builder calls the original
 * query callbacks, so miss behavior, return codes and failure modes stay the
 * legacy ones.
 */
#ifndef VP_GXM_SHADER_PLAN_H
#define VP_GXM_SHADER_PLAN_H

#include "gxm_resource_layout.h"
#include "gxm_sampler_slots.h"

#include <stdint.h>
#include <string.h>

#define VP_GXM_SHADER_PLAN_CACHE_ENTRIES 64u

typedef struct {
    /* Original vp_gxm_shader_resource return for this slot. Slots that the
     * registered shader does not declare keep 1. A declared slot is queried
     * whenever resources_active is nonzero, exactly like the original caller;
     * with resources_active < 0 the sidecar query returns its own error code,
     * which the caller reports from the resources_active branch first. */
    int layout_rc;
    VPResourceTexture layout;
} VPGxmShaderPlanSlot;

typedef struct {
    unsigned declared_mask;
    int resources_active;
    VPGxmShaderPlanSlot slots[VP_GXM_FRAGMENT_SAMPLER_LIMIT];
} VPGxmShaderResourcePlan;

/* Callbacks must reproduce the exact original API semantics:
 *   sampler_declared -> vp_gxm_shader_has_{fragment,vertex}_sampler
 *   resources_active -> vp_gxm_static_resources_active
 *   shader_resource  -> vp_gxm_shader_resource
 * gpu_stage is 0 for vertex and 4 for fragment. */
typedef struct {
    void *user;
    int (*sampler_declared)(void *user, uint32_t token, unsigned gpu_stage,
                            unsigned viewport, unsigned slot);
    int (*resources_active)(void *user);
    int (*shader_resource)(void *user, uint32_t token, unsigned gpu_stage,
                           unsigned viewport, unsigned slot,
                           VPResourceTexture *out);
} VPGxmShaderPlanOps;

typedef struct {
    unsigned used;
    uint32_t generation;
    uint32_t token;
    uint32_t gpu_stage;
    uint32_t viewport;
    VPGxmShaderResourcePlan plan;
} VPGxmShaderPlanCacheEntry;

typedef struct {
    unsigned hits, misses, builds, invalidations;
} VPGxmShaderPlanCounts;

/* Build one stage plan through the original query callbacks. Returns 0 on
 * success, -1 for bad arguments or a stage outside {0,4}. */
static inline int vp_gxm_shader_plan_build(const VPGxmShaderPlanOps *ops,
                                           uint32_t token, unsigned gpu_stage,
                                           unsigned viewport,
                                           VPGxmShaderResourcePlan *out) {
    unsigned slot;
    if (ops == NULL || ops->sampler_declared == NULL || out == NULL)
        return -1;
    if (gpu_stage != 0u && gpu_stage != 4u) return -1;
    memset(out, 0, sizeof(*out));
    out->resources_active = ops->resources_active != NULL
                                ? ops->resources_active(ops->user) : 0;
    for (slot = 0; slot < VP_GXM_FRAGMENT_SAMPLER_LIMIT; ++slot) {
        int declared = ops->sampler_declared(ops->user, token, gpu_stage,
                                             viewport, slot);
        VPGxmShaderPlanSlot *planned = &out->slots[slot];
        if (!declared) {
            planned->layout_rc = 1;
            continue;
        }
        out->declared_mask |= 1u << slot;
        if (out->resources_active == 0) {
            /* Legacy package: the original caller never resolved a sidecar
             * layout and used the fixed handle offset instead. */
            planned->layout_rc = 0;
            continue;
        }
        if (ops->shader_resource == NULL) return -1;
        planned->layout_rc = ops->shader_resource(
            ops->user, token, gpu_stage, viewport, slot, &planned->layout);
        if (planned->layout_rc != 0) memset(&planned->layout, 0,
                                            sizeof(planned->layout));
    }
    return 0;
}

static inline unsigned vp_gxm_shader_plan_cache_index(uint32_t token,
                                                      unsigned gpu_stage,
                                                      unsigned viewport,
                                                      unsigned count) {
    uint32_t hash = token * 2654435761u + gpu_stage * 2246822519u +
                    viewport * 3266489917u;
    return hash % count;
}

/* Exact-key lookup with a generation check. On a stale or missing entry the
 * plan is rebuilt through the original callbacks and stored. Returns 0 with the
 * value copied into out, or -1 for bad arguments/builder failure. */
static inline int vp_gxm_shader_plan_cached(VPGxmShaderPlanCacheEntry *entries,
                                            unsigned count, uint32_t generation,
                                            const VPGxmShaderPlanOps *ops,
                                            uint32_t token, unsigned gpu_stage,
                                            unsigned viewport,
                                            VPGxmShaderResourcePlan *out,
                                            VPGxmShaderPlanCounts *counts) {
    VPGxmShaderPlanCacheEntry *entry;
    unsigned index;
    if (entries == NULL || count == 0u || out == NULL) return -1;
    if (gpu_stage != 0u && gpu_stage != 4u) return -1;
    index = vp_gxm_shader_plan_cache_index(token, gpu_stage, viewport, count);
    entry = &entries[index];
    if (entry->used && entry->generation == generation &&
        entry->token == token && entry->gpu_stage == gpu_stage &&
        entry->viewport == viewport) {
        if (counts != NULL) counts->hits++;
        *out = entry->plan;
        return 0;
    }
    if (counts != NULL) {
        counts->misses++;
        if (entry->used && entry->generation != generation)
            counts->invalidations++;
    }
    if (vp_gxm_shader_plan_build(ops, token, gpu_stage, viewport, out) < 0)
        return -1;
    entry->used = 1u;
    entry->generation = generation;
    entry->token = token;
    entry->gpu_stage = gpu_stage;
    entry->viewport = viewport;
    entry->plan = *out;
    if (counts != NULL) counts->builds++;
    return 0;
}

#endif

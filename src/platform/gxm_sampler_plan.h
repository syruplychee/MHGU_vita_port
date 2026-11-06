/* Pure guest-TSC to GXM sampler-enum planning shared by the runtime and the
 * host tests.
 *
 * The guest sampler words are the same ones the draw resolver already stores
 * in VPGxmDrawRequest.sampler / VPGxmExtraTexture.sampler. Parsing them is a
 * pure function: min/mag filter codes 1..2, U/V address modes 0..4, the mip
 * code used only by extra/vertex samplers, and the two "border" raw values
 * that the runtime logs. The primary sampler deliberately never applies a mip
 * filter, so callers select VP_GXM_SAMPLER_PLAN_PRIMARY and must not use the
 * plan's mip field.
 *
 * The bounded cache keeps the full sampler words plus the plan kind in every
 * key; a collision only evicts, it never returns another key's plan. The cache
 * stores only enum decisions and raw words, never pixels or texture contents.
 */
#ifndef VP_GXM_SAMPLER_PLAN_H
#define VP_GXM_SAMPLER_PLAN_H

#include <stdint.h>
#include <string.h>

#define VP_GXM_SAMPLER_PLAN_PRIMARY 0u
#define VP_GXM_SAMPLER_PLAN_MIP 1u
#define VP_GXM_SAMPLER_PLAN_CACHE_ENTRIES 64u

/* Address-mode values match the VitaSDK SceGxmTextureAddrMode enumerators.
 * The runtime asserts that equality where the GXM header is available. */
enum {
    VP_GXM_SAMPLER_ADDR_REPEAT = 0,
    VP_GXM_SAMPLER_ADDR_MIRROR = 1,
    VP_GXM_SAMPLER_ADDR_CLAMP = 2,
    VP_GXM_SAMPLER_ADDR_MIRROR_CLAMP = 3,
    VP_GXM_SAMPLER_ADDR_CLAMP_FULL_BORDER = 5
};

/* Filter values: 0 point, 1 linear. Mip: 0 disabled, 1 enabled. */
typedef struct {
    int rc;                 /* 0 valid, -2 unsupported guest enum */
    unsigned min_filter;
    unsigned mag_filter;
    unsigned mip_filter;
    unsigned addr_u;
    unsigned addr_v;
    unsigned raw_u;         /* guest TSC U field (0..4), for the border log */
    unsigned raw_v;
} VPGxmSamplerPlan;

typedef struct {
    unsigned used;
    unsigned kind;
    uint32_t sampler[2];
    VPGxmSamplerPlan plan;
} VPGxmSamplerPlanCacheEntry;

static inline int vp_gxm_sampler_plan_addr(unsigned raw, unsigned *out) {
    switch (raw & 7u) {
    case 0u: *out = VP_GXM_SAMPLER_ADDR_REPEAT; return 0;
    case 1u: *out = VP_GXM_SAMPLER_ADDR_MIRROR; return 0;
    case 2u: *out = VP_GXM_SAMPLER_ADDR_CLAMP; return 0;
    case 3u: *out = VP_GXM_SAMPLER_ADDR_CLAMP_FULL_BORDER; return 0;
    case 4u: *out = VP_GXM_SAMPLER_ADDR_MIRROR_CLAMP; return 0;
    default: return -1;
    }
}

/* Build one plan. This mirrors the checks in the original primary/extra/vertex
 * appliers exactly; it never logs, so the caller keeps the original log text
 * and error return at the site that actually applies the state. */
static inline int vp_gxm_sampler_plan_build(uint32_t sampler0,
                                            uint32_t sampler1,
                                            VPGxmSamplerPlan *out) {
    unsigned min_code, mag_code, raw_u, raw_v;
    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    min_code = (sampler1 >> 4) & 3u;
    mag_code = sampler1 & 3u;
    raw_u = sampler0 & 7u;
    raw_v = (sampler0 >> 3) & 7u;
    if (min_code < 1u || min_code > 2u || mag_code < 1u || mag_code > 2u ||
        vp_gxm_sampler_plan_addr(raw_u, &out->addr_u) < 0 ||
        vp_gxm_sampler_plan_addr(raw_v, &out->addr_v) < 0) {
        out->rc = -2;
        return -2;
    }
    out->min_filter = min_code == 2u ? 1u : 0u;
    out->mag_filter = mag_code == 2u ? 1u : 0u;
    out->mip_filter = ((sampler1 >> 6) & 3u) != 0u ? 1u : 0u;
    out->raw_u = raw_u;
    out->raw_v = raw_v;
    out->rc = 0;
    return 0;
}

static inline unsigned vp_gxm_sampler_plan_cache_index(unsigned kind,
                                                       uint32_t sampler0,
                                                       uint32_t sampler1,
                                                       unsigned count) {
    uint32_t hash = sampler0 * 2654435761u + sampler1 * 2246822519u +
                    kind * 3266489917u;
    return hash % count;
}

/* Look up one exact (kind, sampler0, sampler1) key, building and storing the
 * plan on a miss. Invalid plans are cached too; their rc still makes every
 * apply site emit the original rejection. Returns NULL only for bad arguments.
 * hits/misses may be NULL. */
static inline const VPGxmSamplerPlan *vp_gxm_sampler_plan_cache_get(
        VPGxmSamplerPlanCacheEntry *entries, unsigned count, unsigned kind,
        uint32_t sampler0, uint32_t sampler1,
        unsigned *hits, unsigned *misses) {
    VPGxmSamplerPlanCacheEntry *entry;
    unsigned index;
    if (entries == NULL || count == 0u) return NULL;
    if (kind != VP_GXM_SAMPLER_PLAN_PRIMARY && kind != VP_GXM_SAMPLER_PLAN_MIP)
        return NULL;
    index = vp_gxm_sampler_plan_cache_index(kind, sampler0, sampler1, count);
    entry = &entries[index];
    if (entry->used && entry->kind == kind &&
        entry->sampler[0] == sampler0 && entry->sampler[1] == sampler1) {
        if (hits != NULL) (*hits)++;
        return &entry->plan;
    }
    if (misses != NULL) (*misses)++;
    entry->used = 1u;
    entry->kind = kind;
    entry->sampler[0] = sampler0;
    entry->sampler[1] = sampler1;
    (void)vp_gxm_sampler_plan_build(sampler0, sampler1, &entry->plan);
    return &entry->plan;
}

#endif

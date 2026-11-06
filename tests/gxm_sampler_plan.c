/* Host regression for the pure guest-TSC sampler plan and its bounded cache.
 * The production parser/cache are the static-inline functions in
 * src/platform/gxm_sampler_plan.h, not a test copy. */
#include "gxm_sampler_plan.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned expected_addr(unsigned raw) {
    switch (raw) {
    case 0u: return 0u;
    case 1u: return 1u;
    case 2u: return 2u;
    case 3u: return 5u; /* clamp-to-border is GXM CLAMP_FULL_BORDER */
    case 4u: return 3u; /* mirror-clamp */
    default: return 0xffffffffu;
    }
}

static void test_all_valid_enums(void) {
    unsigned min_code, mag_code, u, v, mip_code;
    for (min_code = 1u; min_code <= 2u; ++min_code) {
        for (mag_code = 1u; mag_code <= 2u; ++mag_code) {
            for (u = 0u; u <= 4u; ++u) {
                for (v = 0u; v <= 4u; ++v) {
                    for (mip_code = 0u; mip_code <= 3u; ++mip_code) {
                        uint32_t sampler0 = u | (v << 3);
                        uint32_t sampler1 = mag_code | (min_code << 4) |
                                            (mip_code << 6);
                        VPGxmSamplerPlan plan;
                        assert(vp_gxm_sampler_plan_build(sampler0, sampler1,
                                                         &plan) == 0);
                        assert(plan.rc == 0);
                        assert(plan.min_filter == (min_code == 2u ? 1u : 0u));
                        assert(plan.mag_filter == (mag_code == 2u ? 1u : 0u));
                        assert(plan.mip_filter == (mip_code != 0u ? 1u : 0u));
                        assert(plan.addr_u == expected_addr(u));
                        assert(plan.addr_v == expected_addr(v));
                        assert(plan.raw_u == u);
                        assert(plan.raw_v == v);
                    }
                }
            }
        }
    }
    puts("PASS sampler plan maps all valid filter/address/mip enums");
}

static void test_invalid_enums(void) {
    static const unsigned bad_filter[] = {0u, 3u};
    unsigned i, code;
    VPGxmSamplerPlan plan;
    for (i = 0u; i < sizeof(bad_filter) / sizeof(bad_filter[0]); ++i) {
        assert(vp_gxm_sampler_plan_build(0u, bad_filter[i] | (1u << 4),
                                         &plan) == -2);
        assert(plan.rc == -2);
        assert(vp_gxm_sampler_plan_build(0u, 1u | (bad_filter[i] << 4),
                                         &plan) == -2);
        assert(plan.rc == -2);
    }
    /* U raw 5..7 are outside the original addr-mode switch. V raw lives in
     * bits 3..5, so raw 5..7 at that position must also be refused. */
    for (code = 5u; code < 8u; ++code) {
        assert(vp_gxm_sampler_plan_build(code, 0x11u, &plan) == -2);
        assert(vp_gxm_sampler_plan_build(code << 3, 0x11u, &plan) == -2);
    }
    /* Bits above the parsed fields are ignored, as in the original code. */
    assert(vp_gxm_sampler_plan_build(0x80000000u, 0x11111111u, &plan) == 0);
    assert(plan.rc == 0);
    assert(vp_gxm_sampler_plan_build(0u, 1u, NULL) == -1);
    puts("PASS sampler plan rejects every unsupported enum");
}

static void test_primary_and_mip_entries_do_not_alias(void) {
    VPGxmSamplerPlanCacheEntry cache[16];
    const VPGxmSamplerPlan *primary, *mip;
    unsigned hits = 0u, misses = 0u;
    memset(cache, 0, sizeof(cache));
    primary = vp_gxm_sampler_plan_cache_get(cache, 16u,
        VP_GXM_SAMPLER_PLAN_PRIMARY, 0x02u, 0x51u, &hits, &misses);
    assert(primary != NULL && primary->rc == 0);
    assert(primary->mip_filter == 1u); /* parsed, but primary never applies it */
    mip = vp_gxm_sampler_plan_cache_get(cache, 16u, VP_GXM_SAMPLER_PLAN_MIP,
                                        0x02u, 0x51u, &hits, &misses);
    assert(mip != NULL && mip->rc == 0);
    assert(mip != primary);
    assert(hits == 0u && misses == 2u);
    primary = vp_gxm_sampler_plan_cache_get(cache, 16u,
        VP_GXM_SAMPLER_PLAN_PRIMARY, 0x02u, 0x51u, &hits, &misses);
    assert(primary != NULL);
    assert(hits == 1u && misses == 2u);
    puts("PASS sampler cache keeps primary and mip keys separate");
}

static void test_invalid_plan_is_cached_with_its_rc(void) {
    VPGxmSamplerPlanCacheEntry cache[8];
    const VPGxmSamplerPlan *plan;
    unsigned hits = 0u, misses = 0u;
    memset(cache, 0, sizeof(cache));
    plan = vp_gxm_sampler_plan_cache_get(cache, 8u, VP_GXM_SAMPLER_PLAN_MIP,
                                         0u, 0x10u, &hits, &misses);
    assert(plan != NULL && plan->rc == -2);
    plan = vp_gxm_sampler_plan_cache_get(cache, 8u, VP_GXM_SAMPLER_PLAN_MIP,
                                         0u, 0x10u, &hits, &misses);
    assert(plan != NULL && plan->rc == -2);
    assert(hits == 1u && misses == 1u);
    puts("PASS sampler cache retains invalid plans without changing the rc");
}

static void test_cache_collision_keeps_full_key(void) {
    VPGxmSamplerPlanCacheEntry cache[8];
    uint32_t first = 1u, second = 2u, index;
    const VPGxmSamplerPlan *plan;
    unsigned hits = 0u, misses = 0u;
    memset(cache, 0, sizeof(cache));
    index = vp_gxm_sampler_plan_cache_index(VP_GXM_SAMPLER_PLAN_PRIMARY,
                                             first, 0x11u, 8u);
    while (vp_gxm_sampler_plan_cache_index(VP_GXM_SAMPLER_PLAN_PRIMARY,
                                           second, 0x11u, 8u) != index)
        second++;
    plan = vp_gxm_sampler_plan_cache_get(cache, 8u,
        VP_GXM_SAMPLER_PLAN_PRIMARY, first, 0x11u, &hits, &misses);
    assert(plan != NULL && plan->raw_u == 1u);
    plan = vp_gxm_sampler_plan_cache_get(cache, 8u,
        VP_GXM_SAMPLER_PLAN_PRIMARY, second, 0x11u, &hits, &misses);
    assert(plan != NULL && plan->raw_u == (second & 7u));
    plan = vp_gxm_sampler_plan_cache_get(cache, 8u,
        VP_GXM_SAMPLER_PLAN_PRIMARY, first, 0x11u, &hits, &misses);
    assert(plan != NULL && plan->raw_u == 1u);
    assert(hits == 0u && misses == 3u);
    puts("PASS sampler cache collision compares the complete key");
}

int main(void) {
    test_all_valid_enums();
    test_invalid_enums();
    test_primary_and_mip_entries_do_not_alias();
    test_invalid_plan_is_cached_with_its_rc();
    test_cache_collision_keeps_full_key();
    puts("gxm_sampler_plan: all checks passed");
    return 0;
}

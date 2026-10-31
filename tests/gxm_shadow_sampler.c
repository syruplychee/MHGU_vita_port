/* Host test for the host-pure shadow TSC planner.
 *
 * Expected values are written independently here (literal status codes and
 * float4 layouts), not copied from the implementation.  The test exercises all
 * eight compare functions, both filters, all four supported wrap modes, the
 * exported helper ABI and every named refusal.
 */
#include "gxm_shadow_sampler.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static uint32_t f32_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void make_tsc(uint32_t tsc[8], unsigned compare_func, unsigned filter,
                     unsigned wrap_u, unsigned wrap_v, float border) {
    memset(tsc, 0, 8u * sizeof(uint32_t));
    tsc[0] = (wrap_u & 7u) | ((wrap_v & 7u) << 3) | (1u << 9) |
             ((compare_func & 7u) << 10);
    tsc[1] = (filter & 3u) | ((filter & 3u) << 4) | (1u << 6);
    tsc[4] = f32_bits(border);
}

static void expect_status(const char *name, int actual, int expected) {
    if (actual != expected) {
        printf("FAIL %-32s expected=%d (%s) actual=%d (%s)\n", name, expected,
               vp_gxm_shadow_status_name(expected), actual,
               vp_gxm_shadow_status_name(actual));
        failures++;
    }
}

static void expect_u32(const char *name, unsigned actual, unsigned expected) {
    if (actual != expected) {
        printf("FAIL %-32s expected=%u actual=%u\n", name, expected, actual);
        failures++;
    }
}

static void expect_f32(const char *name, float actual, float expected) {
    if (actual != expected) {
        printf("FAIL %-32s expected=%.9g actual=%.9g\n", name, expected,
               (double)actual);
        failures++;
    }
}

static void test_accepted_matrix(void) {
    unsigned func, filter, wu, wv;
    unsigned cases = 0;
    VPGxmShadowSamplerPlan plan;
    uint32_t tsc[8];
    for (func = 0; func < 8u; ++func) {
        for (filter = 1; filter <= 2u; ++filter) {
            for (wu = 0; wu < 4u; ++wu) {
                for (wv = 0; wv < 4u; ++wv) {
                    float border = (wu == 3u || wv == 3u) ? 0.25f : 0.0f;
                    make_tsc(tsc, func, filter, wu, wv, border);
                    expect_status("accepted", vp_gxm_shadow_sampler_plan(
                                      tsc, 2u, 1u, 0, 64u, 32u, &plan),
                                  VP_GXM_SHADOW_OK);
                    expect_u32("compare_func", plan.compare_func, func);
                    expect_u32("maxwell_filter", plan.maxwell_filter, filter);
                    expect_u32("filter_code", plan.filter_code,
                               filter == 1u ? 0u : 1u);
                    expect_u32("wrap_u", plan.wrap_u, wu);
                    expect_u32("wrap_v", plan.wrap_v, wv);
                    expect_u32("native_min", plan.native_min_filter, 1u);
                    expect_u32("native_mag", plan.native_mag_filter, 1u);
                    expect_u32("native_compare_off",
                               plan.native_compare_disabled, 1u);
                    expect_u32("native_mip_off",
                               plan.native_mip_filter_disabled, 1u);
                    expect_u32("native_addr_u", plan.native_addr_u, 2u);
                    expect_u32("native_addr_v", plan.native_addr_v, 2u);
                    expect_f32("extent.w", plan.extent[0], 64.0f);
                    expect_f32("extent.h", plan.extent[1], 32.0f);
                    expect_f32("extent.invw", plan.extent[2], 1.0f / 64.0f);
                    expect_f32("extent.invh", plan.extent[3], 1.0f / 32.0f);
                    expect_f32("state.compare", plan.state[0], (float)func);
                    expect_f32("state.filter", plan.state[1],
                               filter == 1u ? 0.0f : 1.0f);
                    expect_f32("state.wrap_u", plan.state[2], (float)wu);
                    expect_f32("state.wrap_v", plan.state[3], (float)wv);
                    expect_f32("border.value", plan.border[0],
                               (wu == 3u || wv == 3u) ? 0.25f : 0.0f);
                    expect_f32("border.y", plan.border[1], 0.0f);
                    expect_u32("border_used", plan.border_used,
                               (wu == 3u || wv == 3u) ? 1u : 0u);
                    cases++;
                }
            }
        }
    }
    printf("accepted matrix: %u cases\n", cases);
}

static void test_rejections(void) {
    VPGxmShadowSamplerPlan plan;
    uint32_t tsc[8];
    int rc;

    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);

    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, NULL);
    expect_status("null plan", rc, VP_GXM_SHADOW_ERR_ARG);
    rc = vp_gxm_shadow_sampler_plan(NULL, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("null tsc", rc, VP_GXM_SHADOW_ERR_ARG);
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 0u, 64u, &plan);
    expect_status("zero width", rc, VP_GXM_SHADOW_ERR_EXTENT);
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 0u, &plan);
    expect_status("zero height", rc, VP_GXM_SHADOW_ERR_EXTENT);

    rc = vp_gxm_shadow_sampler_plan(tsc, 1u, 1u, 0, 64u, 64u, &plan);
    expect_status("texture type 1", rc, VP_GXM_SHADOW_ERR_TEXTURE_TYPE);
    rc = vp_gxm_shadow_sampler_plan(tsc, 0u, 1u, 0, 64u, 64u, &plan);
    expect_status("texture type 0", rc, VP_GXM_SHADOW_ERR_TEXTURE_TYPE);
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 0u, 0, 64u, 64u, &plan);
    expect_status("depth flag 0", rc, VP_GXM_SHADOW_ERR_NOT_DEPTH);
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 2u, 0, 64u, 64u, &plan);
    expect_status("depth flag 2", rc, VP_GXM_SHADOW_ERR_NOT_DEPTH);

    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[0] &= ~(1u << 9);
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("compare disabled", rc, VP_GXM_SHADOW_ERR_COMPARE_DISABLED);

    for (unsigned wrap = 4u; wrap < 8u; ++wrap) {
        make_tsc(tsc, 1u, 1u, wrap, 0u, 0.5f);
        rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
        expect_status("wrap_u 4..7", rc, VP_GXM_SHADOW_ERR_WRAP_U);
        make_tsc(tsc, 1u, 1u, 0u, wrap, 0.5f);
        rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
        expect_status("wrap_v 4..7", rc, VP_GXM_SHADOW_ERR_WRAP_V);
    }

    for (unsigned filter = 0u; filter != 4u; ++filter) {
        if (filter == 1u || filter == 2u) continue;
        make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
        tsc[1] = filter;
        rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
        expect_status("mag filter 0/3", rc, VP_GXM_SHADOW_ERR_FILTER_MAG);
        make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
        tsc[1] = 1u | (filter << 4) | (1u << 6);
        rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
        expect_status("min filter 0/3", rc, VP_GXM_SHADOW_ERR_FILTER_MIN);
    }
    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[1] = 1u | (2u << 4);
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("filter mismatch", rc, VP_GXM_SHADOW_ERR_FILTER_MISMATCH);
    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[1] &= ~(3u << 6);
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("mipmap filter 0", rc, VP_GXM_SHADOW_ERR_MIPMAP_FILTER);
    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[1] |= (3u << 6);
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("mipmap filter linear", rc, VP_GXM_SHADOW_OK);

    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, -1, 64u, 64u, &plan);
    expect_status("mip unknown", rc, VP_GXM_SHADOW_ERR_MIP_LEVEL_UNKNOWN);
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 1, 64u, 64u, &plan);
    expect_status("mip present", rc, VP_GXM_SHADOW_ERR_MIP_LEVEL_PRESENT);

    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[1] |= 1u << 10;
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("reduction", rc, VP_GXM_SHADOW_ERR_REDUCTION);
    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[0] |= 1u << 20;
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("anisotropy", rc, VP_GXM_SHADOW_ERR_ANISOTROPY);
    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[1] |= 1u << 8;
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    /* policy-2: cube-only bits are inert for the accepted 2D depth view. */
    expect_status("cubemap aniso", rc, VP_GXM_SHADOW_OK);
    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[1] |= 1u << 9;
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("cubemap interface", rc, VP_GXM_SHADOW_OK);

    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[1] |= 1u << 12;
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("lod bias +1", rc, VP_GXM_SHADOW_ERR_LOD_BIAS);
    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[1] |= 0x1fffu << 12;
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("lod bias -1", rc, VP_GXM_SHADOW_ERR_LOD_BIAS);
    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[1] |= 1u << 25;
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("float coord", rc, VP_GXM_SHADOW_ERR_FLOAT_COORD);
    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[1] |= 1u << 26;
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("trilinear", rc, VP_GXM_SHADOW_ERR_TRILINEAR);
    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[2] |= 1u;
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("min lod clamp", rc, VP_GXM_SHADOW_ERR_MIN_LOD);
    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[0] |= 1u << 13;
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    /* policy-2: depth-format TIC has no sRGB transfer; bit is inert. */
    expect_status("srgb", rc, VP_GXM_SHADOW_OK);

    make_tsc(tsc, 1u, 1u, 3u, 0u, 0.0f);
    tsc[4] = 0x7fc00000u; /* quiet NaN */
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("border nan used", rc,
                  VP_GXM_SHADOW_ERR_BORDER_NONFINITE);
    make_tsc(tsc, 1u, 1u, 0u, 3u, 0.0f);
    tsc[4] = 0x7f800000u; /* +inf */
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("border inf used", rc,
                  VP_GXM_SHADOW_ERR_BORDER_NONFINITE);
    make_tsc(tsc, 1u, 1u, 3u, 0u, 0.0f);
    tsc[4] = f32_bits(1.5f);
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("border >1 used", rc, VP_GXM_SHADOW_ERR_BORDER_RANGE);
    make_tsc(tsc, 1u, 1u, 3u, 0u, 0.0f);
    tsc[4] = f32_bits(-0.25f);
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("border <0 used", rc, VP_GXM_SHADOW_ERR_BORDER_RANGE);

    /* The border value is not consulted when no axis uses Border. */
    make_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[4] = 0x7fc00000u;
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("border nan unused", rc, VP_GXM_SHADOW_OK);
    expect_u32("border unused flag", plan.border_used, 0u);
    expect_f32("border unused value", plan.border[0], 0.0f);
    tsc[4] = f32_bits(2.0f);
    rc = vp_gxm_shadow_sampler_plan(tsc, 2u, 1u, 0, 64u, 64u, &plan);
    expect_status("border >1 unused", rc, VP_GXM_SHADOW_OK);
}

static void test_named_values(void) {
    VPGxmShadowSamplerPlan plan;
    uint32_t tsc[8];
    make_tsc(tsc, 6u, 2u, 3u, 1u, 0.75f);
    expect_status("named plan", vp_gxm_shadow_sampler_plan(
                      tsc, 2u, 1u, 0, 128u, 64u, &plan), VP_GXM_SHADOW_OK);
    expect_u32("named compare", plan.compare_func, 6u);
    expect_u32("named filter enum", plan.maxwell_filter, 2u);
    expect_u32("named filter code", plan.filter_code, 1u);
    expect_f32("named extent", plan.extent[0], 128.0f);
    expect_f32("named invh", plan.extent[3], 1.0f / 64.0f);
    expect_f32("named state", plan.state[0], 6.0f);
    expect_f32("named state filter", plan.state[1], 1.0f);
    expect_f32("named border", plan.border[0], 0.75f);

    if (fabsf(plan.extent[2] - 0.0078125f) > 1e-9f) {
        printf("FAIL named invw\n");
        failures++;
    }
    if (strcmp(vp_gxm_shadow_status_name(VP_GXM_SHADOW_ERR_SRGB),
               "srgb-unsupported") != 0) {
        printf("FAIL status name srgb\n");
        failures++;
    }
}

static void test_source_mode(void) {
    const char *reason = NULL;
    int mode;
    /* An ordinary 2D texture has TIC depth/layer 1 but sidecar descriptor
     * depth 0.  The mode helper never receives the TIC depth field; both a
     * non-depth source and a depth-pool ordinary sampler stay ordinary. */
    mode = vp_gxm_shadow_source_mode(0u, 0u, 0u, &reason);
    expect_status("ordinary non-depth source", mode,
                  VP_GXM_SHADOW_SOURCE_ORDINARY);
    mode = vp_gxm_shadow_source_mode(0u, 0u, 1u, &reason);
    expect_status("ordinary depth-pool source", mode,
                  VP_GXM_SHADOW_SOURCE_ORDINARY);
    mode = vp_gxm_shadow_source_mode(1u, 3u, 1u, &reason);
    expect_status("software shadow", mode, VP_GXM_SHADOW_SOURCE_SOFTWARE);
    mode = vp_gxm_shadow_source_mode(1u, 3u, 0u, &reason);
    expect_status("shadow requires depth source", mode, -1);
    if (strcmp(reason, "shadow-requires-depth-source") != 0) {
        printf("FAIL shadow source reason: %s\n", reason);
        failures++;
    }
    mode = vp_gxm_shadow_source_mode(1u, 0u, 1u, &reason);
    expect_status("descriptor depth without contract", mode, -1);
    if (strcmp(reason, "shadow-depth-without-contract") != 0) {
        printf("FAIL descriptor depth reason: %s\n", reason);
        failures++;
    }
    mode = vp_gxm_shadow_source_mode(0u, 3u, 1u, &reason);
    expect_status("contract without descriptor depth", mode, -1);
    if (strcmp(reason, "shadow-params-without-depth-flag") != 0) {
        printf("FAIL contract reason: %s\n", reason);
        failures++;
    }
    mode = vp_gxm_shadow_source_mode(0u, 1u, 1u, &reason);
    expect_status("partial contract", mode, -1);
    if (strcmp(reason, "incomplete-contract") != 0) {
        printf("FAIL partial contract reason: %s\n", reason);
        failures++;
    }
    mode = vp_gxm_shadow_source_mode(2u, 0u, 1u, &reason);
    expect_status("invalid descriptor depth", mode, -1);
    if (strcmp(reason, "descriptor-depth-not-boolean") != 0) {
        printf("FAIL descriptor depth domain reason: %s\n", reason);
        failures++;
    }
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--crosscheck") == 0) {
        FILE *file = fopen(argv[2], "r");
        char id[64];
        unsigned w[8], texture_type, depth, width, height;
        int max_mip;
        if (file == NULL) {
            printf("cannot open %s\n", argv[2]);
            return 1;
        }
        while (fscanf(file, "%63s %u %u %u %u %u %u %u %u %u %u %d %u %u",
                      id, &w[0], &w[1], &w[2], &w[3], &w[4], &w[5], &w[6],
                      &w[7], &texture_type, &depth, &max_mip, &width,
                      &height) == 14) {
            VPGxmShadowSamplerPlan plan;
            int status = vp_gxm_shadow_sampler_plan(
                (const uint32_t *)w, texture_type, depth, max_mip, width,
                height, &plan);
            printf("%s %d %u %u %u %u %u %u %a %a %a %a %a %a %a\n", id,
                   status, plan.compare_func, plan.maxwell_filter,
                   plan.filter_code, plan.wrap_u, plan.wrap_v,
                   plan.border_used, (double)plan.extent[0],
                   (double)plan.extent[1], (double)plan.state[0],
                   (double)plan.state[1], (double)plan.state[2],
                   (double)plan.state[3], (double)plan.border[0]);
        }
        fclose(file);
        return 0;
    }
    test_accepted_matrix();
    test_rejections();
    test_named_values();
    test_source_mode();
    if (failures != 0) {
        printf("FAILURES %d\n", failures);
        return 1;
    }
    printf("gxm_shadow_sampler: all assertions passed\n");
    return 0;
}

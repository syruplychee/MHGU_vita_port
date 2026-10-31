/* Host test for explicit ordinary vs software-shadow depth-view mode. */
#include "gxm_depth_view.h"
#include "gxm_resource_layout.h"
#include "gxm_shadow_sampler.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static uint32_t f32_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void build_tic(uint32_t tic[8], unsigned width, unsigned height) {
    memset(tic, 0, 8u * sizeof(uint32_t));
    tic[0] = VP_GXM_TIC_FORMAT_Z32 | (7u << 7) | (7u << 10) | (7u << 13) |
             (7u << 16) | (2u << 19) | (0u << 22) | (0u << 25) | (7u << 28);
    tic[2] = 3u << 21;
    tic[3] = 4u << 3;
    tic[4] = (width - 1u) | (1u << 23);
    tic[5] = (height - 1u) | (1u << 31);
}

static void build_shadow_tsc(uint32_t tsc[8], unsigned func, unsigned filter,
                             unsigned wrap_u, unsigned wrap_v, float border) {
    memset(tsc, 0, 8u * sizeof(uint32_t));
    tsc[0] = (wrap_u & 7u) | ((wrap_v & 7u) << 3) | (1u << 9) |
             ((func & 7u) << 10);
    tsc[1] = (filter & 3u) | ((filter & 3u) << 4) | (1u << 6);
    tsc[4] = f32_bits(border);
}

static const uint32_t ordinary_tsc[8] = {
    0x00026092u, 0x00000391u, 0x00f00000u, 0u, 0u, 0u, 0u, 0u
};

static void base_input(VPGxmDepthViewInput *in, uint32_t tic[8],
                       uint32_t tsc[8]) {
    memset(in, 0, sizeof(*in));
    in->va = 0x12340000u;
    in->tic = tic;
    in->tsc = tsc;
    in->expected_descriptor_type = 2u;
    in->descriptor_depth = 0u;
    in->sampler_mode = VP_GXM_DEPTH_SAMPLER_MODE_ORDINARY;
    in->shadow_contract_complete = 0u;
    in->usage_flags = VP_RESOURCE_TEXTURE_USAGE_ANALYZED |
                      VP_RESOURCE_TEXTURE_USAGE_R;
    in->entry_va = in->va;
    in->entry_width = 64u;
    in->entry_height = 64u;
    in->entry_format_class = VP_GXM_DEPTH_CLASS_DF32;
    in->entry_stride_samples = 64u;
    in->content_state = VP_GXM_DEPTH_CONTENT_VALID;
    in->content_generation = 1u;
    in->entry_storage_generation = 1u;
}

static void expect_status(const char *name, int actual, int expected) {
    if (actual != expected) {
        printf("FAIL %-34s expected=%d %s actual=%d %s\n", name, expected,
               vp_gxm_depth_view_status_name(expected), actual,
               vp_gxm_depth_view_status_name(actual));
        failures++;
    }
}

static void expect_u32(const char *name, unsigned actual, unsigned expected) {
    if (actual != expected) {
        printf("FAIL %-34s expected=%u actual=%u\n", name, expected, actual);
        failures++;
    }
}

int main(void) {
    uint32_t tic[8], tsc[8];
    VPGxmDepthViewInput in;
    VPGxmDepthViewPlan plan;
    int rc;

    build_tic(tic, 64u, 64u);
    build_shadow_tsc(tsc, 3u, 2u, 3u, 1u, 0.25f);
    base_input(&in, tic, tsc);
    in.descriptor_depth = 1u;
    in.sampler_mode = VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW;
    in.shadow_contract_complete = 1u;
    rc = vp_gxm_depth_view_plan(&in, &plan);
    expect_status("shadow accepted", rc, VP_GXM_DEPTH_VIEW_OK);
    expect_u32("shadow mode", plan.sampler_mode,
               VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW);
    expect_u32("native min point", plan.min_filter, 1u);
    expect_u32("native mag point", plan.mag_filter, 1u);
    expect_u32("native clamp u", plan.addr_u, 2u);
    expect_u32("native clamp v", plan.addr_v, 2u);
    expect_u32("compare", plan.shadow_compare_func, 3u);
    expect_u32("maxwell filter", plan.shadow_maxwell_filter, 2u);
    expect_u32("filter code", plan.shadow_filter_code, 1u);
    expect_u32("border used", plan.shadow_border_used, 1u);
    expect_u32("shadow status", (unsigned)plan.shadow_status,
               (unsigned)VP_GXM_SHADOW_OK);
    if (plan.shadow_state[0] != 3.0f || plan.shadow_state[2] != 3.0f ||
        plan.shadow_state[3] != 1.0f || plan.shadow_border[0] != 0.25f ||
        plan.shadow_extent[0] != 64.0f) {
        printf("FAIL shadow float4 layout\n");
        failures++;
    }

    base_input(&in, tic, (uint32_t *)ordinary_tsc);
    /* The TIC depth/layer field is 1 for this ordinary 2D descriptor too.  The
     * sidecar descriptor depth is 0, so this must stay an ordinary view and
     * must not be mistaken for a shadow descriptor. */
    expect_u32("tic depth/layers", ((tic[5] >> 16) & 0x3fffu) + 1u, 1u);
    rc = vp_gxm_depth_view_plan(&in, &plan);
    expect_status("ordinary compare off", rc, VP_GXM_DEPTH_VIEW_OK);
    expect_u32("ordinary mode", plan.sampler_mode,
               VP_GXM_DEPTH_SAMPLER_MODE_ORDINARY);
    expect_u32("ordinary tsc filter", plan.min_filter, 1u);

    base_input(&in, tic, tsc);
    rc = vp_gxm_depth_view_plan(&in, &plan);
    expect_status("ordinary compare on rejected", rc,
                  VP_GXM_DEPTH_VIEW_ERR_COMPARE);

    base_input(&in, tic, tsc);
    in.descriptor_depth = 1u;
    in.sampler_mode = VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW;
    in.shadow_contract_complete = 0u;
    rc = vp_gxm_depth_view_plan(&in, &plan);
    expect_status("shadow incomplete contract", rc,
                  VP_GXM_DEPTH_VIEW_ERR_SHADOW_CONTRACT);

    base_input(&in, tic, tsc);
    in.descriptor_depth = 0u;
    in.sampler_mode = VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW;
    in.shadow_contract_complete = 1u;
    rc = vp_gxm_depth_view_plan(&in, &plan);
    expect_status("shadow without depth flag", rc,
                  VP_GXM_DEPTH_VIEW_ERR_SHADOW_DEPTH);

    base_input(&in, tic, tsc);
    in.descriptor_depth = 1u;
    in.sampler_mode = VP_GXM_DEPTH_SAMPLER_MODE_ORDINARY;
    rc = vp_gxm_depth_view_plan(&in, &plan);
    expect_status("ordinary with shadow flag", rc,
                  VP_GXM_DEPTH_VIEW_ERR_SHADOW_CONTRACT);

    build_shadow_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    tsc[0] |= 1u << 13; /* sRGB bit: the software shadow path ignores it */
    base_input(&in, tic, tsc);
    in.descriptor_depth = 1u;
    in.sampler_mode = VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW;
    in.shadow_contract_complete = 1u;
    rc = vp_gxm_depth_view_plan(&in, &plan);
    expect_status("shadow srgb accepted", rc, VP_GXM_DEPTH_VIEW_OK);
    expect_u32("shadow srgb status", (unsigned)plan.shadow_status,
               (unsigned)VP_GXM_SHADOW_OK);

    build_shadow_tsc(tsc, 1u, 1u, 0u, 0u, 0.0f);
    base_input(&in, tic, tsc);
    in.descriptor_depth = 1u;
    in.sampler_mode = VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW;
    in.shadow_contract_complete = 1u;
    in.content_state = VP_GXM_DEPTH_CONTENT_CLEARED;
    rc = vp_gxm_depth_view_plan(&in, &plan);
    expect_status("shadow cleared rejected", rc,
                  VP_GXM_DEPTH_VIEW_ERR_CLEARED);
    base_input(&in, tic, tsc);
    in.descriptor_depth = 1u;
    in.sampler_mode = VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW;
    in.shadow_contract_complete = 1u;
    in.entry_storage_generation = 0u;
    rc = vp_gxm_depth_view_plan(&in, &plan);
    expect_status("shadow epoch rejected", rc,
                  VP_GXM_DEPTH_VIEW_ERR_GENERATION);
    base_input(&in, tic, tsc);
    in.descriptor_depth = 1u;
    in.sampler_mode = VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW;
    in.shadow_contract_complete = 1u;
    in.entry_va += 4u;
    rc = vp_gxm_depth_view_plan(&in, &plan);
    expect_status("shadow wrong va", rc, VP_GXM_DEPTH_VIEW_ERR_VA);

    if (failures != 0) {
        printf("FAILURES %d\n", failures);
        return 1;
    }
    printf("gxm_depth_view_shadow: all assertions passed\n");
    return 0;
}

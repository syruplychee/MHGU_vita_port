/* Host truth table for the depth-texture view and TSC sampler planner. */
#include "gxm_depth_view.h"
#include "gxm_resource_layout.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

/* Build the eight raw TIC words from the fields the planner decodes. */
static void build_tic(uint32_t tic[8], unsigned format,
                      unsigned r, unsigned g, unsigned b, unsigned a,
                      unsigned x, unsigned y, unsigned z, unsigned w,
                      unsigned width, unsigned height) {
    memset(tic, 0, 8 * sizeof(uint32_t));
    tic[0] = (format & 0x7fu) | (r << 7) | (g << 10) | (b << 13) |
             (a << 16) | (x << 19) | (y << 22) | (z << 25) | (w << 28);
    tic[2] = 3u << 21;                 /* header version 3, block linear */
    tic[3] = 4u << 3;                  /* observed block height, no mips */
    tic[4] = (width - 1u) | (1u << 23);
    tic[5] = (height - 1u) | (1u << 31);
    tic[7] = 0;
}

static void canonical_z24s8(uint32_t tic[8], unsigned width, unsigned height) {
    /* The live stage-429 descriptor 0x76da4a29: Z24S8, component types
     * UINT/UNORM/UNORM/UNORM, channel selectors G/G/G/one. */
    build_tic(tic, VP_GXM_TIC_FORMAT_Z24S8, 4, 2, 2, 2, 3, 3, 3, 7,
              width, height);
}

static void canonical_s8z24(uint32_t tic[8], unsigned width, unsigned height) {
    build_tic(tic, VP_GXM_TIC_FORMAT_S8Z24, 2, 4, 4, 4, 2, 2, 2, 7,
              width, height);
}

static void canonical_z32(uint32_t tic[8], unsigned width, unsigned height) {
    build_tic(tic, VP_GXM_TIC_FORMAT_Z32, 7, 7, 7, 7, 2, 0, 0, 7,
              width, height);
}

typedef struct {
    uint32_t tic[8];
    uint32_t tsc[8];
    VPGxmDepthViewInput in;
} Fixture;

static void base_input(Fixture *f) {
    memset(f, 0, sizeof(*f));
    canonical_z24s8(f->tic, 1280, 720);
    /* The live descriptor: wrap_u/v clamp, no compare, sRGB bit set,
     * anisotropy 0; filters nearest/nearest, mip nearest with base only. */
    f->tsc[0] = 0x00026092u;
    f->tsc[1] = 0x00000391u;
    f->tsc[2] = 0x00f00000u;
    f->in.va = 0x56e940000ull;
    f->in.tic = f->tic;
    f->in.tsc = f->tsc;
    f->in.expected_descriptor_type = 2;
    f->in.entry_va = f->in.va;
    f->in.entry_width = 1280;
    f->in.entry_height = 720;
    f->in.entry_format_class = VP_GXM_DEPTH_CLASS_S8D24;
    f->in.entry_stride_samples = 1280;
    f->in.content_state = VP_GXM_DEPTH_CONTENT_VALID;
    f->in.content_generation = 7;
    f->in.entry_storage_generation = 3;
    f->in.usage_flags = VP_RESOURCE_TEXTURE_USAGE_ANALYZED |
                        VP_RESOURCE_TEXTURE_USAGE_R;
}

static int plan_status(const VPGxmDepthViewInput *in) {
    VPGxmDepthViewPlan plan;
    return vp_gxm_depth_view_plan(in, &plan);
}

static void test_positive(void) {
    Fixture f;
    VPGxmDepthViewPlan plan;
    int rc;

    base_input(&f);

    rc = vp_gxm_depth_view_plan(&f.in, &plan);
    CHECK(rc == 0);
    CHECK(plan.format == VP_GXM_DEPTH_VIEW_FORMAT_X8U24_SD);
    CHECK(plan.depth_class == VP_GXM_DEPTH_CLASS_S8D24);
    CHECK(plan.tic_format == VP_GXM_TIC_FORMAT_Z24S8);
    CHECK(plan.source_component == 3u);
    CHECK(plan.width == 1280u && plan.height == 720u);
    CHECK(plan.content_generation == 7u);
    CHECK(plan.storage_generation == 3u);
    CHECK(plan.min_filter == VP_GXM_DEPTH_SAMPLER_FILTER_POINT);
    CHECK(plan.mag_filter == VP_GXM_DEPTH_SAMPLER_FILTER_POINT);
    CHECK(plan.addr_u == VP_GXM_DEPTH_SAMPLER_ADDR_CLAMP);
    CHECK(plan.addr_v == VP_GXM_DEPTH_SAMPLER_ADDR_CLAMP);

    /* DF32 1024x3072 atlas, nearest, mirror/clamp. */
    canonical_z32(f.tic, 1024, 3072);
    f.in.entry_format_class = VP_GXM_DEPTH_CLASS_DF32;
    f.in.entry_width = 1024;
    f.in.entry_height = 3072;
    f.in.entry_stride_samples = 1024;
    f.tsc[0] = 1u | (2u << 3); /* wrap_u mirror, wrap_v clamp */
    f.tsc[1] = 1u | (1u << 4) | (1u << 6);
    rc = vp_gxm_depth_view_plan(&f.in, &plan);
    CHECK(rc == 0);
    CHECK(plan.format == VP_GXM_DEPTH_VIEW_FORMAT_F32_R);
    CHECK(plan.min_filter == VP_GXM_DEPTH_SAMPLER_FILTER_POINT);
    CHECK(plan.mag_filter == VP_GXM_DEPTH_SAMPLER_FILTER_POINT);
    CHECK(plan.addr_u == VP_GXM_DEPTH_SAMPLER_ADDR_MIRROR);
    CHECK(plan.addr_v == VP_GXM_DEPTH_SAMPLER_ADDR_CLAMP);

    /* S8Z24 TIC and 640x360 shape. */
    canonical_s8z24(f.tic, 640, 360);
    f.in.entry_format_class = VP_GXM_DEPTH_CLASS_S8D24;
    f.in.entry_width = 640;
    f.in.entry_height = 360;
    f.in.entry_stride_samples = 640;
    f.tsc[0] = 0;
    f.tsc[1] = 2u | (2u << 4) | (1u << 6);
    rc = vp_gxm_depth_view_plan(&f.in, &plan);
    CHECK(rc == 0);
    CHECK(plan.format == VP_GXM_DEPTH_VIEW_FORMAT_X8U24_SD);
    CHECK(plan.source_component == 2u);
}

static void test_view_rejections(void) {
    Fixture f;
    VPGxmDepthViewPlan plan;

    base_input(&f);
    f.in.content_state = VP_GXM_DEPTH_CONTENT_NONE;
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_UNPRODUCED);

    base_input(&f);
    f.in.content_state = VP_GXM_DEPTH_CONTENT_CLEARED;
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_CLEARED);

    base_input(&f);
    f.in.content_generation = 0;
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_UNPRODUCED);

    base_input(&f);
    f.in.entry_storage_generation = 0; /* recreated/overflowed storage epoch */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_GENERATION);

    base_input(&f);
    f.in.tic = NULL;
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_ARG);
    base_input(&f);
    f.in.tsc = NULL;
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_ARG);

    base_input(&f);
    f.in.expected_descriptor_type = 3;
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_TYPE);

    base_input(&f);
    f.in.entry_va += 0x1000;
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_VA);

    base_input(&f);
    f.in.usage_flags = 0u; /* no offline GLSL usage identity */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_USAGE);
    base_input(&f);
    f.in.usage_flags = VP_RESOURCE_TEXTURE_USAGE_ANALYZED |
                       VP_RESOURCE_TEXTURE_USAGE_R |
                       VP_RESOURCE_TEXTURE_USAGE_G; /* shader reads .g */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_USAGE);
    base_input(&f);
    f.in.usage_flags = VP_RESOURCE_TEXTURE_USAGE_R; /* not analyzed */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_USAGE);

    base_input(&f);
    f.in.entry_format_class = VP_GXM_DEPTH_CLASS_DF32;
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_CLASS);

    base_input(&f);
    build_tic(f.tic, 8, 2, 2, 2, 2, 2, 2, 2, 7, 1280, 720);
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_NOT_DEPTH);

    base_input(&f);
    build_tic(f.tic, VP_GXM_TIC_FORMAT_X8Z24, 2, 2, 2, 2, 2, 2, 2, 7,
              1280, 720);
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_TIC_FORMAT);

    base_input(&f);
    build_tic(f.tic, 0x3a, 2, 2, 2, 2, 2, 2, 2, 7, 1280, 720); /* Z16 */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_TIC_FORMAT);

    base_input(&f);
    f.tic[2] = 2u << 21; /* header 2 */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_HEADER);

    base_input(&f);
    f.tic[4] = (f.tic[4] & 0xffffu) | (3u << 23);
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_TYPE);

    base_input(&f);
    f.tic[5] |= 1u << 16; /* two layers */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_LAYERS);

    base_input(&f);
    f.tic[3] |= 1u << 6; /* block depth */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_LAYERS);

    base_input(&f);
    f.tic[3] |= 1u << 28; /* max mip level 1 */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_MIPS);

    base_input(&f);
    f.tic[7] |= 1u << 8; /* 2x MSAA */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_MSAA);

    base_input(&f);
    f.tic[5] &= ~(1u << 31);
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_NORMALIZED);

    base_input(&f);
    f.tic[5] |= 1u << 30;
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_SPARSE);

    base_input(&f);
    f.tic[4] |= 1u << 22;
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_SRGB);

    base_input(&f);
    f.tic[0] = (f.tic[0] & ~(7u << 7)) | (2u << 7); /* R must be UINT */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_COMPONENT);

    base_input(&f);
    f.tic[0] = (f.tic[0] & ~(7u << 19)) | (2u << 19); /* .r from R, not G */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_SWIZZLE);
    base_input(&f);
    f.tic[0] = (f.tic[0] & ~(7u << 22)) | (5u << 22); /* .g from alpha */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_SWIZZLE);
    base_input(&f);
    f.tic[0] = (f.tic[0] & ~(7u << 28)) | (6u << 28); /* integer one */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_SWIZZLE);

    base_input(&f);
    f.tic[5] = (f.tic[5] & ~0xffffu) | (719u - 16u); /* 704 rows */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_SHAPE);

    base_input(&f);
    f.tic[4] = (f.tic[4] & ~0xffffu) | 1278u;
    f.in.entry_width = 1279;
    f.in.entry_stride_samples = 1280;
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_STRIDE);

    base_input(&f);
    f.in.entry_stride_samples = 1344; /* valid 64-aligned stride != width */
    CHECK(plan_status(&f.in) == VP_GXM_DEPTH_VIEW_ERR_STRIDE);

    base_input(&f);
    f.tic[1] = 0; /* address word cannot change the identity, but VA check
                    * still uses the guest VA/entry VA fields. */
    CHECK(vp_gxm_depth_view_plan(&f.in, &plan) == 0);
    CHECK(plan.width == 1280u);
}

static void test_tsc(void) {
    unsigned minf, magf, u, v;
    uint32_t tsc[8];

    memset(tsc, 0, sizeof(tsc));
    tsc[1] = 2u | (2u << 4) | (1u << 6);
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) == 0);
    CHECK(minf == VP_GXM_DEPTH_SAMPLER_FILTER_LINEAR);
    CHECK(magf == VP_GXM_DEPTH_SAMPLER_FILTER_LINEAR);

    /* The live depth TSC: sRGB set, base-only mip filter nearest.  Neither
     * changes a depth value; both are accepted and the native mip filter is
     * disabled by the caller. */
    tsc[0] = 0x00026092u;
    tsc[1] = 0x00000391u;
    tsc[2] = 0x00f00000u;
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) == 0);
    CHECK(minf == VP_GXM_DEPTH_SAMPLER_FILTER_POINT);
    CHECK(magf == VP_GXM_DEPTH_SAMPLER_FILTER_POINT);
    CHECK(u == VP_GXM_DEPTH_SAMPLER_ADDR_CLAMP);
    CHECK(v == VP_GXM_DEPTH_SAMPLER_ADDR_CLAMP);
    /* With real mip levels the same mip filter is not representable. */
    CHECK(vp_gxm_depth_tsc_plan(tsc, 1, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_FILTER);

    /* Comparison enable bit 9 is not lowered in this milestone. */
    tsc[0] = 1u << 9;
    tsc[1] = 2u | (2u << 4) | (1u << 6);
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_COMPARE);

    /* Maxwell wrap=3 without other flags must be refused, not coerced. */
    tsc[0] = 0x000000dbu;
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_WRAP);
    /* The observed raw 0x000260db also sets the sRGB bit; it must still be
     * refused for wrap=3. */
    tsc[0] = 0x000260dbu;
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_WRAP);

    tsc[0] = 0;
    tsc[1] = 0u | (2u << 4) | (1u << 6); /* mag 0 reserved */
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_FILTER);
    tsc[1] = 3u | (2u << 4) | (1u << 6);
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_FILTER);
    tsc[1] = 2u | (0u << 4) | (1u << 6);
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_FILTER);
    tsc[1] = 2u | (2u << 4) | (0u << 6); /* mip 0 reserved */
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_FILTER);
    tsc[1] = 2u | (2u << 4) | (2u << 6); /* base-only nearest mip */
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) == 0);
    tsc[1] = 2u | (2u << 4) | (1u << 6);

    tsc[0] = 4u; /* wrap 4 = Clamp, not proven */
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_WRAP);
    tsc[0] = 5u; /* MirrorOnceClampToEdge */
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_WRAP);
    tsc[0] = 1u << 20; /* forbidden anisotropy */
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_ANISOTROPY);
    tsc[0] = 1u << 10; /* compare func bits with compare disabled: ignored */
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) == 0);
    tsc[0] = (1u << 9) | (2u << 10); /* compare enabled: unsupported */
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_COMPARE);
    tsc[0] = 0;
    tsc[1] = 2u | (2u << 4) | (1u << 6) | (1u << 10); /* reduction */
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_TSC);
    tsc[1] = 2u | (2u << 4) | (1u << 6) | (1u << 12); /* LOD bias */
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_TSC);
    tsc[1] = 2u | (2u << 4) | (1u << 6) | (1u << 25); /* float coord */
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_TSC);
    tsc[1] = 2u | (2u << 4) | (1u << 6) | (1u << 26); /* trilinear opt */
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) ==
          VP_GXM_DEPTH_VIEW_ERR_TSC);
    tsc[0] = 1u | (2u << 3); /* mirror U, clamp V */
    tsc[1] = 1u | (2u << 4) | (1u << 6);
    CHECK(vp_gxm_depth_tsc_plan(tsc, 0, &minf, &magf, &u, &v) == 0);
    CHECK(u == VP_GXM_DEPTH_SAMPLER_ADDR_MIRROR);
    CHECK(v == VP_GXM_DEPTH_SAMPLER_ADDR_CLAMP);
}

int main(void) {
    test_positive();
    test_view_rejections();
    test_tsc();
    if (failures != 0) {
        fprintf(stderr, "gxm_depth_view: %d failure(s)\n", failures);
        return 1;
    }
    printf("gxm_depth_view: all checks passed\n");
    return 0;
}

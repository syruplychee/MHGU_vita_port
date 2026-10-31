/* Host tests for the pure guest-depth identity/pool planner.
 *
 * The pool never sees a colour address: a draw into colour A and a draw into
 * colour B that both bind the same guest zeta VA perform identical acquire
 * calls and must reuse the same depth identity. Clear uses the same acquire
 * path. A full pool refuses; it never evicts a live identity whose content is
 * still required.
 */
#include "gxm_depth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static VPGxmDepthDesc make_desc(unsigned enabled, uint64_t va, unsigned width,
                                unsigned height, unsigned depth,
                                unsigned dim_control, unsigned format,
                                unsigned tile_mode, unsigned array_pitch) {
    VPGxmDepthDesc desc;
    int rc = vp_gxm_depth_desc_build(&desc, enabled, va, width, height, depth,
                                     dim_control, format, tile_mode,
                                     array_pitch);
    CHECK(rc == VP_GXM_DEPTH_OK);
    return desc;
}

static void test_descriptor_plans(void) {
    VPGxmDepthDesc d = make_desc(1u, 0x1000u, 1920u, 1080u, 1u, 0u,
                                 VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    CHECK(d.format_class == VP_GXM_DEPTH_CLASS_DF32);
    CHECK(d.stride_samples == 1920u);
    CHECK(d.storage_height == 1088u); /* 1080 rows occupy 34 whole tiles */
    CHECK(d.depth_bytes == 1920u * 1088u * 4u);
    CHECK(d.depth_alloc_bytes == d.depth_bytes);
    CHECK(d.stencil_bytes == 0u);
    CHECK(d.stencil_alloc_bytes == 0u);
    CHECK(d.total_alloc_bytes == d.depth_bytes);
    CHECK(d.layers == 1u);

    d = make_desc(1u, 0x2000u, 1280u, 720u, 1u, 0u,
                  VP_GXM_DEPTH_FORMAT_S8Z24_UNORM, 1u, 0u);
    CHECK(d.format_class == VP_GXM_DEPTH_CLASS_S8D24);
    CHECK(d.storage_height == 736u);
    CHECK(d.depth_bytes == 1280u * 736u * 4u);
    CHECK(d.stencil_bytes == 1280u * 736u);
    CHECK(d.depth_alloc_bytes == 1280u * 736u * 4u);
    CHECK(d.stencil_alloc_bytes == 1280u * 736u);
    CHECK(d.total_alloc_bytes == 1280u * 736u * 5u);

    /* Half-page stencil payload must be counted as a full page allocation. */
    d = make_desc(1u, 0x2100u, 64u, 32u, 1u, 0u,
                  VP_GXM_DEPTH_FORMAT_Z24_UNORM_S8_UINT, 0u, 0u);
    CHECK(d.depth_bytes == 64u * 32u * 4u);
    CHECK(d.stencil_bytes == 64u * 32u);
    CHECK(d.stencil_alloc_bytes == VP_GXM_DEPTH_ALLOC_ALIGN);
    CHECK(d.total_alloc_bytes == 64u * 32u * 4u + VP_GXM_DEPTH_ALLOC_ALIGN);
    CHECK(d.tile_mode == 0u);

    /* Non-64 width and non-32 height get explicit native padding. */
    d = make_desc(1u, 0x3000u, 100u, 50u, 1u, 0u,
                  VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    CHECK(d.stride_samples == 128u);
    CHECK(d.storage_height == 64u);
    CHECK(d.depth_bytes == 128u * 64u * 4u);

    /* TileMode is packed, not an enum: block_width=3 must be accepted. */
    d = make_desc(1u, 0x4100u, 64u, 64u, 1u, 0u,
                  VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 3u, 0u);
    CHECK(d.tile_block_width == 3u);
    CHECK(d.tile_block_height == 0u);
    CHECK(d.tile_dim_control == 0u);

    /* Pitch-linear plus block_height=1 (bits 12 and 4). */
    d = make_desc(1u, 0x4200u, 64u, 64u, 1u, 0u,
                  VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0x1010u, 0u);
    CHECK(d.tile_pitch_linear == 1u);
    CHECK(d.tile_block_height == 1u);

    /* A nonzero one-layer array pitch is valid metadata, not an error. */
    d = make_desc(1u, 0x4300u, 64u, 64u, 1u, 0u,
                  VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0x1234u);
    CHECK(d.array_pitch == 0x1234u);
    CHECK(d.layers == 1u);

    /* ArraySizeIsOne makes the raw depth field irrelevant for layer count. */
    d = make_desc(1u, 0x4400u, 64u, 64u, 7u, 1u,
                  VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    CHECK(d.layers == 1u);
    CHECK(d.raw_depth == 7u);
    CHECK(d.dim_control == 1u);
}

static void test_descriptor_rejections(void) {
    VPGxmDepthDesc d;
    struct {
        unsigned enabled;
        uint64_t va;
        unsigned w, h, depth, dim, format, tile, pitch;
        int want;
    } cases[] = {
        {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, VP_GXM_DEPTH_DISABLED},
        {1u, 0u, 64u, 64u, 1u, 0u, VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u,
         VP_GXM_DEPTH_ERR_VA},
        {1u, 0x10u, 0u, 64u, 1u, 0u, VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u,
         VP_GXM_DEPTH_ERR_DIM},
        {1u, 0x10u, 64u, 0u, 1u, 0u, VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u,
         VP_GXM_DEPTH_ERR_DIM},
        {1u, 0x10u, 4097u, 64u, 1u, 0u, VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u,
         VP_GXM_DEPTH_ERR_DIM},
        {1u, 0x10u, 64u, 4097u, 1u, 0u, VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u,
         VP_GXM_DEPTH_ERR_DIM},
        {1u, 0x10u, 64u, 64u, 0u, 0u, VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u,
         VP_GXM_DEPTH_ERR_LAYERS},
        /* DefineArraySize with depth 2 is a layered array: unsupported. */
        {1u, 0x10u, 64u, 64u, 2u, 0u, VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u,
         VP_GXM_DEPTH_ERR_LAYERS},
        {1u, 0x10u, 64u, 64u, 1u, 2u, VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u,
         VP_GXM_DEPTH_ERR_DIM_CONTROL},
        /* Undefined TileMode bits 13 and 17. */
        {1u, 0x10u, 64u, 64u, 1u, 0u, VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0x2000u,
         0u, VP_GXM_DEPTH_ERR_TILE_MODE},
        {1u, 0x10u, 64u, 64u, 1u, 0u, VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0x20000u,
         0u, VP_GXM_DEPTH_ERR_TILE_MODE},
        /* TileMode::DefineDepthSize is a 3D/volume zeta. */
        {1u, 0x10u, 64u, 64u, 1u, 1u, VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0x10000u,
         0u, VP_GXM_DEPTH_ERR_VOLUME},
        {1u, 0x10u, 64u, 64u, 1u, 0u, 0x13u, 0u, 0u, /* Z16_UNORM */
         VP_GXM_DEPTH_ERR_FORMAT},
        {1u, 0x10u, 64u, 64u, 1u, 0u, 0x15u, 0u, 0u, /* X8Z24 */
         VP_GXM_DEPTH_ERR_FORMAT},
        {1u, 0x10u, 64u, 64u, 1u, 0u, 0x17u, 0u, 0u, /* S8_UINT */
         VP_GXM_DEPTH_ERR_FORMAT},
        {1u, 0x10u, 64u, 64u, 1u, 0u, 0x18u, 0u, 0u, /* V8Z24 */
         VP_GXM_DEPTH_ERR_FORMAT},
        {1u, 0x10u, 64u, 64u, 1u, 0u, 0x19u, 0u, 0u, /* Z32F_X24S8 */
         VP_GXM_DEPTH_ERR_FORMAT},
        {1u, 0x10u, 64u, 64u, 1u, 0u, 0xd5u, 0u, 0u, /* RT colour format */
         VP_GXM_DEPTH_ERR_FORMAT}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int rc = vp_gxm_depth_desc_build(&d, cases[i].enabled, cases[i].va,
                cases[i].w, cases[i].h, cases[i].depth, cases[i].dim,
                cases[i].format, cases[i].tile, cases[i].pitch);
        CHECK(rc == cases[i].want);
        if (cases[i].enabled) CHECK(d.format_class == VP_GXM_DEPTH_CLASS_NONE);
    }
}

static void test_byte_planner_and_fits(void) {
    size_t bytes = 123u;
    CHECK(vp_gxm_depth_plan_bytes(0u, 10u, 4u, &bytes) == VP_GXM_DEPTH_ERR_ARG);
    CHECK(vp_gxm_depth_plan_bytes(10u, 0u, 4u, &bytes) == VP_GXM_DEPTH_ERR_ARG);
    CHECK(vp_gxm_depth_plan_bytes(10u, 10u, 0u, &bytes) == VP_GXM_DEPTH_ERR_ARG);
    CHECK(vp_gxm_depth_plan_bytes(10u, 10u, 4u, NULL) == VP_GXM_DEPTH_ERR_ARG);
    CHECK(vp_gxm_depth_plan_bytes(UINT32_MAX, UINT32_MAX, 4u, &bytes) ==
          VP_GXM_DEPTH_ERR_OVERFLOW);
    CHECK(bytes == 123u);

    {
        VPGxmDepthDesc d = make_desc(1u, 0x10u, 640u, 360u, 1u, 0u,
                                     VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
        CHECK(vp_gxm_depth_desc_fits(&d, 640u, 360u));
        CHECK(vp_gxm_depth_desc_fits(&d, 320u, 180u));
        CHECK(!vp_gxm_depth_desc_fits(&d, 641u, 360u));
        CHECK(!vp_gxm_depth_desc_fits(&d, 640u, 361u));
        CHECK(!vp_gxm_depth_desc_fits(&d, 0u, 360u));
        CHECK(!vp_gxm_depth_desc_fits(NULL, 640u, 360u));
        {
            VPGxmDepthDesc disabled;
            CHECK(vp_gxm_depth_desc_build(&disabled, 0u, 0u, 0u, 0u, 0u, 0u,
                                          0u, 0u, 0u) ==
                  VP_GXM_DEPTH_DISABLED);
            CHECK(!vp_gxm_depth_desc_fits(&disabled, 1u, 1u));
        }
    }
}

static void test_pool_reuse_and_isolation(void) {
    VPGxmDepthPool pool;
    VPGxmDepthPoolPlan plan;
    VPGxmDepthDesc a = make_desc(1u, 0xaaa000u, 1280u, 720u, 1u, 0u,
                                 VP_GXM_DEPTH_FORMAT_S8Z24_UNORM, 0u, 0u);
    VPGxmDepthDesc b = make_desc(1u, 0xbbb000u, 1280u, 720u, 1u, 0u,
                                 VP_GXM_DEPTH_FORMAT_S8Z24_UNORM, 0u, 0u);

    CHECK(vp_gxm_depth_pool_init(&pool, 8u, VP_GXM_DEPTH_BYTE_CAP) ==
          VP_GXM_DEPTH_OK);
    CHECK(vp_gxm_depth_pool_acquire(&pool, &a, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.action == VP_GXM_DEPTH_ACTION_CREATE);
    CHECK(plan.slot >= 0);
    CHECK(pool.bytes == a.total_alloc_bytes);
    {
        int slot_a = plan.slot;
        size_t bytes_after_a = pool.bytes;

        /* Colour A draw then colour B draw, same zeta: one identity. */
        CHECK(vp_gxm_depth_pool_acquire(&pool, &a, &plan) == VP_GXM_DEPTH_OK);
        CHECK(plan.action == VP_GXM_DEPTH_ACTION_REUSE);
        CHECK(plan.slot == slot_a);
        CHECK(pool.bytes == bytes_after_a);

        /* Depth clear on the same zeta reuses the same entry. */
        CHECK(vp_gxm_depth_pool_acquire(&pool, &a, &plan) == VP_GXM_DEPTH_OK);
        CHECK(plan.action == VP_GXM_DEPTH_ACTION_REUSE);
        CHECK(plan.slot == slot_a);
        CHECK(plan.evict_mask == 0u);
        CHECK(pool.bytes == bytes_after_a);

        /* A different guest zeta VA must not alias that identity. */
        CHECK(vp_gxm_depth_pool_acquire(&pool, &b, &plan) == VP_GXM_DEPTH_OK);
        CHECK(plan.action == VP_GXM_DEPTH_ACTION_CREATE);
        CHECK(plan.slot != slot_a);
        CHECK(pool.bytes == bytes_after_a + b.total_alloc_bytes);
    }
}

static void test_pool_hint_only_reuse(void) {
    VPGxmDepthPool pool;
    VPGxmDepthPoolPlan plan;
    VPGxmDepthDesc a = make_desc(1u, 0x5000u, 640u, 360u, 1u, 0u,
                                 VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    VPGxmDepthDesc hint = make_desc(1u, 0x5000u, 640u, 360u, 1u, 0u,
                                    VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0x1010u,
                                    0x80u);

    CHECK(vp_gxm_depth_pool_init(&pool, 8u, VP_GXM_DEPTH_BYTE_CAP) ==
          VP_GXM_DEPTH_OK);
    CHECK(vp_gxm_depth_pool_acquire(&pool, &a, &plan) == VP_GXM_DEPTH_OK);
    {
        int slot = plan.slot;
        size_t bytes = pool.bytes;
        CHECK(vp_gxm_depth_pool_acquire(&pool, &hint, &plan) ==
              VP_GXM_DEPTH_OK);
        CHECK(plan.action == VP_GXM_DEPTH_ACTION_REUSE);
        CHECK(plan.slot == slot);
        CHECK(plan.evict_mask == 0u);
        CHECK(pool.bytes == bytes);
        CHECK(pool.entries[slot].desc.tile_mode == 0x1010u);
        CHECK(pool.entries[slot].desc.array_pitch == 0x80u);
    }
}

static void test_pool_shape_and_format_changes(void) {
    VPGxmDepthPool pool;
    VPGxmDepthPoolPlan plan;
    VPGxmDepthDesc small = make_desc(1u, 0x6000u, 640u, 360u, 1u, 0u,
                                     VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    VPGxmDepthDesc large = make_desc(1u, 0x6000u, 1280u, 720u, 1u, 0u,
                                     VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    VPGxmDepthDesc stencil = make_desc(1u, 0x6000u, 1280u, 720u, 1u, 0u,
                                       VP_GXM_DEPTH_FORMAT_S8Z24_UNORM, 0u, 0u);

    CHECK(vp_gxm_depth_pool_init(&pool, 8u, VP_GXM_DEPTH_BYTE_CAP) ==
          VP_GXM_DEPTH_OK);
    CHECK(vp_gxm_depth_pool_acquire(&pool, &small, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.action == VP_GXM_DEPTH_ACTION_CREATE);
    {
        int slot = plan.slot;
        unsigned long long clock_before = pool.clock;
        CHECK(vp_gxm_depth_pool_acquire(&pool, &large, &plan) ==
              VP_GXM_DEPTH_OK);
        CHECK(plan.action == VP_GXM_DEPTH_ACTION_RECREATE);
        CHECK(plan.slot == slot);
        CHECK((plan.evict_mask & (1u << (unsigned)slot)) != 0u);
        CHECK(pool.clock == clock_before + 1u);
        CHECK(pool.bytes == large.total_alloc_bytes);
        CHECK(pool.entries[slot].desc.width == 1280u);

        CHECK(vp_gxm_depth_pool_acquire(&pool, &stencil, &plan) ==
              VP_GXM_DEPTH_OK);
        CHECK(plan.action == VP_GXM_DEPTH_ACTION_RECREATE);
        CHECK(plan.slot == slot);
        CHECK(pool.bytes == stencil.total_alloc_bytes);
        CHECK(pool.entries[slot].desc.format_class ==
              VP_GXM_DEPTH_CLASS_S8D24);
    }
}

static void test_pool_full_refuses_unchanged(void) {
    VPGxmDepthPool pool;
    VPGxmDepthPoolPlan plan;
    VPGxmDepthDesc a = make_desc(1u, 0xa0u, 640u, 360u, 1u, 0u,
                                 VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    VPGxmDepthDesc b = make_desc(1u, 0xb0u, 640u, 360u, 1u, 0u,
                                 VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    VPGxmDepthDesc c = make_desc(1u, 0xc0u, 640u, 360u, 1u, 0u,
                                 VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    VPGxmDepthPool before;

    CHECK(vp_gxm_depth_pool_init(&pool, 2u, VP_GXM_DEPTH_BYTE_CAP) ==
          VP_GXM_DEPTH_OK);
    CHECK(vp_gxm_depth_pool_acquire(&pool, &a, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.slot == 0);
    CHECK(vp_gxm_depth_pool_acquire(&pool, &b, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.slot == 1);
    /* Touch A does not make B dead: returning to B without a clear must
     * preserve B's depth content, so no LRU eviction is permitted. */
    CHECK(vp_gxm_depth_pool_acquire(&pool, &a, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.action == VP_GXM_DEPTH_ACTION_REUSE);
    before = pool;
    CHECK(vp_gxm_depth_pool_acquire(&pool, &c, &plan) ==
          VP_GXM_DEPTH_ERR_POOL);
    CHECK(plan.slot == -1);
    CHECK(memcmp(&pool, &before, sizeof(pool)) == 0);
    CHECK(pool.entries[0].desc.va == a.va);
    CHECK(pool.entries[1].desc.va == b.va);
}

static void test_pool_eight_slots_exhausted(void) {
    VPGxmDepthPool pool;
    VPGxmDepthPoolPlan plan;
    VPGxmDepthDesc base = make_desc(1u, 0x100u, 640u, 360u, 1u, 0u,
                                    VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    VPGxmDepthPool before;

    CHECK(vp_gxm_depth_pool_init(&pool, 8u, VP_GXM_DEPTH_BYTE_CAP) ==
          VP_GXM_DEPTH_OK);
    for (unsigned i = 0u; i < 8u; i++) {
        VPGxmDepthDesc d = base;
        d.va = 0x100u + i;
        CHECK(vp_gxm_depth_pool_acquire(&pool, &d, &plan) == VP_GXM_DEPTH_OK);
        CHECK(plan.action == VP_GXM_DEPTH_ACTION_CREATE);
    }
    CHECK(pool.bytes == 8u * base.total_alloc_bytes);
    before = pool;
    {
        VPGxmDepthDesc ninth = base;
        ninth.va = 0x900u;
        CHECK(vp_gxm_depth_pool_acquire(&pool, &ninth, &plan) ==
              VP_GXM_DEPTH_ERR_POOL);
        CHECK(plan.slot == -1);
        CHECK(memcmp(&pool, &before, sizeof(pool)) == 0);
    }
}

static void test_pool_byte_cap_refusal_and_recreate(void) {
    VPGxmDepthPool pool;
    VPGxmDepthPoolPlan plan;
    VPGxmDepthDesc max_s8d24 = make_desc(1u, 0x7000u, 2048u, 2048u, 1u, 0u,
                                         VP_GXM_DEPTH_FORMAT_S8Z24_UNORM, 0u, 0u);
    VPGxmDepthDesc second = max_s8d24;
    VPGxmDepthDesc fits = make_desc(1u, 0x9000u, 128u, 128u, 1u, 0u,
                                    VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    VPGxmDepthDesc too_big = make_desc(1u, 0x9008u, 1280u, 720u, 1u, 0u,
                                       VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    VPGxmDepthPool before;
    int fit_slot;

    CHECK(max_s8d24.total_alloc_bytes == 2048u * 2048u * 5u);
    CHECK(vp_gxm_depth_pool_init(&pool, 8u, VP_GXM_DEPTH_BYTE_CAP) ==
          VP_GXM_DEPTH_OK);
    CHECK(vp_gxm_depth_pool_acquire(&pool, &max_s8d24, &plan) ==
          VP_GXM_DEPTH_OK);
    CHECK(plan.action == VP_GXM_DEPTH_ACTION_CREATE);
    before = pool;
    second.va = 0x7008u;
    CHECK(vp_gxm_depth_pool_acquire(&pool, &second, &plan) ==
          VP_GXM_DEPTH_ERR_BUDGET);
    CHECK(plan.slot == -1);
    CHECK(memcmp(&pool, &before, sizeof(pool)) == 0);

    /* Tiny cap: a single entry larger than the cap is refused unchanged. */
    CHECK(vp_gxm_depth_pool_init(&pool, 8u, 1024u * 1024u) ==
          VP_GXM_DEPTH_OK);
    CHECK(too_big.total_alloc_bytes > 1024u * 1024u);
    CHECK(vp_gxm_depth_pool_acquire(&pool, &too_big, &plan) ==
          VP_GXM_DEPTH_ERR_BUDGET);
    CHECK(plan.slot == -1);
    CHECK(pool.bytes == 0u);
    CHECK(pool.clock == 0u);
    CHECK(fits.total_alloc_bytes < 1024u * 1024u);
    CHECK(vp_gxm_depth_pool_acquire(&pool, &fits, &plan) == VP_GXM_DEPTH_OK);
    fit_slot = plan.slot;
    before = pool;
    CHECK(vp_gxm_depth_pool_acquire(&pool, &too_big, &plan) ==
          VP_GXM_DEPTH_ERR_BUDGET);
    CHECK(memcmp(&pool, &before, sizeof(pool)) == 0);
    CHECK(pool.entries[fit_slot].desc.va == fits.va);

    /* A same-VA recreate that no longer fits the cap is refused unchanged. */
    {
        VPGxmDepthDesc huge = make_desc(1u, fits.va, 1920u, 1080u, 1u, 0u,
                                        VP_GXM_DEPTH_FORMAT_S8Z24_UNORM, 0u,
                                        0u);
        CHECK(huge.total_alloc_bytes > 1024u * 1024u);
        before = pool;
        CHECK(vp_gxm_depth_pool_acquire(&pool, &huge, &plan) ==
              VP_GXM_DEPTH_ERR_BUDGET);
        CHECK(memcmp(&pool, &before, sizeof(pool)) == 0);
    }
}

static void test_pool_release(void) {
    VPGxmDepthPool pool;
    VPGxmDepthPoolPlan plan;
    VPGxmDepthDesc a = make_desc(1u, 0xa1u, 640u, 360u, 1u, 0u,
                                 VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);

    CHECK(vp_gxm_depth_pool_init(&pool, 4u, VP_GXM_DEPTH_BYTE_CAP) ==
          VP_GXM_DEPTH_OK);
    CHECK(vp_gxm_depth_pool_acquire(&pool, &a, &plan) == VP_GXM_DEPTH_OK);
    vp_gxm_depth_pool_release(&pool, (unsigned)plan.slot);
    CHECK(pool.bytes == 0u);
    CHECK(!pool.entries[plan.slot].used);
    vp_gxm_depth_pool_release(&pool, (unsigned)plan.slot); /* idempotent */
    CHECK(pool.bytes == 0u);
}

static void test_clear_policy(void) {
    unsigned apply = 99u, coupled = 99u;

    /* Maxwell/GL: depth test off disables depth writes even if the raw write
     * bit is set. This normalization must precede masked-draw classification. */
    CHECK(vp_gxm_depth_effective_write(1u, 1u) == 1u);
    CHECK(vp_gxm_depth_effective_write(1u, 0u) == 0u);
    CHECK(vp_gxm_depth_effective_write(0u, 1u) == 0u);
    CHECK(vp_gxm_depth_effective_write(0u, 0u) == 0u);

    /* No depth/stencil bits: no depth action, colour may still clear. */
    CHECK(vp_gxm_depth_clear_plan(0x3cu, VP_GXM_DEPTH_CLASS_S8D24, 1u,
                                  &apply, &coupled) == VP_GXM_DEPTH_OK);
    CHECK(apply == 0u && coupled == 0u);

    /* Both cleared is the only exact stencil-reset representation. */
    CHECK(vp_gxm_depth_clear_plan(3u, VP_GXM_DEPTH_CLASS_S8D24, 1u,
                                  &apply, &coupled) == VP_GXM_DEPTH_OK);
    CHECK(apply == 3u && coupled == 0u);
    CHECK(vp_gxm_depth_clear_plan(0x3fu, VP_GXM_DEPTH_CLASS_S8D24, 0u,
                                  &apply, &coupled) == VP_GXM_DEPTH_OK);
    CHECK(apply == 3u && coupled == 0u);

    /* A stencil-only clear has no independent native load control. It is
     * refused even when accompanied by colour bits, before colour effects. */
    CHECK(vp_gxm_depth_clear_plan(2u, VP_GXM_DEPTH_CLASS_S8D24, 0u,
                                  &apply, &coupled) ==
          VP_GXM_DEPTH_CLEAR_ERR_STANDALONE);
    CHECK(apply == 0u && coupled == 0u);
    CHECK(vp_gxm_depth_clear_plan(0x3eu, VP_GXM_DEPTH_CLASS_S8D24, 0u,
                                  &apply, &coupled) ==
          VP_GXM_DEPTH_CLEAR_ERR_STANDALONE);

    /* A DF32 surface cannot carry stencil at all. */
    CHECK(vp_gxm_depth_clear_plan(3u, VP_GXM_DEPTH_CLASS_DF32, 0u,
                                  &apply, &coupled) ==
          VP_GXM_DEPTH_ERR_STENCIL);
    CHECK(vp_gxm_depth_clear_plan(1u, VP_GXM_DEPTH_CLASS_DF32, 0u,
                                  &apply, &coupled) == VP_GXM_DEPTH_OK);
    CHECK(apply == 1u && coupled == 0u);

    /* Depth-only on S8D24: allowed while stencil has no live content, with the
     * coupled stencil background load recorded; refused once it may be live. */
    CHECK(vp_gxm_depth_clear_plan(1u, VP_GXM_DEPTH_CLASS_S8D24, 0u,
                                  &apply, &coupled) == VP_GXM_DEPTH_OK);
    CHECK(apply == 1u && coupled == 1u);
    apply = 99u;
    coupled = 99u;
    CHECK(vp_gxm_depth_clear_plan(0x3du, VP_GXM_DEPTH_CLASS_S8D24, 1u,
                                  &apply, &coupled) ==
          VP_GXM_DEPTH_CLEAR_ERR_PRESERVE);
    CHECK(apply == 0u && coupled == 0u);

    CHECK(vp_gxm_depth_clear_plan(1u, VP_GXM_DEPTH_CLASS_NONE, 0u,
                                  &apply, &coupled) == VP_GXM_DEPTH_ERR_ARG);
    CHECK(vp_gxm_depth_clear_plan(0u, VP_GXM_DEPTH_CLASS_DF32, 0u, NULL,
                                  &coupled) == VP_GXM_DEPTH_ERR_ARG);
    CHECK(vp_gxm_depth_clear_plan(0u, VP_GXM_DEPTH_CLASS_DF32, 0u, &apply,
                                  NULL) == VP_GXM_DEPTH_ERR_ARG);
}

static void test_clear_target_independence(void) {
    VPGxmDepthDesc depth = make_desc(1u, 0x7100u, 1280u, 720u, 1u, 0u,
                                     VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0x40u,
                                     0xf0000u);
    unsigned fit_w = 0u, fit_h = 0u, apply = 99u, coupled = 99u;

    /* Target run18 metadata: a 1920x1080 colour clear with a 1280x720 zeta.
     * The draw guard still rejects this pair, but the clear is bound to the
     * zeta's own dimensions and must remain valid. */
    CHECK(!vp_gxm_depth_desc_fits(&depth, 1920u, 1080u));
    CHECK(vp_gxm_depth_clear_target_plan(&depth, 1920u, 1080u, 1u, 0u,
                                         &fit_w, &fit_h, &apply, &coupled) ==
          VP_GXM_DEPTH_OK);
    CHECK(fit_w == 1280u && fit_h == 720u);
    CHECK(apply == 1u && coupled == 0u);

    /* The reverse size relation is also not a clear constraint. */
    fit_w = fit_h = 0u;
    CHECK(vp_gxm_depth_clear_target_plan(&depth, 640u, 360u, 1u, 0u,
                                         &fit_w, &fit_h, &apply, &coupled) ==
          VP_GXM_DEPTH_OK);
    CHECK(fit_w == 1280u && fit_h == 720u);
    CHECK(apply == 1u && coupled == 0u);

    /* A larger colour with an S8D24 both-clear stays exact, and a depth-only
     * clear with live guest stencil still refuses regardless of colour size. */
    {
        VPGxmDepthDesc s8 = make_desc(1u, 0x7200u, 1280u, 720u, 1u, 0u,
                                      VP_GXM_DEPTH_FORMAT_S8Z24_UNORM, 0u, 0u);
        fit_w = fit_h = 0u;
        apply = coupled = 99u;
        CHECK(vp_gxm_depth_clear_target_plan(&s8, 1920u, 1080u, 3u, 1u,
                                             &fit_w, &fit_h, &apply,
                                             &coupled) == VP_GXM_DEPTH_OK);
        CHECK(fit_w == 1280u && fit_h == 720u);
        CHECK(apply == 3u && coupled == 0u);
        CHECK(vp_gxm_depth_clear_target_plan(&s8, 1920u, 1080u, 1u, 1u,
                                             &fit_w, &fit_h, &apply,
                                             &coupled) ==
              VP_GXM_DEPTH_CLEAR_ERR_PRESERVE);
        CHECK(vp_gxm_depth_clear_target_plan(&s8, 1920u, 1080u, 2u, 0u,
                                             &fit_w, &fit_h, &apply,
                                             &coupled) ==
              VP_GXM_DEPTH_CLEAR_ERR_STANDALONE);
    }

    {
        VPGxmDepthDesc invalid;
        memset(&invalid, 0, sizeof(invalid));
        CHECK(vp_gxm_depth_clear_target_plan(&invalid, 1920u, 1080u, 1u, 0u,
                                             &fit_w, &fit_h, &apply,
                                             &coupled) ==
              VP_GXM_DEPTH_ERR_ARG);
        CHECK(vp_gxm_depth_clear_target_plan(NULL, 1920u, 1080u, 1u, 0u,
                                             &fit_w, &fit_h, &apply,
                                             &coupled) ==
              VP_GXM_DEPTH_ERR_ARG);
        CHECK(vp_gxm_depth_clear_target_plan(&depth, 1920u, 1080u, 1u, 0u,
                                             NULL, &fit_h, &apply,
                                             &coupled) ==
              VP_GXM_DEPTH_ERR_ARG);
    }
}

/* Target run19 shape: the real masked shadow producers bind the verified
 * 1024x3072 DF32 zeta while the stale Maxwell RT0 colour binding is
 * 1920x1080. The atlas must fit the checked byte planner; a larger identity
 * must be refused by the unchanged 32 MiB pool cap without mutating the pool. */
static void test_atlas_bytes_and_cap(void) {
    VPGxmDepthDesc atlas = make_desc(1u, 0x57d350000u, 1024u, 3072u, 1u, 0u,
                                     VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0x40u,
                                     0x300000u);
    CHECK(atlas.format_class == VP_GXM_DEPTH_CLASS_DF32);
    CHECK(atlas.stride_samples == 1024u);
    CHECK(atlas.storage_height == 3072u);
    CHECK(atlas.depth_bytes == 1024u * 3072u * 4u);
    CHECK(atlas.depth_bytes == 12582912u);
    CHECK(atlas.total_alloc_bytes == 12582912u);
    CHECK(vp_gxm_depth_desc_fits(&atlas, 1024u, 3072u));
    CHECK(!vp_gxm_depth_desc_fits(&atlas, 1920u, 1080u));
    CHECK(vp_gxm_depth_desc_fits(&atlas, 1024u, 1080u));
    CHECK(!vp_gxm_depth_desc_fits(&atlas, 2048u, 3072u));
    CHECK(VP_GXM_RENDER_TARGET_MAX_DIM == 4096u);
    CHECK(VP_GXM_DEPTH_MAX_DIM == 4096u);
    CHECK(VP_GXM_DEPTH_POOL_SLOTS == 8u);
    CHECK(VP_GXM_DEPTH_BYTE_CAP == 32u * 1024u * 1024u);

    {
        VPGxmDepthDesc four_k = make_desc(1u, 0x8000u, 4096u, 4096u, 1u, 0u,
                                          VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
        VPGxmDepthPool pool;
        VPGxmDepthPoolPlan plan;
        VPGxmDepthPool before;
        CHECK(four_k.depth_bytes == 4096u * 4096u * 4u);
        CHECK(four_k.total_alloc_bytes > VP_GXM_DEPTH_BYTE_CAP);
        CHECK(vp_gxm_depth_pool_init(&pool, 8u, VP_GXM_DEPTH_BYTE_CAP) ==
              VP_GXM_DEPTH_OK);
        before = pool;
        CHECK(vp_gxm_depth_pool_acquire(&pool, &four_k, &plan) ==
              VP_GXM_DEPTH_ERR_BUDGET);
        CHECK(plan.slot == -1);
        CHECK(memcmp(&pool, &before, sizeof(pool)) == 0);
        CHECK(pool.slot_limit == 8u);
        CHECK(pool.byte_cap == VP_GXM_DEPTH_BYTE_CAP);
    }

    {
        /* Two real 1280x720 S8D24 identities plus the atlas fit the shared
         * pool; no slot-limit or byte-cap change was needed. */
        VPGxmDepthDesc s8a = make_desc(1u, 0x56e940000u, 1280u, 720u, 1u, 0u,
                                       VP_GXM_DEPTH_FORMAT_Z24_UNORM_S8_UINT,
                                       0x40u, 0xf0000u);
        VPGxmDepthDesc s8b = s8a;
        VPGxmDepthPool pool;
        VPGxmDepthPoolPlan plan;
        s8b.va = 0x56fd10000u;
        CHECK(vp_gxm_depth_pool_init(&pool, 8u, VP_GXM_DEPTH_BYTE_CAP) ==
              VP_GXM_DEPTH_OK);
        CHECK(vp_gxm_depth_pool_acquire(&pool, &s8a, &plan) ==
              VP_GXM_DEPTH_OK);
        CHECK(vp_gxm_depth_pool_acquire(&pool, &s8b, &plan) ==
              VP_GXM_DEPTH_OK);
        CHECK(vp_gxm_depth_pool_acquire(&pool, &atlas, &plan) ==
              VP_GXM_DEPTH_OK);
        CHECK(plan.action == VP_GXM_DEPTH_ACTION_CREATE);
        CHECK(pool.entries[plan.slot].desc.va == atlas.va);
        CHECK(pool.bytes == s8a.total_alloc_bytes + s8b.total_alloc_bytes +
                            atlas.total_alloc_bytes);
        CHECK(pool.bytes < VP_GXM_DEPTH_BYTE_CAP);
        CHECK(pool.slot_limit == 8u);
        CHECK(pool.byte_cap == VP_GXM_DEPTH_BYTE_CAP);
    }
}

/* Draw-side policy that the runtime now executes: a fully masked colour draw
 * with a real depth write is a depth-only render using the zeta extent, never
 * the stale colour binding. Disabled/NEVER tests and unimplemented stencil
 * operations must not become successful draws. */
static void test_depth_draw_plan(void) {
    VPGxmDepthDrawPlanInput in;
    VPGxmDepthDrawPlan plan;

    memset(&in, 0, sizeof(in));
    in.color_mask = 0u;
    in.depth_test = 1u;
    in.depth_write_raw = 1u;
    in.depth_func = 2u; /* LESS */
    in.depth_status = VP_GXM_DEPTH_OK;
    in.depth_format_class = VP_GXM_DEPTH_CLASS_DF32;
    in.color_width = 1920u;   /* stale RT0 binding */
    in.color_height = 1080u;
    in.depth_width = 1024u;   /* real atlas extent */
    in.depth_height = 3072u;
    CHECK(vp_gxm_depth_draw_plan(&in, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.depth_only == 1u);
    CHECK(plan.noop == 0u && plan.refuse == 0u);
    CHECK(plan.color_surface == 0u);
    CHECK(plan.effective_write == 1u);
    CHECK(plan.render_width == 1024u && plan.render_height == 3072u);
    CHECK(strcmp(plan.reason, "depth-only") == 0);

    /* A different stale colour size does not change the selected extent. */
    in.color_width = 640u;
    in.color_height = 360u;
    CHECK(vp_gxm_depth_draw_plan(&in, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.depth_only == 1u);
    CHECK(plan.render_width == 1024u && plan.render_height == 3072u);

    /* depth test off disables the write: no GPU work is needed. */
    in.depth_test = 0u;
    CHECK(vp_gxm_depth_draw_plan(&in, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.noop == 1u && plan.depth_only == 0u && plan.refuse == 0u);
    CHECK(plan.effective_write == 0u);
    CHECK(strcmp(plan.reason, "side-effect-free") == 0);

    /* NEVER cannot pass a fragment, so it cannot write depth. Both Maxwell
     * encodings (1 and 0x200) are the same function. */
    in.depth_test = 1u;
    in.depth_func = 1u;
    CHECK(vp_gxm_depth_draw_plan(&in, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.noop == 1u && plan.depth_only == 0u);
    in.depth_func = 0x200u;
    CHECK(vp_gxm_depth_draw_plan(&in, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.noop == 1u && plan.depth_only == 0u);

    /* Disabled guest zeta has no target side effect. */
    in.depth_func = 2u;
    in.depth_status = VP_GXM_DEPTH_DISABLED;
    CHECK(vp_gxm_depth_draw_plan(&in, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.noop == 1u && plan.depth_only == 0u);

    /* Malformed metadata is a refusal, never an ignored state. */
    in.depth_status = VP_GXM_DEPTH_ERR_TILE_MODE;
    CHECK(vp_gxm_depth_draw_plan(&in, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.refuse == 1u && plan.noop == 0u && plan.depth_only == 0u);
    CHECK(strcmp(plan.reason, "invalid-depth-metadata") == 0);

    /* Stencil test/write is not implemented. A masked stencil state on S8D24
     * is an explicit refusal both with and without a depth write. */
    in.depth_status = VP_GXM_DEPTH_OK;
    in.depth_format_class = VP_GXM_DEPTH_CLASS_S8D24;
    in.stencil_enable = 1u;
    in.depth_write_raw = 0u;
    CHECK(vp_gxm_depth_draw_plan(&in, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.refuse == 1u && plan.depth_only == 0u);
    CHECK(strcmp(plan.reason, "masked-stencil-state") == 0);
    in.depth_write_raw = 1u;
    CHECK(vp_gxm_depth_draw_plan(&in, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.refuse == 1u);
    CHECK(strcmp(plan.reason, "masked-depth-stencil") == 0);
    in.stencil_enable = 0u;

    /* Colour path: the existing colour extent and fit guard remain in charge;
     * a nonzero mask is never reclassified as depth-only. 0x100 is the real
     * green channel bit observed on depth-atlas consumers (not an all-off
     * mask). */
    in.color_mask = 0x1111u;
    in.depth_format_class = VP_GXM_DEPTH_CLASS_DF32;
    CHECK(vp_gxm_depth_draw_plan(&in, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.color_surface == 1u && plan.depth_only == 0u);
    CHECK(plan.render_width == 640u && plan.render_height == 360u);
    in.color_mask = 0x100u;
    CHECK(vp_gxm_depth_draw_plan(&in, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.color_surface == 1u && plan.depth_only == 0u);

    CHECK(vp_gxm_depth_draw_plan(NULL, &plan) == VP_GXM_DEPTH_ERR_ARG);
    CHECK(vp_gxm_depth_draw_plan(&in, NULL) == VP_GXM_DEPTH_ERR_ARG);
}

static void test_commit_isolation(void) {
    unsigned depth_only = vp_gxm_draw_commit_mask(0u, 0u, 1u);
    unsigned colour = vp_gxm_draw_commit_mask(1u, 0x1111u, 1u);
    unsigned colour_no_depth = vp_gxm_draw_commit_mask(1u, 0x1111u, 0u);
    unsigned masked_colour_surface = vp_gxm_draw_commit_mask(1u, 0u, 1u);

    /* A depth-only draw may update depth content only: no colour producer,
     * display rectangle or last-colour-target publication. */
    CHECK(depth_only == (VP_GXM_DRAW_COMMIT_DEPTH_VALID |
                         VP_GXM_DRAW_COMMIT_DEPTH_DRAW_COUNT));
    CHECK((depth_only & VP_GXM_DRAW_COMMIT_COLOR_DRAWN) == 0u);
    CHECK((depth_only & VP_GXM_DRAW_COMMIT_COLOR_DRAW_COUNT) == 0u);
    CHECK((depth_only & VP_GXM_DRAW_COMMIT_DISPLAY_RECT) == 0u);
    CHECK((depth_only & VP_GXM_DRAW_COMMIT_LAST_TARGET) == 0u);

    CHECK(colour == (VP_GXM_DRAW_COMMIT_COLOR_DRAWN |
                     VP_GXM_DRAW_COMMIT_COLOR_DRAW_COUNT |
                     VP_GXM_DRAW_COMMIT_DISPLAY_RECT |
                     VP_GXM_DRAW_COMMIT_LAST_TARGET |
                     VP_GXM_DRAW_COMMIT_DEPTH_VALID |
                     VP_GXM_DRAW_COMMIT_DEPTH_DRAW_COUNT));
    CHECK(colour_no_depth == (VP_GXM_DRAW_COMMIT_COLOR_DRAWN |
                              VP_GXM_DRAW_COMMIT_COLOR_DRAW_COUNT |
                              VP_GXM_DRAW_COMMIT_DISPLAY_RECT |
                              VP_GXM_DRAW_COMMIT_LAST_TARGET));
    CHECK(masked_colour_surface == (VP_GXM_DRAW_COMMIT_DEPTH_VALID |
                                    VP_GXM_DRAW_COMMIT_DEPTH_DRAW_COUNT));
    CHECK(vp_gxm_draw_commit_mask(0u, 0u, 0u) == 0u);
}

static void test_display_target_requires_colour_mask(void) {
    /* Regression for the submission ordering bug: display_target was computed
     * before req.color_mask was assigned from the guest draw, so every draw
     * looked depth-only and the binder presented while the runtime never did.
     * The decision must never consult request state that is still zeroed. */
    CHECK(vp_gxm_display_target(1u, 0x1111u, 0x1234u, 1u) == 1u);
    CHECK(vp_gxm_display_target(1u, 0x0u, 0x1234u, 1u) == 0u);
    CHECK(vp_gxm_display_target(1u, 0x100u, 0x1234u, 1u) == 1u);
    CHECK(vp_gxm_display_target(1u, 0x1111u, 0x0u, 1u) == 0u);
    CHECK(vp_gxm_display_target(1u, 0x1111u, 0x1234u, 0u) == 0u);
    CHECK(vp_gxm_display_target(0u, 0x1111u, 0x1234u, 1u) == 0u);
}

static void test_colour_fit_guard_unchanged(void) {
    VPGxmDepthDesc depth = make_desc(1u, 0x7300u, 640u, 360u, 1u, 0u,
                                     VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    VPGxmDepthDesc atlas = make_desc(1u, 0x7400u, 1024u, 3072u, 1u, 0u,
                                     VP_GXM_DEPTH_FORMAT_Z32_FLOAT, 0u, 0u);
    VPGxmDepthDrawPlanInput in;
    VPGxmDepthDrawPlan plan;

    /* The colour path still rejects a depth smaller than its colour target;
     * the raised render-target ceiling does not relax this draw guard. */
    CHECK(!vp_gxm_depth_desc_fits(&depth, 1920u, 1080u));
    /* A 1024x3072 zeta cannot cover a 1920-wide colour target. */
    CHECK(!vp_gxm_depth_desc_fits(&atlas, 1920u, 1080u));
    CHECK(vp_gxm_depth_desc_fits(&atlas, 1024u, 1080u));
    memset(&in, 0, sizeof(in));
    in.color_mask = 0x1111u;
    in.depth_test = 1u;
    in.depth_write_raw = 1u;
    in.depth_func = 2u;
    in.depth_status = VP_GXM_DEPTH_OK;
    in.depth_format_class = VP_GXM_DEPTH_CLASS_DF32;
    in.color_width = 1920u;
    in.color_height = 1080u;
    in.depth_width = 640u;
    in.depth_height = 360u;
    CHECK(vp_gxm_depth_draw_plan(&in, &plan) == VP_GXM_DEPTH_OK);
    CHECK(plan.color_surface == 1u);
    CHECK(plan.render_width == 1920u && plan.render_height == 1080u);
    CHECK(!vp_gxm_depth_desc_fits(&depth, plan.render_width, plan.render_height));
}

static void test_contract_names(void) {
    CHECK(strcmp(vp_gxm_depth_status_name(VP_GXM_DEPTH_ERR_FORMAT),
                 "format") == 0);
    CHECK(strcmp(vp_gxm_depth_status_name(VP_GXM_DEPTH_ERR_VOLUME),
                 "volume-depth") == 0);
    CHECK(strcmp(vp_gxm_depth_status_name(VP_GXM_DEPTH_CLEAR_ERR_STANDALONE),
                 "stencil-standalone") == 0);
    CHECK(strcmp(vp_gxm_depth_status_name(VP_GXM_DEPTH_CLEAR_ERR_PRESERVE),
                 "stencil-preserve") == 0);
    CHECK(strcmp(vp_gxm_depth_action_name(VP_GXM_DEPTH_ACTION_RECREATE),
                 "recreate") == 0);
}

int main(void) {
    test_descriptor_plans();
    test_descriptor_rejections();
    test_byte_planner_and_fits();
    test_pool_reuse_and_isolation();
    test_pool_hint_only_reuse();
    test_pool_shape_and_format_changes();
    test_pool_full_refuses_unchanged();
    test_pool_eight_slots_exhausted();
    test_pool_byte_cap_refusal_and_recreate();
    test_pool_release();
    test_clear_policy();
    test_clear_target_independence();
    test_atlas_bytes_and_cap();
    test_depth_draw_plan();
    test_commit_isolation();
    test_display_target_requires_colour_mask();
    test_colour_fit_guard_unchanged();
    test_contract_names();
    if (failures != 0) {
        fprintf(stderr, "gxm_depth: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("gxm_depth: all checks passed\n");
    return EXIT_SUCCESS;
}

/* B111 host checks for the clear-fusion enable policy and clear ordering.
 *
 * The same pure helpers used by gxm_runtime.c are exercised here. Depth/stencil
 * representability comes from the production gxm_depth planner, so the
 * refusal cases assert that an unsupported stencil or live-stencil preserve
 * rule produces no colour side effect. A tiny state model applies only the
 * effects requested by the returned plan; no GXM call is made.
 */
#include "clear_fusion.h"
#include "gxm_depth.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

typedef struct {
    int pending;
    uint32_t pending_color;
    uint32_t surface_color;
    uint32_t surface_mask;
    unsigned deferred;
    unsigned combined_deferred;
    unsigned materialized;
    unsigned surface_clears;
    unsigned depth_applies;
    unsigned depth_apply_mask;
} ClearState;

static void state_init(ClearState *st) {
    memset(st, 0, sizeof(*st));
}

/* Apply one plan in the same order as frame_clear_internal: materialize an
 * older pending value first, then either defer the full colour or issue the
 * surface clear, then run the depth/stencil bookkeeping once. */
static VPClearFusionPlan apply_clear(ClearState *st, int representable,
                                     int fusion_enabled, int has_color_mem,
                                     uint32_t mask, uint32_t color,
                                     int dump_due, unsigned depth_apply_mask) {
    VPClearFusionPlan plan = vp_clear_fusion_plan(
        representable, fusion_enabled, has_color_mem, st->pending != 0,
        mask, dump_due, depth_apply_mask);
    if (plan.refused) {
        CHECK(!plan.materialize_pending && !plan.defer_colour &&
              !plan.surface_clear && !plan.apply_depth);
        return plan;
    }
    if (plan.materialize_pending) {
        CHECK(st->pending != 0);
        st->pending = 0;
        st->materialized++;
    }
    if (plan.defer_colour) {
        st->pending = 1;
        st->pending_color = color;
        st->deferred++;
        if (plan.apply_depth) st->combined_deferred++;
    } else if (plan.surface_clear) {
        st->surface_clears++;
        st->surface_mask = mask;
        if (vp_clear_fusion_full_colour(mask)) {
            st->surface_color = color;
            st->pending = 0;
        }
    }
    if (plan.apply_depth) {
        st->depth_applies++;
        st->depth_apply_mask = depth_apply_mask;
    }
    return plan;
}

static VPGxmDepthDesc s8d24_desc(void) {
    VPGxmDepthDesc desc;
    int rc = vp_gxm_depth_desc_build(&desc, 1u, 0x2000u, 1280u, 720u, 1u, 0u,
                                     VP_GXM_DEPTH_FORMAT_S8Z24_UNORM, 0u, 0u);
    CHECK(rc == VP_GXM_DEPTH_OK);
    return desc;
}

static void test_resolve(void) {
    VPClearFusionSetting setting;

    setting = vp_clear_fusion_resolve(0, 0, 1, 1, 0, -1);
    CHECK(setting.enabled == 1);
    CHECK(setting.source == VP_CLEAR_FUSION_SOURCE_DEFAULT);

    /* The disable marker wins over both the enable marker and the default. */
    setting = vp_clear_fusion_resolve(1, 1, 1, 1, 0, -1);
    CHECK(setting.enabled == 0);
    CHECK(setting.source == VP_CLEAR_FUSION_SOURCE_DISABLE);

    /* An explicit enable still serves old tools, diagnostics and self tests. */
    setting = vp_clear_fusion_resolve(0, 1, 0, 0, 1, 1);
    CHECK(setting.enabled == 1);
    CHECK(setting.source == VP_CLEAR_FUSION_SOURCE_ENABLE);

    /* Default requires ordinary bounded-batch gameplay. */
    setting = vp_clear_fusion_resolve(0, 0, 1, 1, 0, 0);
    CHECK(setting.enabled == 0);
    CHECK(setting.source == VP_CLEAR_FUSION_SOURCE_LEGACY);
    setting = vp_clear_fusion_resolve(0, 0, 1, 1, 1, -1);
    CHECK(setting.enabled == 0);
    CHECK(setting.source == VP_CLEAR_FUSION_SOURCE_LEGACY);
    setting = vp_clear_fusion_resolve(0, 0, 1, 0, 0, -1);
    CHECK(setting.enabled == 0);
    CHECK(setting.source == VP_CLEAR_FUSION_SOURCE_LEGACY);
    setting = vp_clear_fusion_resolve(0, 0, 0, 1, 0, -1);
    CHECK(setting.enabled == 0);
    CHECK(setting.source == VP_CLEAR_FUSION_SOURCE_LEGACY);
}

static void test_mask_helpers(void) {
    CHECK(vp_clear_fusion_full_colour(0x3cu));
    CHECK(vp_clear_fusion_full_colour(0x3du));
    CHECK(vp_clear_fusion_full_colour(0x3eu)); /* full colour + stencil */
    CHECK(vp_clear_fusion_full_colour(0x3fu));
    CHECK(!vp_clear_fusion_full_colour(0x38u));
    CHECK(!vp_clear_fusion_full_colour(0x00u));
    CHECK(vp_clear_fusion_writes_colour(0x3cu));
    CHECK(vp_clear_fusion_writes_colour(0x04u));
    CHECK(!vp_clear_fusion_writes_colour(0x03u));
    CHECK(!vp_clear_fusion_writes_colour(0x00u));
}

static void test_full_then_full(void) {
    ClearState st;
    VPClearFusionPlan plan;

    state_init(&st);
    plan = apply_clear(&st, 1, 1, 1, 0x3cu, 0xff112233u, 0, 0u);
    CHECK(plan.defer_colour && !plan.materialize_pending &&
          !plan.surface_clear && !plan.apply_depth);
    CHECK(st.pending == 1 && st.pending_color == 0xff112233u);
    CHECK(st.deferred == 1u && st.materialized == 0u && st.surface_clears == 0u);

    /* Last full clear wins; the older value is never materialized. */
    plan = apply_clear(&st, 1, 1, 1, 0x3cu, 0xff445566u, 0, 0u);
    CHECK(plan.defer_colour && !plan.materialize_pending);
    CHECK(st.pending == 1 && st.pending_color == 0xff445566u);
    CHECK(st.deferred == 2u && st.materialized == 0u && st.surface_clears == 0u);
}

static void test_full_then_partial(void) {
    ClearState st;
    VPClearFusionPlan plan;

    state_init(&st);
    apply_clear(&st, 1, 1, 1, 0x3cu, 0xff112233u, 0, 0u);

    /* A partial colour write cannot replace a full pending value. The old
     * value must be materialized before the partial clear runs. */
    plan = apply_clear(&st, 1, 1, 1, 0x04u, 0xff445566u, 0, 0u);
    CHECK(!plan.defer_colour && plan.materialize_pending && plan.surface_clear);
    CHECK(st.pending == 0);
    CHECK(st.materialized == 1u && st.surface_clears == 1u);
    CHECK(st.surface_mask == 0x04u);

    /* A full clear that cannot coalesce because a dump is due follows the same
     * materialize-before ordering. */
    state_init(&st);
    apply_clear(&st, 1, 1, 1, 0x3cu, 0xff112233u, 0, 0u);
    plan = apply_clear(&st, 1, 1, 1, 0x3cu, 0xff445566u, 1, 0u);
    CHECK(!plan.defer_colour && plan.materialize_pending && plan.surface_clear);
    CHECK(st.materialized == 1u && st.surface_clears == 1u);
    CHECK(st.surface_color == 0xff445566u && st.pending == 0);
}

static void test_combined_depth(void) {
    ClearState st;
    VPGxmDepthDesc desc = s8d24_desc();
    unsigned fit_w = 0u, fit_h = 0u, apply_mask = 0u, coupled = 0u;
    VPClearFusionPlan plan;
    int rc;

    /* Depth-only bit on S8D24 with no live stencil is representable and applies
     * the coupled stencil reload exactly once. */
    rc = vp_gxm_depth_clear_target_plan(&desc, 1280u, 720u, 1u, 0u,
                                        &fit_w, &fit_h, &apply_mask, &coupled);
    CHECK(rc == VP_GXM_DEPTH_OK);
    CHECK(apply_mask == 1u && coupled == 1u);

    state_init(&st);
    plan = apply_clear(&st, 1, 1, 1, 0x3du, 0xff778899u, 0, apply_mask);
    CHECK(plan.defer_colour && plan.apply_depth);
    CHECK(!plan.materialize_pending && !plan.surface_clear);
    CHECK(st.pending == 1 && st.pending_color == 0xff778899u);
    CHECK(st.deferred == 1u && st.combined_deferred == 1u);
    CHECK(st.depth_applies == 1u && st.depth_apply_mask == 1u);
    CHECK(st.materialized == 0u && st.surface_clears == 0u);

    /* A previous pending full clear is still replaced (not materialized) and
     * the combined request applies depth once on top of the new colour. */
    state_init(&st);
    apply_clear(&st, 1, 1, 1, 0x3cu, 0xff010203u, 0, 0u);
    plan = apply_clear(&st, 1, 1, 1, 0x3du, 0xff0a0b0cu, 0, apply_mask);
    CHECK(plan.defer_colour && plan.apply_depth && !plan.materialize_pending);
    CHECK(st.pending_color == 0xff0a0b0cu);
    CHECK(st.deferred == 2u && st.combined_deferred == 1u);
    CHECK(st.depth_applies == 1u && st.materialized == 0u);
}

static void test_depth_only_keeps_pending(void) {
    ClearState st;
    VPGxmDepthDesc desc = s8d24_desc();
    unsigned fit_w = 0u, fit_h = 0u, apply_mask = 0u, coupled = 0u;
    VPClearFusionPlan plan;
    int rc;

    rc = vp_gxm_depth_clear_target_plan(&desc, 1280u, 720u, 1u, 0u,
                                        &fit_w, &fit_h, &apply_mask, &coupled);
    CHECK(rc == VP_GXM_DEPTH_OK);

    state_init(&st);
    apply_clear(&st, 1, 1, 1, 0x3cu, 0xffaabbccu, 0, 0u);

    /* A depth-only clear writes no colour; the pending full colour value must
     * survive and the depth bookkeeping still runs once. */
    plan = apply_clear(&st, 1, 1, 1, 0x01u, 0u, 0, apply_mask);
    CHECK(!plan.defer_colour && !plan.materialize_pending && plan.surface_clear);
    CHECK(plan.apply_depth);
    CHECK(st.pending == 1 && st.pending_color == 0xffaabbccu);
    CHECK(st.materialized == 0u && st.deferred == 1u);
    CHECK(st.surface_clears == 1u && st.surface_mask == 0x01u);
    CHECK(st.depth_applies == 1u);
}

static void test_refusals_have_no_effect(void) {
    ClearState st;
    VPGxmDepthDesc desc = s8d24_desc();
    unsigned fit_w = 0u, fit_h = 0u, apply_mask = 0u, coupled = 0u;
    VPClearFusionPlan plan;
    int rc;

    /* Stencil without depth is standalone; the whole request, including any
     * full colour bits, is refused before the colour plan. */
    rc = vp_gxm_depth_clear_target_plan(&desc, 1280u, 720u, 2u, 0u,
                                        &fit_w, &fit_h, &apply_mask, &coupled);
    CHECK(rc == VP_GXM_DEPTH_CLEAR_ERR_STANDALONE);
    state_init(&st);
    apply_clear(&st, 1, 1, 1, 0x3cu, 0xff102030u, 0, 0u);
    plan = apply_clear(&st, 0, 1, 1, 0x3eu, 0xff405060u, 0, apply_mask);
    CHECK(plan.refused);
    CHECK(st.pending == 1 && st.pending_color == 0xff102030u);
    CHECK(st.deferred == 1u && st.materialized == 0u && st.surface_clears == 0u);
    CHECK(st.depth_applies == 0u);

    /* A depth-only clear would reload live stencil from its background, so it
     * is refused; the pending colour and all counters stay untouched. */
    rc = vp_gxm_depth_clear_target_plan(&desc, 1280u, 720u, 1u, 1u,
                                        &fit_w, &fit_h, &apply_mask, &coupled);
    CHECK(rc == VP_GXM_DEPTH_CLEAR_ERR_PRESERVE);
    state_init(&st);
    apply_clear(&st, 1, 1, 1, 0x3cu, 0xff102030u, 0, 0u);
    plan = apply_clear(&st, 0, 1, 1, 0x3du, 0xff405060u, 0, apply_mask);
    CHECK(plan.refused);
    CHECK(st.pending == 1 && st.pending_color == 0xff102030u);
    CHECK(st.deferred == 1u && st.materialized == 0u && st.surface_clears == 0u);
    CHECK(st.depth_applies == 0u);
}

static void test_non_deferrable_mask(void) {
    ClearState st;
    VPClearFusionPlan plan;

    /* No CPU-visible colour backing: never defer. */
    state_init(&st);
    plan = apply_clear(&st, 1, 1, 0, 0x3cu, 0xff000000u, 0, 0u);
    CHECK(!plan.defer_colour && plan.surface_clear);
    CHECK(st.pending == 0 && st.surface_clears == 1u);

    /* Fusion disabled: legacy standalone path. */
    state_init(&st);
    plan = apply_clear(&st, 1, 0, 1, 0x3cu, 0xff000000u, 0, 0u);
    CHECK(!plan.defer_colour && plan.surface_clear);
    CHECK(st.pending == 0 && st.surface_clears == 1u);
}

int main(void) {
    test_resolve();
    test_mask_helpers();
    test_full_then_full();
    test_full_then_partial();
    test_combined_depth();
    test_depth_only_keeps_pending();
    test_refusals_have_no_effect();
    test_non_deferrable_mask();

    if (failures != 0) {
        fprintf(stderr, "FAIL clear fusion: %d checks\n", failures);
        return 1;
    }
    puts("PASS clear fusion: resolve/default/disable/order/combined-depth/"
         "refusal");
    return 0;
}

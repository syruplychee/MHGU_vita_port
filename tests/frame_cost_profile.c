#include "frame_cost_profile.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_disabled_begin_takes_nothing(void) {
    VPFrameCostProfile profile;
    vp_frame_cost_profile_reset(&profile);
    assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_ENTRY, 0u) == 0);
    assert(profile.depth == 0u);
    assert(profile.categories[VP_FRAME_COST_ENTRY].calls == 0u);
    assert(vp_frame_cost_error_count(&profile) == 0u);
}

static void test_nested_self_and_roots(void) {
    VPFrameCostProfile profile;
    vp_frame_cost_profile_reset(&profile);
    assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_ENTRY, 1000u) == 1);
    assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_DISPATCH, 1010u) == 1);
    assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_GXM_DRAW, 1020u) == 1);
    assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_TEX_DECODE, 1030u) == 1);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_TEX_DECODE, 1050u) == 0);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_GXM_DRAW, 1060u) == 0);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_DISPATCH, 1080u) == 0);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_ENTRY, 1100u) == 0);
    assert(profile.depth == 0u);
    assert(vp_frame_cost_error_count(&profile) == 0u);
    assert(profile.categories[VP_FRAME_COST_TEX_DECODE].inclusive_us == 20u);
    assert(profile.categories[VP_FRAME_COST_TEX_DECODE].self_us == 20u);
    assert(profile.categories[VP_FRAME_COST_GXM_DRAW].inclusive_us == 40u);
    assert(profile.categories[VP_FRAME_COST_GXM_DRAW].self_us == 20u);
    assert(profile.categories[VP_FRAME_COST_DISPATCH].inclusive_us == 70u);
    assert(profile.categories[VP_FRAME_COST_DISPATCH].self_us == 30u);
    assert(profile.categories[VP_FRAME_COST_ENTRY].inclusive_us == 100u);
    assert(profile.categories[VP_FRAME_COST_ENTRY].self_us == 30u);
    assert(profile.categories[VP_FRAME_COST_ENTRY].max_us == 100u);
    assert(profile.categories[VP_FRAME_COST_ENTRY].root_calls == 1u);
    assert(profile.categories[VP_FRAME_COST_ENTRY].root_inclusive_us == 100u);
    /* Only the bottom span is a root; children are not added to frame wall. */
    assert(profile.categories[VP_FRAME_COST_DISPATCH].root_calls == 0u);
    assert(profile.categories[VP_FRAME_COST_GXM_DRAW].root_calls == 0u);
    assert(profile.categories[VP_FRAME_COST_TEX_DECODE].root_calls == 0u);
}

static void test_recursive_category(void) {
    VPFrameCostProfile profile;
    vp_frame_cost_profile_reset(&profile);
    assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_GXM_DRAW,
                                    100u) == 1);
    assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_GXM_DRAW,
                                    110u) == 1);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_GXM_DRAW, 120u) == 0);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_GXM_DRAW, 130u) == 0);
    assert(profile.categories[VP_FRAME_COST_GXM_DRAW].calls == 2u);
    assert(profile.categories[VP_FRAME_COST_GXM_DRAW].inclusive_us == 40u);
    assert(profile.categories[VP_FRAME_COST_GXM_DRAW].self_us == 30u);
    assert(profile.categories[VP_FRAME_COST_GXM_DRAW].root_calls == 1u);
    assert(profile.categories[VP_FRAME_COST_GXM_DRAW].root_inclusive_us == 30u);
    assert(vp_frame_cost_error_count(&profile) == 0u);
}

/* Mirrors the production wrapper pattern: the implementation may return early,
 * but the wrapper always closes exactly one span. */
static int submit_span_once(VPFrameCostProfile *profile, int fail) {
    uint64_t started = 500u;
    assert(vp_frame_cost_span_begin(profile, VP_FRAME_COST_DRAW_SUBMIT,
                                    started) == 1);
    if (fail) {
        assert(vp_frame_cost_span_end(profile, VP_FRAME_COST_DRAW_SUBMIT,
                                      520u) == 0);
        return -1;
    }
    assert(vp_frame_cost_span_end(profile, VP_FRAME_COST_DRAW_SUBMIT, 600u) == 0);
    return 0;
}

static void test_early_return_wrapper(void) {
    VPFrameCostProfile profile;
    vp_frame_cost_profile_reset(&profile);
    assert(submit_span_once(&profile, 1) == -1);
    assert(submit_span_once(&profile, 0) == 0);
    assert(profile.depth == 0u);
    assert(profile.categories[VP_FRAME_COST_DRAW_SUBMIT].calls == 2u);
    assert(profile.categories[VP_FRAME_COST_DRAW_SUBMIT].inclusive_us == 120u);
    assert(profile.categories[VP_FRAME_COST_DRAW_SUBMIT].root_calls == 2u);
    assert(vp_frame_cost_error_count(&profile) == 0u);
}

static void test_overflow_and_balance(void) {
    VPFrameCostProfile profile;
    unsigned i;
    vp_frame_cost_profile_reset(&profile);
    for (i = 0u; i < VP_FRAME_COST_STACK; ++i)
        assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_ENTRY,
                                        100u + i) == 1);
    assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_ENTRY, 200u) == 0);
    assert(profile.depth == VP_FRAME_COST_STACK);
    assert(profile.overflow == 1u);
    for (i = 0u; i < VP_FRAME_COST_STACK; ++i)
        assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_ENTRY,
                                      1000u + i) == 0);
    assert(profile.depth == 0u);
    assert(profile.mismatch == 0u);
    assert(profile.underflow == 0u);
    assert(profile.categories[VP_FRAME_COST_ENTRY].calls ==
           VP_FRAME_COST_STACK);
    assert(profile.categories[VP_FRAME_COST_ENTRY].root_calls == 1u);
    assert(vp_frame_cost_error_count(&profile) == 1u);
}

static void test_mismatch_underflow_and_recursion(void) {
    VPFrameCostProfile profile;
    vp_frame_cost_profile_reset(&profile);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_ENTRY, 10u) == -1);
    assert(profile.underflow == 1u);
    assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_ENTRY, 100u) == 1);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_DISPATCH, 110u) == -1);
    assert(profile.mismatch == 1u);
    assert(profile.depth == 1u);
    assert(profile.categories[VP_FRAME_COST_ENTRY].calls == 0u);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_ENTRY, 120u) == 0);
    assert(profile.depth == 0u);
    assert(profile.categories[VP_FRAME_COST_ENTRY].calls == 1u);
    assert(profile.categories[VP_FRAME_COST_ENTRY].inclusive_us == 20u);
}

static void test_clock_regression_and_child_underflow(void) {
    VPFrameCostProfile profile;
    vp_frame_cost_profile_reset(&profile);
    assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_WAIT_NOTIFY,
                                    100u) == 1);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_WAIT_NOTIFY,
                                  50u) == 0);
    assert(profile.clock_regression == 1u);
    assert(profile.categories[VP_FRAME_COST_WAIT_NOTIFY].inclusive_us == 0u);
    assert(profile.depth == 0u);

    vp_frame_cost_profile_reset(&profile);
    assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_ENTRY, 100u) == 1);
    assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_DISPATCH,
                                    100u) == 1);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_DISPATCH,
                                  200u) == 0);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_ENTRY, 150u) == 0);
    assert(profile.underflow == 1u);
    assert(profile.categories[VP_FRAME_COST_ENTRY].self_us == 0u);
    assert(profile.categories[VP_FRAME_COST_ENTRY].inclusive_us == 50u);
    assert(profile.depth == 0u);
}

static void test_missing_end_time(void) {
    VPFrameCostProfile profile;
    vp_frame_cost_profile_reset(&profile);
    assert(vp_frame_cost_span_begin(&profile, VP_FRAME_COST_ENTRY, 10u) == 1);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_ENTRY, 0u) == -1);
    assert(profile.missing_time == 1u);
    assert(profile.depth == 1u);
    assert(vp_frame_cost_span_end(&profile, VP_FRAME_COST_ENTRY, 20u) == 0);
    assert(profile.depth == 0u);
}

static void test_parse_mode(void) {
    unsigned mode = 9u;
    assert(vp_frame_cost_parse_mode("0", 1u, &mode) == 0 && mode == 0u);
    assert(vp_frame_cost_parse_mode("1", 1u, &mode) == 0 && mode == 1u);
    assert(vp_frame_cost_parse_mode("0 \r\n\t", 5u, &mode) == 0 && mode == 0u);
    assert(vp_frame_cost_parse_mode("1\n", 2u, &mode) == 0 && mode == 1u);
    assert(vp_frame_cost_parse_mode("1\v\f", 3u, &mode) == 0 && mode == 1u);
    assert(vp_frame_cost_parse_mode("", 0u, &mode) == -1);
    assert(vp_frame_cost_parse_mode("2", 1u, &mode) == -1);
    assert(vp_frame_cost_parse_mode("10", 2u, &mode) == -1);
    assert(vp_frame_cost_parse_mode(" 0", 2u, &mode) == -1);
    assert(vp_frame_cost_parse_mode("0x", 2u, &mode) == -1);
    assert(vp_frame_cost_parse_mode("0\n1", 3u, &mode) == -1);
}

int main(void) {
    test_disabled_begin_takes_nothing();
    test_nested_self_and_roots();
    test_recursive_category();
    test_early_return_wrapper();
    test_overflow_and_balance();
    test_mismatch_underflow_and_recursion();
    test_clock_regression_and_child_underflow();
    test_missing_end_time();
    test_parse_mode();
    puts("frame_cost_profile: all checks passed");
    return 0;
}

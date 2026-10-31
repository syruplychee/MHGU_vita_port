/* B108 host checks for the configurable render-resolution policy.
 *
 * Expected values are derived from the written contract, independently of the
 * runtime: scale = chosen/720 with a 540-line cap; both axes round up; only
 * 16:9 images at least 160x90 change. Scissor bounds are half-open on input,
 * floor/ceil scaled, clamped, then converted to GXM's inclusive upper edge.
 */
#include "render_resolution.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void check_extent(unsigned width, unsigned height, unsigned chosen,
                         unsigned expect_width, unsigned expect_height) {
    VPRenderExtent extent = vp_render_resolution_extent(width, height, chosen);
    CHECK(extent.width == expect_width);
    CHECK(extent.height == expect_height);
}

static void test_parser(void) {
    unsigned chosen = 0u;
    CHECK(vp_render_resolution_parse("540p", 4u, &chosen) == 0 &&
          chosen == 540u);
    CHECK(vp_render_resolution_parse("360p", 4u, &chosen) == 0 &&
          chosen == 360u);
    CHECK(vp_render_resolution_parse("540p\n", 5u, &chosen) == 0 &&
          chosen == 540u);
    CHECK(vp_render_resolution_parse("360p \t\r\n", 8u, &chosen) == 0 &&
          chosen == 360u);
    CHECK(vp_render_resolution_parse("540p  ", 6u, &chosen) == 0);
    CHECK(vp_render_resolution_parse("", 0u, &chosen) != 0);
    CHECK(vp_render_resolution_parse("\n", 1u, &chosen) != 0);
    CHECK(vp_render_resolution_parse("720p", 4u, &chosen) != 0);
    CHECK(vp_render_resolution_parse("540", 3u, &chosen) != 0);
    CHECK(vp_render_resolution_parse("540pX", 5u, &chosen) != 0);
    CHECK(vp_render_resolution_parse("540p 360p", 9u, &chosen) != 0);
    CHECK(vp_render_resolution_parse(" 540p", 5u, &chosen) != 0);
    CHECK(vp_render_resolution_parse("54\0p", 5u, &chosen) != 0);
    CHECK(vp_render_resolution_parse(NULL, 4u, &chosen) != 0);
    CHECK(vp_render_resolution_parse("540p", 4u, NULL) != 0);
}

static void test_extents(void) {
    /* Main screen: 540p is the default, 360p preserves the old storage. */
    check_extent(1280u, 720u, 540u, 960u, 540u);
    check_extent(1280u, 720u, 360u, 640u, 360u);
    /* 1080p is capped at 540 lines in both modes, never 1440x810. */
    check_extent(1920u, 1080u, 540u, 960u, 540u);
    check_extent(1920u, 1080u, 360u, 960u, 540u);
    /* Screen-relative post effects follow the same selection. */
    check_extent(640u, 360u, 540u, 480u, 270u);
    check_extent(640u, 360u, 360u, 320u, 180u);
    check_extent(160u, 90u, 540u, 120u, 68u);
    check_extent(160u, 90u, 360u, 80u, 45u);
    /* Odd native heights round up independently: 168.75 -> 169, 112.5 -> 113. */
    check_extent(400u, 225u, 540u, 300u, 169u);
    check_extent(400u, 225u, 360u, 200u, 113u);
    /* Square, non-16:9, small, and invalid-mode inputs stay logical. */
    check_extent(512u, 512u, 540u, 512u, 512u);
    check_extent(1024u, 1024u, 360u, 1024u, 1024u);
    check_extent(1280u, 800u, 540u, 1280u, 800u);
    check_extent(128u, 72u, 540u, 128u, 72u);
    check_extent(160u, 89u, 540u, 160u, 89u);
    check_extent(1280u, 720u, 720u, 1280u, 720u);
}

static void test_scissor(void) {
    VPRenderScissor s;
    /* Full surface: exclusive guest end 1280/720 -> inclusive 959/539. */
    s = vp_render_resolution_scissor(1280u, 720u, 960u, 540u,
                                     0u, 0u, 1280u, 720u);
    CHECK(!s.clip_all && s.min_x == 0u && s.min_y == 0u &&
          s.max_x == 959u && s.max_y == 539u);
    /* Partial: floor min, ceil exclusive max, then inclusive upper. */
    s = vp_render_resolution_scissor(1280u, 720u, 960u, 540u,
                                     10u, 20u, 100u, 200u);
    CHECK(!s.clip_all && s.min_x == 7u && s.min_y == 15u &&
          s.max_x == 74u && s.max_y == 149u);
    /* Empty logical rectangles stay CLIP_ALL and are not rounded into pixels. */
    s = vp_render_resolution_scissor(1280u, 720u, 960u, 540u,
                                     50u, 50u, 50u, 100u);
    CHECK(s.clip_all);
    s = vp_render_resolution_scissor(1280u, 720u, 960u, 540u,
                                     50u, 50u, 100u, 50u);
    CHECK(s.clip_all);
    s = vp_render_resolution_scissor(1280u, 720u, 960u, 540u,
                                     100u, 100u, 50u, 50u);
    CHECK(s.clip_all);
    /* Clamping can also make a nonempty guest rectangle empty. */
    s = vp_render_resolution_scissor(1280u, 720u, 960u, 540u,
                                     1280u, 0u, 2000u, 720u);
    CHECK(s.clip_all);
    /* X/Y ratios are independent when ceil rounding differs (160x90 -> 120x68). */
    s = vp_render_resolution_scissor(160u, 90u, 120u, 68u,
                                     0u, 0u, 160u, 90u);
    CHECK(!s.clip_all && s.max_x == 119u && s.max_y == 67u);
    s = vp_render_resolution_scissor(160u, 90u, 120u, 68u,
                                     0u, 45u, 160u, 90u);
    CHECK(!s.clip_all && s.min_y == 34u && s.max_y == 67u);
    /* Unscaled square image: the same conversion still yields N-1 inclusive. */
    s = vp_render_resolution_scissor(512u, 512u, 512u, 512u,
                                     0u, 0u, 512u, 512u);
    CHECK(!s.clip_all && s.min_x == 0u && s.min_y == 0u &&
          s.max_x == 511u && s.max_y == 511u);
}

int main(void) {
    test_parser();
    test_extents();
    test_scissor();
    if (failures != 0) {
        fprintf(stderr, "FAIL render resolution: %d checks\n", failures);
        return 1;
    }
    puts("PASS render resolution: parser/extent/cap/rounding/scissor");
    return 0;
}

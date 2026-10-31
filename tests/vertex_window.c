/* Host checks for bounded indexed-vertex window planning and remapping.
 *
 * Expected values are derived from the Maxwell contract independently of the
 * implementation: the source window is [min(index)+base, max(index)+base] with
 * restart markers excluded, indices are rebased by the raw minimum, and the
 * canonical GXM restart marker replaces the source marker.
 */
#include "vertex_window.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void test_u16_window(void) {
    const uint16_t indices[] = {100, 102, 101, 102};
    VpVertexWindowPlan plan;
    CHECK(vp_vertex_window_plan(indices, 4, 2, 0, 0xffffu, 0, 24, 0, &plan) ==
          VP_VERTEX_WINDOW_OK);
    CHECK(plan.first_index == 100 && plan.min_index == 100 && plan.max_index == 102);
    CHECK(plan.first_vertex == 100 && plan.vertex_count == 3);
    CHECK(plan.real_indices == 4 && plan.restart_indices == 0);
    CHECK(plan.source_offset == 100u * 24u && plan.source_bytes == 3u * 24u);
}

static void test_restart_window(void) {
    const uint16_t indices[] = {100, 0xffffu, 102, 0xffffu, 101};
    VpVertexWindowPlan plan;
    CHECK(vp_vertex_window_plan(indices, 5, 2, 1, 0xffffu, 0, 16, 0, &plan) ==
          VP_VERTEX_WINDOW_OK);
    CHECK(plan.min_index == 100 && plan.max_index == 102 && plan.vertex_count == 3);
    CHECK(plan.real_indices == 3 && plan.restart_indices == 2);
    /* With restart disabled the marker is an ordinary index. */
    const uint16_t ordinary[] = {1, 7, 2};
    CHECK(vp_vertex_window_plan(ordinary, 3, 2, 0, 7, 0, 16, 0, &plan) ==
          VP_VERTEX_WINDOW_OK);
    CHECK(plan.max_index == 7 && plan.real_indices == 3 && plan.restart_indices == 0);
}

static void test_all_restart(void) {
    const uint8_t indices[] = {0xffu, 0xffu, 0xffu};
    VpVertexWindowPlan plan;
    CHECK(vp_vertex_window_plan(indices, 3, 1, 1, 0xffu, 0, 12, 0, &plan) ==
          VP_VERTEX_WINDOW_EMPTY);
    CHECK(plan.real_indices == 0 && plan.restart_indices == 3);
    CHECK(vp_vertex_window_plan(indices, 0, 1, 1, 0xffu, 0, 12, 0, &plan) ==
          VP_VERTEX_WINDOW_EMPTY);
}

static void test_high_window(void) {
    const uint32_t indices[] = {1000000u, 1000002u, 1000001u};
    VpVertexWindowPlan plan;
    CHECK(vp_vertex_window_plan(indices, 3, 4, 0, 0xffffffffu, 0, 32, 0, &plan) ==
          VP_VERTEX_WINDOW_OK);
    CHECK(plan.first_vertex == 1000000u && plan.vertex_count == 3);
    CHECK(plan.source_offset == 1000000ull * 32ull);
    CHECK(plan.source_bytes == 3ull * 32ull);
}

static void test_base_vertex(void) {
    const uint16_t indices[] = {100, 102, 101};
    VpVertexWindowPlan plan;
    CHECK(vp_vertex_window_plan(indices, 3, 2, 0, 0, 50, 12, 0, &plan) ==
          VP_VERTEX_WINDOW_OK);
    CHECK(plan.first_vertex == 150 && plan.vertex_count == 3);
    CHECK(plan.source_offset == 150u * 12u);
    CHECK(vp_vertex_window_plan(indices, 3, 2, 0, 0, -50, 12, 0, &plan) ==
          VP_VERTEX_WINDOW_OK);
    CHECK(plan.first_vertex == 50 && plan.vertex_count == 3);

    CHECK(vp_vertex_window_plan(indices, 3, 2, 0, 0, -101, 12, 0, &plan) ==
          VP_VERTEX_WINDOW_BASE_UNDERFLOW);
    const uint32_t top[] = {0xfffffffeu, 0xffffffffu, 0u};
    CHECK(vp_vertex_window_plan(top, 3, 4, 0, 0, 1, 12, 0, &plan) ==
          VP_VERTEX_WINDOW_BASE_OVERFLOW);
    const uint32_t high[] = {0xffffffffu, 0xfffffffeu, 0xffffffffu};
    CHECK(vp_vertex_window_plan(high, 3, 4, 0, 0, 0x7ffffff, 12, 0, &plan) ==
          VP_VERTEX_WINDOW_BASE_OVERFLOW);
    const uint32_t full[] = {0u, 0xffffffffu, 1u};
    CHECK(vp_vertex_window_plan(full, 3, 4, 0, 0, 0, 12, 0, &plan) ==
          VP_VERTEX_WINDOW_SPAN_OVERFLOW);
}

static void test_identity_gate(void) {
    const uint16_t indices[] = {0, 2, 1};
    VpVertexWindowPlan plan;
    /* VertexIdCopy is not emulated even at window zero. */
    CHECK(vp_vertex_window_plan(indices, 3, 2, 0, 0, 0, 12, 1, &plan) ==
          VP_VERTEX_WINDOW_IDENTITY_REQUIRED);
    const uint16_t shifted[] = {4, 6, 5};
    CHECK(vp_vertex_window_plan(shifted, 3, 2, 0, 0, 0, 12, 1, &plan) ==
          VP_VERTEX_WINDOW_IDENTITY_REQUIRED);
    CHECK(vp_vertex_window_plan(indices, 3, 2, 0, 0, 1, 12, 1, &plan) ==
          VP_VERTEX_WINDOW_IDENTITY_REQUIRED);
}

static void test_limits(void) {
    uint16_t indices[VP_VERTEX_WINDOW_MAX_VERTICES + 1u];
    VpVertexWindowPlan plan;
    for (unsigned i = 0; i < VP_VERTEX_WINDOW_MAX_VERTICES + 1u; i++)
        indices[i] = (uint16_t)i;
    CHECK(vp_vertex_window_plan(indices, VP_VERTEX_WINDOW_MAX_VERTICES + 1u,
                                2, 0, 0, 0, 8, 0, &plan) ==
          VP_VERTEX_WINDOW_COUNT_LIMIT);
    CHECK(plan.vertex_count == VP_VERTEX_WINDOW_MAX_VERTICES + 1u);

    /* Measured actual window from run10: 5480 vertices at stride 24. */
    CHECK(vp_vertex_window_plan(indices, 5480, 2, 0, 0, 0, 24, 0, &plan) ==
          VP_VERTEX_WINDOW_OK);
    CHECK(plan.vertex_count == 5480 && plan.source_bytes == 5480ull * 24ull);
    /* Exactly 8192 vertices / 262144 bytes is inside the finite budget. */
    CHECK(vp_vertex_window_plan(indices, 8192, 2, 0, 0, 0, 32, 0, &plan) ==
          VP_VERTEX_WINDOW_OK);
    CHECK(plan.vertex_count == 8192 && plan.source_bytes == 262144ull);
    /* 8192 vertices at stride 33 exceed 256 KiB. */
    CHECK(vp_vertex_window_plan(indices, 8192, 2, 0, 0, 0, 33, 0, &plan) ==
          VP_VERTEX_WINDOW_BYTE_LIMIT);
    CHECK(plan.vertex_count == 8192 && plan.source_bytes == 8192ull * 33ull);
    /* Index count has its own unchanged 65,536 limit and is checked before
     * any index dereference. */
    CHECK(vp_vertex_window_plan(indices, VP_VERTEX_WINDOW_MAX_INDEX_COUNT + 1u,
                                2, 0, 0, 0, 8, 0, &plan) ==
          VP_VERTEX_WINDOW_INDEX_LIMIT);
}

static void test_invalid(void) {
    const uint16_t indices[] = {0, 1, 2};
    VpVertexWindowPlan plan;
    CHECK(vp_vertex_window_plan(NULL, 3, 2, 0, 0, 0, 12, 0, &plan) ==
          VP_VERTEX_WINDOW_INVALID_ARGUMENT);
    CHECK(vp_vertex_window_plan(indices, 3, 3, 0, 0, 0, 12, 0, &plan) ==
          VP_VERTEX_WINDOW_INVALID_ARGUMENT);
    CHECK(vp_vertex_window_plan(indices, 3, 2, 0, 0, 0, 0, 0, &plan) ==
          VP_VERTEX_WINDOW_INVALID_ARGUMENT);
    CHECK(vp_vertex_window_plan(indices, 3, 2, 0, 0, 0, 12, 0, NULL) ==
          VP_VERTEX_WINDOW_INVALID_ARGUMENT);
}

static void test_remap(void) {
    const uint16_t source[] = {100, 0xffffu, 102, 101};
    uint16_t out16[4] = {0};
    uint8_t out8[8] = {0};
    CHECK(vp_vertex_window_remap(source, 4, 2, 1, 0xffffu, 100,
                                 out16, sizeof(out16), 2, 0xffffu) == 0);
    CHECK(out16[0] == 0 && out16[1] == 0xffffu && out16[2] == 2 && out16[3] == 1);

    const uint8_t small[] = {5, 0xffu, 7, 6};
    CHECK(vp_vertex_window_remap(small, 4, 1, 1, 0xffu, 5,
                                 out8, sizeof(out8), 2, 0xffffu) == 0);
    CHECK(((uint16_t *)out8)[0] == 0 && ((uint16_t *)out8)[1] == 0xffffu);
    CHECK(((uint16_t *)out8)[2] == 2 && ((uint16_t *)out8)[3] == 1);

    /* A non-canonical source marker is normalized without moving boundaries. */
    const uint16_t odd[] = {100, 5, 102, 101};
    CHECK(vp_vertex_window_remap(odd, 4, 2, 1, 5, 100,
                                 out16, sizeof(out16), 2, 0xffffu) == 0);
    CHECK(out16[0] == 0 && out16[1] == 0xffffu && out16[2] == 2 && out16[3] == 1);

    /* A remapped real index must not become the marker. */
    const uint16_t collision[] = {105, 100, 101};
    CHECK(vp_vertex_window_remap(collision, 3, 2, 1, 0xffffu, 100,
                                 out16, sizeof(out16), 2, 5) < 0);

    const uint32_t wide[] = {100, 0xffffffffu, 101};
    uint32_t out32[3] = {0};
    CHECK(vp_vertex_window_remap(wide, 3, 4, 1, 0xffffffffu, 100,
                                 out32, sizeof(out32), 4, 0xffffffffu) == 0);
    CHECK(out32[0] == 0 && out32[1] == 0xffffffffu && out32[2] == 1);

    CHECK(vp_vertex_window_remap(source, 4, 2, 0, 0, 101,
                                 out16, sizeof(out16), 2, 0xffffu) < 0);
    CHECK(vp_vertex_window_remap(source, 4, 2, 0, 0, 100,
                                 out16, 6, 2, 0xffffu) < 0);
    CHECK(vp_vertex_window_remap(source, 4, 2, 0, 0, 100,
                                 out16, sizeof(out16), 3, 0xffffu) < 0);
}

static void test_same_width_copy(void) {
    /* Same width, window zero, restart disabled: exact bulk copy, including
     * values that would otherwise look like markers. */
    const uint16_t source16[] = {0xffffu, 7u, 0u, 9u};
    uint16_t out16[4];
    memset(out16, 0xa5, sizeof(out16));
    CHECK(vp_vertex_window_remap(source16, 4, 2, 0, 0xffffu, 0,
                                 out16, sizeof(out16), 2, 0xffffu) == 0);
    CHECK(memcmp(out16, source16, sizeof(source16)) == 0);

    const uint32_t source32[] = {0xffffffffu, 0u, 123456789u};
    uint32_t out32[3];
    memset(out32, 0xa5, sizeof(out32));
    CHECK(vp_vertex_window_remap(source32, 3, 4, 0, 0xffffffffu, 0,
                                 out32, sizeof(out32), 4, 0xffffffffu) == 0);
    CHECK(memcmp(out32, source32, sizeof(source32)) == 0);

    /* Unaligned same-width staging takes the same bulk path. */
    const uint8_t bytes16[] = {0xee, 4u, 0u, 6u, 0u, 5u, 0u};
    uint8_t copy16[sizeof(bytes16)];
    memset(copy16, 0xa5, sizeof(copy16));
    CHECK(vp_vertex_window_remap(bytes16 + 1, 3, 2, 0, 0, 0,
                                 copy16 + 1, sizeof(copy16) - 1u, 2, 0xffffu) == 0);
    CHECK(memcmp(copy16 + 1, bytes16 + 1, 6u) == 0);

    const uint8_t bytes32[] = {0xee, 1u,0u,0u,0u, 3u,0u,0u,0u, 2u,0u,0u,0u};
    uint8_t copy32[sizeof(bytes32)];
    memset(copy32, 0xa5, sizeof(copy32));
    CHECK(vp_vertex_window_remap(bytes32 + 1, 3, 4, 0, 0, 0,
                                 copy32 + 1, sizeof(copy32) - 1u, 4, 0xffffffffu) == 0);
    CHECK(memcmp(copy32 + 1, bytes32 + 1, 12u) == 0);

    /* Non-fast cases keep their exact validation: narrowing, restart
     * normalization, window underflow and output overflow. */
    const uint32_t narrow[] = {0x10000u, 0u, 1u};
    uint16_t out_narrow[3];
    CHECK(vp_vertex_window_remap(narrow, 3, 4, 0, 0, 0,
                                 out_narrow, sizeof(out_narrow), 2, 0xffffu) < 0);
    const uint16_t restart[] = {5u, 7u, 9u};
    uint16_t out_restart[3];
    CHECK(vp_vertex_window_remap(restart, 3, 2, 1, 7u, 0,
                                 out_restart, sizeof(out_restart), 2, 0xffffu) == 0);
    CHECK(out_restart[0] == 5u && out_restart[1] == 0xffffu && out_restart[2] == 9u);
    CHECK(vp_vertex_window_remap(source16, 4, 2, 0, 0, 1,
                                 out16, sizeof(out16), 2, 0xffffu) < 0);
    CHECK(vp_vertex_window_remap(source16, 4, 2, 0, 0, 0,
                                 out16, sizeof(out16) - 2u, 2, 0xffffu) < 0);
}

static void test_unaligned_and_source_vertices(void) {
    const uint8_t bytes[] = {0xee, 100, 0, 102, 0, 101, 0};
    const uint16_t *unaligned = (const uint16_t *)(const void *)(bytes + 1);
    VpVertexWindowPlan plan;
    CHECK(vp_vertex_window_plan(unaligned, 3, 2, 0, 0, 0, 4, 0, &plan) ==
          VP_VERTEX_WINDOW_OK);
    CHECK(plan.first_vertex == 100 && plan.vertex_count == 3);

    /* The source window must contain the actual vertices, not a dense copy of
     * max(index)+1 entries starting at the stream base. */
    uint32_t vertices[128];
    const uint16_t indices[] = {100, 102, 101};
    uint16_t remapped[3] = {0};
    for (unsigned i = 0; i < 128; i++) vertices[i] = i * 7u;
    uint32_t window[3] = {0};
    memcpy(window, (const uint8_t *)vertices + plan.source_offset, sizeof(window));
    CHECK(window[0] == 700u && window[1] == 707u && window[2] == 714u);
    CHECK(vp_vertex_window_remap(indices, 3, 2, 0, 0, plan.first_index,
                                 remapped, sizeof(remapped), 2, 0xffffu) == 0);
    CHECK(window[remapped[0]] == 700u);
    CHECK(window[remapped[1]] == 714u);
    CHECK(window[remapped[2]] == 707u);
}

int main(void) {
    test_u16_window();
    test_restart_window();
    test_all_restart();
    test_high_window();
    test_base_vertex();
    test_identity_gate();
    test_limits();
    test_invalid();
    test_remap();
    test_same_width_copy();
    test_unaligned_and_source_vertices();
    if (failures != 0) {
        fprintf(stderr, "vertex window: %d failure(s)\n", failures);
        return 1;
    }
    puts("PASS vertex window: range/restart/base/u8/u16/u32/overflow/limits/unaligned/remap/copy");
    return 0;
}

/* Host regression for effective colour-attachment classification.
 *
 * Reference semantics: yuzu maxwell_3d.h RenderTargetConfig/RtControl and
 * texture_cache FindColorBuffer. A colour target is active only when
 * RT_CONTROL (GXM 0x487) selects it and the configuration has a nonzero
 * address/extent with a format other than NONE (0). A stale RT0 address plus
 * a nonzero colour mask is not a colour attachment.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gpu_stream.h"

static unsigned failures;

#define CHECK(condition, name)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("GPU_EFFECTIVE_RT FAIL case=%s line=%d\n", (name),          \
                   __LINE__);                                                 \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void set_target(vp_gpu_state *state, uint8_t sub, unsigned index,
                       uint64_t address, uint32_t width, uint32_t height,
                       uint32_t format) {
    uint16_t base = (uint16_t)(0x200u + 16u * index);
    vp_gpu_state_apply(state, sub, base, (uint32_t)(address >> 32));
    vp_gpu_state_apply(state, sub, (uint16_t)(base + 1u),
                       (uint32_t)(address & 0xffffffffu));
    vp_gpu_state_apply(state, sub, (uint16_t)(base + 2u), width);
    vp_gpu_state_apply(state, sub, (uint16_t)(base + 3u), height);
    vp_gpu_state_apply(state, sub, (uint16_t)(base + 4u), format);
}

static void set_rt_control(vp_gpu_state *state, uint8_t sub, unsigned count,
                           unsigned map0) {
    vp_gpu_state_apply(state, sub, 0x487u,
                       (count & 0xfu) | ((map0 & 7u) << 4));
}

static vp_gpu_draw_state snapshot(vp_gpu_state *state) {
    vp_gpu_draw_state draw;
    int rc = vp_gpu_state_snapshot(state, 0u, 0u, 0, &draw);
    CHECK(rc == 0, "snapshot_rc");
    return draw;
}

int main(void) {
    /* Stale RT0 with a nonzero mask but format NONE: no colour target. */
    {
        vp_gpu_state state;
        vp_gpu_state_init(&state);
        set_target(&state, 0u, 0u, 0x56ace0000ull, 1920u, 1080u, 0u);
        vp_gpu_state_apply(&state, 0u, 0x680u, 0x1111u);
        set_rt_control(&state, 0u, 1u, 0u);
        vp_gpu_draw_state draw = snapshot(&state);
        CHECK(draw.color_attachment == 0u, "none_format_inactive");
        CHECK(draw.color_mask == 0u, "none_format_mask_cleared");
        CHECK(draw.color_mask_raw == 0x1111u, "none_format_raw_preserved");
        CHECK(draw.target_count == 1u, "none_format_target_still_decoded");
    }

    /* RT_CONTROL count zero: the stale address and mask are inactive. */
    {
        vp_gpu_state state;
        vp_gpu_state_init(&state);
        set_target(&state, 0u, 0u, 0x56ace0000ull, 1920u, 1080u, 213u);
        vp_gpu_state_apply(&state, 0u, 0x680u, 0x1111u);
        set_rt_control(&state, 0u, 0u, 0u);
        vp_gpu_draw_state draw = snapshot(&state);
        CHECK(draw.rt_control_observed == 1u, "count_zero_observed");
        CHECK(draw.color_attachment == 0u, "count_zero_inactive");
        CHECK(draw.color_mask == 0u, "count_zero_mask_cleared");
    }

    /* Normal single RT0: effective mask is the raw per-target mask. */
    {
        vp_gpu_state state;
        vp_gpu_state_init(&state);
        set_target(&state, 0u, 0u, 0x56ace0000ull, 1920u, 1080u, 213u);
        vp_gpu_state_apply(&state, 0u, 0x680u, 0x1111u);
        set_rt_control(&state, 0u, 1u, 0u);
        vp_gpu_draw_state draw = snapshot(&state);
        CHECK(draw.color_attachment == 1u, "single_rt_active");
        CHECK(draw.color_mask == 0x1111u, "single_rt_mask");
        CHECK(draw.active_rt_index == 0u, "single_rt_index");
    }

    /* MRT count > 1 has no backend path and must be an explicit refusal. */
    {
        vp_gpu_state state;
        vp_gpu_state_init(&state);
        set_target(&state, 0u, 0u, 0x56ace0000ull, 1920u, 1080u, 213u);
        set_target(&state, 0u, 1u, 0x56b580000ull, 1920u, 1080u, 213u);
        vp_gpu_state_apply(&state, 0u, 0x680u, 0x1111u);
        set_rt_control(&state, 0u, 2u, 0u);
        vp_gpu_draw_state draw = snapshot(&state);
        CHECK(draw.color_mrt_unsupported == 1u, "mrt_unsupported");
        CHECK(draw.color_attachment == 0u, "mrt_no_silent_rt0");
    }

    /* Mapped RT with no common mask cannot be replayed faithfully. */
    {
        vp_gpu_state state;
        vp_gpu_state_init(&state);
        set_target(&state, 0u, 0u, 0x56ace0000ull, 1920u, 1080u, 213u);
        set_target(&state, 0u, 1u, 0x56b580000ull, 1280u, 720u, 213u);
        vp_gpu_state_apply(&state, 0u, 0x680u, 0x1111u);
        set_rt_control(&state, 0u, 1u, 1u);
        vp_gpu_draw_state draw = snapshot(&state);
        CHECK(draw.active_rt_index == 1u, "mapped_index");
        CHECK(draw.color_mapping_unsupported == 1u, "mapped_mask_unsupported");
        CHECK(draw.color_attachment == 0u, "mapped_not_silently_accepted");
    }

    /* A common mask makes the mapped target replayable. */
    {
        vp_gpu_state state;
        vp_gpu_state_init(&state);
        set_target(&state, 0u, 0u, 0x56ace0000ull, 1920u, 1080u, 213u);
        set_target(&state, 0u, 1u, 0x56b580000ull, 1280u, 720u, 213u);
        vp_gpu_state_apply(&state, 0u, 0x680u, 0x1111u);
        /* Independent literal word method 0x3E4 (byte 0x0F90). */
        vp_gpu_state_apply(&state, 0u, 0x3e4u, 1u);
        /* 0xF90 is only a word-method index and must not set the flag. */
        vp_gpu_state_apply(&state, 0u, 0xf90u, 1u);
        set_rt_control(&state, 0u, 1u, 1u);
        vp_gpu_draw_state draw = snapshot(&state);
        CHECK(draw.color_mask_common == 1u, "mapped_common_decoded");
        CHECK(draw.color_attachment == 1u, "mapped_common_active");
        CHECK(draw.active_rt_index == 1u, "mapped_common_index");
        CHECK(draw.color_mask == 0x1111u, "mapped_common_mask");
        CHECK(draw.color_mapping_unsupported == 0u, "mapped_common_supported");
    }

    /* Word method 0xF90 is not ColorMaskCommon; setting it alone must not
     * make a mapped target replayable. */
    {
        vp_gpu_state state;
        vp_gpu_state_init(&state);
        set_target(&state, 0u, 0u, 0x56ace0000ull, 1920u, 1080u, 213u);
        set_target(&state, 0u, 1u, 0x56b580000ull, 1280u, 720u, 213u);
        vp_gpu_state_apply(&state, 0u, 0x680u, 0x1111u);
        vp_gpu_state_apply(&state, 0u, 0xf90u, 1u);
        set_rt_control(&state, 0u, 1u, 1u);
        vp_gpu_draw_state draw = snapshot(&state);
        CHECK(draw.color_mask_common == 0u, "f90_not_common");
        CHECK(draw.color_mapping_unsupported == 1u, "f90_alone_unsupported");
        CHECK(draw.color_attachment == 0u, "f90_alone_no_attachment");
    }

    /* RT_CONTROL not yet observed: keep legacy classification, still reject
     * format NONE. */
    {
        vp_gpu_state state;
        vp_gpu_state_init(&state);
        set_target(&state, 0u, 0u, 0x56ace0000ull, 1920u, 1080u, 213u);
        vp_gpu_state_apply(&state, 0u, 0x680u, 0x1111u);
        vp_gpu_draw_state draw = snapshot(&state);
        CHECK(draw.rt_control_observed == 0u, "absent_observed");
        CHECK(draw.color_attachment == 1u, "absent_legacy_active");
        CHECK(draw.color_mask == 0x1111u, "absent_legacy_mask");
    }

    if (failures != 0) {
        printf("GPU_EFFECTIVE_RT failures=%u\n", failures);
        return 1;
    }
    printf("GPU_EFFECTIVE_RT failures=0 cases=8\n");
    return 0;
}

/* M6-B11 host regression: automatic-execution catch-up from a renderer-owned
 * snapshot.
 *
 * Drives the actual mixer implementation (audio_render.c is included) with a
 * real UpdateDataHeader-like command buffer and real MAP_32BIT PCM payloads.
 * The tests assert exact PCM content, not counters alone:
 *   - three RequestUpdate snapshots with three fresh periods each yield
 *     3 * 3 * 240 contiguous, unique source frames (no duplicate/skip);
 *   - the new-voice flag held in a snapshot resets the cursor only once;
 *   - mutating the client command buffer after apply cannot change periods;
 *   - retired wave slots are never re-latched until a later client update
 *     observes completion, including same-address refills;
 *   - pause, stop and EOS do not replay one-shot or invalid waves;
 *   - invalid/zero wave inputs report source starvation separately;
 *   - an unreadable PCM address is refused on every period;
 *   - the bounded demand planner never overfills the pace target/ring.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define VP_HOST_BRIDGE_H
#include <stdint.h>
typedef struct VPBridgeFrame VPBridgeFrame;
#include "../src/platform/audio_sink.h"
#include "../src/platform/audio_render.c"

/* The test owns one low mapping so every guest address is a real 32-bit
 * pointer, including the command buffer that apply_config copies out. */
#define MAP_BYTES 0x80000u
#define CFG_TOTAL 0xC90u
#define OFF_RES   0x40u
#define OFF_VOICE 0xB0u
#define OFF_MIX   0x220u
#define OFF_SINK  0xB50u
#define PCM_OFF   0x10000u
#define FRAMES    240u
#define SOURCE_FRAMES 4096u

static uint32_t map_base;
static uint32_t map_end;
static uint32_t cfg_addr;
static uint32_t pcm_addr;

int vp_guest_svc_readable(const VPGuestSvc *s, uint32_t a, uint32_t n) {
    (void)s;
    if (!a || !n) return 0;
    if (a < map_base || a > map_end) return 0;
    if (n > map_end - a) return 0;
    return 1;
}
void vp_guest_svc_log(VPGuestSvc *s, const char *fmt, ...) { (void)s; (void)fmt; }

static void w8(uint32_t a, uint8_t v) { memcpy((void *)(uintptr_t)a, &v, 1); }
static void w16(uint32_t a, uint16_t v) { memcpy((void *)(uintptr_t)a, &v, 2); }
static void w32(uint32_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static void w64(uint32_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }
static void wf(uint32_t a, float v) { memcpy((void *)(uintptr_t)a, &v, 4); }

static int16_t conv(int16_t v) {
    float l = (float)v / 32768.0f;
    if (l > 1.0f) l = 1.0f;
    if (l < -1.0f) l = -1.0f;
    return (int16_t)(int32_t)(l * 32767.0f);
}

static void cfg_defaults(void) {
    memset((void *)(uintptr_t)cfg_addr, 0, CFG_TOTAL);
    w32(cfg_addr + 0x00u, 0x34564552u);
    w32(cfg_addr + 0x0cu, 0x170u); /* voices size */
    w32(cfg_addr + 0x10u, 0x70u);  /* voice-channel resources size */
    w32(cfg_addr + 0x18u, 0x930u); /* mixes size */
    w32(cfg_addr + 0x1cu, 0x140u); /* sinks size */
    w32(cfg_addr + 0x3cu, CFG_TOTAL);

    wf(cfg_addr + OFF_RES + 0x04u, 1.0f);
    w8(cfg_addr + OFF_RES + 0x64u, 1);

    wf(cfg_addr + OFF_MIX + 0x00u, 1.0f);
    w32(cfg_addr + OFF_MIX + 0x08u, 1);
    w8(cfg_addr + OFF_MIX + 0x0cu, 1);
    w32(cfg_addr + OFF_MIX + 0x10u, 0);
    w32(cfg_addr + OFF_MIX + 0x924u, 0);

    w8(cfg_addr + OFF_SINK + 0x00u, 1);
    w8(cfg_addr + OFF_SINK + 0x01u, 1);
    w32(cfg_addr + OFF_SINK + 0x120u, 1);
    w8(cfg_addr + OFF_SINK + 0x124u, 0);

    w32(cfg_addr + OFF_VOICE + 0x0cu, 48000);
    w32(cfg_addr + OFF_VOICE + 0x18u, 1);
    wf(cfg_addr + OFF_VOICE + 0x1cu, 1.0f);
    wf(cfg_addr + OFF_VOICE + 0x20u, 1.0f);
    w32(cfg_addr + OFF_VOICE + 0x58u, 0);
    w32(cfg_addr + OFF_VOICE + 0x140u, 0);
}

static void voice_state(uint32_t play, uint8_t new_voice) {
    w8(cfg_addr + OFF_VOICE + 0x08u, new_voice);
    w8(cfg_addr + OFF_VOICE + 0x09u, 1);
    w8(cfg_addr + OFF_VOICE + 0x0au, (uint8_t)play);
    w8(cfg_addr + OFF_VOICE + 0x0bu, 2); /* PCM16 */
}

static void wave_slot(unsigned slot, uint32_t data, uint32_t frames,
                      uint8_t loop, uint8_t sent) {
    uint32_t at = cfg_addr + OFF_VOICE + 0x60u + slot * 0x38u;
    w64(at + 0x00u, data);
    w64(at + 0x08u, (uint64_t)frames * 2u);
    w32(at + 0x10u, 0);
    w32(at + 0x14u, frames);
    w8(at + 0x18u, loop);
    w8(at + 0x1au, sent);
    w32(at + 0x30u, 0);
    w32(at + 0x34u, 0);
}

static void wave_count(uint32_t count, uint32_t index) {
    w32(cfg_addr + OFF_VOICE + 0x3cu, count);
    w16(cfg_addr + OFF_VOICE + 0x40u, (uint16_t)index);
}

static VPGuestSvc svc = {0};

static int apply(VPAudioRenderer *r) {
    return vp_audio_renderer_apply_config(r, &svc, cfg_addr, CFG_TOTAL, NULL);
}

static const int16_t *period(VPAudioRenderer *r, VPAudioRenderStats *st,
                             uint32_t *frames) {
    int rc = vp_audio_renderer_render_period(r, &svc, st);
    assert(rc == 0);
    return vp_audio_renderer_output(r, frames);
}

static void check_mono(const int16_t *pcm, uint32_t frames,
                       const int16_t *src, uint32_t src_start,
                       uint32_t src_len) {
    for (uint32_t i = 0; i < frames; ++i) {
        int16_t want = conv(src[(src_start + i) % src_len]);
        assert(pcm[i * 2u] == want);
        assert(pcm[i * 2u + 1u] == want);
    }
}

static void check_zero(const int16_t *pcm, uint32_t frames) {
    for (uint32_t i = 0; i < frames * 2u; ++i) assert(pcm[i] == 0);
}

static int16_t source[SOURCE_FRAMES];

static void test_three_ipc_three_periods(void) {
    static unsigned seen[65536];
    memset(seen, 0, sizeof(seen));
    for (uint32_t i = 0; i < SOURCE_FRAMES; ++i)
        source[i] = (int16_t)(1000u + i);
    int16_t *pcm_src = (int16_t *)(uintptr_t)pcm_addr;
    memcpy(pcm_src, source, sizeof(source));

    cfg_defaults();
    voice_state(0, 1);
    wave_count(1, 0);
    wave_slot(0, pcm_addr, SOURCE_FRAMES, 1, 0);

    VPAudioRenderer *r = vp_audio_renderer_create(48000, FRAMES);
    assert(r);
    uint32_t expected_frame = 0;

    for (unsigned ipc = 0; ipc < 3u; ++ipc) {
        if (ipc) {
            /* The client's new-voice flag is a one-shot at creation. */
            voice_state(0, 0);
        }
        assert(apply(r) == 0);
        if (ipc == 1u) {
            /* Snapshot independence: mutate the client's command buffer after
             * apply. The three periods must still use the applied volume and
             * wave range, not this garbage. */
            wf(cfg_addr + OFF_VOICE + 0x20u, 0.25f);
            w32(cfg_addr + OFF_VOICE + 0x60u + 0x14u, 10);
            w64(cfg_addr + OFF_VOICE + 0x60u, 0x10u);
        }
        for (unsigned p = 0; p < 3u; ++p) {
            VPAudioRenderStats st;
            uint32_t frames = 0;
            const int16_t *pcm = period(r, &st, &frames);
            assert(frames == FRAMES);
            assert(st.source_frames == FRAMES);
            assert(st.voice_starved == 0);
            assert(st.voices_unsupported == 0);
            check_mono(pcm, frames, source, expected_frame, SOURCE_FRAMES);
            for (uint32_t i = 0; i < frames; ++i) {
                int16_t v = pcm[i * 2u];
                assert(v != 0);
                ++seen[(uint16_t)v];
            }
            expected_frame += frames;
        }
        if (ipc == 1u) {
            /* Restore only the fields mutated above for the next apply. */
            wf(cfg_addr + OFF_VOICE + 0x20u, 1.0f);
            w32(cfg_addr + OFF_VOICE + 0x60u + 0x14u, SOURCE_FRAMES);
            w64(cfg_addr + OFF_VOICE + 0x60u, pcm_addr);
        }
    }
    /* 2160 frames, each source frame used exactly once: no duplicate, no
     * skip, and no reset from the new-voice flag held in the snapshot. */
    assert(expected_frame == 3u * 3u * FRAMES);
    for (uint32_t i = 0; i < expected_frame; ++i)
        assert(seen[(uint16_t)conv(source[i])] == 1u);

    uint64_t played = 0;
    uint32_t consumed = 0;
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == expected_frame);
    assert(consumed == 0); /* looping slot is never retired */

    /* The one-period public API still snapshots and renders exactly once. */
    assert(apply(r) == 0);
    VPAudioRenderStats st;
    uint32_t frames = 0;
    assert(vp_audio_renderer_update(r, &svc, cfg_addr, CFG_TOTAL, &st) == 0);
    const int16_t *pcm = vp_audio_renderer_output(r, &frames);
    assert(frames == FRAMES && st.source_frames == FRAMES);
    check_mono(pcm, frames, source, expected_frame, SOURCE_FRAMES);

    vp_audio_renderer_destroy(r);
}

static void test_retired_slot_and_same_address_refill(void) {
    /* Four short slots are consumed in one period. The same snapshot must not
     * re-latch any of them; only a later client update that observes their
     * completion may submit a refill (same address, new data). */
    for (unsigned s = 0; s < 4u; ++s)
        for (unsigned j = 0; j < 60u; ++j)
            ((int16_t *)(uintptr_t)(pcm_addr + s * 0x80u))[j] =
                (int16_t)(1000u + s * 1000u + j);

    cfg_defaults();
    voice_state(0, 1);
    wave_count(4, 0);
    for (unsigned s = 0; s < 4u; ++s)
        wave_slot(s, pcm_addr + s * 0x80u, 60, 0, 0);

    VPAudioRenderer *r = vp_audio_renderer_create(48000, FRAMES);
    assert(r);
    assert(apply(r) == 0);
    VPAudioRenderStats st;
    uint32_t frames = 0;
    const int16_t *pcm = period(r, &st, &frames);
    assert(frames == FRAMES && st.source_frames == FRAMES);
    for (unsigned s = 0; s < 4u; ++s) {
        const int16_t *src = (const int16_t *)(uintptr_t)(pcm_addr + s * 0x80u);
        check_mono(pcm + s * 60u * 2u, 60, src, 0, 60);
    }
    uint64_t played = 0;
    uint32_t consumed = 0;
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == 240u && consumed == 4u);

    /* Extra periods from the same snapshot: retired slots stay retired. */
    for (unsigned n = 0; n < 2u; ++n) {
        pcm = period(r, &st, &frames);
        assert(frames == FRAMES);
        assert(st.source_frames == 0u);
        assert(st.voice_starved == 1u);
        check_zero(pcm, frames);
        assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
        assert(played == 240u && consumed == 4u);
    }

    /* The client observes completion for all four in a distinct update. The
     * observation itself produces no PCM but clears the sent edge latch; the
     * retired slots stay retired. */
    for (unsigned s = 0; s < 4u; ++s)
        w8(cfg_addr + OFF_VOICE + 0x60u + s * 0x38u + 0x1au, 1);
    voice_state(0, 0);
    wave_count(4, 0);
    assert(apply(r) == 0);
    pcm = period(r, &st, &frames);
    assert(frames == FRAMES && st.source_frames == 0u);
    check_zero(pcm, frames);
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == 240u && consumed == 4u);

    /* A later update refills slot 0 at the same address with new content.
     * The submitted slot is latched once and plays the new bytes. */
    for (unsigned j = 0; j < 60u; ++j)
        ((int16_t *)(uintptr_t)pcm_addr)[j] = (int16_t)(20000u + j);
    wave_slot(0, pcm_addr, 60, 0, 0); /* same address, sent cleared */
    voice_state(0, 0);
    wave_count(4, 0);
    assert(apply(r) == 0);
    pcm = period(r, &st, &frames);
    assert(frames == FRAMES && st.source_frames == 60u);
    check_mono(pcm, 60, (const int16_t *)(uintptr_t)pcm_addr, 0, 60);
    check_zero(pcm + 60u * 2u, FRAMES - 60u);
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    /* Cumulative VoiceOutStatus progress: 240 frames from the first period
     * plus 60 from the observed refill. */
    assert(played == 300u && consumed == 5u);

    /* And the refilled slot is not replayed by further periods. */
    pcm = period(r, &st, &frames);
    assert(st.source_frames == 0u && st.voice_starved == 1u);
    check_zero(pcm, frames);
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == 300u && consumed == 5u);

    vp_audio_renderer_destroy(r);
}

static void test_pause_resume(void) {
    for (uint32_t i = 0; i < SOURCE_FRAMES; ++i)
        source[i] = (int16_t)(i + 1u);
    memcpy((void *)(uintptr_t)pcm_addr, source, sizeof(source));

    cfg_defaults();
    voice_state(0, 1);
    wave_count(1, 0);
    wave_slot(0, pcm_addr, SOURCE_FRAMES, 1, 0);

    VPAudioRenderer *r = vp_audio_renderer_create(48000, FRAMES);
    assert(r);
    assert(apply(r) == 0);
    VPAudioRenderStats st;
    uint32_t frames = 0;
    const int16_t *pcm = period(r, &st, &frames);
    check_mono(pcm, frames, source, 0, SOURCE_FRAMES);
    uint64_t played = 0;
    uint32_t consumed = 0;
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == FRAMES);

    voice_state(2, 0); /* paused */
    assert(apply(r) == 0);
    for (unsigned n = 0; n < 2u; ++n) {
        pcm = period(r, &st, &frames);
        assert(st.source_frames == 0u);
        check_zero(pcm, frames);
        assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
        assert(played == FRAMES);
    }

    voice_state(0, 0); /* resumed: continues, no replay */
    assert(apply(r) == 0);
    pcm = period(r, &st, &frames);
    check_mono(pcm, frames, source, FRAMES, SOURCE_FRAMES);
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == 2u * FRAMES);

    vp_audio_renderer_destroy(r);
}

static void test_stop_and_restart(void) {
    for (uint32_t i = 0; i < SOURCE_FRAMES; ++i)
        source[i] = (int16_t)(i + 1u);
    memcpy((void *)(uintptr_t)pcm_addr, source, sizeof(source));

    cfg_defaults();
    voice_state(0, 1);
    wave_count(1, 0);
    wave_slot(0, pcm_addr, SOURCE_FRAMES, 1, 0);

    VPAudioRenderer *r = vp_audio_renderer_create(48000, FRAMES);
    assert(r);
    assert(apply(r) == 0);
    VPAudioRenderStats st;
    uint32_t frames = 0;
    const int16_t *pcm = period(r, &st, &frames);
    check_mono(pcm, frames, source, 0, SOURCE_FRAMES);
    uint64_t played = 0;
    uint32_t consumed = 0;
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == FRAMES && consumed == 0);

    voice_state(1, 0); /* RequestStop flushes the outstanding slot */
    assert(apply(r) == 0);
    pcm = period(r, &st, &frames);
    check_zero(pcm, frames);
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == 0u && consumed == 1u);
    /* Additional periods of the same stopped snapshot do not retire twice. */
    pcm = period(r, &st, &frames);
    check_zero(pcm, frames);
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == 0u && consumed == 1u);

    /* The client observes the old slot and submits a refill at the same
     * address in the server's next slot (the stop flush advanced the ring
     * head to slot 1). Restart plays the refill, not the old data and not
     * the flushed slot a second time. */
    w8(cfg_addr + OFF_VOICE + 0x60u + 0x1au, 1); /* old slot observed */
    for (unsigned j = 0; j < 120u; ++j)
        ((int16_t *)(uintptr_t)pcm_addr)[j] = (int16_t)(25000u + j);
    wave_slot(1, pcm_addr, 120, 0, 0);
    voice_state(0, 0);
    wave_count(2, 1);
    assert(apply(r) == 0);
    pcm = period(r, &st, &frames);
    assert(st.source_frames == 120u);
    check_mono(pcm, 120, (const int16_t *)(uintptr_t)pcm_addr, 0, 120);
    check_zero(pcm + 120u * 2u, FRAMES - 120u);
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == 120u && consumed == 2u);

    vp_audio_renderer_destroy(r);
}

static void test_eos(void) {
    for (uint32_t i = 0; i < SOURCE_FRAMES; ++i)
        source[i] = (int16_t)(i + 1u);
    memcpy((void *)(uintptr_t)pcm_addr, source, sizeof(source));

    cfg_defaults();
    voice_state(0, 1);
    wave_count(1, 0);
    wave_slot(0, pcm_addr, 100, 0, 0); /* one-shot, shorter than a period */

    VPAudioRenderer *r = vp_audio_renderer_create(48000, FRAMES);
    assert(r);
    assert(apply(r) == 0);
    VPAudioRenderStats st;
    uint32_t frames = 0;
    const int16_t *pcm = period(r, &st, &frames);
    assert(st.source_frames == 100u);
    check_mono(pcm, 100, source, 0, 100);
    check_zero(pcm + 100u * 2u, FRAMES - 100u);
    uint64_t played = 0;
    uint32_t consumed = 0;
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == 100u && consumed == 1u);

    /* Neither additional periods nor the next client update (with the
     * one-shot new-voice flag clear) replay EOS. */
    for (unsigned n = 0; n < 2u; ++n) {
        if (n == 1u) {
            voice_state(0, 0);
            assert(apply(r) == 0);
        }
        pcm = period(r, &st, &frames);
        assert(st.source_frames == 0u && st.voice_starved == 1u);
        check_zero(pcm, frames);
        assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
        assert(played == 100u && consumed == 1u);
    }

    /* A stream-end slot resets the played clock exactly when consumed. */
    cfg_defaults();
    voice_state(0, 1);
    wave_count(2, 0);
    wave_slot(0, pcm_addr, 100, 0, 0);
    wave_slot(1, pcm_addr + 0x400u, 100, 0, 0);
    w8(cfg_addr + OFF_VOICE + 0x60u + 0x38u + 0x19u, 1);
    assert(apply(r) == 0);
    pcm = period(r, &st, &frames);
    assert(st.source_frames == 200u);
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == 0u && consumed == 2u); /* EOS reset after slot 1 */
    pcm = period(r, &st, &frames); /* no auto-queue of a third wave */
    assert(st.source_frames == 0u);
    check_zero(pcm, frames);
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == 0u && consumed == 2u);

    vp_audio_renderer_destroy(r);
}

static void test_invalid_and_starved_inputs(void) {
    cfg_defaults();
    voice_state(0, 1);
    wave_count(1, 0);
    wave_slot(0, 0, 0, 0, 0); /* no submitted source */

    VPAudioRenderer *r = vp_audio_renderer_create(48000, FRAMES);
    assert(r);
    /* No snapshot yet. */
    VPAudioRenderStats st;
    assert(vp_audio_renderer_render_period(r, &svc, &st) < 0);
    assert(apply(r) == 0);
    uint32_t frames = 0;
    const int16_t *pcm = period(r, &st, &frames);
    assert(st.source_frames == 0u && st.voice_starved == 1u);
    assert(st.voices_unsupported == 0u);
    check_zero(pcm, frames);

    /* A submitted but unreadable guest wave address is refused every period,
     * never read and never converted into fabricated PCM. */
    wave_slot(0, 0x10u, 100, 0, 0);
    voice_state(0, 1);
    assert(apply(r) == 0);
    for (unsigned n = 0; n < 2u; ++n) {
        pcm = period(r, &st, &frames);
        assert(st.source_frames == 0u);
        assert(st.voices_unsupported == 1u);
        assert(st.voice_starved == 0u);
        check_zero(pcm, frames);
    }
    uint64_t played = 0;
    uint32_t consumed = 0;
    assert(vp_audio_renderer_voice_status(r, 0, &played, &consumed) == 0);
    assert(played == 0u && consumed == 0u);

    /* Apply failures invalidate the snapshot instead of leaving a partially
     * applied configuration behind. */
    assert(apply(r) == 0);
    w32(cfg_addr, 0xdeadbeefu);
    assert(apply(r) == -102);
    assert(vp_audio_renderer_render_period(r, &svc, &st) < 0);
    w32(cfg_addr, 0x34564552u);

    /* Unreadable command buffer. */
    assert(vp_audio_renderer_apply_config(r, &svc, 0xeu, CFG_TOTAL, NULL) < 0);
    assert(vp_audio_renderer_render_period(r, &svc, &st) < 0);
    /* Oversized command buffer (> 1 MiB limit). */
    assert(vp_audio_renderer_apply_config(r, &svc, cfg_addr, 0x100001u, NULL) < 0);
    assert(vp_audio_renderer_render_period(r, &svc, &st) < 0);

    /* Destroy frees the owned snapshot under ASan/UBSan. */
    vp_audio_renderer_destroy(r);
}

static void test_bounded_demand_and_timing(void) {
    assert(VP_AR_AUTO_MAX_PERIODS == 16u);
    /* M6-B12 raised the production pace target to 128 ms; these remain the
     * original 64 ms planner cases and stay valid for the parameterized
     * planner. */
    assert(VP_AUDIO_SINK_PACE_FRAMES == 6144u);
    assert(VP_AUDIO_SINK_PACE_FRAMES <= VP_AUDIO_SINK_RING_FRAMES);
    assert(vp_audio_catchup_periods(0, FRAMES, 3072u,
                                    VP_AR_AUTO_MAX_PERIODS - 1u) == 12u);
    assert(vp_audio_catchup_periods(FRAMES, FRAMES, 3072u,
                                    VP_AR_AUTO_MAX_PERIODS - 1u) == 11u);
    assert(vp_audio_catchup_periods(2832u, FRAMES, 3072u, 16u) == 1u);
    assert(vp_audio_catchup_periods(3000u, FRAMES, 3072u, 16u) == 0u);
    assert(vp_audio_catchup_periods(3072u, FRAMES, 3072u, 16u) == 0u);
    assert(vp_audio_catchup_periods(0, FRAMES, 3072u, 3u) == 3u);
    assert(vp_audio_catchup_periods(0, 0, 3072u, 16u) == 0u);
    assert(vp_audio_catchup_periods(0, FRAMES, 0, 16u) == 0u);

    /* Model the service loop: first period, then bounded catch-up. The queue
     * never exceeds the pace target and no request exceeds 16 periods. */
    uint32_t pending = FRAMES;
    uint32_t periods = 1;
    while (periods < VP_AR_AUTO_MAX_PERIODS
           && vp_audio_catchup_periods(pending, FRAMES, 3072u, 1u)) {
        pending += FRAMES;
        ++periods;
    }
    assert(periods == 12u);
    assert(pending == 2880u && pending <= 3072u);

    /* With the 128 ms target, an empty queue correctly reports cap-limited
     * demand rather than pretending it was filled in one call. */
    assert(vp_audio_catchup_periods(FRAMES, FRAMES, VP_AUDIO_SINK_PACE_FRAMES,
                                    VP_AR_AUTO_MAX_PERIODS - 1u) == 15u);
    assert(FRAMES + 15u * FRAMES <= VP_AUDIO_SINK_PACE_FRAMES);

    /* Timing window sums accepted frames over all fresh periods. */
    VPAudioTimingWindow window = {0};
    VPAudioTimingSummary summary;
    vp_audio_timing_note_periods(&window, 1000u, 10u, 0u, 20u,
                                 FRAMES * 3u, 3u, FRAMES * 3u, 0u, 0u, 0u);
    vp_audio_timing_note_periods(&window, 16000u, 10u, 0u, 20u,
                                 FRAMES * 2u, 2u, FRAMES * 2u, 120u, 1u, 0u);
    vp_audio_timing_summary(&window, &summary);
    assert(summary.updates == 2u);
    assert(summary.periods == 5u && summary.periods_max == 3u);
    assert(summary.accepted_frames == FRAMES * 5u);
    assert(summary.source_frames == FRAMES * 5u);
    assert(summary.debt_frames == 120u && summary.starvation_periods == 1u);
    assert(summary.interval_mean_us == 15000u);
}

int main(void) {
    void *mem = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(mem != MAP_FAILED && (uintptr_t)mem <= UINT32_MAX);
    map_base = (uint32_t)(uintptr_t)mem;
    map_end = map_base + MAP_BYTES;
    cfg_addr = map_base;
    pcm_addr = map_base + PCM_OFF;
    memset(mem, 0, MAP_BYTES);

    test_three_ipc_three_periods();
    test_retired_slot_and_same_address_refill();
    test_pause_resume();
    test_stop_and_restart();
    test_eos();
    test_invalid_and_starved_inputs();
    test_bounded_demand_and_timing();

    munmap(mem, MAP_BYTES);
    puts("PASS audio auto catch-up: 3x3x240 unique PCM, snapshots, slots, "
         "pause/stop/EOS, starvation, planner bounds");
    return 0;
}

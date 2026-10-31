/* M5: minimal audren renderer for the locked client's update data.
 *
 * The service receives the client-generated UpdateDataHeader on every
 * RequestUpdate. This module parses the reviewed sections and renders the
 * voices into the mix graph, then selects the device sink's stereo input
 * buffers. It is deliberately not a full ADSP replacement:
 *   - voices: PCM8 / PCM16 / float, linear sample-rate conversion by
 *     sample_rate and pitch, wave-buffer loop fields;
 *   - mixes: final mix and submix volume matrices, offsets assigned in
 *     distance-from-final order exactly like the reviewed server;
 *   - effects: skipped (counted and reported, never claimed as applied);
 *   - sinks: device sink with 1/2 inputs, or 6 inputs with the client's explicit
 *     downmix flag and finite four-coefficient matrix; other layouts refused.
 * Anything outside that set increments an explicit unsupported counter and
 * contributes silence; the caller decides whether to fail the update. */
#ifndef VP_AUDIO_RENDER_H
#define VP_AUDIO_RENDER_H

#include <stdint.h>
#include "guest_svc.h"

/* The renderer parameter's sampleCount bounds one RequestUpdate's output.
 * 240 (5 ms) is the usual game value. The M5 probe uses 960 (20 ms): the
 * the emulated Client/Server round trip measured 6-13 ms after warm-up and
 * 20-39 ms for the first updates, so a 480-frame (10 ms) period leaves too
 * little margin during startup and drains the sink's pre-roll. This is a
 * memory bound, not a device constraint. */
#define VP_AR_MAX_FRAMES 1024u
/* The title's own renderer (observed in the M5 play run and the locked SDK
 * probe) opens 140 mixes / 182 voices / 7 sub-mixes / 70 effects. 64 mixes
 * made every such update fail with -104 before the mixer was reached. These
 * are still hard bounds: layouts above them are rejected, never silently
 * truncated. Mix/sample-buffer storage is allocated per update (no static
 * arrays), so the larger bounds only raise the memory ceiling. */
#define VP_AR_MAX_MIXES 192u
#define VP_AR_MAX_VOICES 512u
#define VP_AR_MAX_RESOURCES 512u
#define VP_AR_MAX_SAMPLE_BUFFERS 1024u

/* M6-B11: automatic execution mode renders on its own clock from the last
 * applied configuration. The service may render at most this many periods
 * (the first real RequestUpdate period plus catch-up periods) per IPC. 16
 * periods cover a full pace target from empty at 240 frames per period when
 * the target is 3072 frames (12 periods fit); with the M6-B12 6144-frame
 * target a fully empty queue legitimately needs more than one call and the
 * service reports cap_hits/debt instead of overfilling. The bound keeps each
 * RequestUpdate's synthetic work fixed. */
#define VP_AR_AUTO_MAX_PERIODS 16u

typedef struct {
    uint32_t voices_active;
    uint32_t voices_rendered;
    uint32_t voices_unsupported;
    uint32_t voice_starved;   /* in-use playing voices with no usable source */
    uint32_t source_frames;   /* output frames covered by a real voice */
    uint32_t mix_count;
    uint32_t sink_count;
    uint32_t sink_inputs;
    uint32_t effects_ignored;
    uint32_t final_offset;
    uint32_t peak_left;
    uint32_t peak_right;
    uint32_t rms_left;   /* 0..32767 */
    uint32_t rms_right;
    uint64_t zero_left;
    uint64_t zero_right;
    uint32_t parse_errors;
    uint32_t sink_unsupported;
    uint32_t pcm_frames;
} VPAudioRenderStats;

typedef struct VPAudioRenderer VPAudioRenderer;

VPAudioRenderer *vp_audio_renderer_create(uint32_t sample_rate,
                                          uint32_t sample_count);
void vp_audio_renderer_destroy(VPAudioRenderer *r);

/* Parses `cmd_address` (the client's UpdateDataHeader) and renders exactly
 * sample_count stereo frames into the renderer's output buffer. Equivalent to
 * one apply_config plus one render_period; kept for probes and host tests. */
int vp_audio_renderer_update(VPAudioRenderer *r, VPGuestSvc *svc,
                             uint32_t cmd_address, uint32_t cmd_size,
                             VPAudioRenderStats *stats);

/* Snapshots the client's update command buffer into renderer-owned memory
 * (bounded by the existing 1 MiB command limit) and applies its configuration
 * exactly once: section shapes, resources, mixes, sinks, and the new-voice
 * reset. Later processing periods use this snapshot, so mutating the client's
 * command buffer after the call cannot change them. PCM wave addresses stay
 * actual guest 32-bit addresses and are revalidated on every period. Returns
 * 0 or a negative loader code; a failure leaves the previous snapshot
 * unusable for further periods. `stats` may be NULL. */
int vp_audio_renderer_apply_config(VPAudioRenderer *r, VPGuestSvc *svc,
                                   uint32_t cmd_address, uint32_t cmd_size,
                                   VPAudioRenderStats *stats);

/* Renders the next sample_count period from the applied snapshot. All voices
 * advance their real cursors and one-shot/new-slot edges are not re-latched;
 * the same snapshot can be rendered repeatedly without replaying retired
 * slots. Returns 0 or a negative loader code (no applied snapshot, missing
 * final mix/device sink). */
int vp_audio_renderer_render_period(VPAudioRenderer *r, VPGuestSvc *svc,
                                    VPAudioRenderStats *stats);

/* Pure demand planner shared by the service and the host tests: number of
 * additional periods that fit before `target` would be exceeded, bounded by
 * `cap`. Stops rather than overfilling; 0 when another period does not fit. */
uint32_t vp_audio_catchup_periods(uint32_t pending, uint32_t frames,
                                  uint32_t target, uint32_t cap);

/* Interleaved stereo s16, sample_count frames. Valid after a successful
 * period. */
const int16_t *vp_audio_renderer_output(const VPAudioRenderer *r,
                                        uint32_t *frames);

/* Actual cumulative source samples and retired wave slots for VoiceOutStatus. */
int vp_audio_renderer_voice_status(const VPAudioRenderer *r, uint32_t index,
                                  uint64_t *played, uint32_t *consumed);

#endif

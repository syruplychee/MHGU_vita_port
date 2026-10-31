/* M5: SceAudioOut sink for the modeled audren renderer.
 *
 * The guest renderer produces one sample_count block per RequestUpdate. The
 * Vita output port has its own granularity (a multiple of 64 frames), so the
 * sink keeps a small sample ring: the renderer appends frames, and one host
 * thread drains exactly `block_frames` frames per sceAudioOutOutput call on
 * the 48 kHz clock. This decouples the guest's update rate from the device's
 * port length without resampling either side.
 *
 * Only one port exists per process. The service layer hands ownership to the
 * first renderer that starts and refuses a second started renderer instead of
 * silently mixing two unrelated graphs into one device. */
#ifndef VP_AUDIO_SINK_H
#define VP_AUDIO_SINK_H

#include <stdint.h>
#include <stdio.h>

#define VP_AUDIO_SINK_RATE 48000u
/* Port length must be a multiple of 64 frames on Vita. 256 frames is
 * 5.333 ms, so 240-frame guest blocks are drained as a continuous stream. */
#define VP_AUDIO_SINK_BLOCK_FRAMES 256u
#define VP_AUDIO_SINK_RING_FRAMES 16384u /* 341 ms of stereo s16 */
/* Steady-state playback buffer. The service holds a RequestUpdate until the
 * ring holds at most this many frames, so the update rate tracks the 48 kHz
 * drain rate and the playback latency stays bounded instead of drifting up to
 * the full ring. M6-B12 raised the target from 3072 (64 ms) to 6144 frames
 * (128 ms): the observed run16 scheduling stall reached 110.356 ms
 * (two consecutive 55 ms windows), and 110.356 ms plus two 256-frame device
 * blocks (10.667 ms) is 121.023 ms, below 128 ms. The old 64 ms target
 * derived from the obsolete M5 21 ms stall measurement. This is a bounded
 * latency tradeoff, not evidence of original-hardware timing equivalence.
 * 6144 frames also absorbs the largest scheduler stall measured on this
 * emulator (21 ms). */
#define VP_AUDIO_SINK_PACE_FRAMES 6144u
/* Pre-roll to (almost) the full ring before the drain thread starts. The
 * first Client/Server updates of a session are slow: 39 ms for the first and
 * 20-25 ms for the next, against a 10-20 ms audio period, and the client needs
 * ~64-128 updates to settle at 48 kHz. Filling only one or two blocks before
 * starting the device clock drained the ring during that phase and padded
 * silence (measured: 52-114 partial blocks, 13k-25k silence frames). The
 * service does not apply the 3072-frame pace target until the drain has
 * started, so the client can fill the ring first; the drain then starts with
 * up to 330 ms queued and the pace target drains it down to 64 ms without ever
 * reaching zero. Two device blocks are left free so a whole guest block always
 * fits before the drain's first take. */
#define VP_AUDIO_SINK_PREFILL_FRAMES \
    (VP_AUDIO_SINK_RING_FRAMES - 2u * VP_AUDIO_SINK_BLOCK_FRAMES)
/* Fallback: if a client queues some audio but never reaches the pre-roll
 * level (e.g. a short one-shot renderer), start the drain after this long. */
#define VP_AUDIO_SINK_PREFILL_TIMEOUT_US 500000u

typedef struct {
    uint32_t open;              /* port opened successfully */
    uint32_t running;           /* drain thread running */
    uint32_t rate;
    uint32_t block_frames;
    uint64_t written_frames;    /* frames appended by the renderer */
    uint64_t output_frames;     /* frames handed to sceAudioOutOutput */
    uint64_t output_blocks;
    uint64_t underruns;         /* running blocks with < block_frames guest frames */
    uint64_t silence_frames;    /* silence padded into running blocks (0 = continuous) */
    uint64_t overruns;          /* writes dropped because the ring was full */
    uint64_t zero_crossings;    /* left-channel sign changes actually drained */
    uint32_t pending_frames;    /* current ring occupancy */
    uint32_t peak;              /* max |s16| over all drained blocks */
    uint32_t rest_min;          /* sceAudioOutGetRestSample before submit */
    uint32_t rest_max;
    uint64_t first_write_us;
    uint64_t last_write_us;
    uint64_t first_output_us;
    uint64_t last_output_us;
    uint64_t max_output_gap_us;
    uint32_t last_error;        /* negative SceAudioOut error, 0 if none */
} VPAudioSinkStats;

/* Starts the single sink. Returns 0 on success, a negative SCE error from
 * sceAudioOutOpenPort, or -1 if the sink is already running. The port is
 * opened on the calling thread; the drain thread is created afterwards. */
int vp_audio_sink_start(uint32_t rate, FILE *log);

/* Stops the drain thread, releases the port and clears all stats. */
void vp_audio_sink_stop(void);

int vp_audio_sink_running(void);

/* True once the drain thread has emitted at least one device block. The
 * service paces at the full ring before this point (pre-roll) and at
 * VP_AUDIO_SINK_PACE_FRAMES afterwards. */
int vp_audio_sink_started(void);

/* Appends interleaved stereo s16 frames. Returns the number accepted.
 * `dropped` (optional) receives the number of frames that did not fit. */
uint32_t vp_audio_sink_write(const int16_t *pcm, uint32_t frames,
                             uint32_t *dropped);

/* Frames currently buffered (read by the probe for pacing feedback). */
uint32_t vp_audio_sink_pending(void);

void vp_audio_sink_stats(VPAudioSinkStats *out);

#endif

#include "audio_sink.h"

#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

#define VP_SINK_THREAD_STACK 0x10000u
#define VP_SINK_THREAD_PRIO 0x10000040u

typedef struct {
    SceUID mutex;
    SceUID thread;
    int port;
    volatile uint32_t running;
    volatile uint32_t stop;
    /* Set under the mutex after the first device block is about to be
     * emitted. The service reads only this 32-bit flag, so the 64-bit
     * first_output_us cannot tear into its pacing decision. */
    volatile uint32_t started;
    int16_t ring[VP_AUDIO_SINK_RING_FRAMES * 2];
    uint32_t head; /* write position in frames */
    uint32_t tail; /* read position in frames */
    uint32_t count;
    VPAudioSinkStats stats;
    FILE *log;
} VPAudioSink;

static VPAudioSink sink = { .mutex = -1, .port = -1, .thread = -1 };

static void sink_log(const char *text, int a, int b) {
    if (sink.log) {
        fprintf(sink.log, "AUDIO_SINK %s a=%d b=%d\n", text, a, b);
        fflush(sink.log);
    }
    /* The emulated file layer loses its buffered tail when the emulator
     * aborts. Close-per-line keeps the last completed sink step on the host,
     * which is how an abort inside a platform call is localized. */
    FILE *progress = fopen("ux0:data/mhgu-vita-port/audio-sink-progress.log", "a");
    if (progress) {
        fprintf(progress, "AUDIO_SINK %s a=%d b=%d\n", text, a, b);
        fclose(progress);
    }
}

static uint32_t abs_s16(int16_t v) {
    int32_t x = v;
    return (uint32_t)(x < 0 ? -x : x);
}

/* Fills `out` with exactly `frames` interleaved stereo frames, consuming the
 * ring and padding an underrun with silence. Returns the number of frames
 * taken from the ring. */
static uint32_t sink_take(int16_t *out, uint32_t frames) {
    uint32_t taken = 0;
    sceKernelLockMutex(sink.mutex, 1, NULL);
    if (sink.count) {
        taken = sink.count < frames ? sink.count : frames;
        uint32_t first = VP_AUDIO_SINK_RING_FRAMES - sink.tail;
        if (first > taken) first = taken;
        memcpy(out, &sink.ring[sink.tail * 2], first * 2 * sizeof(int16_t));
        if (taken > first) {
            memcpy(out + first * 2, sink.ring,
                   (taken - first) * 2 * sizeof(int16_t));
        }
        sink.tail = (sink.tail + taken) % VP_AUDIO_SINK_RING_FRAMES;
        sink.count -= taken;
        sink.stats.pending_frames = sink.count;
    }
    sceKernelUnlockMutex(sink.mutex, 1);
    if (taken < frames) {
        memset(out + taken * 2, 0, (frames - taken) * 2 * sizeof(int16_t));
    }
    return taken;
}

static int sink_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    int16_t block[VP_AUDIO_SINK_BLOCK_FRAMES * 2];
    const uint32_t period_us =
        (VP_AUDIO_SINK_BLOCK_FRAMES * 1000000u) / sink.stats.rate;
    int prefill = 1;
    uint64_t next_due = 0;
    int16_t prev_left = 0;
    int have_prev = 0;

    while (1) {
        if (sink.stop) {
            uint32_t left;
            sceKernelLockMutex(sink.mutex, 1, NULL);
            left = sink.count;
            sceKernelUnlockMutex(sink.mutex, 1);
            /* On Stop the drain thread finishes the ring, but an incomplete
             * last block is padded rather than counted as a device underrun. */
            if (!left || prefill) break;
        }
        if (prefill) {
            uint32_t have;
            sceKernelLockMutex(sink.mutex, 1, NULL);
            have = sink.count;
            sceKernelUnlockMutex(sink.mutex, 1);
            int timeout = sink.stats.first_write_us
                && sceKernelGetProcessTimeWide() - sink.stats.first_write_us
                       > VP_AUDIO_SINK_PREFILL_TIMEOUT_US
                && have >= VP_AUDIO_SINK_BLOCK_FRAMES * 2u;
            if (have < VP_AUDIO_SINK_PREFILL_FRAMES && !timeout) {
                sceKernelDelayThread(500);
                continue;
            }
            if (have < VP_AUDIO_SINK_PREFILL_FRAMES)
                sink_log("prefill-timeout", (int)have, 0);
            else
                sink_log("prefill-done", (int)have, 0);
            prefill = 0;
            next_due = sceKernelGetProcessTimeWide();
            sceKernelLockMutex(sink.mutex, 1, NULL);
            sink.stats.first_output_us = next_due;
            sink.started = 1;
            sceKernelUnlockMutex(sink.mutex, 1);
        }

        uint64_t now = sceKernelGetProcessTimeWide();
        if (next_due > now) {
            uint64_t wait = next_due - now;
            if (wait > 20000) wait = 20000; /* never sleep on a stale clock */
            sceKernelDelayThread((SceUInt)wait);
            now = sceKernelGetProcessTimeWide();
        }

        uint32_t taken = sink_take(block, VP_AUDIO_SINK_BLOCK_FRAMES);
        uint32_t peak = 0;
        for (uint32_t i = 0; i < VP_AUDIO_SINK_BLOCK_FRAMES * 2; ++i) {
            uint32_t a = abs_s16(block[i]);
            if (a > peak) peak = a;
        }
        /* Sign changes on the left channel, over frames actually taken from
         * the ring (never over padded silence). The known 440 Hz probe tone
         * produces ~880 crossings per second; a silent or broken stream
         * produces zero. Carry the last sample across blocks. */
        for (uint32_t i = 0; i < taken; ++i) {
            int16_t cur = block[i * 2];
            if (have_prev
                && ((prev_left < 0 && cur >= 0) || (prev_left > 0 && cur <= 0)))
                ++sink.stats.zero_crossings;
            prev_left = cur;
            have_prev = 1;
        }
        if (peak > sink.stats.peak) sink.stats.peak = peak;
        /* A running block that does not get a full 256 guest frames is an
         * underrun: the device still plays it, with the tail padded by
         * silence. Only the Stop flush is exempt (it ends a finite stream). */
        if (taken < VP_AUDIO_SINK_BLOCK_FRAMES && !sink.stop) {
            ++sink.stats.underruns;
            sink.stats.silence_frames +=
                VP_AUDIO_SINK_BLOCK_FRAMES - taken;
            /* Diagnostic only. The submit index maps to wall time at
             * 48 kHz; a contiguous run of empty takes localizes a guest
             * stall. Uses the already-open log (no close-per-line) so the
             * report itself does not lengthen the stall it measures. */
            if (sink.stats.underruns <= 64u && sink.log) {
                fprintf(sink.log,
                        "AUDIO_SINK underrun out=%llu taken=%u pending=%u\n",
                        (unsigned long long)sink.stats.output_frames,
                        taken, sink.count);
                fflush(sink.log);
            }
        }

        int rest = sceAudioOutGetRestSample(sink.port);
        if (rest < 0) rest = 0;
        if (rest > (int)VP_AUDIO_SINK_BLOCK_FRAMES) rest = VP_AUDIO_SINK_BLOCK_FRAMES;
        if (sink.stats.output_blocks == 0 || (uint32_t)rest < sink.stats.rest_min)
            sink.stats.rest_min = (uint32_t)rest;
        if ((uint32_t)rest > sink.stats.rest_max) sink.stats.rest_max = (uint32_t)rest;

        if (sink.stats.last_output_us) {
            uint64_t gap = now - sink.stats.last_output_us;
            if (gap > sink.stats.max_output_gap_us)
                sink.stats.max_output_gap_us = gap;
        }
        int rc = sceAudioOutOutput(sink.port, block);
        if (rc < 0 && !sink.stats.last_error)
            sink.stats.last_error = (uint32_t)rc;
        ++sink.stats.output_blocks;
        sink.stats.output_frames += VP_AUDIO_SINK_BLOCK_FRAMES;
        sink.stats.last_output_us = sceKernelGetProcessTimeWide();

        next_due += period_us;
        now = sceKernelGetProcessTimeWide();
        if (next_due + (uint64_t)period_us * 4u < now) {
            /* The device call fell far behind (scheduler stall or a blocked
             * writer). Re-anchor instead of emitting a burst of catch-up
             * blocks that would overrun the host queue. */
            next_due = now + period_us;
        }
    }
    return 0;
}

int vp_audio_sink_start(uint32_t rate, FILE *log) {
    if (sink.running) return -1;
    memset(&sink, 0, sizeof(sink));
    sink.log = log;
    sink.port = -1;
    sink.mutex = sceKernelCreateMutex("vp-audio-sink", SCE_KERNEL_MUTEX_ATTR_RECURSIVE,
                                      0, NULL);
    if (sink.mutex < 0) return (int)sink.mutex;
    sink.stats.rate = rate;
    sink.stats.block_frames = VP_AUDIO_SINK_BLOCK_FRAMES;
    if (rate != VP_AUDIO_SINK_RATE) {
        /* MAIN is fixed at 48 kHz. Refuse rather than silently changing the
         * renderer's clock. */
        sink_log("unsupported-rate", (int)rate, 0);
        sceKernelDeleteMutex(sink.mutex);
        sink.mutex = -1;
        return -2;
    }
    sink_log("opening-port", (int)rate, (int)VP_AUDIO_SINK_BLOCK_FRAMES);
    sink.port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN,
                                    (int)VP_AUDIO_SINK_BLOCK_FRAMES,
                                    (int)rate, SCE_AUDIO_OUT_MODE_STEREO);
    sink_log("opened-port", sink.port, 0);
    if (sink.port < 0) {
        sink_log("open-failed", sink.port, (int)rate);
        sceKernelDeleteMutex(sink.mutex);
        sink.mutex = -1;
        return sink.port;
    }
    sink.stats.open = 1;
    sink_log("creating-thread", (int)VP_SINK_THREAD_STACK, (int)VP_SINK_THREAD_PRIO);
    sink.thread = sceKernelCreateThread("vp-audio-sink", sink_thread,
                                        VP_SINK_THREAD_PRIO,
                                        VP_SINK_THREAD_STACK, 0,
                                        SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    sink_log("created-thread", sink.thread, 0);
    if (sink.thread < 0) {
        sink_log("thread-failed", sink.thread, 0);
        sceAudioOutReleasePort(sink.port);
        sink.port = -1;
        sceKernelDeleteMutex(sink.mutex);
        sink.mutex = -1;
        return sink.thread;
    }
    sink.running = 1;
    sink_log("starting-thread", sink.thread, 0);
    int rc = sceKernelStartThread(sink.thread, 0, NULL);
    if (rc < 0) {
        sink.running = 0;
        sink.stop = 1;
        sceAudioOutReleasePort(sink.port);
        sink.port = -1;
        sceKernelDeleteMutex(sink.mutex);
        sink.mutex = -1;
        return rc;
    }
    sink_log("started", (int)rate, (int)VP_AUDIO_SINK_BLOCK_FRAMES);
    return 0;
}

void vp_audio_sink_stop(void) {
    if (sink.mutex < 0) return;
    if (sink.running) {
        sink.stop = 1;
        if (sink.thread >= 0) {
            /* The drain thread can be inside sceAudioOutOutput for one port
             * period; wait for it before releasing the port underneath it. */
            sceKernelWaitThreadEnd(sink.thread, NULL, NULL);
            sceKernelDeleteThread(sink.thread);
        }
        sink.running = 0;
    }
    if (sink.port >= 0) {
        sceAudioOutReleasePort(sink.port);
        sink.port = -1;
    }
    sink.stats.open = 0;
    sink.stats.running = 0;
    sceKernelDeleteMutex(sink.mutex);
    sink.mutex = -1;
}

int vp_audio_sink_running(void) {
    return sink.running != 0;
}

int vp_audio_sink_started(void) {
    return sink.running != 0 && sink.started != 0;
}

uint32_t vp_audio_sink_write(const int16_t *pcm, uint32_t frames, uint32_t *dropped) {
    uint32_t accepted = 0;
    if (dropped) *dropped = 0;
    if (!pcm || !frames || sink.mutex < 0) return 0;
    sceKernelLockMutex(sink.mutex, 1, NULL);
    uint32_t free_frames = VP_AUDIO_SINK_RING_FRAMES - sink.count;
    accepted = frames < free_frames ? frames : free_frames;
    if (accepted) {
        uint32_t first = VP_AUDIO_SINK_RING_FRAMES - sink.head;
        if (first > accepted) first = accepted;
        memcpy(&sink.ring[sink.head * 2], pcm, first * 2 * sizeof(int16_t));
        if (accepted > first) {
            memcpy(sink.ring, pcm + first * 2,
                   (accepted - first) * 2 * sizeof(int16_t));
        }
        sink.head = (sink.head + accepted) % VP_AUDIO_SINK_RING_FRAMES;
        sink.count += accepted;
        sink.stats.written_frames += accepted;
        sink.stats.pending_frames = sink.count;
        uint64_t now = sceKernelGetProcessTimeWide();
        if (!sink.stats.first_write_us) sink.stats.first_write_us = now;
        sink.stats.last_write_us = now;
    }
    sceKernelUnlockMutex(sink.mutex, 1);
    if (accepted < frames) {
        uint32_t lost = frames - accepted;
        sink.stats.overruns += lost;
        if (dropped) *dropped = lost;
    }
    return accepted;
}

uint32_t vp_audio_sink_pending(void) {
    uint32_t pending;
    if (sink.mutex < 0) return 0;
    sceKernelLockMutex(sink.mutex, 1, NULL);
    pending = sink.count;
    sceKernelUnlockMutex(sink.mutex, 1);
    return pending;
}

void vp_audio_sink_stats(VPAudioSinkStats *out) {
    if (!out) return;
    if (sink.mutex >= 0) sceKernelLockMutex(sink.mutex, 1, NULL);
    *out = sink.stats;
    out->running = sink.running;
    out->open = sink.stats.open;
    out->pending_frames = sink.count;
    if (sink.mutex >= 0) sceKernelUnlockMutex(sink.mutex, 1);
}

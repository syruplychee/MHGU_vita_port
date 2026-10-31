/* M5 assertion harness ABI between the loader and the guest-side ARM probe.
 *
 * The loader fills this structure in guest-mapped memory, resolves the
 * locked SDK's audio client entry points and starts one SDK thread whose
 * entry is vp_audio_probe_entry(). The probe runs entirely with the original
 * SDK client; the loader only supplies buffers, the PCM test signal and the
 * function addresses. Field offsets are part of the probe object's ABI, so
 * every field is a fixed-width integer and the structure is versioned. */
#ifndef VP_AUDIO_PROBE_H
#define VP_AUDIO_PROBE_H

#include <stdint.h>

#define VP_AUDIO_PROBE_MAGIC 0x5035414Du /* "M5AP" */
#define VP_AUDIO_PROBE_VERSION 4u
#define VP_AUDIO_PROBE_OK 0x100u
#define VP_AUDIO_PROBE_PARAM_BYTES 0x3Cu
#define VP_AUDIO_PROBE_CONFIG_BYTES 0x58u
#define VP_AUDIO_PROBE_PCM_RATE 48000u
/* Frames rendered per RequestUpdate (the renderer parameter's sampleCount).
 * 960 = 20 ms at 48 kHz. The first RequestUpdate of an emulated session
 * measures 20-39 ms and the steady-state round trip 6-13 ms; a 20 ms period
 * gives the client two round trips of headroom when the sink is pacing it. */
#define VP_AUDIO_PROBE_FRAMES 960u
/* One channel: VoiceInfoManager in this locked SDK allocates one 0x70-byte
 * channel resource per voice channel out of a pool sized by the renderer
 * parameter, and the probe's single-voice configuration has one resource. A
 * stereo voice made AcquireVoiceSlot return false (0), which the probe
 * initially misread as success. */
#define VP_AUDIO_PROBE_PCM_CHANNELS 1u

typedef struct {
    uint32_t magic;
    uint32_t version;

    /* SDK client functions, module-relative guest addresses. */
    uint32_t fn_init_param;       /* void (*)(AudioRendererParameter*) */
    uint32_t fn_get_work_size;    /* size_t (*)(const AudioRendererParameter&) */
    uint32_t fn_get_cfg_size;     /* size_t (*)(const AudioRendererParameter&) */
    uint32_t fn_init_cfg;         /* Result (*)(Config*, const Param&, void*, size_t) */
    uint32_t fn_open;             /* Result (*)(Handle*, const Param&, void*, size_t) */
    uint32_t fn_start;            /* Result (*)(Handle) */
    uint32_t fn_stop;             /* Result (*)(Handle) */
    uint32_t fn_update;           /* Result (*)(Handle, const Config*) */
    uint32_t fn_acquire_voice;    /* Result (*)(Config*, Voice*, s32, s32, u32, s32, const void*, size_t) */
    uint32_t fn_set_voice_dest;   /* void (*)(Config*, Voice*, FinalMix*) */
    /* void (*)(Voice*, FinalMix*, float, int voice_channel, int mix_channel).
     * The locked client stores this gain in the per-voice-channel resource
     * that is serialized at voice-info +0x140.. and copied by the server into
     * the ADSP input; AcquireVoiceSlot zeroes it, so a voice that is never
     * given a mix volume is silent by construction. */
    uint32_t fn_set_voice_mix_volume;
    uint32_t fn_set_play_state;   /* void (*)(Voice*, u32) */
    uint32_t fn_acquire_final_mix;/* bool (*)(Config*, FinalMix*, s32) */
    uint32_t fn_add_sink;         /* Result (*)(Config*, Sink*, FinalMix*, const s8*, s32, const char*) */

    /* Guest buffers and opaque client objects. */
    uint32_t param;
    uint32_t config;
    uint32_t handle;              /* 8 bytes (two pointers) */
    uint32_t voice;               /* 4 bytes */
    uint32_t final_mix;           /* 4 bytes */
    uint32_t sink;                /* 4 bytes */
    uint32_t cfg_buffer;
    uint32_t cfg_capacity;
    uint32_t work_buffer;
    uint32_t work_capacity;
    uint32_t pcm;
    uint32_t pcm_bytes;
    uint32_t pcm_frames;
    uint32_t update_count;

    /* Service -> probe diagnostic: interleaved stereo frames buffered by the
     * SceAudioOut sink at the last RequestUpdate. Pacing is done by the
     * service's ring backpressure, not by the probe. */
    volatile uint32_t sink_pending;
    /* Loader -> probe: VI frames presented by the title so far, summed over
     * its vi:obj sessions and refreshed once per second. The probe holds its
     * first RequestUpdate until this reaches VP_AUDIO_PROBE_START_PRESENTS.
     * The emulator executes the title's first GPU submissions and pipeline
     * setup synchronously on the CPU thread and blocks every guest thread for
     * 300-400 ms at a time during that phase (measured in probes 94/95). That
     * stall is outside the audio path; starting the measured stream after it
     * keeps the sink's 64 ms buffer as the steady-state design instead of
     * inflating playback latency to cover a one-time warm-up. */
    volatile uint32_t title_presents;
    /* Bounded wait: if the title never presents (no VI consumer on this
     * platform), the probe starts anyway after this long. */
#define VP_AUDIO_PROBE_START_WAIT_US 15000000u
#define VP_AUDIO_PROBE_START_PRESENTS 32u
    uint32_t reserved0;

    /* Probe -> loader results. */
    volatile uint32_t stage;
    volatile uint32_t done;
    uint32_t error_stage;
    uint32_t error_result;
    uint32_t work_size;
    uint32_t cfg_size;
    uint32_t updates_done;
    uint32_t updates_failed;
    uint32_t last_update_result;
    uint32_t reserved1;

    /* Loader callbacks called as ordinary ARM functions. The probe runs as
     * native guest code, so a raw `svc` would be interpreted by the Vita
     * kernel as an import stub (its NID is the following instruction), not by
     * the loader's guest-SVC bridge. These callbacks keep diagnostics on the
     * loader side; fn_sleep is still filled by the loader but unused since
     * the service's backpressure paces updates. */
    uint32_t fn_mark;   /* void (*)(uint32_t log, uint32_t stage, uint32_t arg) */
    uint32_t fn_sleep;  /* void (*)(uint32_t microseconds); unused */
    uint32_t log_ptr;   /* FILE* for fn_mark, as a guest address */
} VPAudioProbeArgs;

/* ARM function compiled into the native loader ELF and executed on an SDK
 * thread created by nn::os::CreateThread. Returns 0 after writing `done`. */
int vp_audio_probe_entry(void *args);

#endif

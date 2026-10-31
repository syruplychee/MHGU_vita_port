/* M5 assertion probe: the locked SDK's own audio client.
 *
 * This object is compiled for ARM and executed on a thread created through
 * the SDK's nn::os::CreateThread, so the thread has the SDK's TLS, stack and
 * scheduler identity. It calls the public nn::audio client API exactly as a
 * game would:
 *   InitializeAudioRendererParameter -> GetAudioRenderer{Work,Config}BufferSize
 *   -> InitializeAudioRendererConfig -> AcquireFinalMix -> AcquireVoiceSlot
 *   -> SetVoiceDestination -> SetVoiceMixVolume -> AddDeviceSink
 *   -> SetVoicePlayState -> OpenAudioRenderer -> StartAudioRenderer
 *   -> RequestUpdateAudioRenderer* -> StopAudioRenderer
 * The only loader-supplied data is the PCM test signal and memory.
 *
 * Self-authored; no SDK header or reference-runtime source is included. */
#include "audio_probe.h"

#define P(dst, off) (*(volatile uint32_t *)(uintptr_t)((dst) + (off)))
#define P8(dst, off) (*(volatile uint8_t *)(uintptr_t)((dst) + (off)))

typedef struct { uint32_t _0, _8; } ProbeHandle;

typedef void (*FnInitParam)(void *param);
typedef uint32_t (*FnGetSize)(const void *param);
typedef uint32_t (*FnInitConfig)(void *config, const void *param, void *buffer,
                                 uint32_t size);
typedef uint32_t (*FnOpen)(void *handle, const void *param, void *work);
typedef uint32_t (*FnOpenFull)(void *handle, const void *param, void *work,
                               uint32_t size);
typedef uint32_t (*FnHandle)(ProbeHandle handle);
typedef uint32_t (*FnUpdate)(ProbeHandle handle, const void *config);
typedef uint32_t (*FnAcquireVoice)(void *config, void *voice, int32_t sample_rate,
                                   int32_t channel_count, uint32_t sample_format,
                                   int32_t priority, const void *buffer,
                                   uint32_t buffer_size);
typedef void (*FnVoiceDest)(void *config, void *voice, void *mix);
typedef void (*FnVoiceMixVolume)(void *voice, void *mix, float volume,
                                 int32_t voice_channel, int32_t mix_channel);
typedef void (*FnPlayState)(void *voice, uint32_t state);
typedef void (*FnSleep)(uint32_t microseconds);
typedef uint32_t (*FnAcquireFinalMix)(void *config, void *mix, int32_t buffer_count);
typedef uint32_t (*FnAddSink)(void *config, void *sink, void *mix,
                              const int8_t *inputs, int32_t input_count,
                              const char *device_name);

static VPAudioProbeArgs *probe_args;

/* Loader-owned progress marker. It writes through to the loader log
 * synchronously, so an emulator abort still leaves the last completed stage
 * on record. */
static void probe_mark(uint32_t stage, uint32_t arg) {
    if (probe_args && probe_args->fn_mark)
        ((void (*)(uint32_t, uint32_t, uint32_t))(uintptr_t)probe_args->fn_mark)(
            probe_args->log_ptr, stage, arg);
}

static int probe_fail(VPAudioProbeArgs *a, uint32_t stage, uint32_t result) {
    a->stage = stage;
    a->error_stage = stage;
    a->error_result = result;
    a->done = 1;
    return (int)stage;
}

static void zero_bytes(uint32_t address, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; ++i) P8(address, i) = 0;
}

int vp_audio_probe_entry(void *arg) {
    VPAudioProbeArgs *a = (VPAudioProbeArgs *)arg;
    if (!a || a->magic != VP_AUDIO_PROBE_MAGIC || a->version != VP_AUDIO_PROBE_VERSION)
        return -1;
    probe_args = a;
    probe_mark(0, 1);

    a->stage = 1;
    probe_mark(1, a->magic);
    zero_bytes(a->param, VP_AUDIO_PROBE_PARAM_BYTES);
    ((FnInitParam)(uintptr_t)a->fn_init_param)((void *)(uintptr_t)a->param);
    probe_mark(0x11, 0);
    /* A stereo final mix with one stereo voice and one device sink. The
     * defaults are 1/0/1/1; raising mixBufferCount to 2 matches the two
     * buffers AcquireFinalMix allocates below. */
    P(a->param, 0x08) = 2; /* mixBufferCount */
    P(a->param, 0x0c) = 0; /* subMixCount */
    P(a->param, 0x10) = 1; /* voiceCount */
    P(a->param, 0x14) = 1; /* sinkCount */
    P(a->param, 0x18) = 0; /* effectCount */
    P(a->param, 0x1c) = 0; /* performanceFrameCount */
    P(a->param, 0x24) = 0; /* splitterCount */
    P(a->param, 0x28) = 0; /* splitterSendChannelCount */
    /* 960-frame updates (20 ms at 48 kHz). The emulated client's synchronous
     * RequestUpdate round trip measures 6-13 ms after warm-up (20-39 ms for
     * the first calls), so a 240-frame (5 ms) or 480-frame (10 ms) period
     * cannot sustain the device clock during startup and the sink underruns.
     * The service renders exactly the declared sample count. */
    P(a->param, 0x04) = VP_AUDIO_PROBE_FRAMES;

    a->stage = 2;
    probe_mark(2, 0);
    uint32_t work = ((FnGetSize)(uintptr_t)a->fn_get_work_size)((const void *)(uintptr_t)a->param);
    a->work_size = work;
    probe_mark(0x12, work);
    if (!work) return probe_fail(a, 2, 1);
    if (work > a->work_capacity) return probe_fail(a, 2, work);

    a->stage = 3;
    probe_mark(3, 0);
    uint32_t cfg = ((FnGetSize)(uintptr_t)a->fn_get_cfg_size)((const void *)(uintptr_t)a->param);
    a->cfg_size = cfg;
    probe_mark(0x13, cfg);
    if (!cfg) return probe_fail(a, 3, 1);
    if (cfg > a->cfg_capacity) return probe_fail(a, 3, cfg);

    a->stage = 4;
    probe_mark(4, 0);
    uint32_t rc = ((FnInitConfig)(uintptr_t)a->fn_init_cfg)(
        (void *)(uintptr_t)a->config, (const void *)(uintptr_t)a->param,
        (void *)(uintptr_t)a->cfg_buffer, cfg);
    probe_mark(0x14, rc);
    if (rc) return probe_fail(a, 4, rc);

    a->stage = 5;
    probe_mark(5, 0);
    rc = ((FnAcquireFinalMix)(uintptr_t)a->fn_acquire_final_mix)(
            (void *)(uintptr_t)a->config, (void *)(uintptr_t)a->final_mix, 2) ? 0u : 1u;
    probe_mark(0x15, rc);
    if (rc) return probe_fail(a, 5, 1);

    a->stage = 6;
    probe_mark(6, 0);
    rc = ((FnAcquireVoice)(uintptr_t)a->fn_acquire_voice)(
        (void *)(uintptr_t)a->config, (void *)(uintptr_t)a->voice,
        (int32_t)VP_AUDIO_PROBE_PCM_RATE, (int32_t)VP_AUDIO_PROBE_PCM_CHANNELS,
        2u /* SampleFormat_PcmInt16 */, 0, (const void *)(uintptr_t)a->pcm,
        a->pcm_bytes);
    probe_mark(0x16, rc);
    /* AcquireVoiceSlot returns bool: true (1) when the slot was acquired,
     * unlike the Result-returning calls. A false return previously slipped
     * through and left the voice pointer null while the probe carried on. */
    if (rc != 1u) return probe_fail(a, 6, rc ? rc : 0xE0000006u);
    /* VoiceInfoManager serializes a voice into the renderer input only when
     * its internal used flag is set. Record the acquired block's state so a
     * zero input section can be attributed to acquisition or to the
     * serializer rather than guessed at. */
    {
        uint32_t vi = P(a->voice, 0x00);
        probe_mark(0x2E, vi);
        if (vi >= 0x80000000u) {
            probe_mark(0x2F, P(vi, 0x24));
            probe_mark(0x30, P(vi, 0x28)); /* bytes 28..2b: flags/priority */
            probe_mark(0x31, P(vi, 0x2c)); /* sample rate */
            probe_mark(0x32, P(vi, 0x30)); /* sample format */
            probe_mark(0x33, P(vi, 0x38)); /* channel count */
        }
    }

    a->stage = 7;
    probe_mark(7, 0);
    ((FnVoiceDest)(uintptr_t)a->fn_set_voice_dest)(
        (void *)(uintptr_t)a->config, (void *)(uintptr_t)a->voice,
        (void *)(uintptr_t)a->final_mix);
    /* AcquireVoiceSlot zero-fills the voice-channel resource's 24 mix
     * volumes (locked SDK memset at voice-info resource +0xc). Without an
     * explicit SetVoiceMixVolume the voice is connected but contributes
     * zero gain, so route the mono probe voice into both final-mix buffers
     * at unity. The locked signature is (voice, mix, volume, voice channel,
     * mix channel) and stores mix channel m at resource +0xc + m*4, which
     * the client serializes to input resource offset +0x04 + m*4. */
    ((FnVoiceMixVolume)(uintptr_t)a->fn_set_voice_mix_volume)(
        (void *)(uintptr_t)a->voice, (void *)(uintptr_t)a->final_mix,
        1.0f, 0, 0);
    ((FnVoiceMixVolume)(uintptr_t)a->fn_set_voice_mix_volume)(
        (void *)(uintptr_t)a->voice, (void *)(uintptr_t)a->final_mix,
        1.0f, 0, 1);
    probe_mark(0x1D, 0);
    int8_t inputs[2] = {0, 1};
    rc = ((FnAddSink)(uintptr_t)a->fn_add_sink)(
        (void *)(uintptr_t)a->config, (void *)(uintptr_t)a->sink,
        (void *)(uintptr_t)a->final_mix, inputs, 2, "AudioTvOutput");
    probe_mark(0x17, rc);
    if (rc) return probe_fail(a, 7, rc);
    ((FnPlayState)(uintptr_t)a->fn_set_play_state)((void *)(uintptr_t)a->voice, 0u);
    {
        uint32_t vi = P(a->voice, 0x00);
        if (vi >= 0x80000000u) probe_mark(0x34, P(vi, 0x28));
    }

    a->stage = 8;
    probe_mark(8, 0);
    rc = ((FnOpenFull)(uintptr_t)a->fn_open)(
        (void *)(uintptr_t)a->handle, (const void *)(uintptr_t)a->param,
        (void *)(uintptr_t)a->work_buffer, work);
    probe_mark(0x18, rc);
    if (rc) return probe_fail(a, 8, rc);

    a->stage = 9;
    ProbeHandle handle;
    handle._0 = P(a->handle, 0x00);
    handle._8 = P(a->handle, 0x04);
    probe_mark(0x19, handle._0);
    /* The client validates RequestUpdate's status header against the buffers
     * InitializeAudioRendererConfig installed in the config object. Record
     * that pointer/size pair so the service's out-buffer descriptors can be
     * matched against what the client will read. */
    probe_mark(0x2A, P(a->config, 0x30));
    probe_mark(0x2B, P(a->config, 0x34));
    probe_mark(0x2C, P(a->config, 0x38));
    probe_mark(0x2D, P(a->config, 0x3c));
    /* The handle points at the client object whose virtual Start/Stop/Update
     * methods the SDK dispatches through. Record both so a fault inside the
     * first call can be resolved against the locked SDK image. */
    if (handle._0 >= 0x80000000u) {
        uint32_t vt = P(handle._0, 0x00);
        probe_mark(0x26, vt);
        if (vt >= 0x80000000u) probe_mark(0x27, P(vt, 0x24));
    }
    probe_mark(9, 0);
    rc = ((FnHandle)(uintptr_t)a->fn_start)(handle);
    probe_mark(0x1A, rc);
    if (rc) return probe_fail(a, 9, rc);

    a->stage = 10;
    probe_mark(10, 0);
    /* The sink is started and owns the port, but is still waiting for its
     * pre-roll (its fallback timeout needs a first write, so an idle sink
     * neither drains nor pads). Hold the first RequestUpdate until the title
     * has presented VP_AUDIO_PROBE_START_PRESENTS frames: its first GPU
     * submissions and pipeline setup block the emulator's CPU thread for
     * hundreds of milliseconds, and that one-time phase would otherwise
     * consume the sink's 64 ms steady buffer. The wait is bounded; the
     * title's present counter is refreshed once per second by the loader. */
    {
        uint32_t waited_us = 0;
        while (a->title_presents < VP_AUDIO_PROBE_START_PRESENTS
               && waited_us < VP_AUDIO_PROBE_START_WAIT_US) {
            ((FnSleep)(uintptr_t)a->fn_sleep)(5000u);
            waited_us += 5000u;
        }
        probe_mark(0x1E, a->title_presents);
        probe_mark(0x1F, waited_us);
    }
    for (uint32_t i = 0; i < a->update_count; ++i) {
        rc = ((FnUpdate)(uintptr_t)a->fn_update)(handle,
                                                 (const void *)(uintptr_t)a->config);
        if ((i & 63u) == 0u) probe_mark(0x1B, i);
        if (rc) {
            ++a->updates_failed;
            a->last_update_result = rc;
            break;
        }
        ++a->updates_done;
        if (i == 0u) {
            /* Section order in this locked client's input buffer: behaviour,
             * memory pools, voice-channel resources (0x70), then voices
             * (0x170). Verify the serializer wrote both. */
            uint32_t in = P(a->config, 0x28);
            uint32_t res = in + 0x40u + P(in, 0x04u) + P(in, 0x08u);
            uint32_t vb = res + P(in, 0x10u);
            probe_mark(0x35, in);
            probe_mark(0x36, P(res, 0x64u));
            probe_mark(0x37, P(vb, 0x08u));
            probe_mark(0x38, P(vb, 0x0cu));
            probe_mark(0x39, P(vb, 0x00u));
            /* Unit mix volumes for both final-mix channels prove the
             * SetVoiceMixVolume calls above reached the serialized
             * voice-channel resource (0x3f800000 = 1.0f). */
            probe_mark(0x3A, P(res, 0x04u));
            probe_mark(0x3B, P(res, 0x08u));
        }
        /* No client-side pacing. The service holds RequestUpdate until the
         * 48 kHz sink has room for this sample_count block, so the update
         * rate is the device rate by construction. Sleeps here would only
         * keep the ring shallow and starve the drain thread. sink_pending
         * remains a diagnostic field, no longer a pacing input. */
    }

    a->stage = 11;
    probe_mark(11, 0);
    rc = ((FnHandle)(uintptr_t)a->fn_stop)(handle);
    probe_mark(0x1C, rc);
    if (rc) return probe_fail(a, 11, rc);
    if (!a->updates_done) return probe_fail(a, 10, a->last_update_result);
    if (a->updates_failed) {
        a->stage = 0xFEu;
        a->error_stage = 10;
        a->error_result = a->last_update_result;
        a->done = 1;
        return 0xFE;
    }
    a->stage = VP_AUDIO_PROBE_OK;
    a->done = 1;
    return 0;
}

#include "audio_render.h"
#include "audio_downmix.h"
#include "audio_ipc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AR_MAX_CHANNELS 6u
#define AR_MAX_MIX_BUFFERS 24u
#define AR_UNUSED_MIX 0x7fffffffu
#define AR_FINAL_MIX_ID 0u
#define AR_INVALID_DISTANCE 0x7fffffffu
#define AR_ADPCM_SAMPLES_PER_FRAME 14u
#define AR_ADPCM_BYTES_PER_FRAME 8u
#define AR_ADPCM_MAX_PCM_FRAMES 0x200000u

typedef struct {
    uint32_t in_use;
    uint32_t play_state;
    uint32_t format;
    uint32_t sample_rate;
    uint32_t channels;
    float volume;
    float pitch;
    uint32_t mix_id;
    uint32_t wave_index;
    uint32_t wave_valid[4];
    uint32_t wave_sent[4];
    uint64_t played_samples;
    uint32_t wave_consumed;
    uint32_t previous_play_state;
    uint32_t playback_initialized;
    uint64_t played_q16;
    uint64_t cursor_q16;   /* frame position in Q16, persists over updates */
    uint32_t playing;
    uint32_t reported;
    uint32_t route_reported;
    uint32_t pcm_address;
    uint32_t pcm_size;
    uint32_t pcm_start;
    uint32_t pcm_end;
    uint32_t pcm_wave_index;
    int16_t *adpcm_pcm;
    uint32_t adpcm_pcm_frames;
    uint32_t adpcm_address;
    uint32_t adpcm_size;
    uint32_t adpcm_start;
    uint32_t adpcm_end;
    uint32_t adpcm_context_address;
    uint32_t adpcm_context_size;
    uint32_t adpcm_extra_address;
    uint32_t adpcm_extra_size;
    uint32_t adpcm_reported;
    uint32_t adpcm_failed;
} ArVoice;

typedef struct {
    uint32_t in_use;
    float volume[AR_MAX_MIX_BUFFERS];
} ArResource;

typedef struct {
    uint32_t in_use;
    uint32_t buffer_count;
    uint32_t offset;
    uint32_t distance;
    float volume;
    int32_t mix_id;
    int32_t dest_mix;
    float matrix[AR_MAX_MIX_BUFFERS][AR_MAX_MIX_BUFFERS];
} ArMix;

struct VPAudioRenderer {
    uint32_t rate;
    uint32_t frames;
    ArVoice *voices;
    uint32_t voice_cap;
    ArResource *resources;
    uint32_t resource_cap;
    ArMix *mixes;
    uint32_t mix_cap;
    uint32_t *sort_idx;
    uint32_t sort_cap;
    float *samples;          /* sample_cap * frames floats */
    uint32_t sample_cap;
    int16_t *out;            /* frames * 2 interleaved */
    uint8_t *covered;        /* frames of the current period backed by a voice */
    uint64_t updates;
    uint64_t frames_rendered;
    uint32_t splitter_reported_size;
    VPAudioLogCadence splitter_log;
    /* M6-B11 renderer-owned snapshot of the last applied RequestUpdate. Every
     * read of update structure after apply uses these bytes, never the client
     * command buffer. PCM payload addresses inside the snapshot remain guest
     * 32-bit addresses and are revalidated per period. */
    uint8_t *config;
    uint32_t config_cap;
    uint32_t config_size;
    uint32_t config_guest_address;
    uint32_t voice_count;
    uint32_t resource_count;
    uint32_t mix_count;
    uint32_t sink_count;
    uint32_t effect_count;
    uint32_t sample_buffers;
    uint32_t p_voices_off;
    uint32_t p_sinks_off;
    uint32_t final_mix_index;
    uint32_t final_buffer_count;
    uint32_t config_valid;
};

static uint8_t rd8(uint32_t a) { return *(const uint8_t *)(uintptr_t)a; }
static uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static float rdf32(uint32_t a) { float v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }

/* Native reads for the renderer-owned snapshot. These accept a host pointer
 * so the same code is correct on the 32-bit target and in a 64-bit host
 * regression; owned memory is never passed through vp_guest_svc_readable. */
static uint8_t nrd8(const uint8_t *p) { return *p; }
static uint16_t nrd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t nrd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int32_t nrds32(const uint8_t *p) { return (int32_t)nrd32(p); }
static uint64_t nrd64(const uint8_t *p) {
    uint64_t v; memcpy(&v, p, 8); return v;
}
static float nrdf32(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

static int realloc_array(void **ptr, uint32_t *cap, uint32_t want, uint32_t elem) {
    if (want <= *cap) return 0;
    void *next = realloc(*ptr, (size_t)want * elem);
    if (!next) return -1;
    memset((uint8_t *)next + (size_t)(*cap) * elem, 0,
           (size_t)(want - *cap) * elem);
    *ptr = next;
    *cap = want;
    return 0;
}

VPAudioRenderer *vp_audio_renderer_create(uint32_t sample_rate,
                                          uint32_t sample_count) {
    if (!sample_rate || !sample_count || sample_count > VP_AR_MAX_FRAMES) return NULL;
    VPAudioRenderer *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->rate = sample_rate;
    r->frames = sample_count;
    r->out = calloc((size_t)sample_count * 2, sizeof(int16_t));
    if (!r->out) {
        free(r);
        return NULL;
    }
    r->covered = calloc(sample_count, 1);
    if (!r->covered) {
        free(r->out);
        free(r);
        return NULL;
    }
    return r;
}

void vp_audio_renderer_destroy(VPAudioRenderer *r) {
    if (!r) return;
    for (uint32_t i = 0; i < r->voice_cap; ++i)
        free(r->voices[i].adpcm_pcm);
    free(r->voices);
    free(r->resources);
    free(r->mixes);
    free(r->sort_idx);
    free(r->samples);
    free(r->out);
    free(r->covered);
    free(r->config);
    free(r);
}

const int16_t *vp_audio_renderer_output(const VPAudioRenderer *r,
                                        uint32_t *frames) {
    if (frames) *frames = r ? r->frames : 0;
    return r ? r->out : NULL;
}

/* Distance to the final mix by following dst_mix_id. The reviewed server
 * indexes mix_infos by mix_id, so a mix_id outside the array is invalid. */
static uint32_t mix_distance(const ArMix *mixes, uint32_t count, uint32_t start) {
    uint32_t id = start;
    for (uint32_t step = 0; step <= count; ++step) {
        if (id == AR_FINAL_MIX_ID) return step;
        if (id >= count || !mixes[id].in_use) return AR_INVALID_DISTANCE;
        int32_t dest = mixes[id].dest_mix;
        if (dest < 0 || (uint32_t)dest >= count || (uint32_t)dest == id)
            return AR_INVALID_DISTANCE;
        id = (uint32_t)dest;
    }
    return AR_INVALID_DISTANCE;
}

/* Assigns mix buffer offsets farthest-from-final first, exactly as the
 * reviewed MixContext::CalcMixBufferOffset does after sorting by distance. */
static int assign_mix_offsets(VPAudioRenderer *r, uint32_t mix_count,
                              uint32_t *total_buffers) {
    for (uint32_t i = 0; i < mix_count; ++i) {
        r->mixes[i].distance = r->mixes[i].in_use
            ? mix_distance(r->mixes, mix_count, r->mixes[i].mix_id)
            : AR_INVALID_DISTANCE;
        r->sort_idx[i] = i;
    }
    /* Stable insertion sort by distance descending; equal-distance mixes
     * keep the client's array order. */
    for (uint32_t i = 1; i < mix_count; ++i) {
        uint32_t v = r->sort_idx[i];
        uint32_t d = r->mixes[v].distance;
        uint32_t j = i;
        while (j && r->mixes[r->sort_idx[j - 1]].distance < d) {
            r->sort_idx[j] = r->sort_idx[j - 1];
            --j;
        }
        r->sort_idx[j] = v;
    }
    uint32_t offset = 0;
    for (uint32_t i = 0; i < mix_count; ++i) {
        uint32_t idx = r->sort_idx[i];
        ArMix *m = &r->mixes[idx];
        if (!m->in_use) { m->offset = 0; continue; }
        if (m->distance == AR_INVALID_DISTANCE) return -2;
        if (m->buffer_count > AR_MAX_MIX_BUFFERS) return -3;
        if (offset > VP_AR_MAX_SAMPLE_BUFFERS - m->buffer_count) return -4;
        m->offset = offset;
        offset += m->buffer_count;
    }
    *total_buffers = offset;
    return 0;
}

static uint32_t isqrt64(uint64_t v) {
    if (!v) return 0;
    uint64_t x = v, y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return (uint32_t)x;
}

static float voice_sample(uint32_t address, uint64_t size, uint32_t frame,
                          uint32_t channel, uint32_t channels, uint32_t format,
                          int *ok) {
    uint32_t bps = format == 1u ? 1u : format == 2u ? 2u : format == 5u ? 4u : 0u;
    if (!bps || channels == 0 || channels > AR_MAX_CHANNELS) { *ok = 0; return 0.0f; }
    uint64_t off = ((uint64_t)frame * channels + channel) * bps;
    if (off + bps > size) { *ok = 0; return 0.0f; }
    if (format == 1u) return (float)(int8_t)rd8(address + (uint32_t)off) / 128.0f;
    if (format == 2u) return (float)(int16_t)rd16(address + (uint32_t)off) / 32768.0f;
    float f = rdf32(address + (uint32_t)off);
    if (f != f) f = 0.0f; /* NaN from guest data must not poison the mix */
    return f;
}

static float route_volume(const ArVoice *v, int splitter_route,
                          const ArResource *res, uint32_t channel,
                          uint32_t destination) {
    if (!splitter_route) return res->volume[destination];
    /* The title's SplitterType voices are not backed by the direct
     * voice-resource matrix (those entries are zero in the serialized
     * update). Its active setup uses unity for stereo and a centered mono
     * route. Keep the fallback bounded to the final stereo mix; other
     * splitter matrices remain a version-bound limitation. */
    if (v->channels == 1u)
        return destination < 2u ? 0.70710678f : 0.0f;
    return destination == channel ? 1.0f : 0.0f;
}

static void adpcm_release(ArVoice *v) {
    if (!v) return;
    free(v->adpcm_pcm);
    v->adpcm_pcm = NULL;
    v->adpcm_pcm_frames = 0;
    v->adpcm_address = 0;
    v->adpcm_size = 0;
    v->adpcm_start = 0;
    v->adpcm_end = 0;
    v->adpcm_context_address = 0;
    v->adpcm_context_size = 0;
    v->adpcm_extra_address = 0;
    v->adpcm_extra_size = 0;
    v->adpcm_reported = 0;
    v->adpcm_failed = 0;
}

static uint32_t adpcm_data_size(uint32_t sample_count) {
    uint32_t frames = sample_count / AR_ADPCM_SAMPLES_PER_FRAME;
    uint32_t remainder = sample_count % AR_ADPCM_SAMPLES_PER_FRAME;
    uint32_t extra = remainder ? remainder / 2u + 1u + (remainder & 1u) : 0u;
    return frames * AR_ADPCM_BYTES_PER_FRAME + extra;
}

static int32_t adpcm_round_fixed(int64_t value) {
    /* Matches FixedPointHelper.RoundUpAndToInt(value, 11) used by the
     * locked Switch renderer: round to nearest, with the arithmetic right
     * shift defining the negative half behavior. */
    return (int32_t)((value + 1024) >> 11);
}

static int16_t adpcm_saturate(int64_t value) {
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return (int16_t)value;
}

static int adpcm_prepare(VPGuestSvc *svc, ArVoice *v, uint32_t address,
                         uint32_t size, uint32_t start, uint32_t end,
                         uint32_t context_address, uint32_t context_size,
                         uint32_t extra_address, uint32_t extra_size) {
    int same = v->adpcm_address == address && v->adpcm_size == size
        && v->adpcm_start == start && v->adpcm_end == end
        && v->adpcm_context_address == context_address
        && v->adpcm_context_size == context_size
        && v->adpcm_extra_address == extra_address
        && v->adpcm_extra_size == extra_size;
    if (same && v->adpcm_pcm) return 0;
    if (same && v->adpcm_failed) return -1;
    adpcm_release(v);
    v->adpcm_address = address;
    v->adpcm_size = size;
    v->adpcm_start = start;
    v->adpcm_end = end;
    v->adpcm_context_address = context_address;
    v->adpcm_context_size = context_size;
    v->adpcm_extra_address = extra_address;
    v->adpcm_extra_size = extra_size;

    if (v->channels != 1u || !end || end > AR_ADPCM_MAX_PCM_FRAMES
        || start >= end || adpcm_data_size(end) > size
        || extra_address == 0u || extra_size < 32u
        || extra_size > 0x1000u
        || !vp_guest_svc_readable(svc, extra_address, 32u)) {
        v->adpcm_failed = 1;
        vp_guest_svc_log(svc,
            "AUDIO_ADPCM_UNSUPPORTED address=0x%08x size=0x%x"
            " range=0x%x:0x%x channels=%u extra=0x%08x/0x%x"
            " context=0x%08x/0x%x\n",
            address, size, start, end, v->channels, extra_address, extra_size,
            context_address, context_size);
        return -1;
    }
    if (context_address && (context_size < 6u
                            || !vp_guest_svc_readable(svc, context_address, 6u))) {
        v->adpcm_failed = 1;
        vp_guest_svc_log(svc,
            "AUDIO_ADPCM_UNSUPPORTED reason=context address=0x%08x size=0x%x\n",
            context_address, context_size);
        return -1;
    }

    v->adpcm_pcm = calloc(end, sizeof(*v->adpcm_pcm));
    if (!v->adpcm_pcm) {
        v->adpcm_failed = 1;
        vp_guest_svc_log(svc,
            "AUDIO_ADPCM_UNSUPPORTED reason=allocation frames=%u\n", end);
        return -1;
    }

    int16_t coefficients[16];
    for (uint32_t i = 0; i < 16u; ++i)
        coefficients[i] = (int16_t)rd16(extra_address + i * 2u);
    int16_t history0 = 0;
    int16_t history1 = 0;
    if (context_address) {
        /* The first block supplies its own predictor/scale byte. The context
         * predictor is consumed when decoding starts in the middle of a
         * stream; this implementation decodes from sample zero and still
         * preserves the context histories. */
        (void)rd16(context_address + 0u);
        history0 = (int16_t)rd16(context_address + 2u);
        history1 = (int16_t)rd16(context_address + 4u);
    }

    uint32_t data_offset = 0;
    uint32_t sample_offset = 0;
    while (sample_offset < end) {
        if (data_offset >= size) goto decode_fail;
        uint8_t pred_scale_byte = rd8(address + data_offset);
        uint32_t coefficient_index = pred_scale_byte >> 4;
        uint32_t scale = pred_scale_byte & 0xfu;
        uint32_t count = end - sample_offset;
        if (count > AR_ADPCM_SAMPLES_PER_FRAME)
            count = AR_ADPCM_SAMPLES_PER_FRAME;
        uint32_t needed = count == AR_ADPCM_SAMPLES_PER_FRAME
            ? AR_ADPCM_BYTES_PER_FRAME : 1u + (count + 1u) / 2u;
        if (needed > size || data_offset > size - needed) goto decode_fail;
        int16_t coefficient0 = coefficients[coefficient_index * 2u];
        int16_t coefficient1 = coefficients[coefficient_index * 2u + 1u];
        int64_t scale_fixed = (int64_t)1 << (11u + scale);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t packed = rd8(address + data_offset + 1u + i / 2u);
            int32_t nibble = (i & 1u) ? (packed & 0xfu) : (packed >> 4);
            if (nibble & 8) nibble -= 16;
            int64_t prediction = (int64_t)coefficient0 * history0
                + (int64_t)coefficient1 * history1;
            int16_t sample = adpcm_saturate(adpcm_round_fixed(
                (int64_t)nibble * scale_fixed + prediction));
            history1 = history0;
            history0 = sample;
            v->adpcm_pcm[sample_offset++] = sample;
        }
        data_offset += needed;
    }
    if (!v->adpcm_reported) {
        vp_guest_svc_log(svc,
            "AUDIO_ADPCM_INIT address=0x%08x size=0x%x samples=%u"
            " rate=%u channels=%u extra=0x%08x/0x%x context=0x%08x/0x%x"
            " coeff=%d,%d\n",
            address, size, end, v->sample_rate, v->channels, extra_address,
            extra_size, context_address, context_size, coefficients[0],
            coefficients[1]);
        v->adpcm_reported = 1;
    }
    v->adpcm_pcm_frames = end;
    v->playing = 0;
    return 0;

decode_fail:
    free(v->adpcm_pcm);
    v->adpcm_pcm = NULL;
    v->adpcm_pcm_frames = 0;
    v->adpcm_failed = 1;
    vp_guest_svc_log(svc,
        "AUDIO_ADPCM_DECODE_FAIL address=0x%08x size=0x%x"
        " samples=%u decoded=%u data_offset=0x%x\n",
        address, size, end, sample_offset, data_offset);
    return -1;
}

static uint32_t render_adpcm_voice(VPAudioRenderer *r, VPGuestSvc *svc,
                               ArVoice *v, ArMix *mix, int splitter_route,
                               ArResource *res, const uint8_t *voice,
                               uint32_t address, uint32_t size,
                               uint32_t start, uint32_t end, uint32_t loop,
                               uint32_t loop_start, uint32_t loop_end,
                               uint32_t context_address, uint32_t context_size,
                               VPAudioRenderStats *st, uint32_t offset,
                               uint32_t frames) {
    uint64_t extra_address64 = nrd64(voice + 0x48u);
    uint64_t extra_size64 = nrd64(voice + 0x50u);
    if ((extra_address64 >> 32) || (extra_size64 >> 32)
        || adpcm_prepare(svc, v, address, size, start, end,
                         context_address, context_size,
                         (uint32_t)extra_address64,
                         (uint32_t)extra_size64)) {
        ++st->voices_unsupported;
        return 0;
    }

    float step_f = ((float)v->sample_rate / (float)r->rate);
    if (!(v->pitch > 0.0f) || v->pitch != v->pitch) v->pitch = 1.0f;
    step_f *= v->pitch;
    uint32_t step_q16 = (uint32_t)(step_f * 65536.0f + 0.5f);
    if (!step_q16) step_q16 = 1;
    if (!v->playing) {
        v->playing = 1;
        v->cursor_q16 = (uint64_t)start << 16;
    }
    uint64_t pos = v->cursor_q16;
    uint32_t written = 0;
    while (written < frames) {
        uint32_t idx = (uint32_t)(pos >> 16);
        if (idx >= end) {
            if (!loop) break;
            uint32_t ls = (loop_start < end && loop_end > loop_start
                           && loop_end <= end) ? loop_start : start;
            idx = ls;
            pos = (uint64_t)ls << 16;
        }
        float sample = (float)v->adpcm_pcm[idx] / 32768.0f;
        float gain = sample * v->volume;
        r->covered[offset + written] = 1;
        if (gain != 0.0f) {
            for (uint32_t d = 0; d < mix->buffer_count; ++d) {
                float vol = route_volume(v, splitter_route, res, 0, d);
                if (vol == 0.0f) continue;
                float *dst = &r->samples[((size_t)mix->offset + d) * r->frames
                                         + offset + written];
                *dst += gain * vol;
            }
        }
        pos += step_q16;
        ++written;
    }
    v->cursor_q16 = pos;
    if (written) {
        ++st->voices_active;
        ++st->voices_rendered;
    }
    return written;
}

static uint32_t render_voice_buffer(VPAudioRenderer *r, VPGuestSvc *svc, ArVoice *v,
                         const uint8_t *base, uint32_t base_addr,
                         VPAudioRenderStats *st, uint32_t offset, uint32_t frames) {
    if (!v->in_use || v->play_state != 0u) { v->playing = 0; return 0; }
    if (v->format != 6u && v->adpcm_pcm) adpcm_release(v);
    ArMix *mix = NULL;
    int splitter_route = 0;
    uint32_t dest_word = nrd32(base + 0x5cu);
    if (v->mix_id < r->mix_cap && r->mixes[v->mix_id].in_use) {
        mix = &r->mixes[v->mix_id];
    } else if (v->mix_id == AR_UNUSED_MIX && dest_word != 0xffffffffu
               && AR_FINAL_MIX_ID < r->mix_cap
               && r->mixes[AR_FINAL_MIX_ID].in_use) {
        /* The locked SDK encodes a voice routed to SplitterType as
         * mix_id=0x7fffffff and stores the splitter handle in +0x5c. The
         * title's current splitter graph terminates at the final mix; the
         * serialized splitter block is only the 0x20-byte handle table, so
         * collapse that edge here instead of treating a valid voice as
         * malformed. The title initializes the active splitter matrix to
         * unity; use that identity/center route until the runtime exposes
         * the matrix itself. */
        mix = &r->mixes[AR_FINAL_MIX_ID];
        splitter_route = 1;
        if (!v->route_reported) {
            uint32_t wave_count = nrd32(base + 0x3cu);
            uint32_t wave_index = nrd16(base + 0x40u);
            uint64_t address = nrd64(base + 0x48u);
            uint64_t size = nrd64(base + 0x50u);
            uint32_t wave_start = 0;
            uint32_t wave_end = 0;
            uint32_t wave_loop = 0;
            uint32_t wave_loop_start = 0;
            uint32_t wave_loop_end = 0;
            uint32_t context_address = 0;
            uint32_t context_size = 0;
            uint32_t rid0 = nrd32(base + 0x140u);
            uint32_t rid1 = nrd32(base + 0x144u);
            float v00 = 0.0f, v01 = 0.0f, v10 = 0.0f, v11 = 0.0f;
            if (wave_count && wave_index < 4u) {
                const uint8_t *wb = base + 0x60u + wave_index * 0x38u;
                address = nrd64(wb + 0x00u);
                size = nrd64(wb + 0x08u);
                wave_start = nrd32(wb + 0x10u);
                wave_end = nrd32(wb + 0x14u);
                wave_loop = nrd8(wb + 0x18u);
                context_address = nrd32(wb + 0x20u);
                context_size = nrd32(wb + 0x28u);
                wave_loop_start = nrd32(wb + 0x30u);
                wave_loop_end = nrd32(wb + 0x34u);
            }
            if (rid0 < r->resource_cap && r->resources[rid0].in_use) {
                v00 = r->resources[rid0].volume[0];
                v01 = r->resources[rid0].volume[1];
            }
            if (rid1 < r->resource_cap && r->resources[rid1].in_use) {
                v10 = r->resources[rid1].volume[0];
                v11 = r->resources[rid1].volume[1];
            }
            vp_guest_svc_log(svc,
                "AUDIO_MIXER_VOICE_ROUTE splitter=%u fallback=final_mix"
                " address=0x%08x format=%u rate=%u channels=%u"
                " wave_count=%u wave_index=%u data=0x%08x:%08x size=0x%08x:%08x"
                " range=0x%x:0x%x loop=%u loop_range=0x%x:0x%x"
                " extra=0x%08x:%08x context=0x%08x:%08x"
                " rid=%u,%u vol=%f,%f/%f,%f\n",
                dest_word, base_addr, v->format, v->sample_rate, v->channels,
                wave_count, wave_index, (uint32_t)address, (uint32_t)(address >> 32),
                (uint32_t)size, (uint32_t)(size >> 32), wave_start, wave_end,
                wave_loop, wave_loop_start, wave_loop_end, nrd32(base + 0x48u),
                nrd32(base + 0x50u), context_address, context_size, rid0, rid1,
                v00, v01, v10, v11);
            v->route_reported = 1;
        }
    }
    if (!mix) {
        ++st->voices_unsupported;
        if (!v->reported) {
            vp_guest_svc_log(svc,
                "AUDIO_MIXER_VOICE_UNSUPPORTED reason=mix"
                " address=0x%08x mix_id=0x%08x dest_word=0x%08x"
                " play_state=%u format=%u rate=%u channels=%u"
                " volume=%f pitch=%f wave_count=%u wave_index=%u"
                " initial=0x%08x:%08x size=0x%08x:%08x\n",
                base_addr, v->mix_id, nrd32(base + 0x5cu),
                v->play_state, v->format, v->sample_rate, v->channels,
                v->volume, v->pitch, nrd32(base + 0x3cu),
                v->wave_index, nrd32(base + 0x48u),
                nrd32(base + 0x4cu), nrd32(base + 0x50u),
                nrd32(base + 0x54u));
            v->reported = 1;
        }
        v->playing = 0;
        return 0;
    }
    if (v->format != 1u && v->format != 2u && v->format != 5u
        && v->format != 6u) {
        ++st->voices_unsupported;
        if (!v->reported) {
            vp_guest_svc_log(svc, "AUDIO_MIXER_VOICE_UNSUPPORTED reason=format address=0x%08x format=%u\n",
                 base_addr, v->format);
            v->reported = 1;
        }
        v->playing = 0;
        return 0;
    }
    if (!v->channels || v->channels > AR_MAX_CHANNELS) {
        ++st->voices_unsupported;
        v->playing = 0;
        return 0;
    }
    uint32_t wb_count = nrd32(base + 0x3cu);
    uint32_t wb_index = v->wave_index;
    uint64_t address64, size64;
    uint64_t context_address64 = 0;
    uint64_t context_size64 = 0;
    uint32_t start, end, loop, loop_start, loop_end;
    if (wb_count) {
        /* Appended wave buffers: the client serializes the active entry at
         * +0x60 + index*0x38 (address u64, size u64, start/end, loop). */
        if (wb_index >= 4u) { v->playing = 0; ++st->voice_starved; return 0; }
        const uint8_t *wb = base + 0x60u + wb_index * 0x38u;
        address64 = nrd64(wb + 0x00u);
        size64 = nrd64(wb + 0x08u);
        start = (uint32_t)nrds32(wb + 0x10u);
        end = (uint32_t)nrds32(wb + 0x14u);
        loop = nrd8(wb + 0x18u);
        context_address64 = nrd64(wb + 0x20u);
        context_size64 = nrd64(wb + 0x28u);
        loop_start = nrd32(wb + 0x30u);
        loop_end = nrd32(wb + 0x34u);
    } else {
        /* No appended wave buffer: play the buffer given to
         * AcquireVoiceSlot, which the client serializes at +0x48 (address
         * u64) and +0x50 (size u64). The reviewed client stores it in the
         * voice info's initial-buffer fields and never folds it into the
         * wave-buffer array (AppendWaveBuffer is a separate API), so a voice
         * with a zero wave count is not idle. It plays once from frame 0. */
        uint32_t bytes_per_sample = v->format == 1u ? 1u
            : v->format == 2u ? 2u : v->format == 6u ? 1u : 4u;
        uint64_t frames;
        address64 = nrd64(base + 0x48u);
        size64 = nrd64(base + 0x50u);
        frames = size64 / ((uint64_t)bytes_per_sample * v->channels);
        start = 0;
        end = (uint32_t)frames;
        loop = 0;
        loop_start = 0;
        loop_end = 0;
    }
    if (address64 >> 32 || context_address64 >> 32 || context_size64 >> 32
        || !size64 || size64 > 0x8000000u
        || (v->format != 6u && (end <= start || start >= end))) {
        ++st->voices_unsupported;
        v->playing = 0;
        return 0;
    }
    uint32_t address = (uint32_t)address64;
    if (!vp_guest_svc_readable(svc, address, (uint32_t)size64)) {
        ++st->voices_unsupported;
        if (!v->reported) {
            vp_guest_svc_log(svc, "AUDIO_MIXER_VOICE_UNSUPPORTED reason=unreadable address=0x%08x size=0x%x\n",
                 address, (uint32_t)size64);
            v->reported = 1;
        }
        v->playing = 0;
        return 0;
    }

    if (mix->buffer_count == 0) { v->playing = 0; ++st->voice_starved; return 0; }

    if (v->format == 6u) {
        if (!wb_count || !end) {
            ++st->voices_unsupported;
            v->playing = 0;
            return 0;
        }
        uint32_t rid = nrd32(base + 0x140u);
        if (rid >= r->resource_cap || !r->resources[rid].in_use) {
            ++st->voices_unsupported;
            v->playing = 0;
            return 0;
        }
        return render_adpcm_voice(r, svc, v, mix, splitter_route,
                           &r->resources[rid], base, address,
                           (uint32_t)size64, start, end, loop, loop_start,
                           loop_end, (uint32_t)context_address64,
                           (uint32_t)context_size64, st, offset, frames);
    }

    if (v->format != 6u) {
        int source_changed = v->pcm_address != address
            || v->pcm_size != (uint32_t)size64
            || v->pcm_start != start || v->pcm_end != end
            || v->pcm_wave_index != wb_index;
        if (source_changed) {
            v->pcm_address = address;
            v->pcm_size = (uint32_t)size64;
            v->pcm_start = start;
            v->pcm_end = end;
            v->pcm_wave_index = wb_index;
            v->cursor_q16 = (uint64_t)start << 16;
            v->playing = 1;
        }
    }

    float step_f = ((float)v->sample_rate / (float)r->rate);
    if (!(v->pitch > 0.0f) || v->pitch != v->pitch) v->pitch = 1.0f;
    step_f *= v->pitch;
    uint32_t step_q16 = (uint32_t)(step_f * 65536.0f + 0.5f);
    if (!step_q16) step_q16 = 1;

    if (!v->playing) {
        v->playing = 1;
        v->cursor_q16 = (uint64_t)start << 16;
    }
    uint64_t pos = v->cursor_q16;

    uint32_t written = 0;
    for (uint32_t ch = 0; ch < v->channels; ++ch) {
        uint32_t rid = nrd32(base + 0x140u + ch * 4u);
        if (rid >= r->resource_cap || !r->resources[rid].in_use) {
            ++st->voices_unsupported;
            continue;
        }
        ArResource *res = &r->resources[rid];
        uint64_t p = pos;
        for (uint32_t i = 0; i < frames; ++i) {
            uint32_t idx = (uint32_t)(p >> 16);
            if (idx >= end) {
                if (loop) {
                    uint32_t ls = (loop_start < end && loop_end > loop_start
                                   && loop_end <= end) ? loop_start : start;
                    idx = ls;
                    p = (uint64_t)ls << 16;
                } else {
                    break;
                }
            }
            int ok = 1;
            float sample = voice_sample(address, size64, idx, ch, v->channels,
                                        v->format, &ok);
            if (!ok) break;
            r->covered[offset + i] = 1;
            float gain = sample * v->volume;
            if (gain != 0.0f) {
                for (uint32_t d = 0; d < mix->buffer_count; ++d) {
                    float vol;
                    vol = route_volume(v, splitter_route, res, ch, d);
                    if (vol == 0.0f) continue;
                    float *dst = &r->samples[((size_t)mix->offset + d) * r->frames + offset + i];
                    *dst += gain * vol;
                }
            }
            p += step_q16;
            if (i + 1u > written) written = i + 1u;
        }
        v->cursor_q16 = p;
    }
    ++st->voices_active;
    ++st->voices_rendered;
    return written;
}

/* The SDK's wave_buffer_index is its oldest outstanding slot, not a command
 * to restart that slot on every update. Latch newly submitted slots, consume
 * them once, and return cumulative progress so NuPlayer can refill its ring.
 * +0x1a is sent_to_DSP: a client submission clears it, subsequent updates set
 * it. The edge latch also prevents re-consuming a short slot before the next
 * client update observes its completion. */
static void render_voice(VPAudioRenderer *r, VPGuestSvc *svc, ArVoice *v,
                         const uint8_t *base, uint32_t base_addr,
                         VPAudioRenderStats *st) {
    uint32_t previous = v->playback_initialized ? v->previous_play_state : 1u;
    v->previous_play_state = v->play_state;
    v->playback_initialized = 1;
    uint32_t count = nrd32(base + 0x3cu);
    if (count) {
        for (uint32_t j = 0; j < 4u; ++j) {
            const uint8_t *wb = base + 0x60u + j * 0x38u;
            uint32_t sent = nrd8(wb + 0x1au);
            if (!sent && !v->wave_sent[j] && nrd64(wb) && nrd64(wb + 8u)) {
                v->wave_valid[j] = 1;
                v->wave_sent[j] = 1;
            } else if (sent) {
                v->wave_sent[j] = 0;
            }
        }
    }
    if (!v->in_use || v->play_state == 2u) return; /* paused keeps its cursor */
    if (v->play_state == 1u) {
        /* Initial Stopped accepts pre-roll without consuming it. Only a
         * transition from Started/Paused is a RequestStop that flushes the
         * queue. Reporting pre-roll consumed makes the client skip its head
         * before playback even begins. */
        if (previous == 1u) return;
        for (uint32_t j = 0; j < 4u; ++j) {
            if (v->wave_valid[j]) {
                ++v->wave_consumed;
                v->wave_index = (v->wave_index + 1u) % 4u;
            }
            v->wave_valid[j] = 0;
            v->wave_sent[j] = 0;
        }
        v->played_samples = v->played_q16 = 0;
        v->playing = 0;
        v->pcm_address = 0;
        return;
    }
    uint32_t offset = 0;
    uint32_t unsupported_before = st->voices_unsupported;
    for (uint32_t n = 0; n < 4u && offset < r->frames; ++n) {
        if (count && (v->wave_index >= 4u || !v->wave_valid[v->wave_index])) break;
        uint32_t written = render_voice_buffer(r, svc, v, base, base_addr, st,
                                               offset, r->frames - offset);
        float pitch = v->pitch > 0.0f ? v->pitch : 1.0f;
        uint32_t step = (uint32_t)((float)v->sample_rate / (float)r->rate
                                   * pitch * 65536.0f + 0.5f);
        v->played_q16 += (uint64_t)written * (step ? step : 1u);
        v->played_samples = v->played_q16 >> 16;
        offset += written;
        if (!count || !v->in_use || v->play_state != 0u) break;
        const uint8_t *wb = base + 0x60u + v->wave_index * 0x38u;
        uint32_t end = nrd32(wb + 0x14u);
        if (nrd8(wb + 0x18u) || !end || (v->cursor_q16 >> 16) < end) break;
        v->wave_valid[v->wave_index] = 0;
        ++v->wave_consumed;
        if (nrd8(wb + 0x19u)) v->played_samples = v->played_q16 = 0;
        if (v->wave_consumed < 8u || (v->wave_consumed % 64u) == 0u)
            vp_guest_svc_log(svc, "AUDIO_WAVE_CONSUMED voice=0x%08x slot=%u consumed=%u played=%llu\n",
                base_addr, v->wave_index, v->wave_consumed,
                (unsigned long long)v->played_samples);
        v->wave_index = (v->wave_index + 1u) % 4u;
        v->playing = 0;
        v->pcm_address = 0;
    }
    /* An in-use playing voice that produced no frames and had no unsupported
     * input is content-starved: the client has not supplied a usable wave for
     * the slot it is pointing at. This is kept separate from device underruns
     * and from the legitimate empty-graph silence of an idle renderer. */
    if (!offset && st->voices_unsupported == unsupported_before
        && v->play_state == 0u)
        ++st->voice_starved;
}

int vp_audio_renderer_voice_status(const VPAudioRenderer *r, uint32_t index,
                                  uint64_t *played, uint32_t *consumed) {
    if (!r || index >= r->voice_cap) return -1;
    *played = r->voices[index].played_samples;
    *consumed = r->voices[index].wave_consumed;
    return 0;
}

uint32_t vp_audio_catchup_periods(uint32_t pending, uint32_t frames,
                                  uint32_t target, uint32_t cap) {
    if (!frames || pending >= target) return 0;
    uint32_t periods = (target - pending) / frames;
    return periods < cap ? periods : cap;
}

int vp_audio_renderer_apply_config(VPAudioRenderer *r, VPGuestSvc *svc,
                                   uint32_t cmd_address, uint32_t cmd_size,
                                   VPAudioRenderStats *stats) {
    if (stats) memset(stats, 0, sizeof(*stats));
    if (!r || !svc) return -100;
    /* A rejected apply must not leave a partially replaced snapshot usable. */
    r->config_valid = 0;
    if (!cmd_size || cmd_size < 0x40u || cmd_size > 0x100000u
        || !vp_guest_svc_readable(svc, cmd_address, cmd_size)) return -101;
    if (rd32(cmd_address) != 0x34564552u) return -102;

    /* Take the bounded snapshot before interpreting anything. The copy is the
     * only update state used by later periods; the client's command buffer
     * may be overwritten as soon as RequestUpdate returns. 1 MiB is the
     * existing validated command limit. */
    if (realloc_array((void **)&r->config, &r->config_cap, cmd_size, 1))
        return -105;
    memcpy(r->config, (const void *)(uintptr_t)cmd_address, cmd_size);
    r->config_size = cmd_size;
    r->config_guest_address = cmd_address;
    const uint8_t *cfg = r->config;

    uint32_t behaviour = nrd32(cfg + 0x04u);
    uint32_t memory_pool = nrd32(cfg + 0x08u);
    uint32_t voices_size = nrd32(cfg + 0x0cu);
    uint32_t resource_size = nrd32(cfg + 0x10u);
    uint32_t effect_size = nrd32(cfg + 0x14u);
    uint32_t mix_size = nrd32(cfg + 0x18u);
    uint32_t sink_size = nrd32(cfg + 0x1cu);
    uint32_t perf_size = nrd32(cfg + 0x20u);
    uint32_t splitter_size = nrd32(cfg + 0x24u);
    uint32_t total = nrd32(cfg + 0x3cu);
    if (total < 0x40u || total > cmd_size) total = cmd_size;

    uint64_t cursor = 0x40u;
    uint64_t end = total;
    uint32_t p_behaviour = (uint32_t)cursor; cursor += behaviour;
    uint32_t p_memory = (uint32_t)cursor; cursor += memory_pool;
    /* The locked client's VoiceInfoManager serializes its block as the
     * 0x70-byte voice-channel resources first and the 0x170-byte voices
     * after them; the header keeps the two sizes in the opposite field
     * order (+0x0c voice, +0x10 resource). The server's InfoUpdater
     * consumes UpdateVoiceChannelResources before UpdateVoices, so mirror
     * that layout rather than the header field order. */
    uint32_t p_resources = (uint32_t)cursor; cursor += resource_size;
    uint32_t p_voices = (uint32_t)cursor; cursor += voices_size;
    uint32_t p_effects = (uint32_t)cursor; cursor += effect_size;
    uint32_t p_splitter = (uint32_t)cursor; cursor += splitter_size;
    uint32_t p_mixes = (uint32_t)cursor; cursor += mix_size;
    uint32_t p_sinks = (uint32_t)cursor; cursor += sink_size;
    uint32_t p_perf = (uint32_t)cursor; cursor += perf_size;
    (void)p_behaviour; (void)p_memory; (void)p_effects; (void)p_perf;
    if (cursor > end) return -103;

    /* The title uses a compact 0x20-byte splitter-info block once voices are
     * active. Keep a bounded, value-only trace while the serialized layout is
     * being matched to the SDK's splitter destination table. A successful
     * size change is diagnostic, not an error: emit the first few and then a
     * sparse heartbeat so an alternating size cannot log every update. */
    if (splitter_size && r->splitter_reported_size != splitter_size) {
        r->splitter_reported_size = splitter_size;
        if (vp_audio_log_cadence_should_emit(&r->splitter_log, 4u, 64u)) {
            uint32_t w[8] = {0};
            uint32_t n = splitter_size < sizeof(w) ? splitter_size / 4u : 8u;
            for (uint32_t i = 0; i < n; ++i)
                w[i] = nrd32(cfg + p_splitter + i * 4u);
            vp_guest_svc_log(svc,
                "AUDIO_MIXER_SPLITTER size=0x%x words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
                splitter_size, w[0], w[1], w[2], w[3],
                w[4], w[5], w[6], w[7]);
        }
    }

    uint32_t voice_count = voices_size / 0x170u;
    uint32_t resource_count = resource_size / 0x70u;
    uint32_t mix_count = mix_size / 0x930u;
    uint32_t sink_count = sink_size / 0x140u;
    if ((voices_size % 0x170u) || (resource_size % 0x70u)
        || (mix_size % 0x930u) || (sink_size % 0x140u)
        || voice_count > VP_AR_MAX_VOICES || resource_count > VP_AR_MAX_RESOURCES
        || mix_count > VP_AR_MAX_MIXES || !mix_count) {
        return -104;
    }
    if (realloc_array((void **)&r->voices, &r->voice_cap,
                      voice_count ? voice_count : 1, sizeof(ArVoice))
        || realloc_array((void **)&r->resources, &r->resource_cap,
                         resource_count ? resource_count : 1, sizeof(ArResource))
        || realloc_array((void **)&r->mixes, &r->mix_cap,
                         mix_count ? mix_count : 1, sizeof(ArMix))
        || realloc_array((void **)&r->sort_idx, &r->sort_cap,
                         mix_count ? mix_count : 1, sizeof(uint32_t)))
        return -105;

    for (uint32_t i = 0; i < resource_count; ++i) {
        const uint8_t *base = cfg + p_resources + i * 0x70u;
        ArResource *res = &r->resources[i];
        res->in_use = nrd8(base + 0x64u);
        for (uint32_t j = 0; j < AR_MAX_MIX_BUFFERS; ++j)
            res->volume[j] = nrdf32(base + 0x04u + j * 4u);
    }

    for (uint32_t i = 0; i < mix_count; ++i) {
        const uint8_t *base = cfg + p_mixes + i * 0x930u;
        ArMix *m = &r->mixes[i];
        m->volume = nrdf32(base + 0x00u);
        m->buffer_count = nrd32(base + 0x08u);
        m->in_use = nrd8(base + 0x0cu);
        m->mix_id = nrds32(base + 0x10u);
        m->dest_mix = nrds32(base + 0x924u);
        for (uint32_t x = 0; x < AR_MAX_MIX_BUFFERS; ++x)
            for (uint32_t y = 0; y < AR_MAX_MIX_BUFFERS; ++y)
                m->matrix[x][y] = nrdf32(base + 0x24u + (x * 24u + y) * 4u);
    }

    uint32_t total_buffers = 0;
    int mix_rc = assign_mix_offsets(r, mix_count, &total_buffers);
    if (mix_rc) return -110 + mix_rc;
    if (total_buffers > VP_AR_MAX_SAMPLE_BUFFERS) return -106;
    if (total_buffers) {
        if (realloc_array((void **)&r->samples, &r->sample_cap, total_buffers,
                          r->frames * sizeof(float))) return -107;
        memset(r->samples, 0, (size_t)total_buffers * r->frames * sizeof(float));
    }

    r->voice_count = voice_count;
    r->resource_count = resource_count;
    r->mix_count = mix_count;
    r->sink_count = sink_count;
    r->effect_count = effect_size / 0xC0u;
    r->p_voices_off = p_voices;
    r->p_sinks_off = p_sinks;
    r->sample_buffers = total_buffers;
    r->final_mix_index = UINT32_MAX;
    for (uint32_t i = 0; i < mix_count; ++i) {
        if (r->mixes[i].in_use && r->mixes[i].distance == 0u) {
            r->final_mix_index = i;
            break;
        }
    }
    if (r->final_mix_index == UINT32_MAX) return -108;
    r->final_buffer_count = r->mixes[r->final_mix_index].buffer_count;

    /* Apply the voice parameters and the one-shot new-voice reset exactly
     * once per client update. Additional periods re-read this snapshot but
     * never re-run the reset, so a new-voice flag held in the snapshot cannot
     * restart the cursor. */
    for (uint32_t i = 0; i < voice_count; ++i) {
        const uint8_t *base = cfg + p_voices + i * 0x170u;
        uint32_t guest_base = r->config_guest_address + p_voices + i * 0x170u;
        ArVoice *v = &r->voices[i];
        uint32_t was_in_use = v->in_use;
        v->in_use = nrd8(base + 0x09u);
        v->play_state = nrd8(base + 0x0au);
        v->format = nrd8(base + 0x0bu);
        v->sample_rate = nrd32(base + 0x0cu);
        v->channels = nrd32(base + 0x18u);
        v->pitch = nrdf32(base + 0x1cu);
        v->volume = nrdf32(base + 0x20u);
        v->mix_id = nrd32(base + 0x58u);
        if (v->in_use && v->format == 2u && v->channels == 2u) {
            static unsigned stream_state_logs;
            if (stream_state_logs++ < 16u)
                vp_guest_svc_log(svc, "AUDIO_STREAM_STATE voice=0x%08x new=%u play=%u count=%u client=%u server=%u sent=%u,%u,%u,%u valid=%u,%u,%u,%u\n",
                    guest_base, nrd8(base+8u), v->play_state, nrd32(base+0x3cu),
                    nrd16(base+0x40u), v->wave_index,
                    nrd8(base+0x60u+0x1au),nrd8(base+0x98u+0x1au),
                    nrd8(base+0xd0u+0x1au),nrd8(base+0x108u+0x1au),
                    v->wave_valid[0],v->wave_valid[1],v->wave_valid[2],v->wave_valid[3]);
        }
        if (!v->in_use) {
            adpcm_release(v);
            v->pcm_address = 0;
            v->pcm_size = 0;
            v->pcm_start = 0;
            v->pcm_end = 0;
            v->pcm_wave_index = 0;
            v->playing = 0;
            continue;
        }
        if (!was_in_use || nrd8(base + 0x08u)) {
            v->wave_index = nrd16(base + 0x40u);
            memset(v->wave_valid, 0, sizeof(v->wave_valid));
            memset(v->wave_sent, 0, sizeof(v->wave_sent));
            v->played_samples = v->played_q16 = 0;
            v->wave_consumed = 0;
            v->playback_initialized = 0;
            v->playing = 0;
            v->reported = 0;
            v->route_reported = 0;
            v->pcm_address = 0;
            v->pcm_size = 0;
            v->pcm_start = 0;
            v->pcm_end = 0;
            v->pcm_wave_index = 0;
        }
    }

    if (stats) stats->mix_count = mix_count;
    r->config_valid = 1;
    return 0;
}

int vp_audio_renderer_render_period(VPAudioRenderer *r, VPGuestSvc *svc,
                                    VPAudioRenderStats *stats) {
    if (!r || !svc || !stats) return -100;
    memset(stats, 0, sizeof(*stats));
    if (!r->config_valid || !r->config) return -101;
    memset(r->out, 0, (size_t)r->frames * 2 * sizeof(int16_t));
    memset(r->covered, 0, r->frames);
    if (r->sample_buffers)
        memset(r->samples, 0,
               (size_t)r->sample_buffers * r->frames * sizeof(float));

    const uint8_t *cfg = r->config;
    stats->mix_count = r->mix_count;
    for (uint32_t i = 0; i < r->voice_count; ++i) {
        uint32_t off = r->p_voices_off + i * 0x170u;
        render_voice(r, svc, &r->voices[i], cfg + off,
                     r->config_guest_address + off, stats);
    }

    /* A destination mix must be mixed in before its destination is consumed;
     * sort_idx is farthest-first, which is exactly that order. */
    for (uint32_t i = 0; i < r->mix_count; ++i) {
        uint32_t idx = r->sort_idx[i];
        ArMix *m = &r->mixes[idx];
        if (!m->in_use || m->distance == AR_INVALID_DISTANCE) continue;
        if (m->dest_mix < 0 || (uint32_t)m->dest_mix >= r->mix_count) continue;
        ArMix *dm = &r->mixes[m->dest_mix];
        if (!dm->in_use) continue;
        for (uint32_t x = 0; x < m->buffer_count; ++x) {
            const float *src = &r->samples[((size_t)m->offset + x) * r->frames];
            for (uint32_t y = 0; y < dm->buffer_count; ++y) {
                float vol = m->volume * m->matrix[x][y];
                if (vol == 0.0f) continue;
                float *dst = &r->samples[((size_t)dm->offset + y) * r->frames];
                for (uint32_t f = 0; f < r->frames; ++f) dst[f] += src[f] * vol;
            }
        }
    }

    /* Final-mix volume, then the device sink. */
    ArMix *final_mix = &r->mixes[r->final_mix_index];
    stats->final_offset = final_mix->offset;
    if (final_mix->volume != 1.0f) {
        for (uint32_t y = 0; y < final_mix->buffer_count; ++y) {
            float *buf = &r->samples[((size_t)final_mix->offset + y) * r->frames];
            for (uint32_t f = 0; f < r->frames; ++f) buf[f] *= final_mix->volume;
        }
    }

    for (uint32_t i = 0; i < r->sink_count; ++i) {
        const uint8_t *base = cfg + r->p_sinks_off + i * 0x140u;
        if (nrd8(base) != 1u || !nrd8(base + 1u)) continue; /* device sink only */
        uint32_t input_count = nrd32(base + 0x120u);
        int downmix = input_count == 6u && nrd8(base + 0x12bu) != 0u;
        float coeff[4] = {0};
        stats->sink_count++;
        if (input_count != 1u && input_count != 2u && !downmix) {
            ++stats->sink_unsupported;
            continue;
        }
        int8_t inputs[AR_MAX_CHANNELS];
        for (uint32_t j = 0; j < input_count; ++j)
            inputs[j] = (int8_t)nrd8(base + 0x124u + j);
        int valid = 1;
        for (uint32_t j = 0; j < input_count; ++j)
            if (inputs[j] < 0 || (uint32_t)inputs[j] >= final_mix->buffer_count)
                valid = 0;
        if (!valid) { ++stats->sink_unsupported; continue; }
        if (downmix) {
            for (uint32_t j = 0; j < 4u; ++j) {
                coeff[j] = nrdf32(base + 0x12cu + j * 4u);
                if (!isfinite(coeff[j])) valid = 0;
            }
            if (!valid) { ++stats->sink_unsupported; continue; }
        }
        const float *channels[AR_MAX_CHANNELS];
        for (uint32_t j = 0; j < input_count; ++j)
            channels[j] = &r->samples[((size_t)final_mix->offset + inputs[j]) * r->frames];
        const float *left = &r->samples[((size_t)final_mix->offset + inputs[0]) * r->frames];
        const float *right = input_count == 2u
            ? &r->samples[((size_t)final_mix->offset + inputs[1]) * r->frames] : left;
        uint32_t peak_l = 0, peak_r = 0;
        double sum_l = 0.0, sum_r = 0.0;
        int16_t prev_l = 0, prev_r = 0;
        uint64_t zc_l = 0, zc_r = 0;
        for (uint32_t f = 0; f < r->frames; ++f) {
            float l = left[f];
            float rr = right[f];
            if (downmix) {
                float in[6], stereo[2];
                for (uint32_t j = 0; j < 6u; ++j) in[j] = channels[j][f];
                vp_audio_downmix6(in, coeff, stereo);
                l = stereo[0];
                rr = stereo[1];
            }
            if (l != l) l = 0.0f;
            if (rr != rr) rr = 0.0f;
            if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
            if (rr > 1.0f) rr = 1.0f; else if (rr < -1.0f) rr = -1.0f;
            int32_t li = (int32_t)(l * 32767.0f);
            int32_t ri = (int32_t)(rr * 32767.0f);
            int16_t ls = (int16_t)li;
            int16_t rs = (int16_t)ri;
            r->out[f * 2u] = ls;
            r->out[f * 2u + 1u] = rs;
            uint32_t al = (uint32_t)(li < 0 ? -li : li);
            uint32_t ar = (uint32_t)(ri < 0 ? -ri : ri);
            if (al > peak_l) peak_l = al;
            if (ar > peak_r) peak_r = ar;
            sum_l += (double)li * li;
            sum_r += (double)ri * ri;
            if ((prev_l < 0 && ls >= 0) || (prev_l > 0 && ls <= 0)) ++zc_l;
            if ((prev_r < 0 && rs >= 0) || (prev_r > 0 && rs <= 0)) ++zc_r;
            prev_l = ls;
            prev_r = rs;
        }
        stats->sink_inputs = input_count;
        stats->peak_left = peak_l;
        stats->peak_right = peak_r;
        stats->rms_left = isqrt64((uint64_t)(sum_l / r->frames));
        stats->rms_right = isqrt64((uint64_t)(sum_r / r->frames));
        stats->zero_left = zc_l;
        stats->zero_right = zc_r;
        stats->pcm_frames = r->frames;
        break;
    }
    if (!stats->pcm_frames) return -109;

    /* Coverage is unique output time backed by at least one real voice, even
     * when several short wave slots or voices feed the same period. */
    for (uint32_t f = 0; f < r->frames; ++f)
        stats->source_frames += r->covered[f] ? 1u : 0u;
    stats->effects_ignored = r->effect_count;
    ++r->updates;
    r->frames_rendered += r->frames;
    return 0;
}

int vp_audio_renderer_update(VPAudioRenderer *r, VPGuestSvc *svc,
                             uint32_t cmd_address, uint32_t cmd_size,
                             VPAudioRenderStats *stats) {
    if (!stats) return -100;
    memset(stats, 0, sizeof(*stats));
    int rc = vp_audio_renderer_apply_config(r, svc, cmd_address, cmd_size, NULL);
    if (rc) return rc;
    return vp_audio_renderer_render_period(r, svc, stats);
}

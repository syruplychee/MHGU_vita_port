#include "media_decode.h"

#include <stdio.h>
#include <string.h>

#include "media_video.h"

/* Verbose media instrumentation is opt-in: the title's frame loop already
 * writes a large service log, and the method-level trace is only wanted while
 * the NVDEC/VIC evidence is being collected. */
#define VP_MEDIA_INSTRUMENT_MARKER "ux0:data/mhgu-vita-port/media-instrument.enable"

/* Every method-level line goes through this so the state walk can always run
 * while the trace stays opt-in. */
#define MEDIA_LOG(media, svc, ...) \
    do { if ((media)->instrument) vp_guest_svc_log((svc), __VA_ARGS__); } while (0)

/* host1x class ids used by the media devices. */
#define VP_MEDIA_CLASS_CONTROL 0x01u
#define VP_MEDIA_CLASS_VIC     0x5du
#define VP_MEDIA_CLASS_NVDEC   0xf0u

/* THI register offsets inside a command stream. The guest writes the engine
 * method number to method_0 and the argument to method_1; the same layout is
 * used by CDmaPusher for the GPU channel. */
#define VP_MEDIA_THI_INCSYNCPT  0x00u
#define VP_MEDIA_THI_SETMETHOD0 0x10u
#define VP_MEDIA_THI_SETMETHOD1 0x11u

/* NVDEC method indices (the hardware registers are u64 slots). */
#define VP_MEDIA_NVDEC_SET_CODEC_ID 0x80u
#define VP_MEDIA_NVDEC_EXECUTE      0xc0u
#define VP_MEDIA_NVDEC_CONTROL      0x100u
#define VP_MEDIA_NVDEC_PICTURE_INFO 0x101u
#define VP_MEDIA_NVDEC_BITSTREAM    0x102u

/* VIC method numbers. */
#define VP_MEDIA_VIC_EXECUTE 0xc0u
#define VP_MEDIA_VIC_CONFIG  0x1c2u
#define VP_MEDIA_VIC_LUMA    0x1c8u
#define VP_MEDIA_VIC_CHROMA  0x1c9u

/* Method-level lines are bounded; the execute summaries and the first four
 * full dumps are always emitted. */
#define VP_MEDIA_LOG_BUDGET  512u
#define VP_MEDIA_DUMP_BUDGET 4u

static uint32_t media_u32(const uint8_t *p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint64_t media_u64(const uint8_t *p) {
    uint64_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

/* The media nodes do not get a GMMU mapping: nvhost MapBuffer pins the nvmap
 * allocation and returns its guest address, so the command stream names guest
 * memory directly. Try that range first, then the MapBufferEx table used by
 * the 3D channel for any stream that does carry a translated address. */
int vp_media_resolve(VPGuestSvc *svc, VPGuestSession *state, uint64_t va,
                     uint64_t size, uint32_t *cpu) {
    if (va <= 0xffffffffull && size <= 0xffffffffull
        && vp_guest_svc_readable(svc, (uint32_t)va, (uint32_t)size)) {
        *cpu = (uint32_t)va;
        return 1;
    }
    if (!vp_guest_svc_gpu_va_to_cpu(state, va, size, cpu)) return 0;
    return vp_guest_svc_readable(svc, *cpu, (uint32_t)size);
}

/* Cache the instrumentation marker in the channel record. The return value is
 * the state after the check; callers still walk the command stream when it is
 * zero, they only suppress the trace. */
static int media_refresh_instrument(VPGuestGpuChannel *channel) {
    VPGuestMediaParse *media = &channel->media;
    if (media->instrument_checked) return (int)media->instrument;
    media->instrument_checked = 1u;
    FILE *marker = fopen(VP_MEDIA_INSTRUMENT_MARKER, "rb");
    if (marker != NULL) {
        fclose(marker);
        media->instrument = 1u;
    }
    return (int)media->instrument;
}

static void media_log_hex(VPGuestSvc *svc, const char *tag, const uint8_t *data,
                          uint32_t size) {
    static const char hex[] = "0123456789abcdef";
    char line[16u * 3u + 1u];
    for (uint32_t off = 0; off < size; off += 16u) {
        uint32_t count = size - off;
        if (count > 16u) count = 16u;
        for (uint32_t i = 0; i < count; ++i) {
            line[i * 3u] = hex[data[off + i] >> 4];
            line[i * 3u + 1u] = hex[data[off + i] & 0x0fu];
            line[i * 3u + 2u] = ' ';
        }
        line[count * 3u] = '\0';
        vp_guest_svc_log(svc, "%s off=0x%03x %s\n", tag, off, line);
    }
}

/* Read the NVDEC picture-info record and the elementary stream prefix. The
 * record layout is the one the hardware consumes: stream_len at 0x48 and the
 * H.264 parameter set at 0x58. A miss is reported as a miss; nothing is
 * synthesized in this phase. */
static void media_dump_nvdec(VPGuestSvc *svc, VPGuestSession *state,
                             VPGuestGpuChannel *channel) {
    VPGuestMediaParse *media = &channel->media;
    uint32_t info_cpu = 0;
    if (media->nvdec_picture_info == 0u) {
        vp_guest_svc_log(svc, "VP_MEDIA_NVDEC_PICINFO fd=%d va=0 mapped=0\n",
                         channel->fd);
        return;
    }
    if (!vp_media_resolve(svc, state, media->nvdec_picture_info, 0x2a0u,
                       &info_cpu)) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_NVDEC_PICINFO fd=%d va=0x%llx mapped=0\n",
                         channel->fd,
                         (unsigned long long)media->nvdec_picture_info);
        return;
    }
    const uint8_t *info = (const uint8_t *)(uintptr_t)info_cpu;
    const uint32_t stream_len = media_u32(info + 0x48u);
    const uint32_t frame_mbs_only = media_u32(info + 0x60u);
    const uint32_t width_mbs = media_u32(info + 0x64u);
    const uint32_t height_map = media_u32(info + 0x68u);
    const uint32_t entropy = media_u32(info + 0x70u);
    const uint32_t transform8 = media_u32(info + 0x88u);
    const uint64_t flags = media_u64(info + 0xb0u);
    const uint32_t frame_number = (uint32_t)((flags >> 46) & 0xffffu);
    const uint32_t chroma_format = (uint32_t)((flags >> 12) & 3u);
    const uint32_t pic_order_cnt = (uint32_t)((flags >> 14) & 3u);
    const uint32_t log2_frame_num = (uint32_t)((flags >> 8) & 0xfu);

    vp_guest_svc_log(svc,
                     "VP_MEDIA_NVDEC_PICINFO fd=%d va=0x%llx mapped=1 "
                     "stream_len=%u frame=%u frame_mbs_only=%u w_mbs=%u "
                     "h_map=%u entropy=%u transform8x8=%u chroma=%u poct=%u "
                     "log2fn=%u\n",
                     channel->fd,
                     (unsigned long long)media->nvdec_picture_info, stream_len,
                     frame_number, frame_mbs_only, width_mbs, height_map,
                     entropy, transform8, chroma_format, pic_order_cnt,
                     log2_frame_num);

    if (media->executes <= VP_MEDIA_DUMP_BUDGET) {
        media_log_hex(svc, "VP_MEDIA_NVDEC_PICINFO_BYTES", info, 0x2a0u);
    }

    uint32_t dump_len = stream_len;
    if (dump_len > 64u) dump_len = 64u;
    uint32_t stream_cpu = 0;
    if (dump_len == 0u || media->nvdec_bitstream == 0u
        || !vp_media_resolve(svc, state, media->nvdec_bitstream, dump_len,
                          &stream_cpu)) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_NVDEC_AU fd=%d va=0x%llx len=%u mapped=0\n",
                         channel->fd,
                         (unsigned long long)media->nvdec_bitstream, stream_len);
        return;
    }
    const uint8_t *stream = (const uint8_t *)(uintptr_t)stream_cpu;
    uint32_t first_word = 0;
    if (dump_len >= 4u) {
        first_word = ((uint32_t)stream[0] << 24) | ((uint32_t)stream[1] << 16)
                   | ((uint32_t)stream[2] << 8) | (uint32_t)stream[3];
    }
    vp_guest_svc_log(svc,
                     "VP_MEDIA_NVDEC_AU fd=%d va=0x%llx len=%u dump=%u "
                     "first_be=0x%08x\n",
                     channel->fd,
                     (unsigned long long)media->nvdec_bitstream, stream_len,
                     dump_len, first_word);
    media_log_hex(svc, "VP_MEDIA_NVDEC_AU_BYTES", stream, dump_len);
    for (uint32_t i = 0; i + 3u < dump_len; ++i) {
        if (stream[i] == 0u && stream[i + 1u] == 0u && stream[i + 2u] == 1u) {
            vp_guest_svc_log(svc,
                             "VP_MEDIA_NVDEC_AU_NAL fd=%d off=%u type=%u\n",
                             channel->fd, i, stream[i + 3u] & 0x1fu);
            i += 3u;
        }
    }
    /* The prefix dump only proves how the AU starts. Scan the whole AU once so
     * the report can state whether parameter sets are present anywhere and
     * how the driver marks frame boundaries. Bounded to the dump budget and a
     * sane length; the access is read-only. */
    if (media->executes <= VP_MEDIA_DUMP_BUDGET && stream_len <= 0x100000u) {
        uint32_t full_cpu = 0;
        if (!vp_media_resolve(svc, state, media->nvdec_bitstream, stream_len,
                           &full_cpu)) {
            vp_guest_svc_log(svc,
                             "VP_MEDIA_NVDEC_AU_SCAN fd=%d len=%u mapped=0\n",
                             channel->fd, stream_len);
            return;
        }
        const uint8_t *full = (const uint8_t *)(uintptr_t)full_cpu;
        uint32_t counts[32];
        memset(counts, 0, sizeof(counts));
        uint32_t nals = 0;
        for (uint32_t i = 0; i + 3u < stream_len; ++i) {
            if (full[i] == 0u && full[i + 1u] == 0u && full[i + 2u] == 1u) {
                ++counts[full[i + 3u] & 0x1fu];
                ++nals;
                i += 3u;
            }
        }
        char types[160];
        uint32_t used = 0;
        types[0] = '\0';
        for (uint32_t t = 0; t < 32u; ++t) {
            if (counts[t] == 0u) continue;
            int wrote = snprintf(types + used, sizeof(types) - used,
                                 "%s%u:%u", used ? "," : "", t, counts[t]);
            if (wrote < 0 || (uint32_t)wrote >= sizeof(types) - used) break;
            used += (uint32_t)wrote;
        }
        vp_guest_svc_log(svc,
                         "VP_MEDIA_NVDEC_AU_SCAN fd=%d len=%u nals=%u "
                         "types=%s sps=%u pps=%u idr=%u\n",
                         channel->fd, stream_len, nals, types, counts[7],
                         counts[8], counts[5]);
        if (stream_len >= 32u) {
            media_log_hex(svc, "VP_MEDIA_NVDEC_AU_TAIL", full + stream_len - 32u,
                          32u);
        }
    }
}

/* Read the VIC surface configuration. The surface block sits at config+0x20:
 * pixel format in bits 0-6, block-linear kind in 11-14, height log2 in 15-18,
 * width-1 in 32-45 and height-1 in 46-59. */
static void media_dump_vic(VPGuestSvc *svc, VPGuestSession *state,
                           VPGuestGpuChannel *channel) {
    VPGuestMediaParse *media = &channel->media;
    uint32_t config_cpu = 0;
    uint64_t raw = 0;
    int config_mapped = 0;
    if (media->vic_config != 0u
        && vp_media_resolve(svc, state, media->vic_config + 0x20u, 8u,
                         &config_cpu)) {
        raw = media_u64((const uint8_t *)(uintptr_t)config_cpu);
        config_mapped = 1;
    }
    const uint32_t pixel_format = (uint32_t)(raw & 0x7fu);
    const uint32_t block_kind = (uint32_t)((raw >> 11) & 0x0fu);
    const uint32_t block_height = (uint32_t)((raw >> 15) & 0x0fu);
    const uint32_t width = (uint32_t)((raw >> 32) & 0x3fffu) + 1u;
    const uint32_t height = (uint32_t)((raw >> 46) & 0x3fffu) + 1u;

    uint32_t luma_cpu = 0;
    uint32_t chroma_cpu = 0;
    const int luma_mapped = media->vic_luma != 0u
        && vp_media_resolve(svc, state, media->vic_luma, 1u, &luma_cpu);
    const int chroma_mapped = media->vic_chroma != 0u
        && vp_media_resolve(svc, state, media->vic_chroma, 1u, &chroma_cpu);

    vp_guest_svc_log(svc,
                     "VP_MEDIA_VIC_EXECUTE fd=%d n=%u config=0x%llx "
                     "cfg_mapped=%d raw=0x%llx pixfmt=0x%x block=%u bh=%u "
                     "w=%u h=%u luma=0x%llx luma_cpu=0x%x luma_mapped=%d "
                     "chroma=0x%llx chroma_cpu=0x%x chroma_mapped=%d\n",
                     channel->fd, media->executes,
                     (unsigned long long)media->vic_config, config_mapped,
                     (unsigned long long)raw, pixel_format, block_kind,
                     block_height, width, height,
                     (unsigned long long)media->vic_luma, luma_cpu,
                     luma_mapped, (unsigned long long)media->vic_chroma,
                     chroma_cpu, chroma_mapped);

    if (media->executes <= VP_MEDIA_DUMP_BUDGET && media->vic_config != 0u) {
        uint32_t base_cpu = 0;
        if (vp_media_resolve(svc, state, media->vic_config, 0x40u, &base_cpu)) {
            media_log_hex(svc, "VP_MEDIA_VIC_CONFIG_BYTES",
                          (const uint8_t *)(uintptr_t)base_cpu, 0x40u);
        }
    }
}

static void media_nvdec_method(VPGuestSvc *svc, VPGuestSession *state,
                               VPGuestGpuChannel *channel, uint32_t state_offset,
                               uint32_t data) {
    VPGuestMediaParse *media = &channel->media;
    if (state_offset == VP_MEDIA_THI_INCSYNCPT) {
        MEDIA_LOG(media, svc, "VP_MEDIA_NVDEC_SYNCPT fd=%d id=%u cond=%u\n",
                         channel->fd, data & 0xffu, (data >> 8) & 0xffu);
        return;
    }
    if (state_offset == VP_MEDIA_THI_SETMETHOD0) {
        media->method0 = data;
        return;
    }
    if (state_offset != VP_MEDIA_THI_SETMETHOD1) {
        if (media->commands <= VP_MEDIA_LOG_BUDGET) {
            MEDIA_LOG(media, svc,
                             "VP_MEDIA_NVDEC_THI fd=%d reg=0x%x data=0x%08x\n",
                             channel->fd, state_offset, data);
        }
        return;
    }
    switch (media->method0) {
    case VP_MEDIA_NVDEC_SET_CODEC_ID:
        media->nvdec_codec = data;
        MEDIA_LOG(media, svc, "VP_MEDIA_NVDEC_SET_CODEC fd=%d codec=%u\n",
                         channel->fd, data);
        break;
    case VP_MEDIA_NVDEC_EXECUTE:
        ++media->executes;
        if (media->instrument) {
            MEDIA_LOG(media, svc,
                     "VP_MEDIA_NVDEC_EXECUTE fd=%d n=%u codec=%u "
                     "picinfo=0x%llx bitstream=0x%llx control=0x%llx\n",
                     channel->fd, media->executes, media->nvdec_codec,
                     (unsigned long long)media->nvdec_picture_info,
                     (unsigned long long)media->nvdec_bitstream,
                     (unsigned long long)media->nvdec_control);
            media_dump_nvdec(svc, state, channel);
        }
        /* The decoder hook is independent of the trace: it checks its own
         * media-video.enable marker and is a no-op when that is absent. */
        vp_media_video_decode(svc, state, (uint32_t)channel->fd,
                              media->nvdec_picture_info,
                              media->nvdec_bitstream);
        break;
    case VP_MEDIA_NVDEC_CONTROL:
        media->nvdec_control = (uint64_t)data << 8;
        if (media->commands <= VP_MEDIA_LOG_BUDGET) {
            MEDIA_LOG(media, svc,
                             "VP_MEDIA_NVDEC_REG fd=%d method=0x%x "
                             "value=0x%llx\n",
                             channel->fd, media->method0,
                             (unsigned long long)media->nvdec_control);
        }
        break;
    case VP_MEDIA_NVDEC_PICTURE_INFO:
        media->nvdec_picture_info = (uint64_t)data << 8;
        if (media->commands <= VP_MEDIA_LOG_BUDGET) {
            MEDIA_LOG(media, svc,
                             "VP_MEDIA_NVDEC_REG fd=%d method=0x%x "
                             "value=0x%llx\n",
                             channel->fd, media->method0,
                             (unsigned long long)media->nvdec_picture_info);
        }
        break;
    case VP_MEDIA_NVDEC_BITSTREAM:
        media->nvdec_bitstream = (uint64_t)data << 8;
        if (media->commands <= VP_MEDIA_LOG_BUDGET) {
            MEDIA_LOG(media, svc,
                             "VP_MEDIA_NVDEC_REG fd=%d method=0x%x "
                             "value=0x%llx\n",
                             channel->fd, media->method0,
                             (unsigned long long)media->nvdec_bitstream);
        }
        break;
    default:
        if (media->commands <= VP_MEDIA_LOG_BUDGET) {
            MEDIA_LOG(media, svc,
                             "VP_MEDIA_NVDEC_REG fd=%d method=0x%x "
                             "data=0x%08x\n",
                             channel->fd, media->method0, data);
        }
        break;
    }
}

static void media_vic_method(VPGuestSvc *svc, VPGuestSession *state,
                             VPGuestGpuChannel *channel, uint32_t state_offset,
                             uint32_t data) {
    VPGuestMediaParse *media = &channel->media;
    if (state_offset == VP_MEDIA_THI_INCSYNCPT) {
        MEDIA_LOG(media, svc, "VP_MEDIA_VIC_SYNCPT fd=%d id=%u cond=%u\n",
                         channel->fd, data & 0xffu, (data >> 8) & 0xffu);
        return;
    }
    if (state_offset == VP_MEDIA_THI_SETMETHOD0) {
        media->method0 = data;
        return;
    }
    if (state_offset != VP_MEDIA_THI_SETMETHOD1) {
        if (media->commands <= VP_MEDIA_LOG_BUDGET) {
            MEDIA_LOG(media, svc, "VP_MEDIA_VIC_THI fd=%d reg=0x%x data=0x%08x\n",
                             channel->fd, state_offset, data);
        }
        return;
    }
    switch (media->method0) {
    case VP_MEDIA_VIC_EXECUTE:
        ++media->executes;
        if (media->instrument) {
            MEDIA_LOG(media, svc,
                     "VP_MEDIA_VIC_METHOD fd=%d execute n=%u config=0x%llx "
                     "luma=0x%llx chroma=0x%llx\n",
                     channel->fd, media->executes,
                     (unsigned long long)media->vic_config,
                     (unsigned long long)media->vic_luma,
                     (unsigned long long)media->vic_chroma);
            media_dump_vic(svc, state, channel);
        }
        vp_media_video_composite(svc, state, (uint32_t)channel->fd,
                                 media->vic_config, media->vic_luma,
                                 media->vic_chroma);
        break;
    case VP_MEDIA_VIC_CONFIG:
        media->vic_config = (uint64_t)data << 8;
        MEDIA_LOG(media, svc,
                         "VP_MEDIA_VIC_REG fd=%d method=0x%x value=0x%llx\n",
                         channel->fd, media->method0,
                         (unsigned long long)media->vic_config);
        break;
    case VP_MEDIA_VIC_LUMA:
        media->vic_luma = (uint64_t)data << 8;
        MEDIA_LOG(media, svc,
                         "VP_MEDIA_VIC_REG fd=%d method=0x%x value=0x%llx\n",
                         channel->fd, media->method0,
                         (unsigned long long)media->vic_luma);
        break;
    case VP_MEDIA_VIC_CHROMA:
        media->vic_chroma = (uint64_t)data << 8;
        MEDIA_LOG(media, svc,
                         "VP_MEDIA_VIC_REG fd=%d method=0x%x value=0x%llx\n",
                         channel->fd, media->method0,
                         (unsigned long long)media->vic_chroma);
        break;
    default:
        if (media->commands <= VP_MEDIA_LOG_BUDGET) {
            MEDIA_LOG(media, svc,
                             "VP_MEDIA_VIC_REG fd=%d method=0x%x data=0x%08x\n",
                             channel->fd, media->method0, data);
        }
        break;
    }
}

static void media_execute_word(VPGuestSvc *svc, VPGuestSession *state,
                               VPGuestGpuChannel *channel, uint32_t state_offset,
                               uint32_t data, int is_nvdec, int is_vic) {
    VPGuestMediaParse *media = &channel->media;
    ++media->commands;
    if (media->current_class == VP_MEDIA_CLASS_CONTROL) {
        if (media->instrument && media->commands <= VP_MEDIA_LOG_BUDGET) {
            MEDIA_LOG(media, svc, "VP_MEDIA_CONTROL fd=%d method=0x%x "
                     "data=0x%08x\n", channel->fd, state_offset, data);
        }
        return;
    }
    if (is_nvdec && media->current_class == VP_MEDIA_CLASS_NVDEC) {
        media_nvdec_method(svc, state, channel, state_offset, data);
        return;
    }
    if (is_vic && media->current_class == VP_MEDIA_CLASS_VIC) {
        media_vic_method(svc, state, channel, state_offset, data);
        return;
    }
    if (media->instrument && media->commands <= 16u) {
        MEDIA_LOG(media, svc,
                  "VP_MEDIA_CLASS_OTHER fd=%d class=0x%x method=0x%x "
                  "data=0x%08x nvdec=%d vic=%d\n",
                  channel->fd, media->current_class, state_offset, data,
                  is_nvdec, is_vic);
    }
}

void vp_media_parse_command_buffer(VPGuestSvc *svc, VPGuestSession *state,
                                   VPGuestGpuChannel *channel,
                                   const uint32_t *words, uint32_t count,
                                   int is_nvdec, int is_vic) {
    if (!svc || !state || !channel || !words || !count) return;
    /* The walk always runs: the media-video hooks need the register state on
     * every submit, not only while the verbose trace is enabled. */
    media_refresh_instrument(channel);
    VPGuestMediaParse *media = &channel->media;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t raw = words[i];
        if (media->mask != 0u) {
            uint32_t bit = 0;
            while (bit < 32u && ((media->mask >> bit) & 1u) == 0u) ++bit;
            if (bit == 32u) {
                media->mask = 0u;
                continue;
            }
            media->mask &= ~(1u << bit);
            media_execute_word(svc, state, channel, media->offset + bit, raw,
                               is_nvdec, is_vic);
            continue;
        }
        if (media->count != 0u) {
            --media->count;
            media_execute_word(svc, state, channel, media->offset, raw,
                               is_nvdec, is_vic);
            if (media->incrementing) ++media->offset;
            continue;
        }
        const uint32_t mode = (raw >> 28) & 0x0fu;
        const uint32_t value = raw & 0xffffu;
        const uint32_t method = (raw >> 16) & 0x0fffu;
        switch (mode) {
        case 0u: /* SetClass */
            media->mask = value & 0x3fu;
            media->offset = method;
            media->current_class = (value >> 6) & 0x3ffu;
            if (media->instrument) {
                MEDIA_LOG(media, svc,
                          "VP_MEDIA_SETCLASS fd=%d class=0x%x method=0x%x "
                          "mask=0x%x index=%u\n",
                          channel->fd, media->current_class, method,
                          media->mask, i);
            }
            break;
        case 1u: /* Incrementing */
        case 2u: /* NonIncrementing */
            media->count = value;
            media->offset = method;
            media->incrementing = (mode == 1u) ? 1u : 0u;
            break;
        case 3u: /* Mask */
            media->mask = value;
            media->offset = method;
            break;
        case 4u: /* Immediate */
            media_execute_word(svc, state, channel, method, value & 0x0fffu,
                               is_nvdec, is_vic);
            break;
        default:
            if (media->instrument) {
                MEDIA_LOG(media, svc,
                          "VP_MEDIA_MODE_UNSUPPORTED fd=%d mode=%u "
                          "word=0x%08x index=%u\n",
                          channel->fd, mode, raw, i);
            }
            return;
        }
    }
}

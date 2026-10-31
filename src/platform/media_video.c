#include "media_video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>

#include "media_decode.h"

/* The operational path is opt-in. media-decode.enable opens the nvhost nodes
 * (the transport model from 0031); this marker additionally executes decode
 * and composite. Without it every hook returns before touching hardware. */
#define VP_MEDIA_VIDEO_MARKER "ux0:data/mhgu-vita-port/media-video.enable"
#define VP_MEDIA_INSTRUMENT_MARKER "ux0:data/mhgu-vita-port/media-instrument.enable"

/* The title's NVDEC stream is 960x544 (60x34 macroblocks) and the VIC output
 * surfaces are 1024-pitch (0x88000 luma / 0x44000 chroma allocations). */
#define VP_MEDIA_VIDEO_REFS 6u
#define VP_MEDIA_VIDEO_QUEUE 4u
#define VP_MEDIA_VIDEO_ES_BYTES 0x100000u
#define VP_MEDIA_VIDEO_OUT_BYTES 0x100000u
#define VP_MEDIA_VIDEO_MAX_STREAM 0xF0000u
#define VP_MEDIA_VIDEO_DUMP_LIMIT 40u
#define VP_MEDIA_VIDEO_HEADER_BYTES 512u
/* Sentinel stored in SceUID fields for heap-backed fallback allocations. A
 * real memblock id is never negative; -2 keeps free() distinct from
 * sceKernelFreeMemBlock(). */
#define VP_MEDIA_VIDEO_HEAP_ID (-2)

typedef struct {
    int marker_checked;
    int enabled;
    int instrument_checked;
    int instrument;
    int initialized;
    int init_failed;
    int lib_loaded;
    int decoder_created;
    uint32_t width;
    uint32_t height;
    uint32_t failed_width;
    uint32_t failed_height;
    SceUID frame_mem_id;
    void *frame_mem;
    uint32_t frame_mem_size;
    SceUID es_id;
    void *es;
    uint32_t es_size;
    SceUID out_id[VP_MEDIA_VIDEO_QUEUE];
    void *out[VP_MEDIA_VIDEO_QUEUE];
    uint32_t out_size;
    SceAvcdecCtrl ctrl;
    SceAvcdecPicture picture;
    SceAvcdecPicture *picture_list[1];
    uint32_t first_submit;
    uint32_t next_write;
    uint32_t ready[VP_MEDIA_VIDEO_QUEUE];
    uint32_t ready_head;
    uint32_t ready_count;
    int have_frame;
    uint32_t decode_count;
    uint32_t composite_count;
    uint32_t no_output_count;
    uint32_t drop_count;
    uint32_t error_count;
    uint32_t dump_count;
    volatile int busy;
} VPMediaVideo;

static VPMediaVideo g_video;

/* Zig-zag scan order used by the picture-info scaling lists (same tables as
 * the reference decoder's H264BitWriter). */
static const uint8_t media_zigzag_direct[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static const uint8_t media_zigzag_scan[16] = {
    0, 1, 4, 8,  5, 2, 3, 6,
    9, 12, 13, 10, 7, 11, 14, 15,
};

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

static int32_t media_sfield(uint64_t value, unsigned shift, unsigned bits) {
    const uint32_t raw =
        (uint32_t)((value >> shift) & ((1ull << bits) - 1ull));
    const uint32_t sign = 1u << (bits - 1u);
    return (int32_t)((raw ^ sign) - sign);
}

static int media_video_enabled(void) {
    VPMediaVideo *v = &g_video;
    if (v->marker_checked) return v->enabled;
    v->marker_checked = 1;
    FILE *marker = fopen(VP_MEDIA_VIDEO_MARKER, "rb");
    if (marker != NULL) {
        fclose(marker);
        v->enabled = 1;
    }
    return v->enabled;
}

int vp_media_video_enabled(void) {
    return media_video_enabled();
}

static int media_video_instrument(void) {
    VPMediaVideo *v = &g_video;
    if (v->instrument_checked) return v->instrument;
    v->instrument_checked = 1;
    FILE *marker = fopen(VP_MEDIA_INSTRUMENT_MARKER, "rb");
    if (marker != NULL) {
        fclose(marker);
        v->instrument = 1;
    }
    return v->instrument;
}

/* One decode or composite at a time. The title's own fences normally order
 * the two engines; if a second thread enters anyway, skip the hook rather than
 * corrupting the shared decoder or frame queue. */
static int media_video_enter(VPGuestSvc *svc, const char *what) {
    VPMediaVideo *v = &g_video;
    if (__sync_lock_test_and_set(&v->busy, 1)) {
        vp_guest_svc_log(svc, "VP_MEDIA_VIDEO_CONTENDED what=%s\n", what);
        return 0;
    }
    return 1;
}

static void media_video_leave(void) {
    __sync_lock_release(&g_video.busy);
}

/* B123: per-call timing for the movie decode/composite hooks. The opening
 * movie advances only about five frames per second while the present loop is
 * near thirty, so the cost has to be attributed to the decoder API and the
 * composite copy rather than guessed from the GXM frame budget. Bounded so a
 * long session cannot flood the log. */
static unsigned media_time_logs;
static int media_time_should_log(void) {
    const unsigned n = media_time_logs++;
    return n < 2000u || (n % 32u) == 0u;
}

static void *media_video_alloc(VPGuestSvc *svc, const char *tag, SceUID *id_out,
                               const char *name, uint32_t bytes,
                               uint32_t *size_out) {
    const uint32_t size = (bytes + 0xFFFu) & ~0xFFFu;
    SceUID id = sceKernelAllocMemBlock(
        name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, size, NULL);
    if (id < 0) {
        const int phycont_rc = id;
        /* Vita3K can run out of the aligned PHYCONT window after the loader's
         * large guest pool is mapped. Fall back to ordinary user memory so the
         * emulator path can still exercise the codec; hardware requires
         * physical contiguity and will reject this at decode time with an
         * explicit error, never silently. */
        id = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                    size, NULL);
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_ALLOC_FALLBACK what=%s size=%u "
                         "phycont_rc=0x%08x rw_rc=0x%08x\n",
                         tag, size, (unsigned)phycont_rc, (unsigned)id);
        if (id < 0) {
            /* The loader's guest pool occupies the whole 32-bit address
             * window under Vita3K, so even a plain memblock can fail. Heap
             * memory is enough for the emulator (its codec ignores physical
             * contiguity); hardware rejects it at decode time with an
             * explicit NOT_PHY_CONTINUOUS_MEMORY error. */
            void *base = malloc(size);
            vp_guest_svc_log(svc,
                             "VP_MEDIA_VIDEO_ALLOC_HEAP what=%s size=%u "
                             "base=%p\n", tag, size, base);
            if (base == NULL) return NULL;
            *id_out = VP_MEDIA_VIDEO_HEAP_ID;
            if (size_out) *size_out = size;
            return base;
        }
    }
    void *base = NULL;
    if (sceKernelGetMemBlockBase(id, &base) < 0 || base == NULL) {
        sceKernelFreeMemBlock(id);
        return NULL;
    }
    *id_out = id;
    if (size_out) *size_out = size;
    return base;
}

static void media_video_free(SceUID *id, void **base) {
    if (*base != NULL && *id == VP_MEDIA_VIDEO_HEAP_ID) {
        free(*base);
    } else if (*base != NULL && *id >= 0) {
        sceKernelFreeMemBlock(*id);
    }
    *id = -1;
    *base = NULL;
}

static void media_video_shutdown(VPGuestSvc *svc) {
    VPMediaVideo *v = &g_video;
    if (v->decoder_created) {
        int rc = sceAvcdecDeleteDecoder(&v->ctrl);
        vp_guest_svc_log(svc, "VP_MEDIA_VIDEO_DELETE_DECODER rc=0x%08x\n", rc);
        v->decoder_created = 0;
    }
    if (v->lib_loaded) {
        int rc = sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
        vp_guest_svc_log(svc, "VP_MEDIA_VIDEO_TERM_LIB rc=0x%08x\n", rc);
        v->lib_loaded = 0;
    }
    media_video_free(&v->frame_mem_id, &v->frame_mem);
    media_video_free(&v->es_id, &v->es);
    for (uint32_t i = 0; i < VP_MEDIA_VIDEO_QUEUE; ++i)
        media_video_free(&v->out_id[i], &v->out[i]);
    v->initialized = 0;
    v->next_write = 0;
    v->ready_head = 0;
    v->ready_count = 0;
    v->have_frame = 0;
    v->first_submit = 1;
}

/* Initialize the hardware decoder for a stream at least as large as
 * width x height. A later, larger stream tears the old decoder down and
 * creates a new one; the first AU of the new stream is an IDR with
 * frame_number 0, so the reference state is rebuilt from scratch. */
static int media_video_init(VPGuestSvc *svc, uint32_t width, uint32_t height) {
    VPMediaVideo *v = &g_video;
    if (v->initialized && width <= v->width && height <= v->height) return 1;
    if (v->init_failed && width <= v->failed_width
        && height <= v->failed_height) {
        return 0;
    }
    if (v->initialized) media_video_shutdown(svc);

    v->width = width;
    v->height = height;
    v->first_submit = 1;

    /* The decoder cannot exceed the Vita's supported image size. */
    if (width == 0 || height == 0 || width > 1920u || height > 1088u
        || (uint64_t)width * height > 1280ull * 720ull) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_INIT_FAIL step=size w=%u h=%u\n",
                         width, height);
        v->init_failed = 1;
        v->failed_width = width;
        v->failed_height = height;
        return 0;
    }

    int rc = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
    vp_guest_svc_log(svc,
                     "VP_MEDIA_VIDEO_SYSMODULE id=0x%x rc=0x%08x\n",
                     (unsigned)SCE_SYSMODULE_AVCDEC, rc);
    if (rc < 0) {
        /* Vita3K's sysmodule bookkeeping can reject the load for modules it
         * does not ship while still providing the codec exports. The library
         * init below is the authoritative check; on hardware a genuinely
         * unavailable module fails there. */
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_SYSMODULE_WARN continuing to "
                         "library init\n");
    } else {
        v->lib_loaded = 1;
    }

    SceVideodecQueryInitInfoHwAvcdec init;
    memset(&init, 0, sizeof(init));
    init.size = sizeof(init);
    init.horizontal = width;
    init.vertical = height;
    init.numOfRefFrames = VP_MEDIA_VIDEO_REFS;
    init.numOfStreams = 1;
    rc = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    if (rc < 0) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_INIT_FAIL step=library rc=0x%08x\n",
                         rc);
        goto fail;
    }

    SceAvcdecQueryDecoderInfo query;
    query.horizontal = width;
    query.vertical = height;
    query.numOfRefFrames = VP_MEDIA_VIDEO_REFS;
    SceAvcdecDecoderInfo info;
    memset(&info, 0, sizeof(info));
    rc = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query,
                                      &info);
    if (rc < 0 || info.frameMemSize == 0) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_INIT_FAIL step=memsize rc=0x%08x "
                         "size=%u\n", rc, info.frameMemSize);
        goto fail;
    }

    v->frame_mem = media_video_alloc(svc, "frame", &v->frame_mem_id,
                                     "vp-avcdec-frame", info.frameMemSize,
                                     &v->frame_mem_size);
    if (v->frame_mem == NULL) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_INIT_FAIL step=frame_alloc size=%u\n",
                         info.frameMemSize);
        goto fail;
    }

    memset(&v->ctrl, 0, sizeof(v->ctrl));
    v->ctrl.frameBuf.pBuf = v->frame_mem;
    v->ctrl.frameBuf.size = info.frameMemSize;
    rc = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &v->ctrl, &query);
    if (rc < 0) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_INIT_FAIL step=create rc=0x%08x\n",
                         rc);
        goto fail;
    }
    v->decoder_created = 1;

    v->es = media_video_alloc(svc, "es", &v->es_id, "vp-avcdec-es",
                              VP_MEDIA_VIDEO_ES_BYTES, &v->es_size);
    if (v->es == NULL) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_INIT_FAIL step=es_alloc size=%u\n",
                         VP_MEDIA_VIDEO_ES_BYTES);
        goto fail;
    }
    for (uint32_t i = 0; i < VP_MEDIA_VIDEO_QUEUE; ++i) {
        v->out[i] = media_video_alloc(svc, "out", &v->out_id[i],
                                      "vp-avcdec-out",
                                      VP_MEDIA_VIDEO_OUT_BYTES, &v->out_size);
        if (v->out[i] == NULL) {
            vp_guest_svc_log(svc,
                             "VP_MEDIA_VIDEO_INIT_FAIL step=out_alloc index=%u\n",
                             i);
            goto fail;
        }
    }

    memset(&v->picture, 0, sizeof(v->picture));
    v->picture.size = sizeof(v->picture);
    v->picture.frame.pixelType =
        SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER;
    v->picture.frame.framePitch = width;
    v->picture.frame.frameWidth = width;
    v->picture.frame.frameHeight = height;
    v->picture.frame.pPicture[0] = NULL;
    v->picture.frame.pPicture[1] = NULL;
    v->picture_list[0] = &v->picture;

    v->initialized = 1;
    v->init_failed = 0;
    vp_guest_svc_log(svc,
                     "VP_MEDIA_VIDEO_INIT w=%u h=%u refs=%u frame_mem=%u "
                     "es=%u out=%u queue=%u\n",
                     width, height, VP_MEDIA_VIDEO_REFS,
                     v->frame_mem_size, v->es_size, v->out_size,
                     VP_MEDIA_VIDEO_QUEUE);
    return 1;

fail:
    v->init_failed = 1;
    v->failed_width = width;
    v->failed_height = height;
    media_video_shutdown(svc);
    return 0;
}

/* --- Annex B parameter-set synthesis ------------------------------------- */

typedef struct {
    uint8_t *data;
    uint32_t capacity;
    uint32_t size;
    uint32_t bit;
    int overflow;
} VPBitWriter;

static void media_bw_bit(VPBitWriter *w, uint32_t bit) {
    if (w->overflow) return;
    if (w->bit == 0) {
        if (w->size >= w->capacity) {
            w->overflow = 1;
            return;
        }
        w->data[w->size] = 0;
    }
    if (bit & 1u) w->data[w->size] |= (uint8_t)(0x80u >> w->bit);
    if (++w->bit == 8u) {
        w->bit = 0;
        ++w->size;
    }
}

static void media_bw_u(VPBitWriter *w, uint32_t value, uint32_t bits) {
    for (uint32_t i = 0; i < bits; ++i)
        media_bw_bit(w, (value >> (bits - 1u - i)) & 1u);
}

static void media_bw_ue(VPBitWriter *w, uint32_t value) {
    const uint32_t code = value + 1u;
    uint32_t bits = 0;
    while ((code >> bits) != 0u) ++bits;
    for (uint32_t i = 1; i < bits; ++i) media_bw_bit(w, 0);
    media_bw_u(w, code, bits);
}

static void media_bw_se(VPBitWriter *w, int32_t value) {
    const uint32_t magnitude =
        value < 0 ? (uint32_t)(-value) : (uint32_t)value;
    const uint32_t code =
        (magnitude << 1) - (value > 0 ? 1u : 0u);
    media_bw_ue(w, code);
}

static void media_bw_start_nal(VPBitWriter *w, uint32_t type) {
    media_bw_u(w, 1, 24); /* 0x000001 start code */
    media_bw_bit(w, 0);   /* forbidden_zero_bit */
    media_bw_u(w, 3, 2);  /* nal_ref_idc */
    media_bw_u(w, type, 5);
}

static void media_bw_scaling_list(VPBitWriter *w, const uint8_t *list,
                                  uint32_t start, uint32_t count) {
    const uint8_t *zigzag = count == 16u ? media_zigzag_scan
                                         : media_zigzag_direct;
    uint8_t last = 8;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t value = list[start + zigzag[i]];
        media_bw_se(w, (int32_t)value - (int32_t)last);
        last = value;
    }
}

static void media_bw_end(VPBitWriter *w) {
    media_bw_bit(w, 1); /* rbsp_stop_one_bit */
    while (w->bit != 0) media_bw_bit(w, 0);
}

/* Compose the Annex B SPS/PPS pair from the NVDEC picture-info record. The
 * layout and field order follow the reference H264BitWriter: profile 100
 * (High), level 31, six reference frames, no cropping and no VUI. */
static uint32_t media_build_headers(const uint8_t *info, uint8_t *dst,
                                    uint32_t capacity) {
    const uint8_t *ps = info + 0x58;
    const uint64_t flags = media_u64(ps + 0x58);
    const uint32_t log2_poc_minus4 = media_u32(ps + 0x00);
    const uint32_t delta_poc = media_u32(ps + 0x04);
    const uint32_t frame_mbs_only = media_u32(ps + 0x08);
    const uint32_t width_mbs = media_u32(ps + 0x0C);
    const uint32_t height_map = media_u32(ps + 0x10);
    const uint32_t entropy = media_u32(ps + 0x18);
    const uint32_t pic_order_present = media_u32(ps + 0x1C);
    const uint32_t num_l0 = media_u32(ps + 0x20);
    const uint32_t num_l1 = media_u32(ps + 0x24);
    const uint32_t deblock = media_u32(ps + 0x28);
    const uint32_t redundant = media_u32(ps + 0x2C);
    const uint32_t transform8 = media_u32(ps + 0x30);
    const uint32_t log2_frame_num_minus4 = (uint32_t)((flags >> 8) & 0xFu);
    const uint32_t chroma_format = (uint32_t)((flags >> 12) & 3u);
    const uint32_t poc_type = (uint32_t)((flags >> 14) & 3u);

    if (width_mbs == 0 || height_map == 0) return 0;

    VPBitWriter w;
    memset(&w, 0, sizeof(w));
    w.data = dst;
    w.capacity = capacity;

    /* SPS */
    media_bw_start_nal(&w, 7);
    media_bw_u(&w, 100, 8); /* profile_idc */
    media_bw_u(&w, 0, 8);   /* constraint_set flags */
    media_bw_u(&w, 31, 8);  /* level_idc */
    media_bw_ue(&w, 0);     /* seq_parameter_set_id */
    media_bw_ue(&w, chroma_format);
    if (chroma_format == 3u) media_bw_bit(&w, 0);
    media_bw_ue(&w, 0); /* bit_depth_luma_minus8 */
    media_bw_ue(&w, 0); /* bit_depth_chroma_minus8 */
    media_bw_bit(&w, 0); /* qpprime_y_zero_transform_bypass_flag */
    media_bw_bit(&w, 0); /* seq_scaling_matrix_present_flag */
    media_bw_ue(&w, log2_frame_num_minus4);
    media_bw_ue(&w, poc_type);
    if (poc_type == 0u) {
        media_bw_ue(&w, log2_poc_minus4);
    } else if (poc_type == 1u) {
        media_bw_bit(&w, delta_poc != 0u);
        media_bw_se(&w, 0);
        media_bw_se(&w, 0);
        media_bw_ue(&w, 0);
    }
    const uint32_t pic_height =
        height_map / (frame_mbs_only ? 1u : 2u);
    media_bw_ue(&w, VP_MEDIA_VIDEO_REFS); /* max_num_ref_frames */
    media_bw_bit(&w, 0);                  /* gaps_in_frame_num_allowed */
    media_bw_ue(&w, width_mbs - 1u);
    media_bw_ue(&w, pic_height - 1u);
    media_bw_bit(&w, frame_mbs_only != 0u);
    if (!frame_mbs_only) media_bw_bit(&w, (uint32_t)(flags & 1u));
    media_bw_bit(&w, (uint32_t)((flags >> 1) & 1u)); /* direct_8x8 */
    media_bw_bit(&w, 0); /* frame_cropping_flag */
    media_bw_bit(&w, 0); /* vui_parameters_present_flag */
    media_bw_end(&w);

    /* PPS */
    media_bw_start_nal(&w, 8);
    media_bw_ue(&w, 0); /* pic_parameter_set_id */
    media_bw_ue(&w, 0); /* seq_parameter_set_id */
    media_bw_bit(&w, entropy != 0u);
    media_bw_bit(&w, pic_order_present != 0u);
    media_bw_ue(&w, 0); /* num_slice_groups_minus1 */
    media_bw_ue(&w, num_l0);
    media_bw_ue(&w, num_l1);
    media_bw_bit(&w, (uint32_t)((flags >> 2) & 1u)); /* weighted_pred */
    media_bw_u(&w, (uint32_t)((flags >> 32) & 3u), 2);
    media_bw_se(&w, media_sfield(flags, 16, 6)); /* pic_init_qp_minus26 */
    media_bw_se(&w, 0);                          /* pic_init_qs_minus26 */
    media_bw_se(&w, media_sfield(flags, 22, 5)); /* chroma_qp_index_offset */
    media_bw_bit(&w, deblock != 0u);
    media_bw_bit(&w, (uint32_t)((flags >> 3) & 1u));
    media_bw_bit(&w, redundant != 0u);
    media_bw_bit(&w, transform8 != 0u);
    media_bw_bit(&w, 1); /* pic_scaling_matrix_present_flag */
    for (uint32_t i = 0; i < 6u; ++i) {
        media_bw_bit(&w, 1);
        media_bw_scaling_list(&w, info + 0x1C0, i * 16u, 16u);
    }
    if (transform8 != 0u) {
        for (uint32_t i = 0; i < 2u; ++i) {
            media_bw_bit(&w, 1);
            media_bw_scaling_list(&w, info + 0x220, i * 64u, 64u);
        }
    }
    media_bw_se(&w, media_sfield(flags, 27, 5));
    media_bw_end(&w);

    if (w.overflow) return 0;
    return w.size;
}

/* --- decode and composite ------------------------------------------------- */

static int media_video_dump(VPMediaVideo *v, uint32_t slot,
                            uint32_t index) {
    char path[80];
    snprintf(path, sizeof(path),
             "ux0:data/mhgu-vita-port/media-frame-%03u.nv12", index);
    FILE *file = fopen(path, "wb");
    if (file == NULL) return 0;
    const uint8_t *luma = (const uint8_t *)v->out[slot];
    const uint32_t luma_size = v->width * v->height;
    const uint32_t chroma_size = v->width * (v->height / 2u);
    const size_t wrote_luma = fwrite(luma, 1, luma_size, file);
    const size_t wrote_chroma =
        fwrite(luma + luma_size, 1, chroma_size, file);
    fclose(file);
    return wrote_luma == luma_size && wrote_chroma == chroma_size;
}

int vp_media_video_decode(VPGuestSvc *svc, VPGuestSession *state, uint32_t fd,
                          uint64_t picture_info, uint64_t bitstream) {
    if (!media_video_enabled()) return 0;
    if (!media_video_enter(svc, "decode")) return 0;

    VPMediaVideo *v = &g_video;
    const uint64_t t_entry = sceKernelGetProcessTimeWide();
    uint64_t t_api = 0u, t_after = 0u;
    int result = -1;
    uint32_t info_cpu = 0;
    if (picture_info == 0u || bitstream == 0u
        || !vp_media_resolve(svc, state, picture_info, 0x2A0u, &info_cpu)) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_DECODE_MISS fd=%u step=picture "
                         "pic=0x%llx bs=0x%llx\n",
                         fd, (unsigned long long)picture_info,
                         (unsigned long long)bitstream);
        ++v->error_count;
        goto done;
    }
    const uint8_t *info = (const uint8_t *)(uintptr_t)info_cpu;
    const uint32_t stream_len = media_u32(info + 0x48u);
    const uint8_t *ps = info + 0x58u;
    const uint32_t width = media_u32(ps + 0x0Cu) * 16u;
    const uint32_t height =
        media_u32(ps + 0x10u) * 16u * (media_u32(ps + 0x08u) ? 1u : 2u);
    if (stream_len == 0u || stream_len > VP_MEDIA_VIDEO_MAX_STREAM
        || width == 0u || height == 0u) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_DECODE_MISS fd=%u step=geometry "
                         "len=%u w=%u h=%u\n", fd, stream_len, width, height);
        ++v->error_count;
        goto done;
    }
    if (!media_video_init(svc, width, height)) {
        ++v->error_count;
        goto done;
    }
    if (stream_len + VP_MEDIA_VIDEO_HEADER_BYTES > v->es_size) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_DECODE_MISS fd=%u step=es_size "
                         "len=%u size=%u\n", fd, stream_len, v->es_size);
        ++v->error_count;
        goto done;
    }
    uint32_t stream_cpu = 0;
    if (!vp_media_resolve(svc, state, bitstream, stream_len, &stream_cpu)) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_DECODE_MISS fd=%u step=bitstream "
                         "va=0x%llx len=%u\n", fd,
                         (unsigned long long)bitstream, stream_len);
        ++v->error_count;
        goto done;
    }

    const uint64_t flags = media_u64(ps + 0x58u);
    const uint32_t frame_number = (uint32_t)((flags >> 46) & 0xFFFFu);
    uint32_t headers = 0;
    if (v->first_submit || frame_number == 0u) {
        headers = media_build_headers(info, (uint8_t *)v->es, v->es_size);
        if (headers == 0u) {
            vp_guest_svc_log(svc,
                             "VP_MEDIA_VIDEO_DECODE_MISS fd=%u step=headers "
                             "frame=%u\n", fd, frame_number);
            ++v->error_count;
            goto done;
        }
        /* A new stream / loop starts from an IDR; do not show queued frames
         * from the previous stream. The decoder keeps its reference state
         * until the new SPS/PPS and IDR arrive. */
        v->ready_head = 0;
        v->ready_count = 0;
        v->next_write = 0;
        v->have_frame = 0;
        v->first_submit = 0;
    }
    memcpy((uint8_t *)v->es + headers,
           (const void *)(uintptr_t)stream_cpu, stream_len);

    if (v->ready_count == VP_MEDIA_VIDEO_QUEUE) {
        /* The ring advances in lockstep with next_write: when full, the oldest
         * queued slot is exactly the slot about to be reused. */
        v->ready_head = (v->ready_head + 1u) % VP_MEDIA_VIDEO_QUEUE;
        --v->ready_count;
        ++v->drop_count;
    }
    const uint32_t slot = v->next_write;

    v->picture.size = sizeof(v->picture);
    v->picture.frame.pixelType =
        SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER;
    v->picture.frame.framePitch = v->width;
    v->picture.frame.frameWidth = v->width;
    v->picture.frame.frameHeight = v->height;
    v->picture.frame.pPicture[0] = v->out[slot];
    v->picture.frame.pPicture[1] = NULL;

    SceAvcdecAu au;
    memset(&au, 0, sizeof(au));
    au.pts.upper = 0xFFFFFFFFu;
    au.pts.lower = 0xFFFFFFFFu;
    au.dts.upper = 0xFFFFFFFFu;
    au.dts.lower = 0xFFFFFFFFu;
    au.es.pBuf = v->es;
    au.es.size = headers + stream_len;

    SceAvcdecArrayPicture array;
    memset(&array, 0, sizeof(array));
    array.numOfElm = 1;
    array.pPicture = v->picture_list;

    t_api = sceKernelGetProcessTimeWide();
    const int rc = sceAvcdecDecode(&v->ctrl, &au, &array);
    t_after = sceKernelGetProcessTimeWide();
    if (rc < 0) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_DECODE_FAIL fd=%u frame=%u len=%u "
                         "headers=%u rc=0x%08x\n",
                         fd, frame_number, stream_len, headers, rc);
        ++v->error_count;
        goto done;
    }
    if (array.numOfOutput > 0u) {
        v->ready[(v->ready_head + v->ready_count) % VP_MEDIA_VIDEO_QUEUE] =
            slot;
        ++v->ready_count;
        v->next_write = (slot + 1u) % VP_MEDIA_VIDEO_QUEUE;
        v->have_frame = 1;
        __sync_synchronize();
        ++v->decode_count;
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_DECODE fd=%u n=%u frame=%u len=%u "
                         "headers=%u slot=%u queue=%u\n",
                         fd, v->decode_count, frame_number, stream_len,
                         headers, slot, v->ready_count);
    } else {
        ++v->no_output_count;
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_DECODE_NO_OUTPUT fd=%u frame=%u "
                         "len=%u headers=%u\n",
                         fd, frame_number, stream_len, headers);
    }
    result = 0;

done:
    if (media_time_should_log()) {
        const uint64_t t_end = sceKernelGetProcessTimeWide();
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_DECODE_TIME fd=%u n=%u rc=%d "
                         "total_us=%llu api_us=%llu t_us=%llu thread=0x%x\n",
                         fd, v->decode_count, result,
                         (unsigned long long)(t_end - t_entry),
                         (unsigned long long)(t_after ? t_after - t_api : 0u),
                         (unsigned long long)t_end,
                         svc->current_thread_handle);
    }
    media_video_leave();
    return result;
}

int vp_media_video_composite(VPGuestSvc *svc, VPGuestSession *state, uint32_t fd,
                             uint64_t config, uint64_t luma, uint64_t chroma) {
    if (!media_video_enabled()) return 0;
    if (!media_video_enter(svc, "composite")) return 0;

    VPMediaVideo *v = &g_video;
    const uint64_t t_entry = sceKernelGetProcessTimeWide();
    uint64_t t_copy = 0u, t_copied = 0u;
    int result = -1;
    if (!v->initialized || !v->have_frame || v->ready_count == 0u) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_COMPOSITE_MISS fd=%u step=no_frame "
                         "init=%d ready=%u\n", fd, v->initialized,
                         v->ready_count);
        result = 0;
        goto done;
    }
    if (config == 0u || luma == 0u || chroma == 0u) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_COMPOSITE_MISS fd=%u step=address "
                         "config=0x%llx luma=0x%llx chroma=0x%llx\n", fd,
                         (unsigned long long)config,
                         (unsigned long long)luma,
                         (unsigned long long)chroma);
        ++v->error_count;
        goto done;
    }
    uint32_t config_cpu = 0;
    if (!vp_media_resolve(svc, state, config + 0x20u, 8u, &config_cpu)) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_COMPOSITE_MISS fd=%u step=config "
                         "va=0x%llx\n", fd, (unsigned long long)config);
        ++v->error_count;
        goto done;
    }
    const uint64_t raw = media_u64((const uint8_t *)(uintptr_t)config_cpu);
    const uint32_t pixel_format = (uint32_t)(raw & 0x7Fu);
    const uint32_t block_kind = (uint32_t)((raw >> 11) & 0xFu);
    const uint32_t surface_width = (uint32_t)((raw >> 32) & 0x3FFFu) + 1u;
    const uint32_t surface_height = (uint32_t)((raw >> 46) & 0x3FFFu) + 1u;
    if (pixel_format != 0x44u) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_COMPOSITE_MISS fd=%u step=format "
                         "pixel=0x%x block=%u w=%u h=%u\n", fd, pixel_format,
                         block_kind, surface_width, surface_height);
        result = 0;
        goto done;
    }
    if (block_kind != 0u) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_COMPOSITE_MISS fd=%u step=block "
                         "kind=%u bh=%u w=%u h=%u\n", fd, block_kind,
                         (uint32_t)((raw >> 15) & 0xFu), surface_width,
                         surface_height);
        ++v->error_count;
        goto done;
    }
    if (surface_width == 0u || surface_height < 2u
        || (surface_height & 1u) != 0u) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_COMPOSITE_MISS fd=%u step=surface "
                         "w=%u h=%u\n", fd, surface_width, surface_height);
        ++v->error_count;
        goto done;
    }

    const uint32_t aligned_width = (surface_width + 0xFFu) & ~0xFFu;
    const uint32_t luma_size = aligned_width * surface_height;
    const uint32_t chroma_size = aligned_width * (surface_height / 2u);
    uint32_t luma_cpu = 0;
    uint32_t chroma_cpu = 0;
    if (!vp_media_resolve(svc, state, luma, luma_size, &luma_cpu)
        || !vp_media_resolve(svc, state, chroma, chroma_size, &chroma_cpu)) {
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_COMPOSITE_MISS fd=%u step=output "
                         "luma=0x%llx luma_size=%u chroma=0x%llx "
                         "chroma_size=%u\n", fd, (unsigned long long)luma,
                         luma_size, (unsigned long long)chroma, chroma_size);
        ++v->error_count;
        goto done;
    }

    const uint32_t slot = v->ready[v->ready_head];
    const uint8_t *frame = (const uint8_t *)v->out[slot];
    const uint8_t *chroma_frame = frame + v->width * v->height;
    const uint32_t copy_width =
        v->width < surface_width ? v->width : surface_width;
    const uint32_t copy_height =
        v->height < surface_height ? v->height : surface_height;

    t_copy = sceKernelGetProcessTimeWide();
    for (uint32_t y = 0; y < copy_height; ++y) {
        uint8_t *dst = (uint8_t *)(uintptr_t)luma_cpu + y * aligned_width;
        memcpy(dst, frame + y * v->width, copy_width);
        if (copy_width < aligned_width)
            memset(dst + copy_width, 0, aligned_width - copy_width);
    }
    for (uint32_t y = copy_height; y < surface_height; ++y)
        memset((uint8_t *)(uintptr_t)luma_cpu + y * aligned_width, 0,
               aligned_width);

    const uint32_t copy_chroma_height = copy_height / 2u;
    for (uint32_t y = 0; y < copy_chroma_height; ++y) {
        uint8_t *dst = (uint8_t *)(uintptr_t)chroma_cpu + y * aligned_width;
        memcpy(dst, chroma_frame + y * v->width, copy_width);
        if (copy_width < aligned_width)
            memset(dst + copy_width, 0, aligned_width - copy_width);
    }
    for (uint32_t y = copy_chroma_height; y < surface_height / 2u; ++y)
        memset((uint8_t *)(uintptr_t)chroma_cpu + y * aligned_width, 0,
               aligned_width);
    t_copied = sceKernelGetProcessTimeWide();

    v->ready_head = (v->ready_head + 1u) % VP_MEDIA_VIDEO_QUEUE;
    --v->ready_count;
    ++v->composite_count;
    __sync_synchronize();

    /* The opening second of the title movie is genuinely black, so dumping
     * only the first three composites cannot tell a decode bug from the
     * content. Dump the head plus every eighth frame and report the luma
     * range in the log. */
    const int dump_frame = v->composite_count <= 3u
        || (v->composite_count % 8u) == 0u;
    if (media_video_instrument() && dump_frame
        && v->dump_count < VP_MEDIA_VIDEO_DUMP_LIMIT) {
        const uint8_t *pixels = (const uint8_t *)v->out[slot];
        const uint32_t pixel_count = v->width * v->height;
        uint32_t lo = 255u;
        uint32_t hi = 0u;
        uint64_t sum = 0u;
        for (uint32_t i = 0; i < pixel_count; ++i) {
            const uint32_t y = pixels[i];
            if (y < lo) lo = y;
            if (y > hi) hi = y;
            sum += y;
        }
        if (media_video_dump(v, slot, v->composite_count)) {
            vp_guest_svc_log(svc,
                             "VP_MEDIA_VIDEO_DUMP fd=%u index=%u w=%u h=%u "
                             "y=%u..%u avg=%u\n",
                             fd, v->composite_count, v->width, v->height,
                             lo, hi, (uint32_t)(sum / pixel_count));
        } else {
            vp_guest_svc_log(svc,
                             "VP_MEDIA_VIDEO_DUMP_FAIL fd=%u index=%u "
                             "w=%u h=%u\n",
                             fd, v->composite_count, v->width, v->height);
        }
        ++v->dump_count;
    }
    vp_guest_svc_log(svc,
                     "VP_MEDIA_VIDEO_COMPOSITE fd=%u n=%u slot=%u "
                     "w=%u h=%u pitch=%u copy=%ux%u queue=%u drops=%u\n",
                     fd, v->composite_count, slot, surface_width,
                     surface_height, aligned_width, copy_width, copy_height,
                     v->ready_count, v->drop_count);
    result = 0;

done:
    if (media_time_should_log()) {
        const uint64_t t_end = sceKernelGetProcessTimeWide();
        vp_guest_svc_log(svc,
                         "VP_MEDIA_VIDEO_COMPOSITE_TIME fd=%u n=%u rc=%d "
                         "total_us=%llu copy_us=%llu t_us=%llu thread=0x%x\n",
                         fd, v->composite_count, result,
                         (unsigned long long)(t_end - t_entry),
                         (unsigned long long)(t_copied ? t_copied - t_copy : 0u),
                         (unsigned long long)t_end,
                         svc->current_thread_handle);
    }
    media_video_leave();
    return result;
}

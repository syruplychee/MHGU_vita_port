#include "native_movie.h"

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/videodec.h>

#include "guest_storage.h"
#include "guest_svc.h"
#include "media_video.h"
#include "movie_control.h"

/* Framework-level movie presentation for the title's MP4 streams.
 *
 * The original guest pipeline remains responsible for opening the file and
 * creating the hardware decoder. This module reads the same MP4 from the
 * discovered ROM-tree path, demuxes its H.264 video samples, feeds Annex B
 * access units to the already-created SceAvcdec decoder, converts NV12 to the
 * display format and presents at the stream cadence. The guest's display and
 * audio output are suppressed while active; the guest movie state is then
 * stopped through the title's own command-8 path so normal game flow resumes.
 *
 * Disabled unless native-movie.enable exists. No game data is stored here. */

#define VP_NATIVE_MOVIE_MARKER \
    "ux0:data/mhgu-vita-port/native-movie.enable"
#define VP_NATIVE_MOVIE_LIMIT_MARKER \
    "ux0:data/mhgu-vita-port/native-movie.limit-ms"
#define VP_NATIVE_MOVIE_TREE_ROOT VP_GUEST_STORAGE_TREE_PATH
#define VP_NATIVE_MOVIE_MAX_PATH 512u
#define VP_NATIVE_MOVIE_MAX_MOOV (8u * 1024u * 1024u)
#define VP_NATIVE_MOVIE_MAX_SAMPLES 20000u
#define VP_NATIVE_MOVIE_MAX_SAMPLE (1024u * 1024u)
#define VP_NATIVE_MOVIE_MAX_ES (1024u * 1024u)
#define VP_NATIVE_MOVIE_DISPLAY_W 960u
#define VP_NATIVE_MOVIE_DISPLAY_H 544u
#define VP_NATIVE_MOVIE_FRAME_BYTES \
    (VP_NATIVE_MOVIE_DISPLAY_W * VP_NATIVE_MOVIE_DISPLAY_H * 4u)

typedef struct {
    SceUID id;
    void *base;
    uint32_t size;
} VPMovieBlock;

typedef struct {
    int fd;
    uint64_t file_size;
    uint32_t video_timescale;
    uint32_t video_duration;
    uint32_t width;
    uint32_t height;
    uint32_t sample_count;
    uint32_t max_sample;
    uint32_t nal_length_size;
    uint8_t *sps_pps;
    uint32_t sps_pps_size;
    uint32_t *sample_size;
    uint64_t *sample_offset;
    uint32_t *sample_time;
    uint8_t *sample_sync;
    uint8_t *moov;
    uint32_t moov_size;
} VPMovieMp4;

static volatile int g_enabled = -1;
static volatile int g_active;
static volatile int g_presenting;
static volatile int g_stop;
static volatile uint32_t g_presented;
static volatile uint32_t g_decoded_samples;
static volatile uint32_t g_limit_ms;
static char g_relative_path[VP_NATIVE_MOVIE_MAX_PATH];
static char g_host_path[VP_NATIVE_MOVIE_MAX_PATH];
static VPGuestSvc *g_svc;
static SceUID g_thread = -1;
static VPMovieBlock g_display[2];
static uint32_t g_display_index;
static int g_display_ever_presented;

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t rd64(const uint8_t *p) {
    return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}
static uint32_t fourcc(const char *s) {
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16)
         | ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3];
}

static void movie_log(const char *format, ...) {
    if (g_svc == NULL) return;
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    vp_guest_svc_log(g_svc, "%s", buffer);
}

/* --- MP4 box walking ----------------------------------------------------- */

typedef struct {
    uint32_t type;
    uint32_t payload;
    uint32_t size;
} VPMovieBox;

static int box_at(const uint8_t *buf, uint32_t size, uint32_t off,
                  VPMovieBox *box) {
    if (off > size || size - off < 8u) return -1;
    uint32_t box_size = rd32(buf + off);
    uint32_t header = 8u;
    if (box_size == 1u) {
        if (size - off < 16u) return -1;
        uint64_t large = rd64(buf + off + 8u);
        if (large < 16u || large > (uint64_t)(size - off)) return -1;
        box_size = (uint32_t)large;
        header = 16u;
    } else if (box_size == 0u) {
        box_size = size - off;
    }
    if (box_size < header || box_size > size - off) return -1;
    box->type = rd32(buf + off + 4u);
    box->payload = off + header;
    box->size = box_size;
    return 0;
}

static int find_box(const uint8_t *buf, uint32_t start, uint32_t end,
                    uint32_t type, VPMovieBox *out) {
    uint32_t off = start;
    while (off + 8u <= end) {
        VPMovieBox box;
        if (box_at(buf, end, off, &box) != 0) return -1;
        if (box.type == type) {
            if (out != NULL) *out = box;
            return 0;
        }
        off += box.size;
    }
    return -1;
}

static uint32_t box_end(const VPMovieBox *box) {
    return box->payload + box->size - 8u;
}

static int parse_mvhd(const uint8_t *p, uint32_t size,
                      uint32_t *timescale, uint32_t *duration) {
    if (size < 20u) return -1;
    if (p[0] == 0u) {
        *timescale = rd32(p + 12u);
        *duration = rd32(p + 16u);
    } else {
        if (size < 28u) return -1;
        *timescale = rd32(p + 20u);
        uint64_t d = rd64(p + 24u);
        *duration = d > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)d;
    }
    return *timescale != 0u ? 0 : -1;
}

static int parse_avcc(const uint8_t *p, uint32_t size, VPMovieMp4 *mp4) {
    if (size < 7u) return -1;
    mp4->nal_length_size = (uint32_t)(p[4] & 3u) + 1u;
    const uint32_t num_sps = p[5] & 0x1Fu;
    if (mp4->nal_length_size < 1u || mp4->nal_length_size > 4u) return -1;
    uint32_t off = 6u;
    uint8_t *out = (uint8_t *)malloc(4096u);
    if (out == NULL) return -1;
    uint32_t used = 0;
    for (uint32_t i = 0; i < num_sps; ++i) {
        if (off + 2u > size) goto fail;
        uint32_t n = rd16(p + off);
        off += 2u;
        if (n == 0u || n > size - off || used + 4u + n > 4096u) goto fail;
        out[used++] = 0; out[used++] = 0; out[used++] = 0; out[used++] = 1;
        memcpy(out + used, p + off, n);
        used += n;
        off += n;
    }
    if (off >= size) goto fail;
    uint32_t num_pps = p[off++];
    for (uint32_t i = 0; i < num_pps; ++i) {
        if (off + 2u > size) goto fail;
        uint32_t n = rd16(p + off);
        off += 2u;
        if (n == 0u || n > size - off || used + 4u + n > 4096u) goto fail;
        out[used++] = 0; out[used++] = 0; out[used++] = 0; out[used++] = 1;
        memcpy(out + used, p + off, n);
        used += n;
        off += n;
    }
    mp4->sps_pps = out;
    mp4->sps_pps_size = used;
    return used != 0u ? 0 : -1;
fail:
    free(out);
    return -1;
}

static int parse_video_track(const uint8_t *moov, uint32_t moov_size,
                             VPMovieMp4 *mp4) {
    uint32_t off = 0;
    while (off + 8u <= moov_size) {
        VPMovieBox trak;
        if (box_at(moov, moov_size, off, &trak) != 0) return -1;
        if (trak.type == fourcc("trak")) {
            VPMovieBox mdia, hdlr, mdhd, minf, stbl;
            if (find_box(moov, trak.payload, box_end(&trak),
                         fourcc("mdia"), &mdia) != 0)
                goto next_trak;
            if (find_box(moov, mdia.payload, box_end(&mdia),
                         fourcc("hdlr"), &hdlr) != 0)
                goto next_trak;
            if (hdlr.size < 20u ||
                rd32(moov + hdlr.payload + 8u) != fourcc("vide"))
                goto next_trak;
            if (find_box(moov, mdia.payload, box_end(&mdia),
                         fourcc("mdhd"), &mdhd) != 0)
                return -1;
            if (parse_mvhd(moov + mdhd.payload, mdhd.size - 8u,
                           &mp4->video_timescale, &mp4->video_duration) != 0)
                return -1;
            if (find_box(moov, mdia.payload, box_end(&mdia),
                         fourcc("minf"), &minf) != 0)
                return -1;
            if (find_box(moov, minf.payload, box_end(&minf),
                         fourcc("stbl"), &stbl) != 0)
                return -1;

            VPMovieBox stsd, stsz, stsc, stts, stco, co64, stss;
            if (find_box(moov, stbl.payload, box_end(&stbl),
                         fourcc("stsd"), &stsd) != 0)
                return -1;
            if (find_box(moov, stbl.payload, box_end(&stbl),
                         fourcc("stsz"), &stsz) != 0)
                return -1;
            if (find_box(moov, stbl.payload, box_end(&stbl),
                         fourcc("stsc"), &stsc) != 0)
                return -1;
            if (find_box(moov, stbl.payload, box_end(&stbl),
                         fourcc("stts"), &stts) != 0)
                return -1;
            const int has_co64 =
                find_box(moov, stbl.payload, box_end(&stbl),
                         fourcc("co64"), &co64) == 0;
            if (!has_co64 &&
                find_box(moov, stbl.payload, box_end(&stbl),
                         fourcc("stco"), &stco) != 0)
                return -1;
            const int has_stss =
                find_box(moov, stbl.payload, box_end(&stbl),
                         fourcc("stss"), &stss) == 0;

            if (stsd.size < 24u) return -1;
            uint32_t entry = stsd.payload + 8u;
            if (entry + 8u > box_end(&stsd)) return -1;
            uint32_t entry_size = rd32(moov + entry);
            uint32_t entry_type = rd32(moov + entry + 4u);
            if (entry_type != fourcc("avc1") || entry_size < 86u
                || entry + entry_size > box_end(&stsd))
                return -1;
            uint32_t content = entry + 8u;
            mp4->width = rd16(moov + content + 24u);
            mp4->height = rd16(moov + content + 26u);
            if (mp4->width == 0u || mp4->height == 0u) return -1;
            VPMovieBox avcc;
            if (find_box(moov, content + 78u, entry + entry_size,
                         fourcc("avcC"), &avcc) != 0)
                return -1;
            if (parse_avcc(moov + avcc.payload, avcc.size - 8u, mp4) != 0)
                return -1;

            if (stsz.size < 20u) return -1;
            uint32_t fixed = rd32(moov + stsz.payload + 4u);
            uint32_t count = rd32(moov + stsz.payload + 8u);
            if (count == 0u || count > VP_NATIVE_MOVIE_MAX_SAMPLES) return -1;
            mp4->sample_count = count;
            mp4->sample_size = (uint32_t *)calloc(count, sizeof(uint32_t));
            if (mp4->sample_size == NULL) return -1;
            if (fixed != 0u) {
                for (uint32_t i = 0; i < count; ++i)
                    mp4->sample_size[i] = fixed;
            } else {
                if (stsz.size < 20u + (uint64_t)count * 4u) return -1;
                for (uint32_t i = 0; i < count; ++i)
                    mp4->sample_size[i] = rd32(moov + stsz.payload + 12u + i * 4u);
            }
            for (uint32_t i = 0; i < count; ++i) {
                if (mp4->sample_size[i] == 0u
                    || mp4->sample_size[i] > VP_NATIVE_MOVIE_MAX_SAMPLE)
                    return -1;
                if (mp4->sample_size[i] > mp4->max_sample)
                    mp4->max_sample = mp4->sample_size[i];
            }

            uint32_t chunk_count;
            uint64_t *chunk_offset;
            if (has_co64) {
                if (co64.size < 16u) return -1;
                chunk_count = rd32(moov + co64.payload + 4u);
                if (co64.size < 16u + (uint64_t)chunk_count * 8u) return -1;
                chunk_offset = (uint64_t *)calloc(chunk_count, sizeof(uint64_t));
                if (chunk_offset == NULL) return -1;
                for (uint32_t i = 0; i < chunk_count; ++i)
                    chunk_offset[i] = rd64(moov + co64.payload + 8u + i * 8u);
            } else {
                if (stco.size < 16u) return -1;
                chunk_count = rd32(moov + stco.payload + 4u);
                if (stco.size < 16u + (uint64_t)chunk_count * 4u) return -1;
                chunk_offset = (uint64_t *)calloc(chunk_count, sizeof(uint64_t));
                if (chunk_offset == NULL) return -1;
                for (uint32_t i = 0; i < chunk_count; ++i)
                    chunk_offset[i] = rd32(moov + stco.payload + 8u + i * 4u);
            }

            if (stsc.size < 16u) return -1;
            uint32_t stsc_count = rd32(moov + stsc.payload + 4u);
            if (stsc.size < 16u + (uint64_t)stsc_count * 12u) return -1;
            mp4->sample_offset = (uint64_t *)calloc(count, sizeof(uint64_t));
            mp4->sample_time = (uint32_t *)calloc(count, sizeof(uint32_t));
            mp4->sample_sync = (uint8_t *)calloc(count, sizeof(uint8_t));
            if (mp4->sample_offset == NULL || mp4->sample_time == NULL
                || mp4->sample_sync == NULL)
                return -1;

            uint32_t sample = 0;
            for (uint32_t i = 0; i < stsc_count && sample < count; ++i) {
                const uint8_t *e = moov + stsc.payload + 8u + i * 12u;
                uint32_t first_chunk = rd32(e);
                uint32_t per_chunk = rd32(e + 4u);
                uint32_t next_first = (i + 1u < stsc_count)
                    ? rd32(moov + stsc.payload + 8u + (i + 1u) * 12u)
                    : chunk_count + 1u;
                if (first_chunk == 0u || per_chunk == 0u
                    || next_first <= first_chunk)
                    return -1;
                for (uint32_t chunk = first_chunk;
                     chunk < next_first && sample < count; ++chunk) {
                    if (chunk > chunk_count) return -1;
                    uint64_t pos = chunk_offset[chunk - 1u];
                    for (uint32_t j = 0; j < per_chunk && sample < count; ++j) {
                        mp4->sample_offset[sample] = pos;
                        pos += mp4->sample_size[sample];
                        ++sample;
                    }
                }
            }
            free(chunk_offset);
            if (sample != count) return -1;

            if (stts.size < 16u) return -1;
            uint32_t stts_count = rd32(moov + stts.payload + 4u);
            if (stts.size < 16u + (uint64_t)stts_count * 8u) return -1;
            uint32_t at = 0;
            uint32_t time = 0;
            for (uint32_t i = 0; i < stts_count && at < count; ++i) {
                const uint8_t *e = moov + stts.payload + 8u + i * 8u;
                uint32_t n = rd32(e);
                uint32_t delta = rd32(e + 4u);
                if (n > count - at) n = count - at;
                for (uint32_t j = 0; j < n; ++j) {
                    mp4->sample_time[at++] = time;
                    time += delta;
                }
            }
            if (at != count) return -1;

            if (has_stss) {
                if (stss.size < 16u) return -1;
                uint32_t n = rd32(moov + stss.payload + 4u);
                if (stss.size < 16u + (uint64_t)n * 4u) return -1;
                for (uint32_t i = 0; i < n; ++i) {
                    uint32_t s = rd32(moov + stss.payload + 8u + i * 4u);
                    if (s >= 1u && s <= count) mp4->sample_sync[s - 1u] = 1u;
                }
            } else {
                memset(mp4->sample_sync, 1, count);
            }
            return 0;
        }
next_trak:
        off += trak.size;
    }
    return -1;
}

static int mp4_read_exact(int fd, uint64_t offset, void *dst, uint32_t bytes) {
    if (sceIoLseek(fd, (SceOff)offset, SCE_SEEK_SET) < 0) return -1;
    uint8_t *p = (uint8_t *)dst;
    while (bytes != 0u) {
        int rc = sceIoRead(fd, p, bytes);
        if (rc <= 0) return -1;
        p += rc;
        bytes -= (uint32_t)rc;
    }
    return 0;
}

static int mp4_open(VPMovieMp4 *mp4, const char *path) {
    memset(mp4, 0, sizeof(*mp4));
    mp4->fd = -1;
    mp4->fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (mp4->fd < 0) {
        movie_log("VP_NATIVE_MOVIE_OPEN_FAIL path=%s rc=0x%08x\n",
                  path, (unsigned)mp4->fd);
        return -1;
    }
    SceOff size = sceIoLseek(mp4->fd, 0, SCE_SEEK_END);
    if (size <= 0) {
        movie_log("VP_NATIVE_MOVIE_SIZE_FAIL path=%s rc=0x%08x\n",
                  path, (unsigned)size);
        return -1;
    }
    mp4->file_size = (uint64_t)size;
    uint64_t pos = 0;
    uint8_t header[16];
    while (pos + 8u <= mp4->file_size) {
        if (mp4_read_exact(mp4->fd, pos, header, 8u) != 0) return -1;
        uint32_t box_size = rd32(header);
        uint32_t type = rd32(header + 4u);
        uint32_t hdr = 8u;
        if (box_size == 1u) {
            if (mp4->file_size - pos < 16u) return -1;
            if (mp4_read_exact(mp4->fd, pos + 8u, header + 8u, 8u) != 0)
                return -1;
            box_size = (uint32_t)rd64(header + 8u);
            hdr = 16u;
        } else if (box_size == 0u) {
            box_size = (uint32_t)(mp4->file_size - pos);
        }
        if (box_size < hdr || (uint64_t)box_size > mp4->file_size - pos)
            return -1;
        if (type == fourcc("moov")) {
            if (box_size - hdr > VP_NATIVE_MOVIE_MAX_MOOV) return -1;
            mp4->moov_size = box_size - hdr;
            mp4->moov = (uint8_t *)malloc(mp4->moov_size);
            if (mp4->moov == NULL) return -1;
            if (mp4_read_exact(mp4->fd, pos + hdr, mp4->moov,
                               mp4->moov_size) != 0)
                return -1;
            break;
        }
        pos += box_size;
    }
    if (mp4->moov == NULL) return -1;
    return parse_video_track(mp4->moov, mp4->moov_size, mp4);
}

static void mp4_close(VPMovieMp4 *mp4) {
    if (mp4->fd >= 0) sceIoClose(mp4->fd);
    free(mp4->moov);
    free(mp4->sps_pps);
    free(mp4->sample_size);
    free(mp4->sample_offset);
    free(mp4->sample_time);
    free(mp4->sample_sync);
    memset(mp4, 0, sizeof(*mp4));
    mp4->fd = -1;
}

/* --- display buffers and conversion -------------------------------------- */

static int alloc_block(VPMovieBlock *block, const char *name, uint32_t bytes) {
    uint32_t size = (bytes + 0xFFFu) & ~0xFFFu;
    SceUID id = sceKernelAllocMemBlock(
        name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL);
    if (id < 0)
        id = sceKernelAllocMemBlock(
            name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW, size, NULL);
    if (id >= 0) {
        void *base = NULL;
        if (sceKernelGetMemBlockBase(id, &base) == 0) {
            block->id = id;
            block->base = base;
            block->size = size;
            return 0;
        }
        sceKernelFreeMemBlock(id);
    }
    void *base = malloc(size);
    if (base == NULL) return -1;
    block->id = -1;
    block->base = base;
    block->size = size;
    return 0;
}

static void free_block(VPMovieBlock *block) {
    if (block->base == NULL) return;
    if (block->id >= 0) sceKernelFreeMemBlock(block->id);
    else free(block->base);
    block->base = NULL;
    block->id = -1;
    block->size = 0;
}

static int display_alloc(void) {
    if (g_display[0].base != NULL && g_display[1].base != NULL) return 0;
    for (unsigned i = 0; i < 2u; ++i) {
        if (g_display[i].base == NULL
            && alloc_block(&g_display[i], "vp-native-fb",
                           VP_NATIVE_MOVIE_FRAME_BYTES) != 0)
            return -1;
    }
    return 0;
}

static void display_free(void) {
    for (unsigned i = 0; i < 2u; ++i) free_block(&g_display[i]);
}

static int display_present(const uint32_t *pixels) {
    if (pixels == NULL) return -1;
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = (void *)pixels;
    fb.pitch = VP_NATIVE_MOVIE_DISPLAY_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = VP_NATIVE_MOVIE_DISPLAY_W;
    fb.height = VP_NATIVE_MOVIE_DISPLAY_H;
    return sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static inline uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* NV12 packed raster -> A8B8G8R8 with nearest-neighbour scaling to 960x544. */
static void nv12_to_rgba(const uint8_t *nv12, uint32_t width,
                         uint32_t coded_height, uint32_t visible_height,
                         uint32_t pitch, uint32_t *dst) {
    const uint8_t *y_plane = nv12;
    const uint8_t *uv_plane = nv12 + (size_t)pitch * coded_height;
    if (visible_height == 0u || visible_height > coded_height)
        visible_height = coded_height;
    for (uint32_t dy = 0; dy < VP_NATIVE_MOVIE_DISPLAY_H; ++dy) {
        uint32_t sy = (uint32_t)(((uint64_t)dy * visible_height)
                                 / VP_NATIVE_MOVIE_DISPLAY_H);
        if (sy >= visible_height) sy = visible_height - 1u;
        const uint8_t *yrow = y_plane + (size_t)sy * pitch;
        const uint8_t *uvrow = uv_plane + (size_t)(sy >> 1) * pitch;
        uint32_t *drow = dst + (size_t)dy * VP_NATIVE_MOVIE_DISPLAY_W;
        for (uint32_t dx = 0; dx < VP_NATIVE_MOVIE_DISPLAY_W; ++dx) {
            uint32_t sx = (uint32_t)(((uint64_t)dx * width)
                                     / VP_NATIVE_MOVIE_DISPLAY_W);
            if (sx >= width) sx = width - 1u;
            const int c = yrow[sx];
            const int u = (int)uvrow[(sx & ~1u) + 0u] - 128;
            const int v = (int)uvrow[(sx & ~1u) + 1u] - 128;
            const int r = (298 * c + 409 * v + 128) >> 8;
            const int g = (298 * c - 100 * u - 208 * v + 128) >> 8;
            const int b = (298 * c + 516 * u + 128) >> 8;
            /* SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 is A,B,G,R from the MSB. */
            drow[dx] = 0xFF000000u | ((uint32_t)clamp_u8(b) << 16)
                     | ((uint32_t)clamp_u8(g) << 8) | (uint32_t)clamp_u8(r);
        }
    }
}

/* --- decode/presentation thread ------------------------------------------ */

static void sleep_until(uint64_t due_us) {
    for (;;) {
        if (g_stop) return;
        uint64_t now = sceKernelGetProcessTimeWide();
        if (now >= due_us) return;
        uint64_t wait = due_us - now;
        if (wait > 10000u) wait = 10000u;
        sceKernelDelayThread((SceUInt)wait);
    }
}

static uint32_t read_limit_ms(void) {
    FILE *file = fopen(VP_NATIVE_MOVIE_LIMIT_MARKER, "rb");
    if (file == NULL) return 0u;
    unsigned value = 0;
    if (fscanf(file, "%u", &value) != 1) value = 0u;
    fclose(file);
    if (value > 3600000u) value = 3600000u;
    return value;
}

static void request_title_stop(const char *reason) {
    const uint32_t state_addr = vp_movie_umovie_state;
    if (state_addr < 0x80000000u || state_addr >= 0x90000000u) {
        movie_log("VP_NATIVE_MOVIE_STOP_SKIP reason=%s state=0x%08x\n",
                  reason, state_addr);
        return;
    }
    volatile uint32_t *command =
        (volatile uint32_t *)(uintptr_t)(state_addr + 0x2Cu);
    const uint32_t previous = *command;
    *command = 8u; /* uMovie command 8: stop and release the decoder. */
    __sync_synchronize();
    movie_log("VP_NATIVE_MOVIE_STOP reason=%s state=0x%08x old=%u new=8\n",
              reason, state_addr, previous);
    /* The title's player thread polls this word every millisecond. Keep the
     * guest decode/composite hooks disabled until its real cleanup returns the
     * state to idle (9); otherwise a stale NVDEC thread could feed a P-frame
     * into the decoder after the native stream advanced it. */
    unsigned polls = 0;
    uint32_t idle = previous;
    for (; polls < 3000u; ++polls) {
        idle = *command;
        if (idle == 9u) break;
        sceKernelDelayThread(1000);
    }
    movie_log("VP_NATIVE_MOVIE_IDLE state=%u polls=%u\n", idle, polls);
}

static int native_movie_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    VPMovieMp4 mp4;
    memset(&mp4, 0, sizeof(mp4));
    mp4.fd = -1;
    g_presented = 0;
    g_decoded_samples = 0;
    const uint64_t begun = sceKernelGetProcessTimeWide();
    (void)begun;
    movie_log("VP_NATIVE_MOVIE_START path=%s limit_ms=%u\n",
              g_host_path, g_limit_ms);

    if (display_alloc() != 0) {
        movie_log("VP_NATIVE_MOVIE_FAIL step=display_alloc\n");
        goto done;
    }
    if (mp4_open(&mp4, g_host_path) != 0) {
        movie_log("VP_NATIVE_MOVIE_FAIL step=mp4_open\n");
        goto done;
    }
    movie_log("VP_NATIVE_MOVIE_MP4 w=%u h=%u samples=%u timescale=%u "
              "duration=%u max_sample=%u nal=%u sps_pps=%u\n",
              mp4.width, mp4.height, mp4.sample_count, mp4.video_timescale,
              mp4.video_duration, mp4.max_sample, mp4.nal_length_size,
              mp4.sps_pps_size);

    uint8_t *sample = (uint8_t *)malloc(mp4.max_sample + 64u);
    uint8_t *es = (uint8_t *)malloc(VP_NATIVE_MOVIE_MAX_ES);
    if (sample == NULL || es == NULL) {
        movie_log("VP_NATIVE_MOVIE_FAIL step=buffer_alloc\n");
        free(sample);
        free(es);
        goto done;
    }

    const uint64_t duration_us = (uint64_t)mp4.video_duration * 1000000ull
                               / mp4.video_timescale;
    uint64_t period_us = duration_us / mp4.sample_count;
    if (period_us == 0u) period_us = 1u;

    uint32_t next_sample = 0;
    uint64_t start_us = 0;
    uint64_t next_due = 0;
    uint32_t decoded = 0;
    int failed = 0;
    while (!g_stop && next_sample < mp4.sample_count) {
        int produced = 0;
        uint32_t width = 0, coded_height = 0, visible_height = 0, pitch = 0;
        const uint8_t *frame = NULL;
        while (!g_stop && next_sample < mp4.sample_count && !produced) {
            const uint32_t this_sample = next_sample++;
            const uint32_t sample_size = mp4.sample_size[this_sample];
            if (sample_size > mp4.max_sample
                || mp4_read_exact(mp4.fd, mp4.sample_offset[this_sample],
                                  sample, sample_size) != 0) {
                movie_log("VP_NATIVE_MOVIE_FAIL step=read_sample index=%u\n",
                          this_sample);
                failed = 1;
                break;
            }
            uint32_t used = 0;
            if (mp4.sps_pps_size + 16u > VP_NATIVE_MOVIE_MAX_ES) {
                failed = 1;
                break;
            }
            memcpy(es, mp4.sps_pps, mp4.sps_pps_size);
            used = mp4.sps_pps_size;
            uint32_t offset = 0;
            while (offset < sample_size) {
                uint32_t nal_len = 0;
                if (mp4.nal_length_size > sample_size - offset) {
                    used = 0;
                    break;
                }
                for (uint32_t i = 0; i < mp4.nal_length_size; ++i)
                    nal_len = (nal_len << 8) | sample[offset + i];
                offset += mp4.nal_length_size;
                if (nal_len == 0u || nal_len > sample_size - offset
                    || used + 4u + nal_len > VP_NATIVE_MOVIE_MAX_ES) {
                    used = 0;
                    break;
                }
                es[used++] = 0; es[used++] = 0; es[used++] = 0; es[used++] = 1;
                memcpy(es + used, sample + offset, nal_len);
                used += nal_len;
                offset += nal_len;
            }
            if (used == 0u) {
                movie_log("VP_NATIVE_MOVIE_FAIL step=annexb index=%u size=%u\n",
                          this_sample, sample_size);
                failed = 1;
                break;
            }
            int rc;
            unsigned busy_retries = 0;
            do {
                rc = vp_media_video_native_decode(es, used, &width,
                                                  &coded_height,
                                                  &visible_height, &pitch,
                                                  &frame);
                if (rc == -2) {
                    if (++busy_retries > 5000u) {
                        movie_log("VP_NATIVE_MOVIE_FAIL step=decoder_busy\n");
                        failed = 1;
                        break;
                    }
                    sceKernelDelayThread(1000);
                }
            } while (rc == -2 && !g_stop && !failed);
            if (failed) break;
            if (rc < 0) {
                movie_log("VP_NATIVE_MOVIE_FAIL step=decode index=%u rc=0x%08x "
                          "size=%u\n", this_sample, (unsigned)rc, used);
                failed = 1;
                break;
            }
            ++decoded;
            g_decoded_samples = decoded;
            if (rc > 0) produced = 1;
        }
        if (failed || g_stop) break;
        if (!produced) break;

        const uint64_t now = sceKernelGetProcessTimeWide();
        if (start_us == 0u) {
            start_us = now;
            next_due = now;
        }
        sleep_until(next_due);
        if (g_stop) break;

        uint32_t *dst = (uint32_t *)g_display[g_display_index].base;
        nv12_to_rgba(frame, width, coded_height, visible_height, pitch, dst);
        if (display_present(dst) < 0) {
            movie_log("VP_NATIVE_MOVIE_FAIL step=display_present\n");
            failed = 1;
            break;
        }
        g_display_index ^= 1u;
        g_display_ever_presented = 1;
        ++g_presented;
        next_due += period_us;

        const uint64_t after = sceKernelGetProcessTimeWide();
        if (after > next_due + period_us * 2u) next_due = after + period_us;
        if (g_limit_ms != 0u
            && after - start_us >= (uint64_t)g_limit_ms * 1000ull) {
            movie_log("VP_NATIVE_MOVIE_LIMIT reached_ms=%u\n", g_limit_ms);
            break;
        }
    }

    const uint64_t ended = sceKernelGetProcessTimeWide();
    const uint64_t elapsed = ended > start_us ? ended - start_us : 0u;
    movie_log("VP_NATIVE_MOVIE_DONE decoded=%u presented=%u samples=%u "
              "elapsed_us=%llu rate_milli=%llu failed=%d stop=%d\n",
              decoded, g_presented, mp4.sample_count,
              (unsigned long long)elapsed,
              (unsigned long long)(elapsed
                  ? (uint64_t)g_presented * 1000000000ull / elapsed : 0u),
              failed, g_stop);
    free(sample);
    free(es);
    if (!failed && !g_stop && g_presented != 0u)
        request_title_stop("completed");

done:
    mp4_close(&mp4);
    /* A buffer that has been handed to sceDisplaySetFrameBuf must not be
     * freed while the display may still scan it out. Keep it for the next
     * movie; only release buffers that never reached the display. */
    if (!g_display_ever_presented) display_free();
    g_presenting = 0;
    g_active = 0;
    movie_log("VP_NATIVE_MOVIE_EXIT presented=%u\n", g_presented);
    g_thread = -1;
    return 0;
}

/* --- public entry points ------------------------------------------------- */

int vp_native_movie_enabled(void) {
    int enabled = g_enabled;
    if (enabled >= 0) return enabled;
    FILE *file = fopen(VP_NATIVE_MOVIE_MARKER, "rb");
    enabled = file != NULL;
    if (file != NULL) fclose(file);
    g_enabled = enabled;
    if (enabled && g_svc != NULL)
        movie_log("VP_NATIVE_MOVIE_MODE enabled=1\n");
    return enabled;
}

void vp_native_movie_note_path(const char *relative_path) {
    if (relative_path == NULL || strstr(relative_path, ".mp4") == NULL)
        return;
    size_t n = strlen(relative_path);
    if (n == 0u || n >= sizeof(g_relative_path)) return;
    if (strcmp(g_relative_path, relative_path) == 0) return;
    memcpy(g_relative_path, relative_path, n + 1u);
    if (g_svc != NULL)
        movie_log("VP_NATIVE_MOVIE_PATH %s\n", g_relative_path);
}

int vp_native_movie_start(VPGuestSvc *svc) {
    if (svc == NULL) return -1;
    g_svc = svc;
    if (!vp_native_movie_enabled()) return -1;
    if (g_active) return 0; /* One presentation; a second stream waits. */
    if (g_relative_path[0] == 0) {
        movie_log("VP_NATIVE_MOVIE_FAIL step=no_path\n");
        return -1;
    }
    if (strlen(g_relative_path) + strlen(VP_NATIVE_MOVIE_TREE_ROOT) + 2u
        >= sizeof(g_host_path)) {
        movie_log("VP_NATIVE_MOVIE_FAIL step=path_too_long\n");
        return -1;
    }
    snprintf(g_host_path, sizeof(g_host_path), "%s/%s",
             VP_NATIVE_MOVIE_TREE_ROOT, g_relative_path);
    g_limit_ms = read_limit_ms();
    g_stop = 0;
    g_presented = 0;
    g_decoded_samples = 0;
    g_display_index = 0;
    g_presenting = 1;
    g_active = 1;
    SceUID thread = sceKernelCreateThread(
        "vp-native-movie", native_movie_thread, 0x10000100, 0x10000, 0,
        SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    if (thread < 0) {
        movie_log("VP_NATIVE_MOVIE_FAIL step=thread rc=0x%08x\n",
                  (unsigned)thread);
        g_presenting = 0;
        g_active = 0;
        return -1;
    }
    g_thread = thread;
    if (sceKernelStartThread(thread, 0, NULL) < 0) {
        movie_log("VP_NATIVE_MOVIE_FAIL step=thread_start\n");
        sceKernelDeleteThread(thread);
        g_thread = -1;
        g_presenting = 0;
        g_active = 0;
        return -1;
    }
    return 0;
}

void vp_native_movie_request_stop(void) {
    g_stop = 1;
}

int vp_native_movie_active(void) {
    return g_active;
}

int vp_native_movie_presenting(void) {
    return g_presenting;
}

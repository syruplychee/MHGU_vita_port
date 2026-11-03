#include "guest_svc.h"
#include <psp2/kernel/sysmem.h>
#include "audio_render.h"
#include "audio_sink.h"
#include "gpu_stream.h"
#include "engine_layout.h"
#include "engine_full.h"
#include "gpu_texture.h"
#include "texture_snapshot.h"
#include "gpu_indices.h"
#include "vertex_window.h"
#include "gxm_depth.h"
#include "gxm_depth_view.h"
#include "gxm_runtime.h"
#include "media_decode.h"
#include "media_video.h"
#include "native_movie.h"
#include "time_calendar.h"
#include <psp2/ctrl.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VP_GUEST_TRACE_PATH "ux0:data/mhgu-vita-port/sdk-svc.log"

static int svc_play_perf_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        FILE *marker = fopen("ux0:data/mhgu-vita-port/play-perf.enable", "rb");
        enabled = marker != NULL;
        if (marker) fclose(marker);
    }
    return enabled;
}

/* Only request headers are sampled. Result/error records, GPU rejects,
 * savedata, input receipts and periodic performance summaries remain intact.
 * Keep the first eight and every 256th header of each kind for correlation. */
static int svc_play_trace_sample(const char *format) {
    static const char *const headers[] = {
        "SVC_IPC_AUDIO_REQUEST ", "SVC_IPC_CONTROL_POINTER ", "SVC_IPC ",
        "AUDIO_MIXER_SPLITTER "
    };
    static unsigned counts[4];
    if (!svc_play_perf_enabled()) return 1;
    for (unsigned i = 0; i < 4u; ++i) {
        if (strncmp(format, headers[i], strlen(headers[i])) == 0) {
            unsigned n = ++counts[i];
            return n <= 8u || (n & 255u) == 0u;
        }
    }
    return 1;
}

static int svc_play_success_sample(const char *line) {
    static unsigned counts[3];
    unsigned kind;
    if (strstr(line, "call=GetAudioDeviceServiceWithRevisionInfo ") &&
        strstr(line, " result=0 ")) kind = 0u;
    else if (strstr(line, "SVC_IPC_AUDIODEV command=10 call=GetActiveAudioDeviceName ") == line)
        kind = 1u;
    else if (strncmp(line, "SVC_RESET_SIGNAL ", 17u) == 0 &&
             (strstr(line, " result=0\n") || strstr(line, " result=0 "))) kind = 2u;
    else return 1;
    unsigned n = ++counts[kind];
    return n <= 8u || (n & 255u) == 0u;
}

static void slog(VPGuestSvc *svc, const char *format, ...) {
    va_list args;
    if (!svc_play_trace_sample(format)) return;
    if (svc_play_perf_enabled()) {
        /* Unbuffered vfprintf emits many emulated writes for one format.
         * Format once and issue one write per sink, preserving the full line.
         * Oversize records retain the original path without truncation. */
        char line[1024];
        va_start(args, format);
        int length = vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        if (length >= 0 && (size_t)length < sizeof(line)) {
            if (!svc_play_success_sample(line)) return;
            fwrite(line, 1u, (size_t)length, svc->log);
            if (svc->trace) fwrite(line, 1u, (size_t)length, svc->trace);
            return;
        }
    }
    va_start(args, format);
    vfprintf(svc->log, format, args);
    va_end(args);
    if (svc->trace) {
        va_start(args, format);
        vfprintf(svc->trace, format, args);
        va_end(args);
    }
}

/* Closing is the only portable flush of the emulated file layer, so the
 * duplicate trace is closed after every event and reopened for the next one. */
static void sflush(VPGuestSvc *svc) {
    static uint64_t next_flush;
    if (svc_play_perf_enabled()) {
        uint64_t now = sceKernelGetProcessTimeWide();
        if (now < next_flush) return;
        next_flush = now + 250000u;
    }
    fflush(svc->log);
    if (svc->trace) {
        fclose(svc->trace);
        svc->trace = fopen(VP_GUEST_TRACE_PATH, "a");
    }
}

/* Diagnostic used only for the M5 probe thread: one transient file handle per
 * event, so a platform-call abort leaves the last completed step on the host
 * even though the primary log's tail is still in the emulated file buffer. */
static void durable_event(const char *format, ...) {
    FILE *f = fopen("ux0:data/mhgu-vita-port/audio-probe-events.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fclose(f);
}

/* Serialize the whole decoder entry and display operations. Lock acquisition
 * itself must yield service state: otherwise a second GPU caller could hold
 * that lock while waiting for an entry whose worker is trying to reacquire it.
 * Requests remain synchronous; vertices/CBs/textures are consumed before the
 * submitting SVC returns and before its fence is retired. */
static SceUID gpu_execution_gate = -1;
static struct {
    uint64_t entries, draws, decode_us, draw_prepare_us, gxm_call_us;
    /* B104: the command worker owns the session state lock for the whole
     * decoder entry, so inline requests no longer release/reacquire it per
     * draw. gxm_request_core_us is the actual vp_gxm_* call wall time and
     * device_release_us/device_reacquire_us no longer advance on this path;
     * they remain for the external handoff, which this profile does not
     * sample. segment_acquire_us/segment_release_us measure the single
     * worker-entry/exit session handoff, outside decode_us. Gated like
     * gxm_call_us. */
    uint64_t device_release_us, gxm_request_core_us, device_reacquire_us;
    uint64_t segment_acquire_us, segment_release_us;
    /* B112 outer-wall observations. These are disjoint sequence segments around
     * one worker-backed entry, but they are nested observations rather than a
     * mutually exclusive residual decomposition: gate_wait is the real
     * gpu_execution_gate mutex block, worker_queue is submit-to-callback-entry,
     * worker_postwake is callback-end-to-submit-return, caller_reacquire is the
     * submitting thread's post-worker Session handshake. They are outside
     * decode_us; do not add any of them to decode_incl or to each other as if
     * they closed the whole frame residual. External counters cover the
     * !on_worker guest_gxm_request path only (present/flush): release, actual
     * vp_gxm_* call and Session reacquire. The worker path keeps the Session
     * lock throughout, so its device_release/device_reacquire fields stay zero;
     * external_* are deliberately separate and must not be mixed with them. */
    uint64_t gate_wait_us;
    uint64_t worker_queue_us, worker_postwake_us, caller_reacquire_us;
    uint64_t external_release_us, external_core_us, external_reacquire_us;
    /* B112 coarse decoder-entry phases. Each is measured once per entry (never
     * per command method) and nested inside decode_us. command_dispatch_us
     * contains all draw/clear/GXM work and submission_finish_us is the final
     * frame_finish_submission call. command_words counts copied entry words;
     * decoded_writes counts only fully successful collects. */
    uint64_t entry_setup_us, command_copy_us, command_collect_us;
    uint64_t command_dispatch_us, submission_finish_us;
    uint64_t command_words, decoded_writes;
} gpu_cpu_perf;
static void device_wait_begin(VPGuestSvc *svc,uint32_t token[2]) {
    if (svc->device_wait_begin) svc->device_wait_begin(svc->device_wait_user,token);
}
static void device_wait_end(VPGuestSvc *svc,const uint32_t token[2]) {
    if (svc->device_wait_end) svc->device_wait_end(svc->device_wait_user,token);
}
static int gpu_gate_enter(VPGuestSvc *svc) {
    uint32_t token[2]={0};
    if (gpu_execution_gate<0)
        gpu_execution_gate=sceKernelCreateMutex("vp-gpu-execution",SCE_KERNEL_MUTEX_ATTR_RECURSIVE,0,NULL);
    if (gpu_execution_gate<0) return -1;
    device_wait_begin(svc,token);
    int timed=svc_play_perf_enabled();
    uint64_t started=timed?sceKernelGetProcessTimeWide():0u;
    int rc=sceKernelLockMutex(gpu_execution_gate,1,NULL);
    if (timed) gpu_cpu_perf.gate_wait_us+=sceKernelGetProcessTimeWide()-started;
    device_wait_end(svc,token);
    return rc;
}
/* Inclusive wall time of one GXM request. Nested calls execute inline on the
 * current worker (B98: submits=0, direct_draws=288011), so this contains the
 * worker's preparation/encode work and any completion wait. It is not a
 * cross-thread queue or submit wait. B104: an inline request inherits the
 * decoder entry's Session lock and thread/TLS identity and keeps both, so only
 * the actual vp_gxm_* call is timed; the former per-request release/reacquire
 * no longer advances those counters. */
static int guest_gxm_request(VPGuestSvc *svc,int kind,void *request,uint64_t va) {
    uint32_t token[2]={0};
    int on_worker = vp_gxm_worker_is_current();
    int perf = svc_play_perf_enabled();
    uint64_t started = on_worker && perf ? sceKernelGetProcessTimeWide() : 0u;
    uint64_t t_core = 0u, t_reacquire = 0u;
    uint64_t ext_release = 0u, ext_core = 0u, ext_reacquire = 0u;
    if (!on_worker && gpu_gate_enter(svc)<0) return -1;
    /* An external caller still hands off the session lock around its wait.
     * gpu_decode_on_worker holds it across the decoder entry, including the
     * inline GXM work, and restored the submitting guest thread's handle/TLS
     * at entry, so the worker path must not drop or reacquire it here. The
     * external handoff is timed separately from the worker-inline path. */
    if (!on_worker) {
        uint64_t t = perf ? sceKernelGetProcessTimeWide() : 0u;
        device_wait_begin(svc,token);
        if (t) ext_release = sceKernelGetProcessTimeWide() - t;
    } else if (started) {
        t_core = started;
    }
    uint64_t call_started = (!on_worker && perf) ? sceKernelGetProcessTimeWide() : 0u;
    int rc=kind==0?vp_gxm_worker_submit(request):kind==1?vp_gxm_frame_clear(request):
           kind==2?vp_gxm_frame_finish():kind==5?vp_gxm_batch_flush("submit-end"):
           vp_gxm_display_present(va);
    if (!on_worker) {
        if (call_started) ext_core = sceKernelGetProcessTimeWide() - call_started;
        uint64_t t = perf ? sceKernelGetProcessTimeWide() : 0u;
        device_wait_end(svc,token);
        if (t) ext_reacquire = sceKernelGetProcessTimeWide() - t;
        if (perf) {
            gpu_cpu_perf.external_release_us += ext_release;
            gpu_cpu_perf.external_core_us += ext_core;
            gpu_cpu_perf.external_reacquire_us += ext_reacquire;
        }
    } else if (started) {
        t_reacquire = sceKernelGetProcessTimeWide();
    }
    if (started) {
        uint64_t ended = sceKernelGetProcessTimeWide();
        gpu_cpu_perf.gxm_call_us += ended - started;
        gpu_cpu_perf.gxm_request_core_us += t_reacquire - t_core;
    }
    if (!on_worker) sceKernelUnlockMutex(gpu_execution_gate,1);
    return rc;
}

/* Observation-only command-stream decode state for one GPU channel. The words
 * come from the GPFIFO entries the guest already handed to the loader; nothing
 * here executes a command or touches the GPU. */
typedef struct {
    vp_macro_table macros;
    vp_macro_upload_state upload;
    vp_mme_shared shared;
    vp_gpu_state state;
    vp_gpu_command_buffer commands;
    vp_gpu_shader_bind binds[2];
    int indexed[VP_GPU_SUBCHANNELS];
    uint32_t cbuf_low[VP_GPU_SUBCHANNELS];
    uint32_t cbuf_high[VP_GPU_SUBCHANNELS];
    uint32_t cbuf_size[VP_GPU_SUBCHANNELS];
    uint32_t draw_count;
    unsigned layout_report_entries;
} VPGpuDecode;

static void vp_gpu_decode_release(void *opaque) {
    VPGpuDecode *decode = opaque;
    if (decode == NULL) return;
    vp_macro_table_free(&decode->macros);
    vp_gpu_command_buffer_free(&decode->commands);
    free(decode);
}

/* nvhost gives every open of /dev/nvhost-gpu its own kernel channel object,
 * so the session keys channel state on the device descriptor. A new open must
 * not inherit an earlier channel's GPFIFO allocation, syncpoint or nvmap fd. */
static VPGuestGpuChannel *vp_guest_gpu_channel(VPGuestSession *state, uint32_t fd) {
    for (uint32_t i = 0; i < state->gpu_channel_count; ++i)
        if (state->gpu_channels[i].fd == (int32_t)fd)
            return &state->gpu_channels[i];
    if (state->gpu_channel_count >= VP_GUEST_NVDRV_DEVICE_LIMIT) return NULL;
    VPGuestGpuChannel *channel = &state->gpu_channels[state->gpu_channel_count++];
    memset(channel, 0, sizeof(*channel));
    channel->fd = (int32_t)fd;
    return channel;
}

/* A closed device descriptor retires its channel state. The slot is compacted
 * away so a later open that reuses the fd number starts from a fresh channel
 * instead of observing the old one's GPFIFO bookkeeping. */
static void vp_guest_gpu_channel_close(VPGuestSession *state, uint32_t fd) {
    for (uint32_t i = 0; i < state->gpu_channel_count; ++i) {
        if (state->gpu_channels[i].fd != (int32_t)fd) continue;
        vp_gpu_decode_release(state->gpu_channels[i].gpu_decode);
        state->gpu_channels[i] = state->gpu_channels[--state->gpu_channel_count];
        memset(&state->gpu_channels[state->gpu_channel_count], 0,
               sizeof(state->gpu_channels[state->gpu_channel_count]));
        return;
    }
}

static VPGuestNvMapHandle *nvmap_find(VPGuestSession *state, uint32_t id) {
    return vp_nvmap_table_find(state->nvmap_handles, state->nvmap_handle_count,
                               id);
}

/* Horizon AArch32 SVC return convention observed at the locked SDK veneers:
 * r0 = Result, r1:r2 = a returned u64 (GetInfo/GetSystemTick style), and the
 * original out pointers remain the caller's responsibility.  The veneer body
 * after the SVC still performs its own stores, so a site bridge only changes
 * these registers. */

#define SVC_RESULT_OUT_OF_MEMORY 0xE0040000u
#define SVC_RESULT_INVALID_HANDLE 0xE0010000u
#define SVC_RESULT_INVALID_NAME 0xE0010001u
#define SVC_RESULT_INVALID_COMBINATION 0xE0010004u
#define SVC_RESULT_INVALID_ENUM 0xE0010008u
#define SVC_INVALID_HANDLE 0xFFFFFFFFu
#define HORIZON_CURRENT_PROCESS 0xFFFF8001u
#define PROGRAM_ID_LOW 0x08DD8000u
#define PROGRAM_ID_HIGH 0x01007700u
/* Locked main NPDM (config/target-1.4.0.json, sha256 bbd9ecdf...): priority
 * 0x2C, core 0, 1 MiB stack. These are the loader's modeled main thread
 * attributes until guest ThreadType objects are created. */
#define MAIN_THREAD_PRIORITY 0x2Cu
#define HORIZON_PRIORITY_MAX 0x3Fu
/* AppletMessage values from the reviewed Horizon applet interface. The native
 * platform models an application that starts in focus, so exactly one
 * FocusStateChanged notification is queued. */
#define APPLET_MESSAGE_FOCUS_STATE_CHANGED 0x0Fu
/* AppletMessage::Exit. Requesting it raises the platform exit request; the
 * title's own message loop then decides when to release its exit lock. */
#define APPLET_MESSAGE_EXIT 0x4u
#define APPLET_FOCUS_STATE_IN_FOCUS 1u
#define APPLET_RESULT_NO_MESSAGES 0x680u
#define SVC_RESULT_TIMEOUT 0xEA01u
#define SVC_RESULT_INVALID_ADDRESS 0xD401u
/* Nonzero Horizon FS result returned for Storage failures the loader cannot
 * satisfy. The exact description only reaches the game's error report; the
 * loader's log carries the real reason, so this is never a silent success. */
#define FS_RESULT_STORAGE_FAILED 0x2EE202u
/* Self-authored offline NgWord2 archive. It contains valid empty AC dictionaries,
 * not firmware word lists. The original SDK still parses and matches them.
 * Missing/invalid archive is a storage error, never a successful empty read. */
static uint8_t *empty_romfs_image;
static uint32_t offline_system_bytes;
static int empty_romfs_ready;
static const uint8_t *empty_romfs(void) {
    if (!empty_romfs_ready) {
        FILE *file=fopen("ux0:data/mhgu-vita-port/offline-ngword2.romfs","rb");
        empty_romfs_ready=1;
        if (!file) return NULL;
        if (!fseek(file,0,SEEK_END)) {
            long bytes=ftell(file);
            if (bytes>=0x50 && bytes<=1024*1024) {
                rewind(file);
                empty_romfs_image=malloc((size_t)bytes);
                if (empty_romfs_image && fread(empty_romfs_image,1,(size_t)bytes,file)==(size_t)bytes)
                    offline_system_bytes=(uint32_t)bytes;
                else {free(empty_romfs_image);empty_romfs_image=NULL;}
            }
        }
        fclose(file);
    }
    return empty_romfs_image;
}
/* Detail every storage read while the mount is being exercised, then only
 * errors and periodic summaries: the shipped title issues thousands of reads. */
#define STORAGE_DETAIL_LIMIT 64
#define STORAGE_SUMMARY_INTERVAL 256
static unsigned storage_mp4_detail_logs;

static void put32(uint32_t address, uint32_t value) { memcpy((void *)(uintptr_t)address, &value, 4); }
static void put64(uint32_t address, uint64_t value) {
    put32(address, (uint32_t)value);
    put32(address + 4, (uint32_t)(value >> 32));
}

void vp_guest_svc_init(VPGuestSvc *svc, FILE *log) {    memset(svc, 0, sizeof(*svc));
    svc->log = log;
    /* The duplicate trace existed to survive a guest fault during the early
     * service bring-up, when the emulator's file buffering could lose the last
     * events. It is disabled now: reopening it per event dominated the frame
     * loop once the title started running, and the primary log keeps the full
     * event order. */
    svc->trace = NULL;
    svc->process_id = 1; /* opaque native process number; ids are loader-owned */
    svc->free_threads = 32;
    svc->thread_priority = MAIN_THREAD_PRIORITY;
    svc->thread_core = 0;
    svc->thread_affinity = 1u;
    svc->tick_origin_us = sceKernelGetProcessTimeWide();
    svc->total_memory = 0;
    svc->used_memory = 0;
    vp_guest_storage_init(&svc->storage);
    vp_guest_save_set_log(log);
#ifdef VP_SAVE_SELFTEST
    vp_guest_save_selftest(log);
#endif
}

void vp_guest_svc_set_module_base(VPGuestSvc *svc, unsigned index, uint32_t base) {
    if (index < sizeof(svc->module_bases) / sizeof(svc->module_bases[0]))
        svc->module_bases[index] = base;
}

void vp_guest_svc_tls(VPGuestSvc *svc, uint32_t tls_buffer) {
    if (svc) svc->tls_buffer = tls_buffer;
}

/* Renderer-side modules (audio_render.c) emit the same service log without
 * owning a copy of the static slog helper. */
void vp_guest_svc_log(VPGuestSvc *svc, const char *format, ...) {
    if (!svc || !svc->log || !svc_play_trace_sample(format)) return;
    va_list args;
    if (svc_play_perf_enabled()) {
        char line[1024];
        va_start(args, format);
        int length = vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        if (length >= 0 && (size_t)length < sizeof(line)) {
            fwrite(line, 1u, (size_t)length, svc->log);
            fflush(svc->log);
            return;
        }
    }
    va_start(args, format);
    vfprintf(svc->log, format, args);
    va_end(args);
    fflush(svc->log);
}

int vp_guest_svc_region(VPGuestSvc *svc, uint32_t base, uint32_t size, uint32_t state,
                        uint32_t permission, const char *name) {
    if (!svc || !base || !size || base > UINT32_MAX - size || svc->region_count >= VP_GUEST_REGION_LIMIT)
        return -1;
    svc->regions[svc->region_count++] = (VPGuestRegion){base, size, state, permission, name};
    if (size > UINT32_MAX - svc->total_memory) svc->total_memory = UINT32_MAX;
    else svc->total_memory += size;
    return 0;
}

int vp_guest_svc_writable(const VPGuestSvc *svc, uint32_t address, uint32_t bytes) {
    if (!svc || !bytes || address > UINT32_MAX - bytes) return 0;
    for (unsigned i = 0; i < svc->region_count; ++i) {
        const VPGuestRegion *r = &svc->regions[i];
        if (r->permission & 2u) {
            if (address >= r->base && address - r->base <= r->size && bytes <= r->size - (address - r->base))
                return 1;
        }
    }
    return 0;
}

int vp_guest_svc_readable(const VPGuestSvc *svc, uint32_t address, uint32_t bytes) {
    if (!svc || !bytes || address > UINT32_MAX - bytes) return 0;
    for (unsigned i = 0; i < svc->region_count; ++i) {
        const VPGuestRegion *r = &svc->regions[i];
        if (r->permission & 1u) {
            if (address >= r->base && address - r->base <= r->size && bytes <= r->size - (address - r->base))
                return 1;
        }
    }
    return 0;
}

static uint32_t get32(uint32_t address) { uint32_t value; memcpy(&value, (void *)(uintptr_t)address, 4); return value; }
static uint64_t get64(uint32_t address) {
    return (uint64_t)get32(address) | ((uint64_t)get32(address + 4) << 32);
}

/* One monotonic clock backs both surfaces the guest sees: the MRRC generic
 * timer islands (19.2 MHz ticks) and nn::time's steady clock (nanoseconds).
 * Vita process time is monotonic microseconds, so 192 ticks per 10 us is the
 * exact 19200000 Hz scale and 625/12 ns per tick gives steady nanoseconds. */
static uint64_t steady_ticks(const VPGuestSvc *svc) {
    uint64_t us = (uint64_t)sceKernelGetProcessTimeWide() - svc->tick_origin_us;
    return us * 192u / 10u;
}

static uint64_t steady_ns(const VPGuestSvc *svc) {
    return steady_ticks(svc) * 625u / 12u;
}

/* HIPC descriptor layout of a request message, as HipcMessageReader walks it:
 * word0 holds the X/A/B/W counts (bits 16-19/20-23/24-27/28-31), word1 holds
 * the raw data size in words (bits 0-9) and the C-descriptor flags (bits
 * 10-13). When bit31 of word1 is set a handle-descriptor word follows the
 * header, optionally with a PID and then the copy and move handle arrays.
 * X entries are two words of BufferDescriptorX ({size in bits 16-31, address});
 * A/B/W entries are three words {size, address, flags}. C entries follow the
 * raw data and are two words {address, size in bits 16-31}. */
typedef struct {
    uint32_t x_base, a_base, b_base, w_base, c_base;
    uint32_t x_count, a_count, b_count, w_count, c_count;
} VPGuestHipcLayout;

static void hipc_layout(uint32_t buffer, uint32_t header0, uint32_t header1,
                        VPGuestHipcLayout *layout) {
    uint32_t words = 2;
    if (header1 & 0x80000000u) {
        uint32_t special = get32(buffer + 8);
        ++words;
        if (special & 1u) words += 2;
        words += (special >> 1) & 0xfu;
        words += (special >> 5) & 0xfu;
    }
    layout->x_count = (header0 >> 16) & 0xfu;
    layout->a_count = (header0 >> 20) & 0xfu;
    layout->b_count = (header0 >> 24) & 0xfu;
    layout->w_count = (header0 >> 28) & 0xfu;
    layout->x_base = buffer + words * 4u; words += layout->x_count * 2u;
    layout->a_base = buffer + words * 4u; words += layout->a_count * 3u;
    layout->b_base = buffer + words * 4u; words += layout->b_count * 3u;
    layout->w_base = buffer + words * 4u; words += layout->w_count * 3u;
    uint32_t c_flags = (header1 >> 10) & 0xfu;
    layout->c_count = c_flags == 2u ? 1u : (c_flags > 2u ? c_flags - 2u : 0u);
    layout->c_base = buffer + (words + (header1 & 0x3ffu)) * 4u;
}

/* Select the read (server receives) buffer at an index. A declared A entry
 * comes first; when the client moved a small map-alias InBuffer into its
 * pointer buffer it zeroes that A entry and describes the same client-side
 * address in the parallel X entry, so the X fallback resolves it. The locked
 * SDK issues requests in the same address space the loader serves, so both
 * forms name guest memory directly. */
static void hipc_read_buffer(const VPGuestHipcLayout *layout,
                             uint32_t index, uint32_t *size, uint32_t *address) {
    *size = 0;
    *address = 0;
    if (index < layout->a_count) {
        uint32_t a_size = get32(layout->a_base + index * 12u);
        if (a_size) {
            *size = a_size;
            *address = get32(layout->a_base + index * 12u + 4u);
            return;
        }
    }
    if (index < layout->x_count) {
        uint32_t word0 = get32(layout->x_base + index * 8u);
        uint32_t x_size = word0 >> 16;
        if (x_size) {
            *size = x_size;
            *address = get32(layout->x_base + index * 8u + 4u);
        }
    }
}

/* Select the write (server fills) buffer at an index: B first, then the C
 * entry after the raw data when the client moved that OutBuffer into its
 * pointer buffer. */
static void hipc_write_buffer(const VPGuestHipcLayout *layout,
                              uint32_t index, uint32_t *size, uint32_t *address) {
    *size = 0;
    *address = 0;
    if (index < layout->b_count) {
        uint32_t b_size = get32(layout->b_base + index * 12u);
        if (b_size) {
            *size = b_size;
            *address = get32(layout->b_base + index * 12u + 4u);
            return;
        }
    }
    if (index < layout->c_count) {
        uint32_t size_word = get32(layout->c_base + index * 8u + 4u);
        uint32_t c_size = size_word >> 16;
        if (c_size) {
            *size = c_size;
            *address = get32(layout->c_base + index * 8u);
        }
    }
}

/* The client reader locates the CMIF reply from the HIPC header it finds in
 * the same thread buffer. Only the simple headers emitted by the locked SDK
 * are modeled; any word0 descriptor counts are rejected rather than guessed.
 * The reader aligns the resulting word offset up to 16 bytes. */
static uint32_t response_meta_for(uint32_t buffer) {
    uint32_t header0 = get32(buffer), header1 = get32(buffer + 4);
    uint32_t words = 2;
    if (header0 >> 16) return 0;
    if (header1) words = 3;
    if (header1 & 0x80000000u) {
        uint32_t header2 = get32(buffer + 8);
        if (header2 & 1u) words += 2;
        words += (header2 >> 1) & 0xfu;
        words += (header2 >> 5) & 0xfu;
    }
    return (buffer + words * 4u + 0xfu) & ~0xfu;
}

/* A type-4 CMIF call on a converted session carries a 16-byte domain request
 * header immediately before the CMIF header. The locked SDK's generated
 * domain client stores the target object id at header+4 (meta-0xc) and the
 * CMIF payload size at header+2. Both reviewed generated clients
 * (ProcessWithOutObjects and Process) use that layout for zero or one handle.
 * Its reply reader derives the CMIF header from the response HIPC header as
 * align16(buffer + raw_offset_words*4 + 0x1f); with no descriptors raw starts
 * at word 2, so a domain reply consists of a 16-byte domain response header
 * at buffer+8 (out-object count) followed by the CMIF response header and the
 * server-assigned out-object ids at meta+0x10. */
static uint32_t domain_response_meta(uint32_t buffer) {
    return (buffer + 8u + 0x10u + 0xfu) & ~0xfu;
}

/* CMIF location of a non-domain response that carries no native handles.
 * The reply HIPC header declares no descriptors, so HipcMessageReader
 * reports raw_offset = 2 words and the SDK's client processor derives the
 * response header as align16(buffer + raw_offset*4 + 0xf). Unlike a domain
 * reply there is no domain header here. The request's descriptor/handle
 * region must not be mirrored: response and request layouts are independent
 * and the client uses the header this server writes. */
static uint32_t plain_response_meta(uint32_t buffer) {
    return (buffer + 8u + 0x0fu) & ~0x0fu;
}

/* Non-domain reply that carries one native handle. The reply header declares
 * the special header, a pid u64 and one handle (raw offset 6 words); the
 * handle word sits at +0x14 and the CMIF header at align16(buffer + 0x18).
 * This is the shape the locked client accepted for hid CreateAppletResource
 * and IAudioRenderer OpenAudioRenderer. */
static uint32_t plain_out_handle_meta(uint32_t buffer) {
    return (buffer + 6u * 4u + 0x0fu) & ~0x0fu;
}

static void reply_domain_out_object(uint32_t buffer, uint32_t object_id) {
    uint32_t domain = buffer + 8;
    uint32_t meta = domain_response_meta(buffer);
    put32(buffer, 0);     /* no copy/move handles and no receive list */
    put32(buffer + 4, 9); /* raw words: domain header, CMIF header, one id */
    put32(domain, 1);     /* one out domain object */
    put32(domain + 4, 0);
    put32(domain + 8, 0);
    put32(domain + 0xc, 0);
    put32(meta, 0x4F434653u); /* "SFCO" */
    put32(meta + 4, 0);       /* version */
    put32(meta + 8, 0);       /* CMIF result */
    put32(meta + 0xc, 0);     /* token */
    put32(meta + 0x10, object_id);
}

/* Out-raw reply with no out object: the domain response header declares zero
 * objects and the CMIF payload carries the reviewed output value. */
static void reply_domain_out_raw(uint32_t buffer, uint32_t value, uint32_t second,
                                 uint32_t words, uint32_t result) {
    uint32_t domain = buffer + 8;
    uint32_t meta = domain_response_meta(buffer);
    put32(buffer, 0);
    put32(buffer + 4, 8 + words); /* domain header, CMIF header, payload words */
    put32(domain, 0);
    put32(domain + 4, 0);
    put32(domain + 8, 0);
    put32(domain + 0xc, 0);
    put32(meta, 0x4F434653u);
    put32(meta + 4, 0);
    put32(meta + 8, result);
    put32(meta + 0xc, 0);
    put32(meta + 0x10, value);
    if (words > 1) put32(meta + 0x14, second);
}

static void reply_domain_out_u64(uint32_t buffer, uint64_t value) {
    reply_domain_out_raw(buffer, (uint32_t)value, (uint32_t)(value >> 32), 2, 0);
}

/* Out-raw reply carrying an arbitrary payload word count. Used by interfaces
 * whose reviewed generated client processor declares a larger
 * OutRawArgumentInfo, for example SystemClockContext (32 bytes aligned 8). */
static void reply_domain_out_words(uint32_t buffer, const uint32_t *words,
                                   uint32_t count, uint32_t result) {
    uint32_t domain = buffer + 8;
    uint32_t meta = domain_response_meta(buffer);
    put32(buffer, 0);
    put32(buffer + 4, 8 + count); /* domain header, CMIF header, payload words */
    put32(domain, 0);
    put32(domain + 4, 0);
    put32(domain + 8, 0);
    put32(domain + 0xc, 0);
    put32(meta, 0x4F434653u);
    put32(meta + 4, 0);
    put32(meta + 8, result);
    put32(meta + 0xc, 0);
    for (uint32_t i = 0; i < count; ++i) put32(meta + 0x10 + i * 4, words[i]);
}

/* nn::time clock source identity. On hardware the standard steady clock has a
 * boot-scoped ClockSourceId; the loader is that clock's source, so it owns a
 * stable 16-byte id. Consistency across GetCurrentTimePoint and
 * GetSystemClockContext is what the SDK's correction math compares. */
static const uint8_t clock_source_id[16] = {
    0x4d, 0x48, 0x47, 0x55, 0x2d, 0x56, 0x50, 0x30,
    0x32, 0x32, 0x2d, 0x53, 0x54, 0x44, 0x59, 0x00,
};

/* The loader acts as the launcher: the preselected-user launch parameter and
 * the account manager both describe this one fixed user identity, because no
 * profile database exists on this platform. Keeping it in one place makes the
 * two paths agree, which the SDK checks between PopLaunchParameter and
 * OpenPreselectedUser. */
static const uint8_t loader_user_id[16] = {
    0x56, 0x50, 0x2d, 0x4c, 0x4f, 0x41, 0x44, 0x45,
    0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

/* Display name of the modeled profile that answers acc:u0 GetProfile. The
 * loader owns no profile database, so it exposes one fixed user whose name
 * identifies the platform rather than impersonating a real account. */
static const char loader_profile_name[] = "VitaPort";

/* Common::UUID::Hash() from the reviewed reference: the two little-endian
 * 64-bit halves of the 16-byte user id, the second rotated left by one, XORed.
 * nn::account reports the account id in this form. */
static uint64_t loader_user_hash(void) {
    uint64_t upper = 0, lower = 0;
    for (unsigned i = 0; i < 8; ++i) {
        upper |= (uint64_t)loader_user_id[i] << (8u * i);
        lower |= (uint64_t)loader_user_id[8u + i] << (8u * i);
    }
    return upper ^ ((lower << 1) | (lower >> 63));
}

/* Out-native-handle reply. The locked SDK's generated out-handle client reads
 * the handle word from the HIPC buffer at reader+0x34; with a receive-list
 * header of 3 (present, one entry) that field is word 5, and the reply CMIF
 * header moves to align16(buffer + 6*4 + 0x1f). */
static void reply_domain_out_handle(uint32_t buffer, uint32_t handle) {
    uint32_t meta = (buffer + 6u * 4u + 0x1fu) & ~0xfu;
    put32(buffer, 0);
    put32(buffer + 4, 0x80000009u); /* raw words incl. domain + CMIF headers */
    put32(buffer + 8, 3);           /* receive list present, one entry */
    put32(buffer + 0xc, 0);
    put32(buffer + 0x10, 0);
    put32(buffer + 0x14, handle);   /* out handle word seen at reader+0x34 */
    put32(buffer + 0x18, 0);        /* domain response header: no out objects */
    put32(buffer + 0x1c, 0);
    put32(buffer + 0x20, 0);
    put32(buffer + 0x24, 0);
    put32(meta, 0x4F434653u);
    put32(meta + 4, 0);
    put32(meta + 8, 0);
    put32(meta + 0xc, 0);
}

static VPGuestHandle *find_handle(VPGuestSvc *svc, uint32_t handle) {
    if (handle < VP_GUEST_HANDLE_FIRST || handle >= VP_GUEST_HANDLE_FIRST + VP_GUEST_HANDLE_LIMIT) return NULL;
    VPGuestHandle *slot = &svc->handles[handle - VP_GUEST_HANDLE_FIRST];
    return slot->kind ? slot : NULL;
}

/* Save file system requests can arrive from more than one guest thread; the
 * backend's object table and the Vita file system calls are serialized. */
static void save_lock(VPGuestSvc *svc) {
    while (__sync_lock_test_and_set(&svc->save_lock, 1)) {
        /* The guest thread is preempted by the Vita scheduler. */
    }
}

static void save_unlock(VPGuestSvc *svc) { __sync_lock_release(&svc->save_lock); }

static void release_save_object(VPGuestSession *state, uint32_t id) {
    if (!state || id >= VP_GUEST_DOMAIN_OBJECT_LIMIT) return;
    VPGuestSaveObject *object = state->save_objects[id];
    if (!object) return;
    vp_guest_save_release(object);
    free(object);
    state->save_objects[id] = NULL;
}

static void release_all_save_objects(VPGuestSession *state) {
    if (!state) return;
    for (unsigned i = 0; i < VP_GUEST_DOMAIN_OBJECT_LIMIT; ++i)
        release_save_object(state, i);
}

/* Allocates the domain object id and its save backend state together. The id
 * is consumed only when both succeed. */
static VPGuestSaveObject *create_save_object(VPGuestSession *state,
                                             unsigned interface,
                                             uint32_t *object_id) {
    uint32_t id = state->next_object_id;
    if (id >= VP_GUEST_DOMAIN_OBJECT_LIMIT) return NULL;
    VPGuestSaveObject *object = (VPGuestSaveObject *)calloc(1, sizeof(*object));
    if (!object) return NULL;
    object->interface = interface;
    state->next_object_id = id + 1;
    state->objects[id] = (uint8_t)interface;
    state->save_objects[id] = object;
    *object_id = id;
    return object;
}

/* Queue an applet message for the title's own message loop and signal the
 * applet event handle. The message type is preserved exactly; callers use the
 * reviewed Horizon AppletMessage values. Only the appletOE session that owns
 * the notification event is targeted. */
int vp_guest_svc_queue_applet_message(VPGuestSvc *svc, uint32_t message,
                                      const char *why) {
    if (!svc) return -1;
    for (unsigned i = 0; i < VP_GUEST_HANDLE_LIMIT; ++i) {
        const VPGuestHandle *h = &svc->handles[i];
        if (h->kind != 2u || strcmp(h->name, "appletOE") != 0) continue;
        if (h->session < 1u || h->session > VP_GUEST_SESSION_LIMIT) continue;
        VPGuestSession *st = &svc->sessions[h->session - 1u];
        if (!st->applet_event) continue;
        if (st->applet_messages >= 8u) {
            slog(svc, "SVC_APPLET_QUEUE_FULL message=0x%x reason=%s pending=%u\n",
                    message, why, st->applet_messages);
            sflush(svc);
            return -1;
        }
        unsigned at = (st->applet_queue_head + st->applet_messages) % 8u;
        st->applet_queue[at] = message;
        ++st->applet_messages;
        if (message == APPLET_MESSAGE_EXIT) st->exit_requested = 1;
        VPGuestHandle *event = find_handle(svc, st->applet_event);
        if (event) event->signaled = 1;
        slog(svc, "SVC_APPLET_QUEUE message=0x%x reason=%s pending=%u event=0x%x exit_requested=%u\n",
                message, why, st->applet_messages, st->applet_event,
                st->exit_requested);
        sflush(svc);
        return 0;
    }
    slog(svc, "SVC_APPLET_QUEUE_NOSESSION message=0x%x reason=%s\n", message, why);
    sflush(svc);
    return -1;
}

int vp_guest_svc_process_exit_requested(VPGuestSvc *svc) {
    if (!svc) return 0;
    for (unsigned i = 0; i < VP_GUEST_HANDLE_LIMIT; ++i) {
        const VPGuestHandle *h = &svc->handles[i];
        if (h->kind != 2u || strcmp(h->name, "appletOE") != 0) continue;
        if (h->session < 1u || h->session > VP_GUEST_SESSION_LIMIT) continue;
        if (svc->sessions[h->session - 1u].process_exit_requested) return 1;
    }
    return 0;
}

/* nvdrv QueryEvent hands out one copy handle per call, tagged with the nvhost
 * event id in its arg. A syncpoint completion must wake every handle the guest
 * may still hold, so signaling scans the handle table by that tag. */
static unsigned signal_nvdrv_event(VPGuestSvc *svc, uint32_t event_id) {
    unsigned woken = 0;
    for (unsigned i = 0; i < VP_GUEST_HANDLE_LIMIT; ++i) {
        VPGuestHandle *slot = &svc->handles[i];
        if (slot->kind != 3 || slot->arg != event_id || strcmp(slot->name, "nvdrv")) continue;
        slot->signaled = 1;
        ++woken;
    }
    return woken;
}

/* Advance a syncpoint's completed counter. The loader executes no GPU command
 * list, so this is the explicit completion model: as soon as a submission is
 * accepted, its declared fence value is what the kernel-side syncpoint
 * bookkeeping would expose. Armed nvhost-ctrl waits for that syncpoint and
 * threshold are completed and their QueryEvent handles signalled. */
static void nvctrl_advance_syncpoint(VPGuestSvc *svc, VPGuestSession *state,
                                     uint32_t syncpoint, uint32_t value) {
    if (syncpoint >= VP_GUEST_NVCTRL_EVENT_LIMIT) return;
    if (value <= state->nvctrl_syncpt_value[syncpoint]) return;
    state->nvctrl_syncpt_value[syncpoint] = value;
    unsigned woken = 0;
    for (uint32_t e = 0; e < VP_GUEST_NVCTRL_EVENT_LIMIT; ++e) {
        if (!state->nvctrl_waits[e].armed) continue;
        if (state->nvctrl_waits[e].fence_id != syncpoint) continue;
        if (state->nvctrl_waits[e].fence_value > value) continue;
        state->nvctrl_waits[e].armed = 0;
        woken += signal_nvdrv_event(svc, e);
    }
    slog(svc, "SVC_IPC_NVDRV_SYNCPT syncpoint=%u value=%u woken=%u model=submission-retired-without-execution\n",
            syncpoint, value, woken);
}

/* Translate one GPU virtual address through the recorded MapBufferEx table.
 * This is the same lookup the submission dump uses; the GMMU page tables are
 * not programmed, so anything outside a recorded mapping has no memory
 * behind it and the caller must treat it as unknown, not as zero. */
static int gpu_va_to_cpu(VPGuestSession *state, uint64_t va, uint64_t size,
                         uint32_t *cpu) {
    for (uint32_t m = 0; m < state->gpu_as.mapping_count; ++m) {
        const uint64_t start = state->gpu_as.mappings[m].offset;
        const uint64_t map_size = state->gpu_as.mappings[m].size;
        /* Mapping ends are exclusive. At an adjacent mapping's base, keep
         * searching rather than treating the previous mapping's end as a
         * zero-byte match. */
        if (va < start || va - start >= map_size) continue;
        if (size > map_size - (va - start)) continue;
        *cpu = state->gpu_as.mappings[m].device_address + (uint32_t)(va - start);
        return 1;
    }
    return 0;
}

/* Public form for the media instrumentation module. The GPU page tables are
 * not programmed, so this is the same recorded-mapping lookup every other
 * consumer uses; an address outside the table has no memory behind it and the
 * caller must report a miss instead of reading zero. */
int vp_guest_svc_gpu_va_to_cpu(const VPGuestSession *state, uint64_t va,
                               uint64_t size, uint32_t *cpu) {
    if (!state || !cpu) return 0;
    return gpu_va_to_cpu((VPGuestSession *)state, va, size, cpu);
}

/* Dump the memory a captured draw would read, so the resources can be turned
 * into a GXM translation without executing anything. The ranges come from the
 * command stream itself: this walks the GPFIFO entries in submission order, so
 * the vertex streams it sees are the state the draw actually ran with, not a
 * guess from one window. Bytes are read from the guest memory the MapBufferEx
 * mappings point at, which is what a real GPU access would touch; nothing is
 * synthesised, and a stream whose address is not mapped is reported as
 * unmapped rather than being filled with zeros. */
#define VP_GPU_DUMP_STREAMS 8u
#define VP_GPU_DUMP_BYTES 128u
/* Dump one vertex stream's first VP_GPU_DUMP_BYTES of memory. The words are
 * the four stream registers: format (stride in the low 12 bits, enable in bit
 * 12), address_high, address_low, frequency. Returns 1 when the memory was
 * really dumped, 0 when the stream is disabled or its address is not mapped. */
static uint32_t gpu_dump_one_stream(VPGuestSvc *svc, VPGuestSession *state,
                                    uint32_t fd, uint32_t submission,
                                    uint32_t index, const uint32_t *stream_word) {
    const uint32_t cfg = stream_word[index * 4u + 0u];
    const uint32_t stride = cfg & 0xFFFu;
    const uint32_t enable = (cfg >> 12) & 1u;
    const uint64_t va = (uint64_t)stream_word[index * 4u + 1u] << 32
        | (uint64_t)stream_word[index * 4u + 2u];
    if (!enable || va == 0) return 0;
    uint32_t cpu = 0;
    if (!gpu_va_to_cpu(state, va, VP_GPU_DUMP_BYTES, &cpu)
        || !vp_guest_svc_readable(svc, cpu, VP_GPU_DUMP_BYTES)) {
        slog(svc, "SVC_IPC_NVDRV_GPUDUMP fd=%u submission=%u stream=%u va=0x%llx stride=%u mapped=0\n",
                fd, submission, index, (unsigned long long)va, stride);
        return 0;
    }
    slog(svc, "SVC_IPC_NVDRV_GPUDUMP fd=%u submission=%u stream=%u va=0x%llx stride=%u cpu=0x%08x mapped=1\n",
            fd, submission, index, (unsigned long long)va, stride, cpu);
    for (uint32_t w = 0; w < VP_GPU_DUMP_BYTES / 4u; w += 8u) {
        slog(svc, "SVC_IPC_NVDRV_GPUDUMP_W stream=%u %u:", index, w);
        for (uint32_t j = 0; j < 8u; ++j)
            slog(svc, " %08x", get32(cpu + (w + j) * 4u));
        slog(svc, "\n");
    }
    return 1;
}

static void gpu_dump_stream_state(VPGuestSvc *svc, VPGuestSession *state,
                                  uint32_t fd, uint32_t submission,
                                  uint32_t inline_address, uint32_t entries) {
    uint32_t stream_word[VP_GPU_DUMP_STREAMS * 4u];
    uint32_t stream_dumped[VP_GPU_DUMP_STREAMS];
    uint32_t touched = 0;
    uint32_t scanned = 0;
    memset(stream_word, 0, sizeof(stream_word));
    memset(stream_dumped, 0, sizeof(stream_dumped));
    for (uint32_t e = 0; e < entries && e < 8u; ++e) {
        const uint64_t raw = (uint64_t)get32(inline_address + e * 8u)
            | ((uint64_t)get32(inline_address + e * 8u + 4u) << 32);
        const uint64_t va = raw & 0xFFFFFFFFFFull;
        const uint32_t words = (uint32_t)((raw >> 42) & 0x1FFFFFull);
        uint32_t cpu = 0;
        if (!words || words > 0xA00u) continue;
        if (!gpu_va_to_cpu(state, va, (uint64_t)words * 4u, &cpu)) continue;
        if (!vp_guest_svc_readable(svc, cpu, words * 4u)) continue;
        scanned += words;
        for (uint32_t w = 0; w < words; ++w) {
            const uint32_t word = get32(cpu + w * 4u);
            const uint32_t opcode = word >> 28;
            uint32_t method = 0, count = 0;
            if (opcode == 8u) { /* non-increasing: one method, one word */
                method = word & 0x1FFFu;
                count = 1u;
            } else if (opcode == 2u || opcode == 0x9u || opcode == 0xAu) {
                /* 0x9 is IncreasingInline and 0xA NonIncreasingInline: the
                 * value travels in the count field and no argument word
                 * follows, which is how the frame writes vertex_streams. */
                method = word & 0x1FFFu;
                count = (word >> 16) & 0x1FFFu;
                if (opcode != 2u) {
                    if (method >= 0x700u && method < 0x700u + VP_GPU_DUMP_STREAMS * 4u) {
                        const uint32_t s = (method - 0x700u) / 4u;
                        stream_word[s * 4u + (method - 0x700u) % 4u] = count;
                        if (touched < s + 1u) touched = s + 1u;
                        const uint32_t addr_set = stream_word[s * 4u + 1u]
                            || stream_word[s * 4u + 2u];
                        if (!stream_dumped[s] && addr_set)
                            stream_dumped[s] = gpu_dump_one_stream(svc, state, fd,
                                    submission, s, stream_word);
                    }
                    count = 0u;
                }
            } else {
                continue;
            }
            for (uint32_t i = 0; i < count && w + 1u + i < words; ++i) {
                const uint32_t m = method + i;
                if (m < 0x700u || m >= 0x700u + VP_GPU_DUMP_STREAMS * 4u) continue;
                const uint32_t s = (m - 0x700u) / 4u;
                stream_word[s * 4u + (m - 0x700u) % 4u] = get32(cpu + (w + 1u + i) * 4u);
                if (touched < s + 1u) touched = s + 1u;
                /* A stream rarely needs all four words set before it names
                 * memory: a zero stride or frequency still leaves a usable
                 * address. Dump on the first write that completes an address
                 * so a later zero-word write cannot mask an earlier stream. */
                const uint32_t addr_set = stream_word[s * 4u + 1u] || stream_word[s * 4u + 2u];
                const uint32_t cfg_set = stream_word[s * 4u + 0u];
                if (!stream_dumped[s] && addr_set && (cfg_set || (m - 0x700u) % 4u >= 2u)) {
                    stream_dumped[s] = gpu_dump_one_stream(svc, state, fd, submission,
                            s, stream_word);
                }
            }
            w += count;
        }
    }
    uint32_t dumped = 0;
    for (uint32_t s = 0; s < VP_GPU_DUMP_STREAMS; ++s) {
        if (!stream_dumped[s]) continue;
        ++dumped;
        /* A later stream write (for example a second render pass) may name
         * different memory than the one already dumped; keep that too. */
        if (stream_word[s * 4u + 0u] != 0) {
            gpu_dump_one_stream(svc, state, fd, submission, s, stream_word);
        }
    }
    slog(svc, "SVC_IPC_NVDRV_GPUDUMP_SUMMARY fd=%u submission=%u streams=%u scanned_words=%u dumped=%u\n",
            fd, submission, touched, scanned, dumped);
}

/* Dump the shader programs a frame binds. BindShader (macro 0xe18) takes
 * (stage, hash, program GPU address, 0, high-and-something) so the frame stream
 * names the exact buffer the hardware would fetch instructions from; there is
 * no guess about where a program lives. Each program is translated through the
 * MapBufferEx table and its first VP_SHADER_DUMP_BYTES bytes are dumped.
 * A program whose address is not mapped is reported, not zero-filled.
 *
 * The 5th argument is NOT (address_high << 24 | size). For the vertex program at
 * 0x4_0DA0F430 it is 0x040DA0F6 and for the fragment at 0x4_0F982830 it is
 * 0x040F982A: the top byte matches the address high half, but the low 24 bits
 * track the address, not a length (0x0DA0F430 >> 12 << 4 | 6 == 0x0DA0F6).
 * Only the address half is trusted; the low bits are logged raw and their
 * meaning stays unknown. The dump window is therefore fixed, not size-derived,
 * so a truncated program is visible as a missing 0x98761234 trailer. */
#define VP_SHADER_DUMP_BYTES 4096u
#define VP_SHADER_DUMP_GLOBAL 256u
/* Entry scan bound. The original limit of 8 was arbitrary and silently ignored
 * any entry after the eighth; a bind living there would never be seen. */
#define VP_SHADER_ENTRY_SCAN 32u
#define VP_SHADER_TRACKED 256u
#define VP_SHADER_BINDLOG_MAX 512u

/* Programs deduplicated by address across the whole run: the same handful is
 * rebound every submission, and re-dumping them each time was the bulk of the
 * log. */
static uint64_t vp_shader_seen[VP_SHADER_DUMP_GLOBAL];
static uint32_t vp_shader_seen_count = 0;
static uint32_t vp_shader_dumped_total = 0;

/* Persistent per-(fd, stage) binding state. The frame sampler walks only
 * submissions 1..3 and every 128th afterwards, but a vertex program is
 * typically bound once early and never rebound - so tracking binds only on
 * sampled submissions means never seeing it. This table is updated on EVERY
 * submission and remembers each (fd, stage) it has ever seen. */
typedef struct {
    uint32_t fd;
    uint32_t stage;
    uint64_t va;
    uint32_t hash;
} VPShaderBinding;
static VPShaderBinding vp_shader_binding[VP_SHADER_TRACKED];
static uint32_t vp_shader_binding_count = 0;
static uint32_t vp_shader_bindlog_count = 0;

/* Word dumps are evidence for shader investigation, not for runtime behavior.
 * Each first-seen program writes 1024 guest words (128 lines) through the
 * emulated file layer while the guest thread waits. Audio evidence runs that
 * need the guest to keep its 48 kHz update cadence disable them with this
 * marker; the program header and summary still identify every program. */
static int vp_shader_word_dump_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        FILE *off = fopen("ux0:data/mhgu-vita-port/shader-word-dump.disable", "rb");
        enabled = off == NULL;
        if (off) fclose(off);
    }
    return enabled;
}

/* Dump one program by address. Returns silently when it was already dumped or
 * the global cap is reached; reports unmapped addresses instead of zero-filling. */
static void gpu_dump_one_program(VPGuestSvc *svc, VPGuestSession *state,
                                 uint32_t fd, uint32_t submission,
                                 uint32_t stage, uint32_t hash,
                                 uint64_t prog_va, uint32_t packed_low) {
    for (uint32_t i = 0; i < vp_shader_seen_count; ++i)
        if (vp_shader_seen[i] == prog_va) return;
    if (vp_shader_seen_count >= VP_SHADER_DUMP_GLOBAL
        || vp_shader_dumped_total >= VP_SHADER_DUMP_GLOBAL) return;
    vp_shader_seen[vp_shader_seen_count++] = prog_va;
    /* Fixed window: the 5th argument's low bits are not a length. */
    const uint32_t bytes = VP_SHADER_DUMP_BYTES;
    uint32_t pcpu = 0;
    if (!gpu_va_to_cpu(state, prog_va, bytes, &pcpu)
        || !vp_guest_svc_readable(svc, pcpu, bytes)) {
        slog(svc, "SVC_IPC_NVDRV_GPUPROG fd=%u submission=%u stage=%u hash=0x%x va=0x%llx packed=0x%06x mapped=0\n",
                fd, submission, stage, hash,
                (unsigned long long)prog_va, packed_low);
        return;
    }
    ++vp_shader_dumped_total;
    /* Report the window and whether the end trailer is present, so a truncated
     * capture is never mistaken for a whole program. */
    uint32_t trailer_at = 0xFFFFFFFFu;
    for (uint32_t q = 0; q < bytes / 4u; ++q)
        if (get32(pcpu + q * 4u) == 0x98761234u) { trailer_at = q * 4u; break; }
    slog(svc, "SVC_IPC_NVDRV_GPUPROG fd=%u submission=%u stage=%u hash=0x%x va=0x%llx packed=0x%06x cpu=0x%08x bytes=%u trailer=%d mapped=1\n",
            fd, submission, stage, hash, (unsigned long long)prog_va,
            packed_low, pcpu, bytes,
            (trailer_at == 0xFFFFFFFFu) ? -1 : (int)trailer_at);
    for (uint32_t q = 0; vp_shader_word_dump_enabled() && q < bytes / 4u; q += 8u) {
        slog(svc, "SVC_IPC_NVDRV_GPUPROG_W va=0x%llx %u:",
                (unsigned long long)prog_va, q);
        for (uint32_t j = 0; j < 8u && (q + j) * 4u < bytes; ++j)
            slog(svc, " %08x", get32(pcpu + (q + j) * 4u));
        slog(svc, "\n");
    }
}

/* Walk a submission's GPFIFO entries for BindShader (macro 0xe18) and keep the
 * per-(fd, stage) binding state current. Called on EVERY submission, not only
 * sampled ones, because a stage bound once early is otherwise invisible. */
static void gpu_track_shader_binds(VPGuestSvc *svc, VPGuestSession *state,
                                   uint32_t fd, uint32_t submission,
                                   uint32_t inline_address, uint32_t entries) {
    for (uint32_t e = 0; e < entries && e < VP_SHADER_ENTRY_SCAN; ++e) {
        const uint64_t raw = (uint64_t)get32(inline_address + e * 8u)
            | ((uint64_t)get32(inline_address + e * 8u + 4u) << 32);
        const uint64_t va = raw & 0xFFFFFFFFFFull;
        const uint32_t words = (uint32_t)((raw >> 42) & 0x1FFFFFull);
        uint32_t cpu = 0;
        if (!words) continue;
        if (!gpu_va_to_cpu(state, va, (uint64_t)words * 4u, &cpu)) continue;
        if (!vp_guest_svc_readable(svc, cpu, words * 4u)) continue;
        for (uint32_t w = 0; w < words; ++w) {
            const uint32_t word = get32(cpu + w * 4u);
            if ((word & 0x1FFFu) != 0xE18u) continue;   /* BindShader */
            const uint32_t args = (word >> 16) & 0x1FFFu;
            if (args < 5u || w + 5u >= words) continue;
            const uint32_t stage = get32(cpu + (w + 1u) * 4u);
            const uint32_t hash = get32(cpu + (w + 2u) * 4u);
            /* BindShader is (stage, hash, addr_low, addr_low_copy,
             * packed_high). Only the low word and the packed word carry the
             * address; the second copy of the low word is not the high half. */
            const uint32_t prog_lo = get32(cpu + (w + 3u) * 4u);
            const uint32_t packed = get32(cpu + (w + 5u) * 4u);
            const uint32_t packed_low = packed & 0xFFFFFFu;
            const uint64_t prog_va = ((uint64_t)(packed >> 24) << 32)
                | (uint64_t)prog_lo;
            w += args;

            /* Find or create this (fd, stage) slot. */
            uint32_t slot = vp_shader_binding_count;
            for (uint32_t i = 0; i < vp_shader_binding_count; ++i)
                if (vp_shader_binding[i].fd == fd
                    && vp_shader_binding[i].stage == stage) { slot = i; break; }
            const int is_new = (slot == vp_shader_binding_count);
            if (is_new) {
                if (vp_shader_binding_count >= VP_SHADER_TRACKED) continue;
                ++vp_shader_binding_count;
                vp_shader_binding[slot].fd = fd;
                vp_shader_binding[slot].stage = stage;
                vp_shader_binding[slot].va = 0;
            }
            const int changed = vp_shader_binding[slot].va != prog_va;
            vp_shader_binding[slot].va = prog_va;
            vp_shader_binding[slot].hash = hash;

            /* Log every first sighting of a stage (bounded), and every change of
             * program address (bounded), so a stage that exists at all is visible
             * even if its program cannot be dumped. */
            if ((is_new || changed) && vp_shader_bindlog_count < VP_SHADER_BINDLOG_MAX) {
                ++vp_shader_bindlog_count;
                slog(svc, "SVC_IPC_NVDRV_SHADERBIND fd=%u stage=%u hash=0x%x va=0x%llx submission=%u first=%d\n",
                        fd, stage, hash, (unsigned long long)prog_va, submission,
                        is_new ? 1 : 0);
            }
            if (changed)
                gpu_dump_one_program(svc, state, fd, submission, stage, hash,
                        prog_va, packed_low);
        }
    }
}

/* Kept for the sampled submissions: emits the running totals so a frame can be
 * correlated with how much has been captured overall. */
static void gpu_dump_shader_programs(VPGuestSvc *svc, VPGuestSession *state,
                                     uint32_t fd, uint32_t submission,
                                     uint32_t inline_address, uint32_t entries) {
    (void)state; (void)inline_address; (void)entries;
    slog(svc, "SVC_IPC_NVDRV_GPUPROG_SUMMARY fd=%u submission=%u programs=%u dumped=%u stages=%u\n",
            fd, submission, vp_shader_seen_count, vp_shader_dumped_total,
            vp_shader_binding_count);
}

/* Observation-only Maxwell/MME decode of one readable GPFIFO entry. It never
 * executes a command: it turns the guest's method writes into the same draw
 * snapshot the host test produces, and logs one VP_GPU_DRAW line per draw on
 * the sampled submissions. */
typedef struct {
    VPGpuDecode *decode;
    VPGuestSvc *svc;
    VPGuestSession *session;
    uint8_t sub;
    uint32_t fd;
    uint32_t submission;
    int log;
    uint64_t upload_va;
    uint32_t upload_cpu, upload_size;
} VPGpuSendContext;

/* G1 inventory: the fragment stage's tex0 handle lives in the guest buffer
 * selected by 0x982, at byte offset 32. The TIC descriptor is indexed by the
 * handle and lives in the pool selected by 0x55d/0x55e. This only resolves the
 * descriptor; decoding texels is left to the caller. */
typedef struct {
    int ok;
    uint32_t handle;
    uint32_t tic_index;
    uint32_t tic[8];
    uint64_t va;
    unsigned width;
    unsigned height;
    unsigned block_height;
    unsigned block_depth;
    unsigned max_mip_level;
    unsigned texture_type;
    unsigned depth;
    unsigned format;
    unsigned header;
    unsigned pitch;
    uint32_t swizzle;
    size_t storage;
    uint32_t sampler[2];
    /* Full 32-byte TSC descriptor and its source bank. sampler[0..1] remains
     * the runtime's partial filter/wrap view; tsc keeps the raw compare/LOD/
     * border words for the next shadow-lowering round. */
    uint32_t tsc[8];
    unsigned source_bank;
} VPGpuTextureInfo;

static void log_f8_texture_descriptor(VPGuestSvc *svc, unsigned hash,
                                     unsigned gpu_stage, unsigned slot,
                                     unsigned handle_offset,
                                     const VPGpuTextureInfo *texture) {
    static unsigned logs;
    if (hash != 0xf8u || !texture || logs >= 18u) return;
    ++logs;
    slog(svc, "VP_GXM_F8_TIC stage=%u slot=%u cbuf_off=%u handle=0x%08x tic=%u "
         "va=0x%llx size=%ux%u depth=%u type=%u header=%u fmt=%u "
         "bh=%u bd=%u mip=%u pitch=%u storage=%u sampler=%08x,%08x "
         "raw=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
         gpu_stage, slot, handle_offset, texture->handle, texture->tic_index,
         (unsigned long long)texture->va, texture->width, texture->height,
         texture->depth, texture->texture_type, texture->header,
         texture->format, texture->block_height, texture->block_depth,
         texture->max_mip_level, texture->pitch, (unsigned)texture->storage,
         texture->sampler[0], texture->sampler[1],
         texture->tic[0], texture->tic[1], texture->tic[2], texture->tic[3],
         texture->tic[4], texture->tic[5], texture->tic[6], texture->tic[7]);
}

/* Bounded observation of real sampled-resource identities. This is input for
 * the next shadow-lowering round: stage token + source bank/offset + raw
 * TIC/TSC words + producer identity. It never substitutes a resource and never
 * changes submission. Normal textures and depth-format/atlas candidates have
 * independent quotas so menu UI cannot exhaust the shadow sample budget.
 * One line per unique (token, viewport, slot, offset, TIC index). */
#define VP_GXM_SHADOW_OBS_NORMAL 16u
#define VP_GXM_SHADOW_OBS_DEPTH 16u
typedef struct {
    unsigned used;
    uint32_t token;
    unsigned viewport, gpu_stage, slot, offset, tic_index;
} VPGxmShadowObservation;
static VPGxmShadowObservation gxm_shadow_observations_normal[VP_GXM_SHADOW_OBS_NORMAL];
static VPGxmShadowObservation gxm_shadow_observations_depth[VP_GXM_SHADOW_OBS_DEPTH];
static unsigned gxm_shadow_observation_normal_count;
static unsigned gxm_shadow_observation_depth_count;

/* Observed TIC depth formats in the current catalogue (0x2f = D32 float on the
 * real atlas). Kept as a small explicit set rather than a blanket "any TIC". */
static int shadow_depth_candidate(const VPGpuTextureInfo *t) {
    return t->format == 0x2fu || t->format == 0x2eu || t->format == 0x30u;
}

static int shadow_observation_duplicate(const VPGxmShadowObservation *table,
                                        unsigned count, uint32_t token,
                                        unsigned viewport, unsigned gpu_stage,
                                        unsigned slot, unsigned handle_offset,
                                        unsigned tic_index) {
    for (unsigned i = 0; i < count; i++) {
        const VPGxmShadowObservation *entry = &table[i];
        if (entry->token == token && entry->viewport == viewport &&
            entry->gpu_stage == gpu_stage && entry->slot == slot &&
            entry->offset == handle_offset && entry->tic_index == tic_index)
            return 1;
    }
    return 0;
}

static void observe_texture_consumer(VPGuestSvc *svc, uint32_t token,
                                     unsigned viewport, unsigned gpu_stage,
                                     unsigned slot, unsigned handle_offset,
                                     const VPGpuTextureInfo *t, int producer) {
    VPGxmShadowObservation *table, *entry;
    unsigned *count;
    unsigned quota, n;
    int depth_candidate;
    if (t == NULL || !t->ok) return;
    depth_candidate = shadow_depth_candidate(t);
    if (depth_candidate) {
        table = gxm_shadow_observations_depth;
        count = &gxm_shadow_observation_depth_count;
        quota = VP_GXM_SHADOW_OBS_DEPTH;
    } else {
        table = gxm_shadow_observations_normal;
        count = &gxm_shadow_observation_normal_count;
        quota = VP_GXM_SHADOW_OBS_NORMAL;
    }
    if (shadow_observation_duplicate(table, *count, token, viewport, gpu_stage,
                                     slot, handle_offset, t->tic_index))
        return;
    /* A depth candidate never falls back into the normal quota. */
    if (*count >= quota) return;
    n = *count;
    entry = &table[n];
    entry->used = 1u;
    entry->token = token;
    entry->viewport = viewport;
    entry->gpu_stage = gpu_stage;
    entry->slot = slot;
    entry->offset = handle_offset;
    entry->tic_index = t->tic_index;
    (*count)++;
    slog(svc,
         "VP_M6_SHADOW_CONSUMER n=%u class=%s fs_token=0x%x viewport=%u "
         "gpu_stage=%u slot=%u source_bank=%u cbuf_off=%u "
         "binding_offset=assumed-runtime-abi handle=0x%x tic_idx=%u "
         "tic_raw=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
         "tsc_raw=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
         "sampler_type=%u compare=unsupported decoded=partial "
         "filter_mag=%u filter_min=%u mip=%u wrap_u=%u wrap_v=%u "
         "lod_max=%u va=0x%llx size=%ux%u fmt=%u header=%u type=%u depth=%u "
         "block_h=%u block_d=%u pitch=%u storage=%u producer=%s\n",
         n, depth_candidate ? "depth-candidate" : "normal", token, viewport,
         gpu_stage, slot, t->source_bank, handle_offset, t->handle,
         t->tic_index,
         t->tic[0], t->tic[1], t->tic[2], t->tic[3],
         t->tic[4], t->tic[5], t->tic[6], t->tic[7],
         t->tsc[0], t->tsc[1], t->tsc[2], t->tsc[3],
         t->tsc[4], t->tsc[5], t->tsc[6], t->tsc[7],
         t->texture_type,
         t->sampler[1] & 3u, (t->sampler[1] >> 4) & 3u,
         (t->sampler[1] >> 6) & 3u, t->sampler[0] & 7u,
         (t->sampler[0] >> 3) & 7u, t->max_mip_level,
         (unsigned long long)t->va, t->width, t->height, t->format, t->header,
         t->texture_type, t->depth, t->block_height, t->block_depth, t->pitch,
         (unsigned)t->storage, producer ? "gpu-surface" : "cpu-guest");
}

#define VP_TEXTURE_CACHE_BYTES (48u*1024u*1024u)

typedef struct {
    uint64_t va;
    unsigned width,height,format,header,pitch,bh,stamp,swizzle,pins;
    unsigned texture_type,mip_count;
    /* Upload identity, not just the guest TIC format: RGBA32F is decoded as
     * either RGBA16F (fragment/legacy) or lossless RG32F (vertex). A cache
     * hit must never mix those two buffers for the same guest descriptor. */
    unsigned upload;
    size_t bytes,rgba_bytes;
    uint8_t *raw,*rgba;
    SceUID raw_uid,rgba_uid;
} VPTextureCache;
#define VP_TEXTURE_CACHE_SLOTS 128u
/* Metadata capacity only: payloads still obey the same 48 MiB LRU budget. */
static VPTextureCache texture_cache[VP_TEXTURE_CACHE_SLOTS];
/* B110 stage 2: direct-mapped hints hold only slot index+1, never payload or
 * content identity. Every probe revalidates the complete layout key and the
 * full raw bytes, so a hint left stale by eviction/reuse can only miss. */
#define VP_TEXTURE_CACHE_HINTS 64u
static uint8_t texture_cache_hint[VP_TEXTURE_CACHE_HINTS];
static unsigned texture_cache_stamp;
static size_t texture_cache_bytes;
static uint64_t texture_requests, texture_hits, texture_misses, texture_updates;
static uint64_t texture_evictions, texture_decoded_us, texture_lookup_us;
static uint64_t texture_allocations, texture_size_reuses;
static uint64_t texture_hint_hits, texture_update_tail_us;
/* B115 experiment: key hits that skipped the exact RAW byte comparison. */
static uint64_t texture_skip_hits;
static uint64_t texture_decode_us_rgba8, texture_decode_us_fp16;
static uint64_t texture_decode_us_blocks, texture_decode_us_cube;
static uint64_t texture_decode_us_other;
/* Exact full-backing comparison, rather than VA or a sampled/hash heuristic.
 * The GPU execution gate owns this cache; a borrowed decode remains alive
 * until the synchronous worker has copied/consumed it. Evidence auto-mode
 * retains its own snapshots and does not borrow from the cache. */
/* B89 direction-2 exploration: bounded dump of the exact raw guest texture
 * storage that the decode cache compares and decodes. It exists so an offline
 * pass can test whether runtime texture content is byte-identical to shipped
 * asset data, which is the precondition for build-time decoding. Diagnostic
 * only, marker-gated, after 30 s, at most 32 textures / 4 MiB each. */
#define VP_TEX_ASSET_DUMP_LIMIT 32u
#define VP_TEX_ASSET_DUMP_BYTES (4u*1024u*1024u)
#define VP_TEX_ASSET_DUMP_SEEN 64u
static struct {
    int state;
    unsigned dumps;
    uint64_t start_us;
    uint64_t seen_va[VP_TEX_ASSET_DUMP_SEEN];
    unsigned seen_count;
} texture_asset_dump = { .state = -1 };

static void texture_asset_dump_maybe(VPGuestSvc *svc, const VPGpuTextureInfo *t,
                                     const uint8_t *source) {
    char path[160];
    FILE *f;
    if (texture_asset_dump.state == 0) return;
    if (texture_asset_dump.state < 0) {
        FILE *marker = fopen("ux0:data/mhgu-vita-port/texture-asset-dump.enable", "rb");
        texture_asset_dump.state = marker != NULL;
        if (marker != NULL) fclose(marker);
        texture_asset_dump.start_us = sceKernelGetProcessTimeWide();
        slog(svc, "VP_TEX_ASSET_DUMP enabled=%d limit=%u bytes=%u\n",
             texture_asset_dump.state, VP_TEX_ASSET_DUMP_LIMIT,
             VP_TEX_ASSET_DUMP_BYTES);
        if (texture_asset_dump.state == 0) return;
    }
    if (texture_asset_dump.dumps >= VP_TEX_ASSET_DUMP_LIMIT) return;
    if (t == NULL || source == NULL || t->storage == 0u ||
        t->storage > VP_TEX_ASSET_DUMP_BYTES)
        return;
    if (sceKernelGetProcessTimeWide() - texture_asset_dump.start_us < 30000000ull)
        return;
    for (unsigned i = 0; i < texture_asset_dump.seen_count; ++i)
        if (texture_asset_dump.seen_va[i] == t->va) return;
    if (texture_asset_dump.seen_count < VP_TEX_ASSET_DUMP_SEEN)
        texture_asset_dump.seen_va[texture_asset_dump.seen_count++] = t->va;
    snprintf(path, sizeof(path),
             "ux0:data/mhgu-vita-port/probe-raw-%u-0x%llx-%ux%u-f%u-bh%u.bin",
             texture_asset_dump.dumps, (unsigned long long)t->va, t->width,
             t->height, t->format, t->block_height);
    f = fopen(path, "wb");
    if (f == NULL) return;
    fwrite(source, 1u, t->storage, f);
    fclose(f);
    f = fopen("ux0:data/mhgu-vita-port/texture-assets.jsonl", "a");
    if (f != NULL) {
        fprintf(f, "{\"file\":\"probe-raw-%u-0x%llx-%ux%u-f%u-bh%u.bin\","
                   "\"va\":\"0x%llx\",\"width\":%u,\"height\":%u,\"format\":%u,"
                   "\"header\":%u,\"pitch\":%u,\"bh\":%u,\"storage\":%u,"
                   "\"texture_type\":%u,\"mips\":%u}\n",
                texture_asset_dump.dumps, (unsigned long long)t->va, t->width,
                t->height, t->format, t->block_height,
                (unsigned long long)t->va, t->width, t->height, t->format,
                t->header, t->pitch, t->block_height, (unsigned)t->storage,
                t->texture_type, t->max_mip_level + 1u);
        fclose(f);
    }
    slog(svc, "VP_TEX_ASSET_DUMP n=%u va=0x%llx %ux%u fmt=%u bh=%u storage=%u\n",
         texture_asset_dump.dumps, (unsigned long long)t->va, t->width,
         t->height, t->format, t->block_height, (unsigned)t->storage);
    texture_asset_dump.dumps++;
}

static int texture_cache_key_matches(const VPTextureCache *c,
                                     const VPGpuTextureInfo *t,unsigned mips,
                                     unsigned upload) {
    return c->raw && c->va==t->va && c->width==t->width && c->height==t->height &&
        c->format==t->format && c->header==t->header && c->pitch==t->pitch &&
        c->bh==t->block_height && c->swizzle==t->swizzle && c->bytes==t->storage &&
        c->texture_type==t->texture_type && c->mip_count==mips &&
        c->upload==upload;
}

static unsigned texture_cache_hint_index(const VPGpuTextureInfo *t,unsigned mips,
                                         unsigned upload) {
    uint64_t k=t->va;
    k^=(uint64_t)t->width<<17;k^=(uint64_t)t->height<<34;
    k^=(uint64_t)t->format<<7;k^=(uint64_t)t->header<<3;
    k^=(uint64_t)t->pitch<<45;k^=(uint64_t)t->block_height<<24;
    k^=(uint64_t)t->swizzle<<11;k^=(uint64_t)t->storage<<28;
    k^=(uint64_t)t->texture_type<<55;k^=(uint64_t)mips<<38;
    k^=(uint64_t)upload<<3;
    return (unsigned)((k^(k>>32)^(k>>17))&(VP_TEXTURE_CACHE_HINTS-1u));
}

/* Vertex-only upload selection. RGBA32F uses the lossless RG32F split; the
 * measured contract is checked here so no unsupported sampler or layout falls
 * back to the lossy fragment representation. Other formats keep the existing
 * shared decode identity. */
static unsigned texture_vertex_upload_format(const VPGpuTextureInfo *t,
                                             const char **reason) {
    VPTextureVertexRg32fView view;
    *reason=NULL;
    if (t->texture_type==3u) return 0u;
    if (t->format!=0x01u)
        return vp_texture_upload_format(t->format,t->swizzle);
    memset(&view,0,sizeof(view));
    view.width=t->width;
    view.height=t->height;
    view.tic_word0=t->swizzle;
    view.tic_format=t->format;
    view.tic_header=t->header;
    view.texture_type=t->texture_type;
    view.depth=t->depth;
    view.block_depth=t->block_depth;
    view.max_mip_level=t->max_mip_level;
    view.pitch=t->pitch;
    view.storage=t->storage;
    view.sampler[0]=t->sampler[0];
    view.sampler[1]=t->sampler[1];
    *reason=vp_texture_vertex_rg32f_check(&view);
    return *reason != NULL ? 0u : VP_TEXTURE_UPLOAD_RGBA32F_RG32F;
}

/* Bounded hint probe followed by the original full scan. Return 0 on miss,
 * 1 for an exact hint hit, 2 for an exact scan hit. On miss *reuse_index is
 * the first unpinned same-key slot in slot order (all raw versions already
 * compared), matching the pre-hint candidate rule. The hint slot's full
 * compare is not repeated during the scan, but its position still takes part
 * in that ordering. */
static int texture_cache_lookup(const VPGpuTextureInfo *t,unsigned mips,
                                unsigned upload,const uint8_t *source,
                                unsigned *hit_index,unsigned *reuse_index) {
    unsigned hint=texture_cache_hint[texture_cache_hint_index(t,mips,upload)];
    unsigned hint_index=hint?hint-1u:~0u;
    int hint_key_match=0;
    int skip_bytes=vp_gxm_texture_skip_bytes_enabled();
    *hit_index=~0u;*reuse_index=~0u;
    if (hint_index<VP_TEXTURE_CACHE_SLOTS) {
        const VPTextureCache *c=&texture_cache[hint_index];
        if (texture_cache_key_matches(c,t,mips,upload)) {
            hint_key_match=1;
            if (skip_bytes || vp_texture_bytes_equal(c->raw,source,t->storage)) {
                if (skip_bytes) texture_skip_hits++;
                *hit_index=hint_index;
                return 1;
            }
        }
    }
    for (unsigned i=0;i<VP_TEXTURE_CACHE_SLOTS;i++) {
        const VPTextureCache *c=&texture_cache[i];
        if (hint_key_match && i==hint_index) {
            if (!c->pins && *reuse_index==~0u) *reuse_index=i;
            continue;
        }
        if (!texture_cache_key_matches(c,t,mips,upload)) continue;
        if (skip_bytes || vp_texture_bytes_equal(c->raw,source,t->storage)) {
            if (skip_bytes) texture_skip_hits++;
            *hit_index=i;
            return 2;
        }
        if (!c->pins && *reuse_index==~0u) *reuse_index=i;
    }
    return 0;
}

enum {
    VP_TEXTURE_DECODE_FRAGMENT = 0u,
    VP_TEXTURE_DECODE_VERTEX = 1u
};

static uint8_t *texture_cached_decode(VPGuestSvc *svc,const VPGpuTextureInfo *t,
    const uint8_t *source,unsigned decode_kind) {
    VPTextureCache *slot=NULL, *reuse=NULL;
    unsigned hit_index=~0u,reuse_index=~0u;
    uint64_t lookup_started = svc_play_perf_enabled() ? sceKernelGetProcessTimeWide() : 0u;
    if ((++texture_requests & 511u) == 0u)
        slog(svc,"VP_TEX_CACHE requests=%llu hits=%llu hint_hits=%llu misses=%llu "
             "updates=%llu evictions=%llu decode_us=%llu rgba8_us=%llu fp16_us=%llu "
             "blocks_us=%llu cube_us=%llu other_us=%llu tail_us=%llu lookup_us=%llu "
             "bytes=%u cap=%u slots=%u allocations=%llu size_reuses=%llu "
             "skip_hits=%llu\n",
             (unsigned long long)texture_requests,(unsigned long long)texture_hits,
             (unsigned long long)texture_hint_hits,
             (unsigned long long)texture_misses,(unsigned long long)texture_updates,
             (unsigned long long)texture_evictions,(unsigned long long)texture_decoded_us,
             (unsigned long long)texture_decode_us_rgba8,
             (unsigned long long)texture_decode_us_fp16,
             (unsigned long long)texture_decode_us_blocks,
             (unsigned long long)texture_decode_us_cube,
             (unsigned long long)texture_decode_us_other,
             (unsigned long long)texture_update_tail_us,
             (unsigned long long)texture_lookup_us,(unsigned)texture_cache_bytes,
             VP_TEXTURE_CACHE_BYTES,VP_TEXTURE_CACHE_SLOTS,
             (unsigned long long)texture_allocations,
             (unsigned long long)texture_size_reuses,
             (unsigned long long)texture_skip_hits);
    int cube=t->texture_type==3u;
    unsigned mips=t->max_mip_level+1u;
    unsigned upload=cube?0u:vp_texture_upload_format(t->format,t->swizzle);
    int native_bc=t->format==0x27u || t->format==0x28u;
    if (!cube && decode_kind==VP_TEXTURE_DECODE_VERTEX) {
        const char *vertex_reason=NULL;
        unsigned vertex_upload=texture_vertex_upload_format(t,&vertex_reason);
        if (vertex_reason!=NULL) {
            static unsigned vertex_format_logs;
            if (vertex_format_logs++<32u)
                slog(svc,"VP_TEX_VERTEX_RG32F_REJECT va=0x%llx fmt=%u "
                     "word0=%08x size=%ux%u header=%u type=%u depth=%u "
                     "block_d=%u mips=%u pitch=%u storage=%u tsc=%08x,%08x "
                     "reason=%s\n",(unsigned long long)t->va,t->format,
                     t->swizzle,t->width,t->height,t->header,t->texture_type,
                     t->depth,t->block_depth,t->max_mip_level,t->pitch,
                     (unsigned)t->storage,t->sampler[0],t->sampler[1],
                     vertex_reason);
            return NULL;
        }
        upload=vertex_upload;
    }
    if (native_bc && !upload) {
        static unsigned format_refusal_logs;
        if (format_refusal_logs++<32u)
            slog(svc,"VP_TEX_FORMAT_UNSUPPORTED va=0x%llx fmt=%u word0=%08x "
                 "size=%ux%u type=%u depth=%u block_d=%u detail=bc-swizzle-or-type\n",
                 (unsigned long long)t->va,t->format,t->swizzle,t->width,t->height,
                 t->texture_type,t->depth,t->block_depth);
        return NULL;
    }
    size_t rgba_bytes=cube?vp_texture_cube_upload_bytes_mips(t->width,t->height,mips):
        vp_texture_upload_bytes(t->width,t->height,upload);
    if (!t->storage || t->storage+rgba_bytes>VP_TEXTURE_CACHE_BYTES) return NULL;
    {
        int found=texture_cache_lookup(t,mips,upload,source,&hit_index,&reuse_index);
        if (found) {
            VPTextureCache *c=&texture_cache[hit_index];
            c->stamp=++texture_cache_stamp;
            c->pins++;
            texture_hits++;
            if (found==1u) texture_hint_hits++;
            /* Only an actual hit establishes a hint; the stored value is a
             * slot index, never a content identity. */
            texture_cache_hint[texture_cache_hint_index(t,mips,upload)]=(uint8_t)(hit_index+1u);
            if (lookup_started) texture_lookup_us += sceKernelGetProcessTimeWide()-lookup_started;
            return c->rgba;
        }
        if (reuse_index<VP_TEXTURE_CACHE_SLOTS) reuse=&texture_cache[reuse_index];
    }
    texture_misses++;
    if (lookup_started) texture_lookup_us += sceKernelGetProcessTimeWide()-lookup_started;
    if (native_bc && !cube && t->max_mip_level!=0u) {
        /* The checked linear-block path carries only the base level. Preserve
         * the existing base-only limitation explicitly instead of claiming
         * that the guest mip chain was uploaded or synthesising empty levels. */
        static unsigned base_only_logs;
        if (base_only_logs++<16u)
            slog(svc,"VP_TEX_MIP_BASE_ONLY va=0x%llx fmt=%u mips=%u "
                 "size=%ux%u detail=base-level-native-blocks\n",
                 (unsigned long long)t->va,t->format,mips,t->width,t->height);
    }
    if (!reuse) for (;;) {
        slot=NULL;
        for (unsigned i=0;i<VP_TEXTURE_CACHE_SLOTS;i++) {
            VPTextureCache *c=&texture_cache[i];
            if (!c->raw) { slot=c; break; }
        }
        if (slot && texture_cache_bytes+t->storage+rgba_bytes<=VP_TEXTURE_CACHE_BYTES) break;
        VPTextureCache *old=NULL, *fit=NULL;
        for (unsigned i=0;i<VP_TEXTURE_CACHE_SLOTS;i++) {
            VPTextureCache *c=&texture_cache[i];
            if (!c->raw || c->pins) continue;
            if (!old || c->stamp<old->stamp) old=c;
            if (c->upload==upload && c->bytes==t->storage &&
                c->rgba_bytes==rgba_bytes &&
                (!fit || c->stamp<fit->stamp)) fit=c;
        }
        /* Reuse an existing allocation pair before freeing a different size. */
        if (fit && fit!=old) { old=fit; texture_size_reuses++; }
        if (!old) return NULL;
        texture_evictions++;
        if (old->bytes == t->storage && old->rgba_bytes == rgba_bytes) {
            reuse = old;
            break;
        }
        texture_cache_bytes-=old->bytes+old->rgba_bytes;
        /* Decoder update event: any GXM upload/snapshot association to this
         * decoded allocation dies before the backing is freed. GPU copies and
         * upload pages themselves are untouched. */
        vp_gxm_texture_owned_retire(old->rgba);
        sceKernelFreeMemBlock(old->raw_uid);sceKernelFreeMemBlock(old->rgba_uid);
        memset(old,0,sizeof(*old));
    }
    uint8_t *raw=NULL,*rgba=NULL;
    SceUID raw_uid=-1,rgba_uid=-1;
    if (reuse) {
        slot=reuse; raw=reuse->raw; rgba=reuse->rgba;
        raw_uid=reuse->raw_uid; rgba_uid=reuse->rgba_uid;
        texture_updates++;
    } else {
        raw_uid=sceKernelAllocMemBlock("vpTexRaw",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
            (t->storage+4095u)&~4095u,NULL);
        rgba_uid=sceKernelAllocMemBlock("vpTexRGBA",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
            (rgba_bytes+4095u)&~4095u,NULL);
        if (raw_uid>=0) sceKernelGetMemBlockBase(raw_uid,(void **)&raw);
        if (rgba_uid>=0) sceKernelGetMemBlockBase(rgba_uid,(void **)&rgba);
        if (!raw || !rgba) {
            static unsigned cache_alloc_logs;
            if (cache_alloc_logs++<8u)
                slog(svc,"VP_TEX_CACHE_ALLOC_FAIL raw=0x%x rgba=0x%x bytes=%u/%u total=%u\n",
                    (unsigned)raw_uid,(unsigned)rgba_uid,(unsigned)t->storage,(unsigned)rgba_bytes,(unsigned)texture_cache_bytes);
            if (raw_uid>=0) sceKernelFreeMemBlock(raw_uid);
            if (rgba_uid>=0) sceKernelFreeMemBlock(rgba_uid);
            return NULL;
        }
        texture_allocations++;
    }
    /* Decoder update event before the first write to a reused rgba buffer.
     * A fresh block has no association; an LRU/same-key reuse must not let the
     * next draw inherit the previous byte proof. */
    if (reuse) vp_gxm_texture_owned_retire(rgba);
    /* Decode-family attribution reuses the one existing decode timer; it adds
     * no timer endpoint and no per-draw log. */
    unsigned family;
    if (cube) family=3u;
    else if (upload==VP_TEXTURE_UPLOAD_RGBA16F) family=1u;
    else if (upload==VP_TEXTURE_UPLOAD_RGBA32F_RG32F) family=4u;
    else if (upload) family=2u;
    else if (t->format==8u) family=0u;
    else family=4u;
    uint64_t decoded_started = svc_play_perf_enabled() ? sceKernelGetProcessTimeWide() : 0u;
    int rc=-1;
    if (cube) {
        rc=vp_texture_cube_rgba(source,t->storage,rgba,rgba_bytes,t->width,t->height,
                               t->format,t->block_height,mips,t->swizzle);
    } else if (upload==VP_TEXTURE_UPLOAD_RGBA32F_RG32F) {
        rc=vp_texture_rgba32f_pitch_to_rg32f(source,t->storage,rgba,rgba_bytes,
            t->width,t->height,t->pitch,t->swizzle);
    } else if (upload==VP_TEXTURE_UPLOAD_RGBA16F) {
        if (t->header==2u && t->format==0x01u)
            rc=vp_texture_rgba32f_pitch_to_rgba16f(source,t->storage,rgba,
                t->width,t->height,t->pitch,t->swizzle);
    } else if (upload) rc=vp_texture_blocks_linear(source,t->storage,rgba,t->width,t->height,t->format,t->block_height);
    else if (t->header==2u && t->format==8u) {
        for (unsigned y=0;y<t->height;y++)
            memcpy(rgba+(size_t)y*t->width*4u,source+(size_t)y*t->pitch,t->width*4u);
        rc=0;
    } else rc=vp_texture_rgba(source,t->storage,rgba,t->width,t->height,t->format,t->block_height);
    if (decoded_started) {
        uint64_t elapsed=sceKernelGetProcessTimeWide()-decoded_started;
        texture_decoded_us+=elapsed;
        if (family==0u) texture_decode_us_rgba8+=elapsed;
        else if (family==1u) texture_decode_us_fp16+=elapsed;
        else if (family==2u) texture_decode_us_blocks+=elapsed;
        else if (family==3u) texture_decode_us_cube+=elapsed;
        else texture_decode_us_other+=elapsed;
    }
    if (rc) {
        if (reuse) {texture_cache_bytes-=reuse->bytes+reuse->rgba_bytes;memset(reuse,0,sizeof(*reuse));}
        sceKernelFreeMemBlock(raw_uid);sceKernelFreeMemBlock(rgba_uid);return NULL;
    }
    uint64_t tail_started = svc_play_perf_enabled() ? sceKernelGetProcessTimeWide() : 0u;
    if (!upload && !cube) vp_texture_swizzle(rgba,(size_t)t->width*t->height,t->swizzle);
    memcpy(raw,source,t->storage);
    if (tail_started) texture_update_tail_us += sceKernelGetProcessTimeWide()-tail_started;
    texture_asset_dump_maybe(svc,t,source);
    *slot=(VPTextureCache){.va=t->va,.width=t->width,.height=t->height,
        .format=t->format,.header=t->header,.pitch=t->pitch,
        .bh=t->block_height,.stamp=++texture_cache_stamp,.swizzle=t->swizzle,
        .pins=1u,.texture_type=t->texture_type,.mip_count=mips,.upload=upload,
        .bytes=t->storage,.rgba_bytes=rgba_bytes,.raw=raw,.rgba=rgba,
        .raw_uid=raw_uid,.rgba_uid=rgba_uid};
    /* Admitted decode: now the hint may name this slot. */
    texture_cache_hint[texture_cache_hint_index(t,mips,upload)]=(uint8_t)((unsigned)(slot-texture_cache)+1u);
    if (!reuse) texture_cache_bytes+=t->storage+rgba_bytes;
    return rgba;
}

void vp_guest_frame_perf(VPGuestFramePerf *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->entries = gpu_cpu_perf.entries;
    out->draws = gpu_cpu_perf.draws;
    out->decode_us = gpu_cpu_perf.decode_us;
    out->draw_prepare_us = gpu_cpu_perf.draw_prepare_us;
    out->gxm_call_us = gpu_cpu_perf.gxm_call_us;
    out->device_release_us = gpu_cpu_perf.device_release_us;
    out->gxm_request_core_us = gpu_cpu_perf.gxm_request_core_us;
    out->device_reacquire_us = gpu_cpu_perf.device_reacquire_us;
    out->segment_acquire_us = gpu_cpu_perf.segment_acquire_us;
    out->segment_release_us = gpu_cpu_perf.segment_release_us;
    out->gate_wait_us = gpu_cpu_perf.gate_wait_us;
    out->worker_queue_us = gpu_cpu_perf.worker_queue_us;
    out->worker_postwake_us = gpu_cpu_perf.worker_postwake_us;
    out->caller_reacquire_us = gpu_cpu_perf.caller_reacquire_us;
    out->external_release_us = gpu_cpu_perf.external_release_us;
    out->external_core_us = gpu_cpu_perf.external_core_us;
    out->external_reacquire_us = gpu_cpu_perf.external_reacquire_us;
    out->entry_setup_us = gpu_cpu_perf.entry_setup_us;
    out->command_copy_us = gpu_cpu_perf.command_copy_us;
    out->command_collect_us = gpu_cpu_perf.command_collect_us;
    out->command_dispatch_us = gpu_cpu_perf.command_dispatch_us;
    out->submission_finish_us = gpu_cpu_perf.submission_finish_us;
    out->command_words = gpu_cpu_perf.command_words;
    out->decoded_writes = gpu_cpu_perf.decoded_writes;
    out->tex_requests = texture_requests;
    out->tex_hits = texture_hits;
    out->tex_misses = texture_misses;
    out->tex_decode_us = texture_decoded_us;
    out->tex_lookup_us = texture_lookup_us;
    out->tex_update_tail_us = texture_update_tail_us;
}


/* B107: only the derived TIC layout is memoised. The guest descriptor is
 * still mapped and all eight words are read and compared on every request;
 * a hit merely reuses the integer width/height/format/storage arithmetic for
 * an identical descriptor. Runtime identity (bank/handle/index), sampler/TSC
 * state, mapping results and source classification stay outside the cache. */
#define VP_GXM_TIC_LAYOUT_SLOTS 32u
typedef struct {
    unsigned valid;
    uint64_t descriptor_va;
    VPGpuTextureInfo layout;
} VPGxmTicLayoutCache;
static VPGxmTicLayoutCache gxm_tic_layout_cache[VP_GXM_TIC_LAYOUT_SLOTS];

static void parse_texture_layout(const uint32_t hw[8], VPGpuTextureInfo *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->tic, hw, sizeof(out->tic));
    out->va = ((uint64_t)(hw[2] & 0xffffu) << 32) | hw[1];
    out->width = (hw[4] & 0xffffu) + 1u;
    out->height = (hw[5] & 0xffffu) + 1u;
    out->block_height = (hw[3] >> 3) & 7u;
    out->block_depth = (hw[3] >> 6) & 7u;
    out->max_mip_level = (hw[3] >> 28) & 0xfu;
    out->texture_type = (hw[4] >> 23) & 0xfu;
    out->depth = ((hw[5] >> 16) & 0x3fffu) + 1u;
    out->format = hw[0] & 0x7fu;
    out->header = (hw[2] >> 21) & 7u;
    out->pitch = (hw[3] & 0xffffu) << 5;
    out->swizzle=hw[0];
    {
        /* BC4/BC5 are accepted natively only for a single-layer 2D view. A
         * layered/3D/cube reinterpretation has no reviewed GXM linear layout;
         * base-only mip handling stays explicit in the texture cache. */
        int native_bc = out->format==0x27u || out->format==0x28u;
        int bc2d = !native_bc || (out->texture_type==1u && out->depth==1u &&
                                  out->block_depth==0u);
        out->storage = out->header == 3u && bc2d ?
            vp_texture_storage(out->width, out->height, out->format, out->block_height) : 0;
    }
    if (out->header==3u && out->texture_type==3u)
        out->storage=out->block_depth==0u ?
            vp_texture_cube_storage(out->width,out->height,out->format,
                                    out->block_height,out->max_mip_level+1u) : 0u;
    if (out->header==2u && out->width<=4096u && out->height<=4096u &&
        out->pitch<=65536u) {
        if (out->format==8u && out->pitch>=out->width*4u)
            out->storage=out->pitch*out->height;
        else if (out->format==0x01u && out->pitch>=out->width*16u)
            out->storage=out->pitch*out->height;
    }
    out->ok = 1;
}

static const VPGpuTextureInfo *tic_layout_lookup(uint64_t descriptor_va,
                                                 const uint32_t hw[8]) {
    unsigned slot = (unsigned)((descriptor_va >> 5u) &
                               (VP_GXM_TIC_LAYOUT_SLOTS - 1u));
    VPGxmTicLayoutCache *c = &gxm_tic_layout_cache[slot];
    if (c->valid && c->descriptor_va == descriptor_va &&
        memcmp(c->layout.tic, hw, sizeof(c->layout.tic)) == 0)
        return &c->layout;
    parse_texture_layout(hw, &c->layout);
    c->descriptor_va = descriptor_va;
    c->valid = 1u;
    return &c->layout;
}

static int resolve_draw_texture_at_stage(const VPGpuSendContext *ctx,
                                         unsigned gpu_stage,
                                         unsigned handle_offset,
                                         VPGpuTextureInfo *out) {
    const vp_gpu_subchannel *state;
    const vp_gpu_cbuf_binding *binding;
    const VPGpuTextureInfo *layout;
    uint32_t bank, handle_cpu = 0, descriptor_cpu = 0;
    uint64_t descriptor_va = 0;
    uint32_t hw[8];
    uint32_t raw, index;

    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    if (ctx->sub >= VP_GPU_SUBCHANNELS || gpu_stage >= VP_GPU_SHADER_STAGES) return -1;
    state = &ctx->decode->state.sub[ctx->sub];
    bank = state->value[0x982u];
    if (bank >= VP_GPU_CBUF_SLOTS) return -2;
    binding = &state->cbuf[gpu_stage][bank];
    if (!binding->enabled || binding->size < handle_offset+4u ||
        !gpu_va_to_cpu(ctx->session, binding->address + handle_offset, 4u, &handle_cpu)) {
        static unsigned binding_miss_logs;
        if (binding_miss_logs++ < 32u)
            slog(ctx->svc, "VP_GXM_TEX_RESOLVE_MISS stage=%u off=%u "
                 "rc=binding bank=%u enabled=%d size=%u addr=0x%llx\n",
                 gpu_stage, handle_offset, bank, binding->enabled,
                 (unsigned)binding->size, (unsigned long long)binding->address);
        return -3;
    }
    raw = get32(handle_cpu);
    index = raw & 0xfffffu;
    {
        uint64_t pool = ((uint64_t)state->value[0x55du] << 32) | state->value[0x55eu];
        descriptor_va = pool + (uint64_t)index * 32u;
        if (index > state->value[0x55fu] ||
            !gpu_va_to_cpu(ctx->session, descriptor_va, 32u, &descriptor_cpu)) {
            static unsigned miss_logs;
            if (miss_logs++ < 32u)
                slog(ctx->svc, "VP_GXM_TEX_RESOLVE_MISS stage=%u off=%u "
                     "rc=descriptor bank=%u index=%u index_max=0x%x raw=0x%08x\n",
                     gpu_stage, handle_offset, bank, index,
                     state->value[0x55fu], raw);
            return -4;
        }
        memcpy(hw, (const void *)(uintptr_t)descriptor_cpu, sizeof(hw));
    }
    layout = tic_layout_lookup(descriptor_va, hw);
    memcpy(out, layout, sizeof(*out));
    /* Fresh per request; never inherited from the layout cache. */
    out->source_bank = bank;
    out->handle = raw;
    out->tic_index = index;
    {
        uint64_t sampler_pool = ((uint64_t)state->value[0x557u] << 32) |
                                state->value[0x558u];
        uint32_t sampler_cpu = 0;
        if ((raw >> 20) <= state->value[0x559u] &&
            gpu_va_to_cpu(ctx->session, sampler_pool + (uint64_t)(raw >> 20) * 32u,
                          32u, &sampler_cpu)) {
            memcpy(out->tsc, (const void *)(uintptr_t)sampler_cpu,
                   sizeof(out->tsc));
            memcpy(out->sampler, out->tsc, sizeof(out->sampler));
        }
    }
    if (!out->storage) {
        static unsigned texture_layout_logs;
        if (texture_layout_logs<64u && out->format!=0x29u) {
            slog(ctx->svc,"VP_TEX_LAYOUT va=0x%llx fmt=%u header=%u pitch=%u size=%ux%u "
                "type=%u depth=%u block_d=%u raw=%08x,%08x,%08x,%08x\n",
                (unsigned long long)out->va,out->format,out->header,out->pitch,out->width,out->height,
                out->texture_type,out->depth,out->block_depth,
                hw[0],hw[2],hw[3],hw[4]);
            texture_layout_logs++;
        }
    }
    return 0;
}

static int resolve_draw_texture_at(const VPGpuSendContext *ctx, unsigned handle_offset,VPGpuTextureInfo *out) {
    return resolve_draw_texture_at_stage(ctx,4u,handle_offset,out);
}

static int resolve_draw_texture(const VPGpuSendContext *ctx,VPGpuTextureInfo *out) {
    /* Current observed ABI reads the primary tex0 handle at bank+32. This is
     * not universal offline binding metadata: catalogue stage 2377
     * (frag_fd9308ade3e06b1d669d1403, Cg tex0 binding 0) declares offset 40.
     * The next round must pass a per-stage offset resolved from the offline
     * resource metadata into this function instead of adding a shader-hash
     * special case here. */
    return resolve_draw_texture_at(ctx,32u,out);
}

/* Full-frame inventory/execution gate. The marker file may contain a decimal
 * submission number; zero means every submission. The isolated 0xb89 path
 * (gxm-test.enable) stays independent of this mode. */
static int frame_mode_state = -1;
static int frame_arrays_ready;
static uint32_t frame_first_submission;
static unsigned frame_repeats = 1u;
static int frame_auto;
static int frame_shape_mode;
static unsigned frame_shape_draws;
static unsigned frame_shape_clears;
/* The shape marker selects the first matching submission by default; an
 * optional "occurrence <n>" selects a later repeat of the same draw shape so
 * animated state (for example a fade overlay) can be compared at another
 * phase. Matches are counted once per submission number, not per channel. */
static unsigned frame_shape_occurrence = 1u;
static unsigned frame_shape_matched;
static uint32_t frame_shape_last_match = 0xffffffffu;
/* Animated overlays repeat the same submission shape with a different vertex
 * colour. `alpha <hex>` binds the selection to one phase of that animation:
 * only a submission whose 0xf78 vertex alpha byte equals the requested value
 * is counted. Occurrence numbers alone are not stable across runs, because
 * load timing changes how many submissions precede the same fade phase. */
static int frame_shape_alpha_enabled;
static unsigned frame_shape_alpha;
static uint32_t frame_shape_alpha_value = 0xffffffffu;
static uint32_t frame_shape_alpha_submission = 0xffffffffu;
/* Diagnostic time marker: `at <ms> <repeats>` selects the first submission
 * whose replay clock has reached <ms>. Shape/occurrence selection depends on
 * animation phase, which shifts between runs; wall-clock selection keeps M4
 * title evidence tied to the replay timeline. */
static int frame_at_mode;
static int frame_at_selected;
static uint32_t frame_at_ms;
static int frame_done;
static uint32_t frame_submission_seen[8];
static unsigned frame_submission_draws[8];

/* The selected submission is replayed a bounded number of times so surfaces
 * that the frame does not clear reach the same steady state as a continuously
 * running game. Vertex/index pointers stay in guest memory, which is stable
 * until the SVC handler returns; CPU-decoded texture copies are owned here. */
#define VP_FRAME_EVENT_LIMIT 64u
typedef struct {
    int kind; /* 1 = clear, 2 = draw */
    VPGxmClearRequest clear;
    VPGxmDrawRequest draw;
    void *texels;
} VPFrameEvent;
static VPFrameEvent frame_events[VP_FRAME_EVENT_LIMIT];
static unsigned frame_event_count;

static void frame_mode_init(VPGuestSvc *svc) {
    FILE *file;

    if (!frame_arrays_ready) {
        for (unsigned i = 0; i < 8u; i++) frame_submission_seen[i] = 0xffffffffu;
        frame_arrays_ready = 1;
    }
    if (frame_mode_state >= 0) return;
    frame_mode_state = 0;
    file = fopen("ux0:data/mhgu-vita-port/m2-frame.enable", "rb");
    if (file == NULL) return;
    {
        char line[128];
        int value = 0;
        if (fgets(line, sizeof(line), file) == NULL) line[0] = 0;
        {
            unsigned occurrence = 1u;
            int fields = sscanf(line, "shape %u %u %u occurrence %u",
                                &frame_shape_draws, &frame_shape_clears,
                                &frame_repeats, &occurrence);
            if (fields >= 3) {
                frame_shape_mode = 1;
                frame_auto = 1;
                frame_shape_occurrence = fields == 4 && occurrence != 0u ?
                                         occurrence : 1u;
                {
                    const char *alpha = strstr(line, "alpha ");
                    unsigned value_alpha = 0;
                    if (alpha != NULL &&
                        sscanf(alpha, "alpha %x", &value_alpha) == 1) {
                        frame_shape_alpha_enabled = 1;
                        frame_shape_alpha = value_alpha & 0xffu;
                    }
                }
                if (frame_shape_draws == 0u || frame_shape_clears > 64u) {
                    frame_mode_state = 0;
                    fclose(file);
                    return;
                }
            } else if (sscanf(line, "at %u %u", &frame_at_ms, &frame_repeats) >= 1) {
                frame_at_mode = 1;
                frame_auto = 0;
                if (frame_repeats < 1u) frame_repeats = 1u;
                if (frame_repeats > 8u) frame_repeats = 8u;
            } else if (sscanf(line, "%d %u", &value, &frame_repeats) < 1) {
                value = 0;
            }
        }
        if (frame_at_mode) {
            /* Time selection keeps the clamped repeats and defers the
             * submission id until the replay clock reaches the marker. */
        } else if (!frame_shape_mode) {
            if (frame_repeats < 1u) frame_repeats = 1u;
            if (frame_repeats > 8u) frame_repeats = 8u;
            frame_first_submission = value > 0 ? (uint32_t)value : 0u;
            frame_auto = value <= 0;
        } else if (frame_repeats < 1u || frame_repeats > 8u) {
            if (frame_repeats < 1u) frame_repeats = 1u;
            if (frame_repeats > 8u) frame_repeats = 8u;
        }
    }
    fclose(file);
    frame_mode_state = 1;
    slog(svc, "VP_FRAME_MODE enabled first_submission=%u repeats=%u mode=%s\n",
         frame_first_submission, frame_repeats,
         frame_shape_mode ? "shape" : (frame_at_mode ? "at" :
             (frame_auto ? "auto" : "fixed")));
    if (frame_at_mode)
        slog(svc, "VP_FRAME_AT ms=%u\n", frame_at_ms);
    if (frame_shape_mode)
        slog(svc, "VP_FRAME_SHAPE draws=%u clears=%u occurrence=%u alpha=%s\n",
             frame_shape_draws, frame_shape_clears, frame_shape_occurrence,
             frame_shape_alpha_enabled ? "selected" : "any");
    if (frame_shape_alpha_enabled)
        slog(svc, "VP_FRAME_SHAPE_ALPHA want=0x%02x\n", frame_shape_alpha);
}

static int frame_mode_active(VPGuestSvc *svc, uint32_t submission) {
    frame_mode_init(svc);
    if (frame_mode_state != 1) return 0;
    if (frame_at_mode) {
        if (!frame_at_selected) {
            uint64_t now_ms = ((uint64_t)sceKernelGetProcessTimeWide() -
                               svc->tick_origin_us) / 1000u;
            if (now_ms < (uint64_t)frame_at_ms) return 0;
            frame_first_submission = submission;
            frame_at_selected = 1;
            slog(svc, "VP_FRAME_AT_SELECT submission=%u now_ms=%llu\n",
                 submission, (unsigned long long)now_ms);
        }
        return submission == frame_first_submission;
    }
    /* A nonzero marker value selects exactly that submission (one frame). Zero
     * keeps inventory/execution on for every submission after startup. */
    return frame_first_submission == 0u || submission == frame_first_submission;
}

/* Continuous-play display bridge. It is opt-in with display-bridge.enable so
 * every existing evidence run keeps its exact behavior (no GXM execution and
 * no present). The M2/M3 one-shot frame mode also forces the bridge off, and
 * display-bridge.disable wins over the enable marker. This gate only controls
 * execution/presentation: the loader still decodes and validates every
 * submission, and a failed lookup or sceDisplaySetFrameBuf is reported, never
 * replaced by a test pattern. */
static int display_bridge_state = -1;
static int display_bridge_enabled;
static unsigned display_present_attempts;
static unsigned display_present_ok;
static unsigned display_present_miss_va;
static unsigned display_present_miss_present;
static unsigned display_note_logs;

static void display_bridge_init(VPGuestSvc *svc) {
    FILE *on;
    FILE *off;
    int enabled, disabled;

    if (display_bridge_state >= 0) return;
    frame_mode_init(svc);
    on = fopen("ux0:data/mhgu-vita-port/display-bridge.enable", "rb");
    off = fopen("ux0:data/mhgu-vita-port/display-bridge.disable", "rb");
    enabled = on != NULL;
    disabled = off != NULL;
    if (on != NULL) fclose(on);
    if (off != NULL) fclose(off);
    display_bridge_enabled = enabled && !disabled && frame_mode_state != 1;
    slog(svc, "VP_DISPLAY_BRIDGE_MODE enabled=%d reason=%s\n",
         display_bridge_enabled,
         frame_mode_state == 1 ? "m2-frame-marker" :
         disabled ? "disable-marker" :
         !enabled ? "no-marker" : "marker");
    display_bridge_state = 1;
}

/* Resolve a preallocated VI slot to every GPU VA the guest could render it
 * through. The slot carries an nvmap handle and an offset inside that
 * allocation; each nvhost-as-gpu mapping of the allocation turns the
 * corresponding CPU address into a GPU VA. The same allocation can be mapped
 * more than once (observed: three mappings of the 0x35200000-byte heap), and
 * the guest renders through one of them; keeping all candidates lets the draw
 * stream select the one that is actually used instead of guessing. The
 * nvmap/address-space model lives on the nvdrv service session, which is not
 * the session that owns the VI binder slots, so the table is found by the
 * handle that this slot names. A failure is a miss, never a guess. */
static unsigned binder_slot_candidates(VPGuestSvc *svc, VPGuestSession *state,
                                       unsigned index) {
    VPGuestSession *gpu = NULL;
    VPGuestNvMapHandle *handle;
    uint64_t offset, bytes, cpu;

    if (svc == NULL || state == NULL || index >= state->binder_slot_count)
        return 0u;
    if (state->binder_slots[index].gpu_va_resolved)
        return state->binder_slots[index].gpu_va_candidate_count;
    /* Leave the slot unresolved on failure so a mapping established later in
     * the boot can still be picked up. */
    state->binder_slots[index].gpu_va_candidate_count = 0u;
    if (!state->binder_slots[index].has_buffer) return 0u;
    if (nvmap_find(state, state->binder_slots[index].handle) != NULL &&
        state->gpu_as.mapping_count != 0u)
        gpu = state;
    for (unsigned i = 0; gpu == NULL && i < VP_GUEST_SESSION_LIMIT; i++) {
        VPGuestSession *candidate = &svc->sessions[i];
        if (candidate->gpu_as.mapping_count == 0u) continue;
        if (nvmap_find(candidate, state->binder_slots[index].handle) != NULL)
            gpu = candidate;
    }
    if (gpu == NULL) return 0u;
    handle = nvmap_find(gpu, state->binder_slots[index].handle);
    if (handle == NULL || !handle->allocated) return 0u;
    offset = state->binder_slots[index].offset;
    bytes = (uint64_t)state->binder_slots[index].stride *
            state->binder_slots[index].height * 4u;
    if (bytes == 0u || offset >= handle->size || offset + bytes > handle->size)
        return 0u;
    cpu = (uint64_t)handle->address + offset;
    for (uint32_t i = 0; i < gpu->gpu_as.mapping_count; ++i) {
        uint64_t device = gpu->gpu_as.mappings[i].device_address;
        uint64_t size = gpu->gpu_as.mappings[i].size;
        if (cpu >= device && cpu + bytes <= device + size) {
            if (state->binder_slots[index].gpu_va_candidate_count <
                VP_GUEST_GPU_AS_MAP_LIMIT) {
                state->binder_slots[index].gpu_va_candidates[
                    state->binder_slots[index].gpu_va_candidate_count++] =
                    gpu->gpu_as.mappings[i].offset + (cpu - device);
            }
        }
    }
    state->binder_slots[index].gpu_va_resolved =
        state->binder_slots[index].gpu_va_candidate_count != 0u;
    return state->binder_slots[index].gpu_va_candidate_count;
}

static uint64_t binder_slot_gpu_va(VPGuestSvc *svc, VPGuestSession *state,
                                   unsigned index) {
    unsigned count;

    if (svc == NULL || state == NULL || index >= state->binder_slot_count)
        return 0u;
    count = binder_slot_candidates(svc, state, index);
    if (state->binder_slots[index].gpu_va_valid)
        return state->binder_slots[index].gpu_va;
    if (count == 0u) return 0u;
    state->binder_slots[index].gpu_va =
        state->binder_slots[index].gpu_va_candidates[0];
    state->binder_slots[index].gpu_va_valid = 1;
    return state->binder_slots[index].gpu_va;
}

/* A draw on any candidate VA means the guest renders this slot through that
 * mapping; remember it so the present uses the surface the draws landed in.
 * Draw submissions arrive on the nvdrv session, while the binder slots belong
 * to the VI session, so every session with slots is scanned. */
static void binder_note_display_va(VPGuestSvc *svc, VPGuestSession *state,
                                   uint64_t va) {
    if (svc == NULL || va == 0u) return;
    for (unsigned si = 0; si < VP_GUEST_SESSION_LIMIT; si++) {
        VPGuestSession *s = &svc->sessions[si];
        if (s->binder_slot_count == 0u && s != state) continue;
        for (unsigned i = 0; i < s->binder_slot_count; i++) {
            if (!s->binder_slots[i].has_buffer) continue;
            unsigned count = binder_slot_candidates(svc, s, i);
            for (unsigned c = 0; c < count; c++) {
                if (s->binder_slots[i].gpu_va_candidates[c] != va) continue;
                if (s->binder_slots[i].gpu_va != va && display_note_logs < 12u) {
                    ++display_note_logs;
                    slog(svc, "VP_DISPLAY_BRIDGE_NOTE sess=%u slot=%u va=0x%llx\n",
                         si, i, (unsigned long long)va);
                }
                s->binder_slots[i].gpu_va = va;
                s->binder_slots[i].gpu_va_valid = 1;
                return;
            }
        }
    }
}

static int binder_is_display_va(VPGuestSvc *svc, VPGuestSession *state,
                                uint64_t va) {
    if (svc == NULL || va == 0u || !display_bridge_enabled) return 0;
    for (unsigned si = 0; si < VP_GUEST_SESSION_LIMIT; si++) {
        VPGuestSession *s = &svc->sessions[si];
        if (s->binder_slot_count == 0u && s != state) continue;
        for (unsigned i = 0; i < s->binder_slot_count; i++) {
            if (!s->binder_slots[i].has_buffer) continue;
            unsigned count = binder_slot_candidates(svc, s, i);
            for (unsigned c = 0; c < count; c++)
                if (s->binder_slots[i].gpu_va_candidates[c] == va) return 1;
        }
    }
    return 0;
}

#define VP_FRAME_TARGET_LIST 64u
static uint64_t frame_targets[VP_FRAME_TARGET_LIST];
static unsigned frame_target_count;
static int frame_execute_fd = -1;
static unsigned frame_submitted_draws;

static void frame_note_target(uint64_t va) {
    if (va == 0u) return;
    for (unsigned i = 0; i < frame_target_count; i++)
        if (frame_targets[i] == va) return;
    if (frame_target_count < VP_FRAME_TARGET_LIST)
        frame_targets[frame_target_count++] = va;
}

static int frame_target_seen(uint64_t va) {
    for (unsigned i = 0; i < frame_target_count; i++)
        if (frame_targets[i] == va) return 1;
    return 0;
}

static unsigned frame_color_byte(float value) {
    int v = (int)(value * 255.0f + 0.5f);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (unsigned)v;
}

/* Carry the guest zeta binding verbatim from the real Maxwell methods, which
 * the decoder stores indexed by 32-bit method word. The runtime validates it;
 * this function never substitutes a colour address or a private depth. */
static void frame_depth_state(VPGxmGuestDepth *out,
                              const vp_gpu_subchannel *s) {
    uint32_t zeta_size = s->value[0x48cu];
    out->enable = s->value[0x54eu];
    out->va = ((uint64_t)s->value[0x3f8u] << 32) | s->value[0x3f9u];
    out->width = s->value[0x48au];
    out->height = s->value[0x48bu];
    out->depth = zeta_size & 0xffffu;
    out->dim_control = (zeta_size >> 16) & 1u;
    out->format = s->value[0x3fau];
    out->tile_mode = s->value[0x3fbu];
    out->array_pitch = s->value[0x3fcu];
    out->stencil_enable = s->value[0x4e0u];
}

static void frame_submit_clear(VPGpuSendContext *ctx, uint32_t value) {
    const vp_gpu_subchannel *s = &ctx->decode->state.sub[ctx->sub];
    unsigned rt = (value >> 6) & 0xfu;
    uint32_t base = 0x200u + 16u * rt;
    uint64_t va = ((uint64_t)s->value[base] << 32) | s->value[base + 1u];
    unsigned width = s->value[base + 2u];
    unsigned height = s->value[base + 3u];
    uint32_t format = s->value[base + 4u];
    uint32_t words[4];
    float color[4];
    uint32_t packed;
    VPGxmClearRequest req;
    int rc;
    int bridge_quiet = display_bridge_enabled;

    if (frame_auto && frame_done) return;
    if (va == 0u || width == 0u || height == 0u) return;
    for (unsigned i = 0; i < 4u; i++) words[i] = s->value[0x360u + i];
    memcpy(color, words, sizeof(color));
    packed = frame_color_byte(color[0]) | (frame_color_byte(color[1]) << 8) |
             (frame_color_byte(color[2]) << 16) | (frame_color_byte(color[3]) << 24);
    memset(&req, 0, sizeof(req));
    req.target_va = va;
    req.width = width;
    req.height = height;
    req.format = format;
    req.mask = value & 0x3fu;
    req.color = packed;
    req.depth_clear = s->value[0x364u];
    req.stencil_clear = s->value[0x368u];
    frame_depth_state(&req.depth_state, s);
    req.quiet = (unsigned)bridge_quiet;
    if (frame_auto && !frame_done) {
        if (frame_event_count < VP_FRAME_EVENT_LIMIT) {
            VPFrameEvent *event = &frame_events[frame_event_count++];
            event->kind = 1;
            event->clear = req;
            event->texels = NULL;
            rc = 0;
        } else {
            rc = -1;
        }
    } else {
        rc = guest_gxm_request(ctx->svc,1,&req,0);
        if (rc == 0 && frame_repeats > 1u && frame_event_count < VP_FRAME_EVENT_LIMIT) {
            VPFrameEvent *event = &frame_events[frame_event_count++];
            event->kind = 1;
            event->clear = req;
            event->texels = NULL;
        }
    }
    /* B121: only an accepted clear/colour write makes its target a producer.
     * The guest used to mark before submission, so a refused clear or draw
     * (for example the quest's 256x1 U16 ramp target, which the surface pool
     * rejects) left the VA looking GPU-produced while no colour surface
     * existed. Recording after rc==0 keeps genuine producers, including
     * depth-only clears over an existing colour surface, without inventing
     * one for a refused request. */
    if (rc == 0) frame_note_target(va);
    if (!bridge_quiet || (ctx->submission % 128u) == 0u || rc != 0)
        slog(ctx->svc, "%s fd=%u submission=%u va=0x%llx %ux%u "
             "mask=0x%x color=0x%08x rc=%d\n",
             frame_auto && !frame_done ? "VP_GXM_FRAME_CLEAR_RECORD" :
                                         "VP_GXM_FRAME_CLEAR_SUBMIT",
             ctx->fd, ctx->submission,
             (unsigned long long)va, width, height, req.mask, packed, rc);
}

/* Prepare one full-frame draw from the decoded state. Guest buffers remain
 * valid across the synchronous worker call, so only converted index data and
 * decoded CPU textures are copied. */
static int frame_submit_draw_impl(VPGpuSendContext *ctx, const vp_gpu_draw_state *draw) {
    const vp_gpu_subchannel *s = &ctx->decode->state.sub[ctx->sub];
    VPGxmDrawRequest req;
    VPGpuTextureInfo tex;
    unsigned char *linear = NULL;
    int linear_borrowed=0;
    uint64_t stream_va;
    uint32_t vertices_cpu = 0;
    uint32_t indices_cpu = 0;
    unsigned index_size;
    unsigned vertex_count = draw->vertex_count;
    unsigned index_count = 0;
    uint32_t index_format = draw->index_format;
    int32_t base_vertex;
    VpVertexWindowPlan window;
    int window_rc;
    int rc;
    int bridge_quiet = display_bridge_enabled;
    unsigned active_rt = draw->active_rt_index < VP_GPU_RENDER_TARGETS
                       ? draw->active_rt_index : 0u;
    const vp_gpu_render_target *color_rt = &draw->targets[active_rt];

    memset(&window, 0, sizeof(window));
    memcpy(&base_vertex, &s->value[0x50du], sizeof(base_vertex));

    if (frame_auto && frame_done) return 0;
    if (draw->color_mrt_unsupported) {
        static unsigned mrt_refuse_logs;
        if (mrt_refuse_logs < 16u || (mrt_refuse_logs % 1024u) == 0u)
            slog(ctx->svc, "VP_GXM_FRAME_RT_UNSUPPORTED fd=%u submission=%u "
                 "idx=%u reason=mrt-count count=%u rt_control=0x%x "
                 "active_rt=%u action=refuse n=%u\n", ctx->fd, ctx->submission,
                 draw->index, draw->rt_control & 0xfu, draw->rt_control,
                 active_rt, mrt_refuse_logs);
        mrt_refuse_logs++;
        return -1;
    }
    if (draw->color_mapping_unsupported) {
        static unsigned map_refuse_logs;
        if (map_refuse_logs < 16u || (map_refuse_logs % 1024u) == 0u)
            slog(ctx->svc, "VP_GXM_FRAME_RT_UNSUPPORTED fd=%u submission=%u "
                 "idx=%u reason=mapped-rt-mask count=%u rt_control=0x%x "
                 "active_rt=%u mask_common=0x%x action=refuse n=%u\n",
                 ctx->fd, ctx->submission, draw->index,
                 draw->rt_control & 0xfu, draw->rt_control, active_rt,
                 draw->color_mask_common, map_refuse_logs);
        map_refuse_logs++;
        return -1;
    }
    if (draw->color_mask_raw != 0u && draw->color_attachment == 0u) {
        /* Bounded census: the raw RT0 fields look like a colour producer but
         * RT_CONTROL/format prove there is no active colour attachment. This
         * is a real depth-only draw, not a shape mismatch against stale RT0. */
        static unsigned stale_rt_logs;
        if (stale_rt_logs < 32u || (stale_rt_logs % 1024u) == 0u)
            slog(ctx->svc, "VP_GXM_RT0_STALE fd=%u submission=%u idx=%u "
                 "rt_control=0x%x observed=%u count=%u map=%u rt=0x%llx "
                 "%ux%u fmt=%u raw_mask=0x%x common=0x%x vp=%u,%u %ux%u "
                 "scissor=%u,%u,%u,%u,%u action=depth-only n=%u\n",
                 ctx->fd, ctx->submission, draw->index, draw->rt_control,
                 draw->rt_control_observed, draw->rt_control & 0xfu,
                 (draw->rt_control >> 4) & 7u,
                 (unsigned long long)color_rt->address, color_rt->width,
                 color_rt->height, color_rt->format, draw->color_mask_raw,
                 draw->color_mask_common, draw->viewport_x, draw->viewport_y,
                 draw->viewport_width, draw->viewport_height,
                 s->value[0x380u], s->value[0x381u] & 0xffffu,
                 s->value[0x381u] >> 16, s->value[0x382u] & 0xffffu,
                 s->value[0x382u] >> 16, stale_rt_logs);
        stale_rt_logs++;
    }
    if (!draw->rt_control_observed) {
        static unsigned rt_control_absent_logs;
        if (rt_control_absent_logs < 8u ||
            (rt_control_absent_logs % 1024u) == 0u)
            slog(ctx->svc, "VP_GXM_RT_CONTROL absent fd=%u submission=%u idx=%u "
                 "sub=%u action=legacy-attachment-classification n=%u\n",
                 ctx->fd, ctx->submission, draw->index, draw->sub,
                 rt_control_absent_logs);
        rt_control_absent_logs++;
    }
    /* A depth-only draw has no colour extent of its own. The Maxwell RT0
     * address may be stale or absent; only an effective colour attachment
     * requires one. */
    if ((color_rt->address == 0u && draw->color_mask != 0u) ||
        draw->streams[0].stride == 0u ||
        !draw->streams[0].enabled) {
        slog(ctx->svc, "VP_GXM_FRAME_SKIP fd=%u submission=%u idx=%u no target/stream\n",
             ctx->fd, ctx->submission, draw->index);
        return -1;
    }
    memset(&req, 0, sizeof(req));
    req.frame_draw = 1;
    req.batch_scope = ((uint64_t)ctx->fd << 32) | ctx->submission;
    req.quiet = (unsigned)bridge_quiet;
    req.draw_index = draw->index;
    req.hash = draw->shaders[0].hash;
    /* color_mask must be initialized before any request field that consults
     * it. display_target below depends on it: assigning it later made the
     * colour-path present decision permanently false (every draw looked
     * depth-only) while the binder still presented. */
    req.color_mask = draw->color_mask;
    req.vs_token = draw->shaders[0].hash;
    req.fs_token = draw->shaders[1].hash;
    if (((s->value[0x458u] >> 27) & 7u) == 1u) {
        static unsigned model_state_logs;
        if (model_state_logs++ < 32u)
            slog(ctx->svc, "VP_MODEL_STATE vs=0x%x fs=0x%x base_vertex=%08x vertex_id=%08x alpha=%u/%u/%08x index_first=%u count=%u\n",
                draw->shaders[0].hash, draw->shaders[1].hash,
                s->value[0x50du], s->value[0x446u], s->value[0x4bbu],
                s->value[0x4c5u], s->value[0x4c4u], draw->index_first, draw->index_count);
    }
    req.viewport_transform = s->value[0x64bu] & 1u;
    req.alpha_test_enable = s->value[0x4bbu];
    req.alpha_test_func = s->value[0x4c5u];
    memcpy(&req.alpha_test_ref, &s->value[0x4c4u], sizeof(req.alpha_test_ref));
    memcpy(req.attributes, &s->value[0x458u], sizeof(req.attributes));
    req.stride = draw->streams[0].stride;
    req.topology = draw->draw_begin;
    req.indexed = draw->indexed;
    req.restart_enabled = s->value[0x591u];
    req.restart_index = s->value[0x592u];
    req.index_window_first = 0;
    req.base_vertex = base_vertex;
    req.vertex_id_base = s->value[0x446u];
    req.vertex_id_copy = s->value[0x587u] & 1u;
    req.vertex_id_copy_slot = (s->value[0x587u] >> 4) & 0xffu;
    req.output_vertex_id = s->value[0x593u];
    if (req.vertex_id_copy) {
        static unsigned vertex_id_copy_logs;
        if (vertex_id_copy_logs++ < 8u)
            slog(ctx->svc, "VP_GXM_FRAME_VERTEX_ID_COPY idx=%u slot=%u "
                 "vertex_id_base=0x%x output=0x%x model=attribute-copy-not-emulated "
                 "action=refuse\n",
                 draw->index, req.vertex_id_copy_slot, req.vertex_id_base,
                 req.output_vertex_id);
        /* The copied vertex-id attribute is not emulated. A zero window shift
         * would still feed the real stream attribute instead of the generated
         * id, so there is no faithful replay for this request. */
        return -1;
    }
    req.target_va = color_rt->address;
    req.target_width = color_rt->width;
    req.target_height = color_rt->height;
    req.target_format = color_rt->format;
    req.color_mask_raw = draw->color_mask_raw;
    req.color_mask_common = draw->color_mask_common;
    req.rt_control = draw->rt_control;
    req.active_rt_index = active_rt;
    req.color_attachment = draw->color_attachment;
    req.color_mrt_unsupported = draw->color_mrt_unsupported;
    req.color_mapping_unsupported = draw->color_mapping_unsupported;
    req.display_target = vp_gxm_display_target(
        (unsigned)bridge_quiet, draw->color_mask, req.target_va,
        (unsigned)binder_is_display_va(ctx->svc, ctx->session,
                                       req.target_va));
    if (req.display_target)
        binder_note_display_va(ctx->svc, ctx->session, req.target_va);
    frame_depth_state(&req.depth_state, s);
    req.viewport_x = draw->viewport_x;
    req.viewport_y = draw->viewport_y;
    req.viewport_width = draw->viewport_width;
    req.viewport_height = draw->viewport_height;
    req.viewport_scale_z = draw->viewport_scale_z;
    req.viewport_translate_z = draw->viewport_translate_z;
    req.viewport_z_valid = draw->viewport_z_valid;
    {
        /* Bounded evidence for the guest's window-depth transform. The first
         * two 3D capture windows showed 0x282=1.0/0x285=0.0 (identity) with
         * depth_mode 1, while the executor substituted 0.5/0.5. */
        static uint32_t viewport_z_seen[8][2];
        static unsigned viewport_z_count;
        unsigned k;
        for (k = 0; k < viewport_z_count; k++) {
            if (viewport_z_seen[k][0] == draw->viewport_scale_z &&
                viewport_z_seen[k][1] == draw->viewport_translate_z)
                break;
        }
        if (k == viewport_z_count && viewport_z_count < 8u) {
            float z_scale, z_translate;
            memcpy(&z_scale, &draw->viewport_scale_z, sizeof(z_scale));
            memcpy(&z_translate, &draw->viewport_translate_z, sizeof(z_translate));
            viewport_z_seen[viewport_z_count][0] = draw->viewport_scale_z;
            viewport_z_seen[viewport_z_count][1] = draw->viewport_translate_z;
            viewport_z_count++;
            slog(ctx->svc,
                 "VP_GXM_VIEWPORT_Z scale=%g translate=%g valid=%u depth_mode=%u "
                 "viewport=%ux%u vs=0x%x fs=0x%x\n",
                 z_scale, z_translate, draw->viewport_z_valid, draw->depth_mode,
                 req.viewport_width, req.viewport_height, req.vs_token,
                 req.fs_token);
        }
    }
    req.depth_test = draw->depth_test_enable;
    req.depth_write = draw->depth_write_enable;
    req.depth_func = draw->depth_func;
    if (s->value[0x372u]) {
        float slope, units;
        memcpy(&slope, &s->value[0x55bu], sizeof(slope));
        memcpy(&units, &s->value[0x56fu], sizeof(units));
        /* Maxwell's constant offset is in half-depth-LSB units. */
        req.depth_bias_factor = (int)slope;
        req.depth_bias_units = (int)(units * 0.5f);
        static unsigned bias_logs;
        if (bias_logs++ < 8u)
            slog(ctx->svc,"VP_GXM_DEPTH_BIAS idx=%u slope=%f units=%f clamp=0x%08x gxm=%d,%d\n",
                 draw->index,(double)slope,(double)units,s->value[0x61fu],
                 req.depth_bias_factor,req.depth_bias_units);
    }
    req.cull_enable = draw->cull_enable;
    req.front_face = draw->front_face;
    req.cull_face = draw->cull_face;
    req.blend_color_op = draw->blend_color_op;
    req.blend_color_src = draw->blend_color_src;
    req.blend_color_dst = draw->blend_color_dst;
    req.blend_alpha_op = draw->blend_alpha_op;
    req.blend_alpha_src = draw->blend_alpha_src;
    req.blend_alpha_dst = draw->blend_alpha_dst;
    req.scissor_enable = s->value[0x380u];
    req.scissor_min_x = s->value[0x381u] & 0xffffu;
    req.scissor_max_x = s->value[0x381u] >> 16;
    req.scissor_min_y = s->value[0x382u] & 0xffffu;
    req.scissor_max_y = s->value[0x382u] >> 16;
    /* Colour producers are recorded only after the runtime accepts the draw
     * (see the submission tail); a refused draw never created a surface. */
    /* The depth surface is not a sampled colour target. Recording the zeta VA
     * here would make a later depth-sampling draw look GPU-resolved when no
     * depth texture path exists. */

    if (draw->indexed) {
        if (draw->index_count < 3u ||
            draw->index_count > VP_VERTEX_WINDOW_MAX_INDEX_COUNT ||
            draw->index_address == 0u) {
            slog(ctx->svc, "VP_GXM_FRAME_SKIP idx=%u indexed count=%u\n",
                 draw->index, draw->index_count);
            return -1;
        }
        index_size = draw->index_format == 0u ? 1u :
                     draw->index_format == 1u ? 2u :
                     draw->index_format == 2u ? 4u : 0u;
        if (index_size==1u) req.restart_index&=0xffu;
        else if (index_size==2u) req.restart_index&=0xffffu;
        if (index_size == 0u) {
            slog(ctx->svc, "VP_GXM_FRAME_SKIP idx=%u index format=%u\n",
                 draw->index, draw->index_format);
            return -1;
        }
        {
            uint64_t first_bytes = (uint64_t)draw->index_first * index_size;
            uint64_t index_va;
            uint64_t bytes = (uint64_t)draw->index_count * index_size;
            if (draw->index_address > UINT64_MAX - first_bytes) {
                slog(ctx->svc, "VP_GXM_FRAME_SKIP idx=%u index address overflow "
                     "va=0x%llx first=%u size=%u\n", draw->index,
                     (unsigned long long)draw->index_address,
                     draw->index_first, index_size);
                return -1;
            }
            index_va = draw->index_address + first_bytes;
            if (bytes > 1024u * 1024u ||
                !gpu_va_to_cpu(ctx->session, index_va, bytes, &indices_cpu)) {
                slog(ctx->svc, "VP_GXM_FRAME_SKIP idx=%u index range va=0x%llx bytes=%llu\n",
                     draw->index, (unsigned long long)index_va,
                     (unsigned long long)bytes);
                return -1;
            }
            /* The real window is [min(index)+base, max(index)+base], not
             * max(index)+1 vertices starting at the stream base. The source
             * vertex stream is adjusted to that window and the indices are
             * rebased by the raw minimum in the GXM worker. */
            window_rc = vp_vertex_window_plan(
                (const void *)(uintptr_t)indices_cpu, draw->index_count,
                index_size, (int)req.restart_enabled, req.restart_index,
                base_vertex, req.stride, (int)req.vertex_id_copy, &window);
            if (window_rc == VP_VERTEX_WINDOW_EMPTY) {
                static unsigned empty_logs;
                if (empty_logs++ < 8u)
                    slog(ctx->svc, "VP_GXM_FRAME_INDEX_EMPTY idx=%u count=%u "
                         "restarts=%u action=no-primitive\n", draw->index,
                         draw->index_count, window.restart_indices);
                return 0;
            }
            if (window_rc != VP_VERTEX_WINDOW_OK) {
                static unsigned reject_logs;
                if (reject_logs++ < 16u || (reject_logs & 0xffu) == 0u)
                    slog(ctx->svc, "VP_GXM_FRAME_INDEX_REJECT idx=%u reason=%s "
                         "count=%u min=%u max=%u base=%d window=%u bytes=%llu "
                         "stride=%u identity=%u vertex_id_base=0x%x\n",
                         draw->index, vp_vertex_window_result_name(window_rc),
                         draw->index_count, window.min_index, window.max_index,
                         base_vertex, window.vertex_count,
                         (unsigned long long)window.source_bytes, req.stride,
                         req.vertex_id_copy, req.vertex_id_base);
                return -1;
            }
            /* The plan scanned this request's actual indices. Carry the proven
             * marker count so the worker can skip strip joining and the second
             * marker scan when the buffer has no restarts. */
            req.index_plan_restart_count = window.restart_indices;
            req.index_plan_valid = 1u;
            if (window.vertex_count < 3u) {
                static unsigned degenerate_logs;
                if (degenerate_logs++ < 8u)
                    slog(ctx->svc, "VP_GXM_FRAME_INDEX_SKIP idx=%u "
                         "reason=window-below-3 min=%u max=%u window=%u\n",
                         draw->index, window.min_index, window.max_index,
                         window.vertex_count);
                return 0;
            }
            if (draw->streams[0].address > UINT64_MAX - window.source_offset) {
                slog(ctx->svc, "VP_GXM_FRAME_SKIP idx=%u vertex address overflow "
                     "va=0x%llx offset=%llu\n", draw->index,
                     (unsigned long long)draw->streams[0].address,
                     (unsigned long long)window.source_offset);
                return -1;
            }
            if (window.min_index != 0u || base_vertex != 0) {
                static unsigned window_logs;
                if (window_logs++ < 16u)
                    slog(ctx->svc, "VP_GXM_FRAME_INDEX_WINDOW idx=%u min=%u max=%u "
                         "base=%d first=%u window=%u bytes=%llu stride=%u\n",
                         draw->index, window.min_index, window.max_index,
                         base_vertex, window.first_index, window.vertex_count,
                         (unsigned long long)window.source_bytes, req.stride);
            }
            vertex_count = window.vertex_count;
            index_count = draw->index_count;
            req.index_window_first = window.first_index;
            stream_va = draw->streams[0].address + window.source_offset;
        }
    } else {
        stream_va = draw->streams[0].address +
                    (uint64_t)draw->vertex_start * draw->streams[0].stride;
        index_count = draw->vertex_count;
    }
    if (vertex_count < 3u ||
        vertex_count > VP_VERTEX_WINDOW_MAX_VERTICES ||
        (uint64_t)vertex_count * req.stride > VP_VERTEX_WINDOW_MAX_BYTES ||
        !gpu_va_to_cpu(ctx->session, stream_va,
                       (uint64_t)vertex_count * req.stride, &vertices_cpu)) {
        slog(ctx->svc, "VP_GXM_FRAME_SKIP idx=%u vertex range va=0x%llx count=%u stride=%u\n",
             draw->index, (unsigned long long)stream_va, vertex_count, req.stride);
        return -1;
    }
    req.vertices = (const void *)(uintptr_t)vertices_cpu;
    req.count = vertex_count;
    req.indices = (const void *)(uintptr_t)indices_cpu;
    req.index_count = index_count;
    req.index_format = index_format;

    for (unsigned st = 0; st < 2u; st++) {
        unsigned gpu_stage = st == 0u ? 0u : 4u;
        for (unsigned bank = 0; bank < VP_GPU_CBUF_SLOTS; bank++) {
            const vp_gpu_cbuf_binding *b = &s->cbuf[gpu_stage][bank];
            uint32_t ptr = 0;
            if (b->enabled && b->size != 0u &&
                gpu_va_to_cpu(ctx->session, b->address, b->size, &ptr)) {
                req.stage_cbuf[st][bank].data = (const void *)(uintptr_t)ptr;
                req.stage_cbuf[st][bank].size = b->size;
            }
        }
    }
    {
        static uint32_t logged_blend[8];
        static unsigned logged_blend_count;
        uint32_t key = s->value[0x780u] ^ (s->value[0x781u] << 1) ^
                       (s->value[0x782u] << 3) ^ (s->value[0x783u] << 6) ^
                       (s->value[0x784u] << 9) ^ (s->value[0x785u] << 12) ^
                       (s->value[0x786u] << 15) ^ (s->value[0x4b9u] << 18);
        unsigned seen = 0;
        for (unsigned b = 0; b < logged_blend_count; b++)
            if (logged_blend[b] == key) seen = 1;
        if (!seen && logged_blend_count < 8u) {
            logged_blend[logged_blend_count++] = key;
            slog(ctx->svc, "VP_FRAME_BLEND_RAW hash=0x%x per_enable=0x%x "
                 "per=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
                 "global=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
                 draw->shaders[0].hash, s->value[0x4b9u],
                 s->value[0x780u], s->value[0x781u], s->value[0x782u],
                 s->value[0x783u], s->value[0x784u], s->value[0x785u],
                 s->value[0x786u], s->value[0x787u],
                 s->value[0x4cfu], s->value[0x4d0u], s->value[0x4d1u],
                 s->value[0x4d2u], s->value[0x4d3u], s->value[0x4d4u],
                 s->value[0x4d5u], s->value[0x4d6u]);
        }
    }
    for (unsigned c=0;c<VP_TEXTURE_CACHE_SLOTS;c++) texture_cache[c].pins=0;
    {
        int primary_declared =
            vp_gxm_shader_has_fragment_sampler(req.fs_token,
                                               req.viewport_transform,0u);
        int resources_active =
            primary_declared ? vp_gxm_static_resources_active() : 0;
        VPResourceTexture primary_layout;
        int primary_layout_rc = 0;
        unsigned handle_offset = 32u;
        if (primary_declared && resources_active != 0) {
            primary_layout_rc = vp_gxm_shader_resource(
                req.fs_token,4u,req.viewport_transform,0u,&primary_layout);
            if (primary_layout_rc == 0)
                handle_offset = primary_layout.offset;
        }
        if (!primary_declared) {
            /* No declared sampler means there is no descriptor to resolve.
             * Clear every primary field unconditionally: a stale VA left from
             * an earlier draw must never feed the depth-feedback check or look
             * like a bound resource. */
            static unsigned primary_skip_logs;
            req.texture_va = 0u;
            req.tex_width = 0u;
            req.tex_height = 0u;
            req.tex_format = 0u;
            memset(req.sampler, 0, sizeof(req.sampler));
            req.texture_source = VP_GXM_EXTRA_SOURCE_NONE;
            req.tex_descriptor_type = 0u;
            req.tex_usage_flags = 0u;
            memset(req.tex_tic, 0, sizeof(req.tex_tic));
            memset(req.tex_tsc, 0, sizeof(req.tex_tsc));
            req.tex_tic_format = 0u;
            req.tex_header = 0u;
            req.tex_texture_type = 0u;
            req.tex_depth = 0u;
            req.tex_descriptor_depth = 0u;
            req.tex_block_depth = 0u;
            req.tex_max_mip_level = 0u;
            req.tex_block_height = 0u;
            req.tex_swizzle = 0u;
            req.tex_storage = 0u;
            req.texels = NULL;
            if (primary_skip_logs++ < 8u)
                slog(ctx->svc,"VP_GXM_TEX_SKIP stage=fragment slot=0 "
                     "token=0x%x reason=undeclared no-resolve no-decode\n",
                     req.fs_token);
        } else if (resources_active < 0) {
            static unsigned primary_layout_logs;
            if (primary_layout_logs++ < 16u)
                slog(ctx->svc,"VP_GXM_TEX_LAYOUT_MISS stage=fragment slot=0 "
                     "token=0x%x detail=static-resource-layout-unavailable\n",
                     req.fs_token);
        } else if (resources_active > 0 &&
                   primary_layout_rc != 0) {
            static unsigned primary_layout_logs;
            if (primary_layout_logs++ < 32u)
                slog(ctx->svc,"VP_GXM_TEX_LAYOUT_MISS stage=fragment slot=0 "
                     "token=0x%x rc=%d detail=%s\n", req.fs_token,
                     primary_layout_rc,
                     primary_layout_rc == 1 ? "sampler-unused"
                                            : "metadata-missing");
        } else if (resolve_draw_texture_at_stage(ctx,4u,handle_offset,&tex) == 0) {
        log_f8_texture_descriptor(ctx->svc, req.hash, 4u, 0u, handle_offset, &tex);
        observe_texture_consumer(ctx->svc, req.fs_token, req.viewport_transform,
                                 4u, 0u, handle_offset, &tex, frame_target_seen(tex.va));
        req.texture_va = tex.va;
        req.tex_width = tex.width;
        req.tex_height = tex.height;
        memcpy(req.sampler, tex.sampler, sizeof(req.sampler));
        req.tex_descriptor_type = (resources_active > 0 && primary_layout_rc == 0)
                                      ? primary_layout.texture_type : 0u;
        req.tex_usage_flags = (resources_active > 0 && primary_layout_rc == 0)
                                  ? primary_layout.flags : 0u;
        memcpy(req.tex_tic, tex.tic, sizeof(req.tex_tic));
        memcpy(req.tex_tsc, tex.tsc, sizeof(req.tex_tsc));
        req.tex_tic_format = tex.format;
        req.tex_header = tex.header;
        req.tex_texture_type = tex.texture_type;
        req.tex_depth = tex.depth;
        req.tex_descriptor_depth =
            (resources_active > 0 && primary_layout_rc == 0)
                ? primary_layout.depth : 0u;
        req.tex_block_depth = tex.block_depth;
        req.tex_max_mip_level = tex.max_mip_level;
        req.tex_block_height = tex.block_height;
        req.tex_swizzle = tex.swizzle;
        req.tex_storage = tex.storage;
        req.texture_source = frame_target_seen(tex.va)
                                 ? VP_GXM_EXTRA_SOURCE_GPU
                                 : VP_GXM_EXTRA_SOURCE_CPU;
        if (vp_gxm_tic_is_depth(tex.format)) {
            static unsigned depth_resolve_logs;
            req.texture_source = VP_GXM_EXTRA_SOURCE_DEPTH;
            if (depth_resolve_logs++ < 16u)
                slog(ctx->svc,"VP_GXM_DEPTH_TEX_RESOLVE stage=fragment slot=0 "
                     "token=0x%x handle_off=%u handle=0x%08x tic_idx=%u "
                     "va=0x%llx %ux%u ticfmt=%u type=%u header=%u storage=%u "
                     "tic=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
                     "tsc=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
                     "source=depth no-cpu-decode\n", req.fs_token, handle_offset,
                     tex.handle, tex.tic_index, (unsigned long long)tex.va,
                     tex.width, tex.height, tex.format, tex.texture_type,
                     tex.header, (unsigned)tex.storage, tex.tic[0], tex.tic[1],
                     tex.tic[2], tex.tic[3], tex.tic[4], tex.tic[5], tex.tic[6],
                     tex.tic[7], tex.tsc[0], tex.tsc[1], tex.tsc[2], tex.tsc[3],
                     tex.tsc[4], tex.tsc[5], tex.tsc[6], tex.tsc[7]);
        }
        if (req.texture_source != VP_GXM_EXTRA_SOURCE_DEPTH &&
            !frame_target_seen(tex.va) && tex.storage != 0u) {
            uint32_t source = 0;
            if (bridge_quiet && gpu_va_to_cpu(ctx->session,tex.va,tex.storage,&source)) {
                linear=texture_cached_decode(ctx->svc,&tex,(const uint8_t *)(uintptr_t)source,
                                             VP_TEXTURE_DECODE_FRAGMENT);
                linear_borrowed=linear!=NULL;
                if (linear_borrowed) {
                    req.tex_format=vp_texture_upload_format(tex.format,tex.swizzle);
                    req.texels_owned=1u;
                }
            }
            if (!linear) linear = malloc((size_t)tex.width * tex.height * 4u);
            int decoded=-1;
            if (linear_borrowed) decoded=0;
            else if (linear != NULL && gpu_va_to_cpu(ctx->session,tex.va,tex.storage,&source)) {
                if (tex.header==2u && tex.format==8u) {
                    for (unsigned y=0;y<tex.height;y++)
                        memcpy(linear+(size_t)y*tex.width*4u,
                               (const uint8_t *)(uintptr_t)source+(size_t)y*tex.pitch,tex.width*4u);
                    decoded=0;
                } else decoded=vp_texture_rgba((const uint8_t *)(uintptr_t)source,tex.storage,
                                               linear,tex.width,tex.height,tex.format,tex.block_height);
            }
            if (decoded==0) {
                unsigned alpha_nonzero = 0;
                static uint64_t dumped_tex[4];
                static unsigned tex_dumps;
                int already = 0;
                req.texels = linear;
                if (!bridge_quiet)
                    for (unsigned p = 0; p < tex.width * tex.height; p++)
                        if (linear[(size_t)p * 4u + 3u] != 0u) alpha_nonzero++;
                if (!bridge_quiet)
                    slog(ctx->svc, "VP_FRAME_TEX_DECODE fd=%u submission=%u va=0x%llx "
                         "%ux%u fmt=%u alpha_nonzero=%u/%u\n", ctx->fd,
                         ctx->submission, (unsigned long long)tex.va,
                         tex.width, tex.height, tex.format, alpha_nonzero,
                         tex.width * tex.height);
                for (unsigned d = 0; d < tex_dumps; d++)
                    if (dumped_tex[d] == tex.va) already = 1;
                if (!bridge_quiet && !already && tex_dumps < 4u) {
                    char path[96];
                    FILE *dump;
                    snprintf(path, sizeof(path),
                             "ux0:data/mhgu-vita-port/m2-frame-tex%u.rgba", tex_dumps);
                    dump = fopen(path, "wb");
                    if (dump != NULL) {
                        fwrite(linear, 4u, (size_t)tex.width * tex.height, dump);
                        fclose(dump);
                        dumped_tex[tex_dumps] = tex.va;
                        slog(ctx->svc, "VP_FRAME_TEX_DUMP file=m2-frame-tex%u.rgba "
                             "va=0x%llx %ux%u\n", tex_dumps,
                             (unsigned long long)tex.va, tex.width, tex.height);
                    }
                    tex_dumps++;
                }
            } else {
                static unsigned texture_decode_fail_logs;
                if (texture_decode_fail_logs++<32u)
                    slog(ctx->svc,"VP_TEX_DECODE_FAIL va=0x%llx fmt=%u header=%u pitch=%u size=%ux%u storage=%u mapped=0x%x rc=%d\n",
                        (unsigned long long)tex.va,tex.format,tex.header,tex.pitch,tex.width,tex.height,
                        (unsigned)tex.storage,source,decoded);
                if (!linear_borrowed) free(linear);
                linear = NULL;
            }
        }
    }
    }
    /* Reflected GXM samplers use handle slots tex0..tex3 at
     * cbuf[bound]+32/+40/+48/+56. Resolve only slots present in the registered
     * fragment program; this covers movie planes and captured character
     * materials without decoding unrelated handles. Keep borrowed planes
     * pinned until the synchronous worker has consumed the request. */
    if (bridge_quiet) {
        static unsigned extra_resolved_logs;
        static unsigned extra_gpu_logs;
        static unsigned extra_error_logs;
        for (unsigned e=0;e<VP_GXM_FRAGMENT_SAMPLER_LIMIT-1u;e++) {
            VPGpuTextureInfo plane;
            uint32_t ptr=0;
            unsigned handle_offset=0;
            int resolve_rc;
            int map_rc=0;
            int resources_active=0;
            int layout_rc=0;
            VPResourceTexture layout;
            VPGxmExtraTexture *extra=&req.extra_textures[e];
            if (!vp_gxm_shader_has_fragment_sampler(req.fs_token,req.viewport_transform,e+1u))
                continue;
            resources_active=vp_gxm_static_resources_active();
            if (resources_active != 0) {
                layout_rc=vp_gxm_shader_resource(req.fs_token,4u,
                    req.viewport_transform,e+1u,&layout);
                if (layout_rc == 0) handle_offset=layout.offset;
            }
            if (resources_active < 0 ||
                (resources_active > 0 && layout_rc != 0)) {
                extra->source=VP_GXM_EXTRA_SOURCE_NONE;
                extra->resolve_rc=resources_active < 0 ? -2 : layout_rc;
                if (extra_error_logs++<32u)
                    slog(ctx->svc,"VP_GXM_TEX_LAYOUT_MISS stage=fragment slot=%u "
                         "token=0x%x active=%d rc=%d detail=%s\n", e+1u,
                         req.fs_token, resources_active, layout_rc,
                         resources_active < 0 ? "layout-unavailable"
                                              : "metadata-missing");
                continue;
            }
            if (resources_active == 0 &&
                vp_gxm_fragment_sampler_offset(e+1u,&handle_offset)<0) {
                extra->source=VP_GXM_EXTRA_SOURCE_NONE;
                extra->resolve_rc=-1;
                if (extra_error_logs++<32u)
                    slog(ctx->svc,"VP_GXM_EXTRA_TEX_MISSING slot=%u "
                         "detail=sampler-slot-out-of-range\n",e+1u);
                continue;
            }
            resolve_rc=resolve_draw_texture_at(ctx,handle_offset,&plane);
            if (resolve_rc != 0 || !plane.ok) {
                /* The handle/TIC could not be resolved at all. Report the
                 * actual slot and resolve result; do not invent a VA. */
                extra->source=VP_GXM_EXTRA_SOURCE_NONE;
                extra->resolve_rc=resolve_rc != 0 ? resolve_rc : -1;
                if (extra_error_logs++<32u)
                    slog(ctx->svc,"VP_GXM_EXTRA_TEX_MISSING slot=%u handle_offset=%u "
                         "resolve=%d detail=descriptor-unresolved\n",
                         e+1u,handle_offset,extra->resolve_rc);
                continue;
            }
            log_f8_texture_descriptor(ctx->svc, req.hash, 4u, e + 1u,
                                      handle_offset, &plane);
            /* Preserve the resolved descriptor even when no CPU backing can
             * be decoded. The worker chooses CPU upload or GPU producer from
             * this identity; a partially filled struct must not look empty. */
            extra->resolve_rc=0;
            extra->width=plane.width;
            extra->height=plane.height;
            extra->tex_format=plane.texture_type==3u?0u:
                vp_texture_upload_format(plane.format,plane.swizzle);
            extra->cube=plane.texture_type==3u;
            extra->mip_count=extra->cube ?
                (plane.max_mip_level+1u<vp_texture_cube_mip_count(plane.width,plane.height)
                    ? plane.max_mip_level+1u
                    : vp_texture_cube_mip_count(plane.width,plane.height)) : 1u;
            extra->va=plane.va;
            memcpy(extra->sampler,plane.sampler,sizeof(extra->sampler));
            extra->tic_format=plane.format;
            extra->tic_header=plane.header;
            extra->texture_type=plane.texture_type;
            extra->depth=plane.depth;
            extra->block_depth=plane.block_depth;
            extra->max_mip_level=plane.max_mip_level;
            extra->block_height=plane.block_height;
            extra->swizzle=plane.swizzle;
            extra->storage=plane.storage;
            memcpy(extra->tic_words,plane.tic,sizeof(extra->tic_words));
            memcpy(extra->tsc_words,plane.tsc,sizeof(extra->tsc_words));
            extra->descriptor_type = (resources_active > 0 && layout_rc == 0)
                                         ? layout.texture_type : 0u;
            extra->descriptor_depth = (resources_active > 0 && layout_rc == 0)
                                          ? layout.depth : 0u;
            extra->usage_flags = (resources_active > 0 && layout_rc == 0)
                                     ? layout.flags : 0u;
            observe_texture_consumer(ctx->svc, req.fs_token,
                                     req.viewport_transform, 4u, e+1u,
                                     handle_offset, &plane,
                                     frame_target_seen(plane.va));
            if (vp_gxm_tic_is_depth(plane.format)) {
                static unsigned extra_depth_logs;
                extra->source=VP_GXM_EXTRA_SOURCE_DEPTH;
                if (extra_depth_logs++<16u)
                    slog(ctx->svc,"VP_GXM_DEPTH_TEX_RESOLVE stage=fragment "
                         "slot=%u token=0x%x handle_off=%u va=0x%llx %ux%u "
                         "ticfmt=%u type=%u header=%u storage=%u "
                         "tic=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
                         "tsc=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
                         "source=depth no-cpu-decode\n", e+1u, req.fs_token,
                         handle_offset, (unsigned long long)plane.va,
                         plane.width, plane.height, plane.format,
                         plane.texture_type, plane.header,
                         (unsigned)plane.storage, plane.tic[0], plane.tic[1],
                         plane.tic[2], plane.tic[3], plane.tic[4], plane.tic[5],
                         plane.tic[6], plane.tic[7], plane.tsc[0], plane.tsc[1],
                         plane.tsc[2], plane.tsc[3], plane.tsc[4], plane.tsc[5],
                         plane.tsc[6], plane.tsc[7]);
                continue;
            }
            if (frame_target_seen(plane.va)) {
                /* A surface in this frame produced the content. Guest memory
                 * may be stale, empty or an unsupported layout, so decoding it
                 * would silently sample the wrong bytes. */
                extra->source=VP_GXM_EXTRA_SOURCE_GPU;
                if (extra_gpu_logs++<16u)
                    slog(ctx->svc,"VP_GXM_EXTRA_TEX_RESOLVE slot=%u source=gpu "
                         "va=0x%llx %ux%u ticfmt=%u type=%u header=%u "
                         "block_h=%u storage=%u map=skipped\n",
                         e+1u,(unsigned long long)plane.va,plane.width,
                         plane.height,plane.format,plane.texture_type,
                         plane.header,plane.block_height,(unsigned)plane.storage);
                continue;
            }
            extra->source=VP_GXM_EXTRA_SOURCE_CPU;
            if (!plane.storage) {
                if (extra_error_logs++<32u)
                    slog(ctx->svc,"VP_GXM_EXTRA_TEX_MISSING slot=%u source=cpu "
                         "va=0x%llx %ux%u ticfmt=%u header=%u type=%u "
                         "block_h=%u block_d=%u mips=%u storage=0 map=skipped "
                         "detail=unsupported-cpu-layout\n",
                         e+1u,(unsigned long long)plane.va,plane.width,
                         plane.height,plane.format,plane.header,plane.texture_type,
                         plane.block_height,plane.block_depth,plane.max_mip_level);
                continue;
            }
            if (!gpu_va_to_cpu(ctx->session,plane.va,plane.storage,&ptr)) {
                map_rc=-1;
                if (extra_error_logs++<32u)
                    slog(ctx->svc,"VP_GXM_EXTRA_TEX_MISSING slot=%u source=cpu "
                         "va=0x%llx %ux%u ticfmt=%u header=%u storage=%u "
                         "map_rc=%d detail=map-failed\n",e+1u,
                         (unsigned long long)plane.va,plane.width,plane.height,
                         plane.format,plane.header,(unsigned)plane.storage,map_rc);
                continue;
            }
            extra->texels=texture_cached_decode(ctx->svc,&plane,
                (const uint8_t *)(uintptr_t)ptr,VP_TEXTURE_DECODE_FRAGMENT);
            extra->texels_owned=extra->texels!=NULL;
            if (!extra->texels) {
                if (extra_error_logs++<32u)
                    slog(ctx->svc,"VP_GXM_EXTRA_TEX_MISSING slot=%u source=cpu "
                         "va=0x%llx %ux%u ticfmt=%u header=%u storage=%u "
                         "map_rc=0 decode=0 detail=cpu-decode-failed\n",e+1u,
                         (unsigned long long)plane.va,plane.width,plane.height,
                         plane.format,plane.header,(unsigned)plane.storage);
                continue;
            }
            if (extra_resolved_logs++<16u)
                slog(ctx->svc,"VP_GXM_EXTRA_TEX_RESOLVE slot=%u source=cpu "
                     "va=0x%llx %ux%u upload_fmt=%u cube=%u mips=%u storage=%u "
                     "map_rc=0\n",e+1u,(unsigned long long)plane.va,plane.width,
                     plane.height,extra->tex_format,extra->cube,extra->mip_count,
                     (unsigned)plane.storage);
        }
    }
    if (vp_gxm_shader_has_vertex_sampler(req.vs_token,req.viewport_transform,0u)) {
        VPGpuTextureInfo vertex_tex;
        uint32_t source=0;
        VPGxmExtraTexture *extra=&req.vertex_texture;
        int resolve_rc=-1;
        const char *vertex_reason=NULL;
        unsigned vertex_upload=0u;
        int resources_active=vp_gxm_static_resources_active();
        int layout_rc=0;
        VPResourceTexture layout;
        unsigned handle_offset=32u;
        memset(&vertex_tex,0,sizeof(vertex_tex));
        if (resources_active != 0) {
            layout_rc=vp_gxm_shader_resource(req.vs_token,0u,
                req.viewport_transform,0u,&layout);
            if (layout_rc == 0) handle_offset=layout.offset;
        }
        if (resources_active < 0 ||
            (resources_active > 0 && layout_rc != 0)) {
            slog(ctx->svc,"VP_GXM_TEX_LAYOUT_MISS stage=vertex slot=0 "
                 "token=0x%x active=%d rc=%d detail=%s\n", req.vs_token,
                 resources_active, layout_rc,
                 resources_active < 0 ? "layout-unavailable" : "metadata-missing");
        } else {
            resolve_rc=resolve_draw_texture_at_stage(ctx,0u,handle_offset,&vertex_tex);
            if (resolve_rc==0) {
                log_f8_texture_descriptor(ctx->svc,req.hash,0u,0u,handle_offset,
                                          &vertex_tex);
                extra->width=vertex_tex.width;
                extra->height=vertex_tex.height;
                vertex_upload=texture_vertex_upload_format(&vertex_tex,&vertex_reason);
                extra->tex_format=vertex_reason != NULL ? 0u : vertex_upload;
                if (vertex_reason != NULL) {
                    static unsigned vertex_contract_logs;
                    if (vertex_contract_logs++<32u)
                        slog(ctx->svc,"VP_VERTEX_TEXTURE_CONTRACT hash=0x%x "
                             "va=0x%llx size=%ux%u fmt=%u header=%u pitch=%u "
                             "storage=%u tsc=%08x,%08x sampler=%08x,%08x "
                             "reason=%s\n", req.hash,
                             (unsigned long long)vertex_tex.va,vertex_tex.width,
                             vertex_tex.height,vertex_tex.format,
                             vertex_tex.header,vertex_tex.pitch,
                             (unsigned)vertex_tex.storage,vertex_tex.tsc[0],
                             vertex_tex.tsc[1],vertex_tex.sampler[0],
                             vertex_tex.sampler[1],vertex_reason);
                }
                extra->cube=vertex_tex.texture_type==3u;
                extra->mip_count=extra->cube ?
                    (vertex_tex.max_mip_level+1u<
                        vp_texture_cube_mip_count(vertex_tex.width,vertex_tex.height)
                        ? vertex_tex.max_mip_level+1u
                        : vp_texture_cube_mip_count(vertex_tex.width,vertex_tex.height)) : 1u;
                extra->va=vertex_tex.va;
                memcpy(extra->sampler,vertex_tex.sampler,sizeof(extra->sampler));
                memcpy(extra->tic_words,vertex_tex.tic,sizeof(extra->tic_words));
                memcpy(extra->tsc_words,vertex_tex.tsc,sizeof(extra->tsc_words));
                extra->descriptor_type =
                    (resources_active > 0 && layout_rc == 0)
                        ? layout.texture_type : 0u;
                extra->descriptor_depth =
                    (resources_active > 0 && layout_rc == 0)
                        ? layout.depth : 0u;
                extra->usage_flags =
                    (resources_active > 0 && layout_rc == 0)
                        ? layout.flags : 0u;
                extra->tic_format=vertex_tex.format;
                extra->tic_header=vertex_tex.header;
                extra->texture_type=vertex_tex.texture_type;
                extra->depth=vertex_tex.depth;
                extra->block_depth=vertex_tex.block_depth;
                extra->max_mip_level=vertex_tex.max_mip_level;
                extra->block_height=vertex_tex.block_height;
                extra->swizzle=vertex_tex.swizzle;
                extra->storage=vertex_tex.storage;
                if (vp_gxm_tic_is_depth(vertex_tex.format)) {
                    static unsigned vertex_depth_logs;
                    extra->source=VP_GXM_EXTRA_SOURCE_DEPTH;
                    if (vertex_depth_logs++<16u)
                        slog(ctx->svc,"VP_GXM_DEPTH_TEX_RESOLVE stage=vertex "
                             "slot=0 token=0x%x handle_off=%u va=0x%llx %ux%u "
                             "ticfmt=%u type=%u header=%u source=depth "
                             "no-cpu-decode\n", req.vs_token, handle_offset,
                             (unsigned long long)vertex_tex.va,
                             vertex_tex.width, vertex_tex.height,
                             vertex_tex.format, vertex_tex.texture_type,
                             vertex_tex.header);
                } else if (vertex_reason == NULL && vertex_tex.storage &&
                    gpu_va_to_cpu(ctx->session,vertex_tex.va,vertex_tex.storage,&source)) {
                    extra->texels=texture_cached_decode(ctx->svc,&vertex_tex,
                        (const uint8_t *)(uintptr_t)source,VP_TEXTURE_DECODE_VERTEX);
                    extra->texels_owned=extra->texels!=NULL;
                }
            }
        }
        if (!extra->texels && extra->source != VP_GXM_EXTRA_SOURCE_DEPTH) {
            slog(ctx->svc,"VP_GXM_VERTEX_TEXTURE_MISSING hash=0x%x stage=0 cbuf=0x%llx "
                 "bank=%u offset=%u resolve=%d va=0x%llx size=%ux%u fmt=%u header=%u storage=%u mapped=0x%x\n",
                 req.hash,(unsigned long long)s->cbuf[0][s->value[0x982u]<VP_GPU_CBUF_SLOTS?
                     s->value[0x982u]:0u].address,s->value[0x982u],handle_offset,
                 resolve_rc,(unsigned long long)vertex_tex.va,vertex_tex.width,
                 vertex_tex.height,vertex_tex.format,vertex_tex.header,
                 (unsigned)vertex_tex.storage,source);
        } else if (!bridge_quiet && extra->source != VP_GXM_EXTRA_SOURCE_DEPTH) {
            slog(ctx->svc,"VP_GXM_VERTEX_TEXTURE hash=0x%x va=0x%llx %ux%u fmt=%u cube=%u mips=%u sampler=%08x,%08x\n",
                 req.hash,(unsigned long long)extra->va,extra->width,extra->height,
                 extra->tex_format,extra->cube,extra->mip_count,
                 extra->sampler[0],extra->sampler[1]);
        }
    }
    if (req.texture_va != 0u && frame_target_seen(req.texture_va) &&
        req.texels == NULL) {
        /* The producer is a surface we cleared/rendered; the worker resolves it
         * from its surface cache. A miss is reported there. */
    }
    if (frame_auto && !frame_done) {
        if (frame_event_count < VP_FRAME_EVENT_LIMIT) {
            VPFrameEvent *event = &frame_events[frame_event_count++];
            event->kind = 2;
            event->draw = req;
            event->texels = linear;
            linear = NULL;
            rc = 0;
        } else {
            rc = -1;
        }
    } else {
        rc = guest_gxm_request(ctx->svc,0,&req,0);
        if (rc == 0 && frame_repeats > 1u && frame_event_count < VP_FRAME_EVENT_LIMIT) {
            VPFrameEvent *event = &frame_events[frame_event_count++];
            event->kind = 2;
            event->draw = req;
            event->texels = linear;
            linear = NULL;
        }
    }
    if (!linear_borrowed) free(linear);
    if (rc == 0) frame_submitted_draws++;
    /* B121: an accepted draw records its colour producer; a refused draw must
     * not (the surface was never created, see frame_submit_clear). */
    if (rc == 0 && req.color_mask != 0u) frame_note_target(req.target_va);
    if (!bridge_quiet || (ctx->submission % 128u) == 0u || rc != 0)
        slog(ctx->svc, "%s fd=%u submission=%u idx=%u hash=0x%x rc=%d "
             "target=0x%llx tex=0x%llx count=%u indexed=%u icount=%u mask=0x%x\n",
             frame_auto && !frame_done ? "VP_GXM_FRAME_RECORD" : "VP_GXM_FRAME_SUBMIT",
             ctx->fd, ctx->submission, draw->index, req.hash, rc,
             (unsigned long long)req.target_va, (unsigned long long)req.texture_va,
             req.count, req.indexed, req.index_count, req.color_mask);
    return rc;
}

static int frame_submit_draw(VPGpuSendContext *ctx, const vp_gpu_draw_state *draw) {
    uint64_t started = svc_play_perf_enabled() ? sceKernelGetProcessTimeWide() : 0u;
    int rc = frame_submit_draw_impl(ctx, draw);
    if (started) {
        gpu_cpu_perf.draws++;
        gpu_cpu_perf.draw_prepare_us += sceKernelGetProcessTimeWide() - started;
    }
    return rc;
}

static void frame_discard_events(void) {
    for (unsigned i = 0; i < frame_event_count; i++) free(frame_events[i].texels);
    frame_event_count = 0;
}

static int frame_execute_events_once(VPGuestSvc *svc, uint32_t fd,
                                     uint32_t submission, unsigned round,
                                     int count_draws) {
    unsigned ok = 0, failed = 0;

    for (unsigned i = 0; i < frame_event_count; i++) {
        VPFrameEvent *event = &frame_events[i];
        int rc;
        event->draw.batch_scope = ((uint64_t)fd << 32) | submission;
        event->draw.batch_encoded = 0u;
        event->draw.batch_fallback = 0u;
        rc = event->kind == 1 ? guest_gxm_request(svc,1,&event->clear,0)
                              : guest_gxm_request(svc,0,&event->draw,0);
        if (rc == 0) {
            ok++;
            if (count_draws && event->kind == 2) frame_submitted_draws++;
        } else {
            failed++;
        }
    }
    if (round == 0u)
        slog(svc, "VP_FRAME_EXECUTE fd=%u submission=%u events=%u ok=%u failed=%u\n",
             fd, submission, frame_event_count, ok, failed);
    else
        slog(svc, "VP_FRAME_REPLAY fd=%u submission=%u round=%u events=%u "
             "ok=%u failed=%u\n", fd, submission, round, frame_event_count,
             ok, failed);
    return failed == 0u ? 0 : -1;
}

/* Auto mode scans submissions until one contains the whole selected screen:
 * the 20-draw UI frame ends with hash 0x20 and includes the font pass 0xe69.
 * Only that submission is executed; earlier ones are recorded and discarded. */
static void frame_finish_submission(VPGuestSvc *svc, uint32_t fd,
                                    uint32_t submission) {
    unsigned draws = 0, clears = 0, saw_final = 0, saw_text = 0;
    unsigned i, r;

    if (frame_event_count == 0u) return;
    if (frame_auto) {
        int selected_shape;
        if (frame_done) { frame_discard_events(); return; }
        for (i = 0; i < frame_event_count; i++) {
            VPFrameEvent *event = &frame_events[i];
            if (event->kind == 1) { clears++; continue; }
            draws++;
            if (event->draw.hash == 0x20u) saw_final = 1;
            if (event->draw.hash == 0xe69u) saw_text = 1;
        }
        if (frame_shape_mode)
            selected_shape = draws == frame_shape_draws &&
                             clears == frame_shape_clears && saw_final;
        else
            selected_shape = draws >= 18u && clears >= 3u && saw_final && saw_text;
        if (selected_shape) {
            int failed, finish_rc;
            if (frame_shape_alpha_enabled &&
                (frame_shape_alpha_submission != submission ||
                 frame_shape_alpha_value != frame_shape_alpha)) {
                if (frame_shape_alpha_submission == submission)
                    slog(svc, "VP_FRAME_SHAPE_ALPHA_SKIP fd=%u submission=%u "
                         "alpha=0x%02x want=0x%02x\n", fd, submission,
                         frame_shape_alpha_value, frame_shape_alpha);
                else
                    slog(svc, "VP_FRAME_SHAPE_ALPHA_SKIP fd=%u submission=%u "
                         "alpha=unknown want=0x%02x\n", fd, submission,
                         frame_shape_alpha);
                frame_discard_events();
                return;
            }
            if (submission != frame_shape_last_match) {
                frame_shape_last_match = submission;
                frame_shape_matched++;
            }
            if (frame_shape_matched < frame_shape_occurrence) {
                slog(svc, "VP_FRAME_SHAPE_MATCH fd=%u submission=%u match=%u "
                     "occurrence=%u draws=%u clears=%u\n", fd, submission,
                     frame_shape_matched, frame_shape_occurrence, draws, clears);
                frame_discard_events();
                return;
            }
            failed = frame_execute_events_once(svc, fd, submission, 0u, 1);
            for (r = 1u; r < frame_repeats; r++)
                failed |= frame_execute_events_once(svc, fd, submission, r, 0);
            finish_rc = guest_gxm_request(svc,2,NULL,0);
            frame_done = 1;
            slog(svc, "VP_FRAME_SELECTED fd=%u submission=%u events=%u draws=%u "
                 "clears=%u occurrence=%u repeats=%u failed=%d finish_rc=%d\n",
                 fd, submission, frame_event_count, draws, clears,
                 frame_shape_occurrence, frame_repeats, failed, finish_rc);
            if (frame_shape_alpha_enabled)
                slog(svc, "VP_FRAME_SELECTED_ALPHA submission=%u alpha=0x%02x\n",
                     submission, frame_shape_alpha_value);
        } else {
            slog(svc, "VP_FRAME_SCAN fd=%u submission=%u events=%u draws=%u "
                 "clears=%u final=%u text=%u\n", fd, submission,
                 frame_event_count, draws, clears, saw_final, saw_text);
        }
        frame_discard_events();
        return;
    }
    if (frame_repeats > 1u && frame_mode_active(svc, submission) &&
        (int)fd == frame_execute_fd) {
        for (r = 1u; r < frame_repeats; r++)
            frame_execute_events_once(svc, fd, submission, r, 0);
    }
    frame_discard_events();
}



static void gpu_state_send(void *opaque, uint16_t address, uint32_t value) {
    VPGpuSendContext *ctx = opaque;
    VPGpuDecode *decode = ctx->decode;
    vp_gpu_draw_state draw;
    const unsigned frame_slot = ctx->fd & 7u;
    int frame_active;

    /* Device waits can release service state and permit mapping changes. */
    if (address == 0x585u || address == 0x674u) ctx->upload_size = 0u;
    display_bridge_init(ctx->svc);
    frame_active = frame_mode_active(ctx->svc, ctx->submission) &&
                   !(frame_auto && frame_done);
    if (frame_active && frame_execute_fd < 0) {
        frame_execute_fd = (int)ctx->fd;
        slog(ctx->svc, "VP_FRAME_CHANNEL fd=%u submission=%u\n",
             ctx->fd, ctx->submission);
    }
    if (frame_submission_seen[frame_slot] != ctx->submission) {
        uint32_t previous = frame_submission_seen[frame_slot];
        int previous_active = frame_mode_state == 1 &&
            (frame_first_submission == 0u || previous == frame_first_submission);
        if (previous_active && !frame_auto && frame_execute_fd >= 0 &&
            (int)ctx->fd == frame_execute_fd && frame_submitted_draws > 0u) {
            int finish_rc = guest_gxm_request(ctx->svc,2,NULL,0);
            slog(ctx->svc, "VP_GXM_FRAME_FINISH_SUBMIT fd=%u submission=%u rc=%d draws=%u\n",
                 ctx->fd, previous, finish_rc, frame_submitted_draws);
            frame_submitted_draws = 0;
        }
        if (previous_active && previous != 0xffffffffu)
            slog(ctx->svc, "VP_FRAME_SUBMISSION_END fd=%u submission=%u draws=%u\n",
                 ctx->fd, previous, frame_submission_draws[frame_slot]);
        frame_submission_seen[frame_slot] = ctx->submission;
        frame_submission_draws[frame_slot] = 0;
    }
    if ((frame_active || display_bridge_enabled) && address == 0x674u &&
        ctx->sub < VP_GPU_SUBCHANNELS) {
        const vp_gpu_subchannel *s = &decode->state.sub[ctx->sub];
        unsigned rt = (value >> 6) & 0xfu;
        uint32_t base = 0x200u + 16u * rt;
        uint64_t ta = ((uint64_t)s->value[base] << 32) | s->value[base + 1u];
        float cc[4];
        uint32_t cw[4];
        for (unsigned q = 0; q < 4u; q++) cw[q] = s->value[0x360u + q];
        memcpy(cc, cw, sizeof(cc));
        if (frame_active)
            slog(ctx->svc, "VP_FRAME_CLEAR fd=%u submission=%u sub=%u value=0x%08x mask=%u rt=%u "
                 "target=0x%llx %ux%u fmt=%u color=%.6f,%.6f,%.6f,%.6f\n",
                 ctx->fd, ctx->submission, ctx->sub, value,
                 value & 0x3fu, rt, (unsigned long long)ta,
                 s->value[base + 2u], s->value[base + 3u], s->value[base + 4u],
                 cc[0], cc[1], cc[2], cc[3]);
        if (frame_active ? (frame_auto ? !frame_done : (int)ctx->fd == frame_execute_fd)
                         : display_bridge_enabled)
            frame_submit_clear(ctx, value);
    }

    /* Constant-buffer uploads are GPU writes, not CPU stores. Preserve their
     * cursor and contents before a later draw reads the latched binding. */
    if (address >= 0x8e4u && address <= 0x8f3u && ctx->sub < VP_GPU_SUBCHANNELS) {
        vp_gpu_subchannel *s = &decode->state.sub[ctx->sub];
        uint32_t offset = s->value[0x8e3u];
        uint64_t base = ((uint64_t)s->value[0x8e1u] << 32) | s->value[0x8e2u];
        uint32_t dst = 0;
        uint32_t size = s->value[0x8e0u];
        if (ctx->upload_va != base || ctx->upload_size != size) {
            ctx->upload_size = 0u;
            /* Cache only if the first intersecting mapping covers the whole
             * buffer, preserving first-match semantics even for overlapping maps. */
            for (unsigned m = 0; size && m < ctx->session->gpu_as.mapping_count; ++m) {
                uint64_t start = ctx->session->gpu_as.mappings[m].offset;
                uint64_t span = ctx->session->gpu_as.mappings[m].size;
                if (!span || (base >= start ? base - start >= span : size <= start - base))
                    continue;
                if (base >= start && size <= span - (base - start)) {
                    ctx->upload_cpu = ctx->session->gpu_as.mappings[m].device_address +
                                      (uint32_t)(base - start);
                    ctx->upload_va = base;
                    ctx->upload_size = size;
                }
                break;
            }
        }
        int mapped = offset <= size && size - offset >= 4u &&
            (ctx->upload_size || gpu_va_to_cpu(ctx->session, base + offset, 4u, &dst));
        if (mapped) {
            if (ctx->upload_size) dst = ctx->upload_cpu + offset;
            memcpy((void *)(uintptr_t)dst, &value, 4u);
        } else {
            slog(ctx->svc, "VP_GPU_CB_UPLOAD rejected va=0x%llx offset=%u size=%u\n",
                 (unsigned long long)base, offset, s->value[0x8e0u]);
        }
    }
    vp_mme_shared_write(&decode->shared, address, value);
    vp_gpu_state_apply(&decode->state, ctx->sub, address, value);
    if (ctx->sub < VP_GPU_SUBCHANNELS) {
        if (address == 0x35eu) decode->indexed[ctx->sub] = 0;
        if (address == 0x5f8u) decode->indexed[ctx->sub] = 1;
    }
    if (address != 0x585u) return;
    vp_gpu_state_snapshot(&decode->state, ctx->sub, decode->draw_count,
                          ctx->sub < VP_GPU_SUBCHANNELS ? decode->indexed[ctx->sub] : 0,
                          &draw);
    draw.shaders[0] = decode->binds[0];
    draw.shaders[1] = decode->binds[1];
    draw.shader_count = 2;
    decode->draw_count++;
    if (frame_active) {
        VPGpuTextureInfo tex;
        uint64_t stream_va = draw.streams[0].address +
                             (uint64_t)draw.vertex_start * draw.streams[0].stride;
        uint32_t vertex_bytes = draw.streams[0].stride * draw.vertex_count;
        frame_submission_draws[frame_slot]++;
        resolve_draw_texture(ctx, &tex);
        {
            const vp_gpu_subchannel *s = &decode->state.sub[ctx->sub];
            uint64_t zeta_va = ((uint64_t)s->value[0x3f8u] << 32) | s->value[0x3f9u];
            slog(ctx->svc, "VP_FRAME_DRAW fd=%u submission=%u sub=%u idx=%u indexed=%u topo=%u "
             "start=%u count=%u icount=%u ifirst=%u ifmt=%u iaddr=0x%llx "
             "rt=0x%llx %ux%u fmt=%u targets=%u vp=%u,%u %ux%u "
             "zeta=0x%llx zen=%u zfmt=0x%x ztile=0x%x zpitch=0x%x "
             "zsize=%ux%u zdepth=%u zdim=%u zstencil=%u "
             "vs=0x%x@0x%llx fs=0x%x@0x%llx stride=%u stream=0x%llx vbytes=%u "
             "depth=%u w=%u func=%u cull=%u front=0x%x face=0x%x mask=0x%x "
             "blend=%u op=%u,%u src=%u,%u dst=%u,%u "
             "scissor=%u,%u,%u,%u,%u\n",
             ctx->fd, ctx->submission, ctx->sub, draw.index, draw.indexed,
             draw.draw_begin, draw.vertex_start, draw.vertex_count,
             draw.index_count, draw.index_first, draw.index_format,
             (unsigned long long)draw.index_address,
             (unsigned long long)draw.targets[0].address,
             draw.targets[0].width, draw.targets[0].height,
             draw.targets[0].format, draw.target_count,
             draw.viewport_x, draw.viewport_y,
             draw.viewport_width, draw.viewport_height,
             (unsigned long long)zeta_va,
             s->value[0x54eu], s->value[0x3fau], s->value[0x3fbu],
             s->value[0x3fcu], s->value[0x48au], s->value[0x48bu],
             s->value[0x48cu] & 0xffffu, (s->value[0x48cu] >> 16) & 1u,
             s->value[0x4e0u],
             draw.shaders[0].hash, (unsigned long long)draw.shaders[0].address,
             draw.shaders[1].hash, (unsigned long long)draw.shaders[1].address,
             draw.streams[0].stride, (unsigned long long)stream_va, vertex_bytes,
             draw.depth_test_enable, draw.depth_write_enable, draw.depth_func,
             draw.cull_enable, draw.front_face, draw.cull_face, draw.color_mask,
             draw.blend_per_target_enabled, draw.blend_color_op, draw.blend_alpha_op,
             draw.blend_color_src, draw.blend_alpha_src,
             draw.blend_color_dst, draw.blend_alpha_dst,
             s->value[0x380u], s->value[0x381u] & 0xffffu,
             s->value[0x381u] >> 16, s->value[0x382u] & 0xffffu,
             s->value[0x382u] >> 16);
            for (unsigned a = 0; a < 16u; a++) {
                uint32_t raw = s->value[0x458u + a];
                if (raw != 0u)
                    slog(ctx->svc, "VP_FRAME_ATTR fd=%u submission=%u idx=%u attr=%u raw=0x%08x\n",
                         ctx->fd, ctx->submission, draw.index, a, raw);
            }
            /* The 0xf78 overlay carries its fade alpha in byte 3 of the third
             * vertex word, one packed colour per vertex. Record it so the shape
             * marker can select a submission by animation phase. */
            if (frame_shape_alpha_enabled && draw.streams[0].stride >= 12u &&
                draw.vertex_count != 0u && draw.shaders[0].hash == 0xf78u) {
                uint32_t vertex_cpu = 0;
                if (gpu_va_to_cpu(ctx->session, stream_va, 12u, &vertex_cpu)) {
                    unsigned alpha = ((const uint8_t *)(uintptr_t)vertex_cpu)[11];
                    if (frame_shape_alpha_submission != ctx->submission)
                        slog(ctx->svc, "VP_FRAME_ALPHA fd=%u submission=%u alpha=0x%02x\n",
                             ctx->fd, ctx->submission, alpha);
                    frame_shape_alpha_value = alpha;
                    frame_shape_alpha_submission = ctx->submission;
                }
            }
        }
        if (tex.ok)
            slog(ctx->svc, "VP_FRAME_TEX fd=%u submission=%u idx=%u handle=0x%08x tic=%u "
                 "va=0x%llx %ux%u bh=%u fmt=%u header=%u bytes=%u sampler=%08x,%08x\n",
                 ctx->fd, ctx->submission, draw.index, tex.handle, tex.tic_index,
                 (unsigned long long)tex.va, tex.width, tex.height, tex.block_height,
                 tex.format, tex.header, (unsigned)tex.storage,
                 tex.sampler[0], tex.sampler[1]);
        else
            slog(ctx->svc, "VP_FRAME_TEX fd=%u submission=%u idx=%u missing\n",
                 ctx->fd, ctx->submission, draw.index);
        for (unsigned t = 0; t < draw.target_count; t++)
            slog(ctx->svc, "VP_FRAME_TARGET fd=%u submission=%u idx=%u rt=%u va=0x%llx "
                 "%ux%u fmt=%u tile=%u\n", ctx->fd, ctx->submission, draw.index, t,
                 (unsigned long long)draw.targets[t].address,
                 draw.targets[t].width, draw.targets[t].height,
                 draw.targets[t].format, draw.targets[t].tile_mode);
        if (frame_auto ? !frame_done : (int)ctx->fd == frame_execute_fd)
            frame_submit_draw(ctx, &draw);
    } else if (display_bridge_enabled) {
        /* Continuous play: execute this draw on both GPU channels. The
         * previous evidence mode selected one channel because it replayed one
         * known frame; the title alternates fd=4/fd=5 and each channel renders
         * a different display slot. */
        frame_submit_draw(ctx, &draw);
    }
    if (ctx->sub < VP_GPU_SUBCHANNELS) decode->indexed[ctx->sub] = 0;
    if (draw.streams[0].enabled &&
        draw.vertex_count >= 3 && draw.streams[0].stride != 0) {
        static int game_draw_count;
        static int game_draw_enabled = -1;
        if (game_draw_enabled < 0) {
            FILE *enable = fopen("ux0:data/mhgu-vita-port/gxm-test.enable", "rb");
            game_draw_enabled = enable != NULL;
            if (enable != NULL) fclose(enable);
        }
        /* M2 uniform regression: real non-indexed scene quads only. Indexed
         * scene draws belong to M3 and must never reuse stale vertex_count. */
        if (game_draw_enabled && draw.shaders[0].hash == 0xb89ul && !draw.indexed &&
            draw.draw_begin == 5u && game_draw_count < 60) {
            uint64_t va = draw.streams[0].address +
                          (uint64_t)draw.vertex_start * draw.streams[0].stride;
            uint32_t bytes = draw.vertex_count * draw.streams[0].stride;
            uint32_t cpu = 0;
            game_draw_count++;
            if (bytes <= 4096u && gpu_va_to_cpu(ctx->session, va, bytes, &cpu)) {
                uint32_t center = 0;
                unsigned nonzero = 0;
                float uniforms[4] = {0, 0, 0, 0};
                const float *frag_uniforms = NULL;
                uint64_t cb_va = ((uint64_t)decode->cbuf_high[0] << 32) |
                                 decode->cbuf_low[0];
                uint32_t cb_cpu = 0;
                uint64_t tex_va = 0;
                unsigned tex_w = 0, tex_h = 0, tex_bh = 0;
                unsigned char *linear = NULL;
                int tic_ok;
                uint32_t sampler[2] = {0, 0};
                int rc;
                if (game_draw_count == 1) {
                    vp_gpu_subchannel *s = &decode->state.sub[ctx->sub];
                    slog(ctx->svc, "VP_GPU_TEXSTATE header=%08x:%08x limit=%u bound=%u\n",
                         s->value[0x55du], s->value[0x55eu], s->value[0x55fu], s->value[0x982u]);
                    for (unsigned bank = 0; bank < 3u; bank++) {
                        const vp_gpu_cbuf_binding *b = &s->cbuf[4][bank];
                        uint32_t ptr = 0;
                        if (b->enabled && b->size >= 64u && gpu_va_to_cpu(ctx->session, b->address, 64u, &ptr))
                            slog(ctx->svc, "VP_GPU_TEXHANDLE bank=%u va=0x%llx words=%08x,%08x,%08x,%08x\n",
                                 bank, (unsigned long long)b->address, get32(ptr+32),get32(ptr+36),get32(ptr+40),get32(ptr+44));
                    }
                }
                if (cb_va != 0 && gpu_va_to_cpu(ctx->session, cb_va, sizeof(uniforms),
                                                &cb_cpu)) {
                    memcpy(uniforms, (const void *)(uintptr_t)cb_cpu, sizeof(uniforms));
                    frag_uniforms = uniforms;
                }
                {
                    vp_gpu_subchannel *state = &decode->state.sub[ctx->sub];
                    unsigned bank = state->value[0x982u];
                    uint64_t pool = ((uint64_t)state->value[0x55du] << 32) | state->value[0x55eu];
                    uint32_t handle_cpu = 0, descriptor_cpu = 0;
                    tic_ok = 0;
                    if (bank < VP_GPU_CBUF_SLOTS) {
                        const vp_gpu_cbuf_binding *binding = &state->cbuf[4][bank];
                        if (binding->enabled && binding->size >= 36u &&
                            gpu_va_to_cpu(ctx->session, binding->address + 32u, 4u, &handle_cpu)) {
                            uint32_t raw = get32(handle_cpu);
                            uint32_t index = raw & 0xfffffu;
                            uint64_t sampler_pool = ((uint64_t)state->value[0x557u] << 32) |
                                                    state->value[0x558u];
                            uint32_t sampler_cpu = 0;
                            if ((raw >> 20) <= state->value[0x559u] &&
                                gpu_va_to_cpu(ctx->session, sampler_pool + (uint64_t)(raw >> 20) * 32u,
                                              sizeof(sampler), &sampler_cpu))
                                memcpy(sampler, (const void *)(uintptr_t)sampler_cpu, sizeof(sampler));
                            if (index <= state->value[0x55fu] &&
                                gpu_va_to_cpu(ctx->session, pool + (uint64_t)index * 32u, 32u, &descriptor_cpu)) {
                                uint32_t hw[8];
                                unsigned fmt, header;
                                size_t storage;
                                memcpy(hw, (const void *)(uintptr_t)descriptor_cpu, sizeof(hw));
                                tex_va = ((uint64_t)(hw[2] & 0xffffu) << 32) | hw[1];
                                tex_w = (hw[4] & 0xffffu) + 1u;
                                tex_h = (hw[5] & 0xffffu) + 1u;
                                tex_bh = (hw[3] >> 3) & 7u;
                                fmt = hw[0] & 0x7fu;
                                header = (hw[2] >> 21) & 7u;
                                storage = header == 3u ? vp_texture_storage(tex_w, tex_h, fmt, tex_bh) : 0;
                                slog(ctx->svc, "VP_GXM_TEXTURE handle=0x%08x index=%u fmt=%u header=%u va=0x%llx %ux%u bh=%u bytes=%u\n",
                                     raw, index, fmt, header, (unsigned long long)tex_va, tex_w, tex_h, tex_bh, (unsigned)storage);
                                if (storage != 0u) {
                                    uint32_t source = 0;
                                    linear = malloc((size_t)tex_w * tex_h * 4u);
                                    if (linear != NULL && gpu_va_to_cpu(ctx->session, tex_va, storage, &source) &&
                                        vp_texture_rgba((const uint8_t *)(uintptr_t)source, storage, linear,
                                                        tex_w, tex_h, fmt, tex_bh) == 0) {
                                        tic_ok = 1;
                                        if (game_draw_count == 1) {
                                            FILE *dump = fopen("ux0:data/mhgu-vita-port/m2-texture.rgba", "wb");
                                            if (dump != NULL) { fwrite(linear, 4u, (size_t)tex_w*tex_h, dump); fclose(dump); }
                                        }
                                    }
                                    else { free(linear); linear = NULL; }
                                }
                            }
                        }
                    }
                }
                {
                    uint32_t t0 = 0;
                    if (linear != NULL) memcpy(&t0, linear, 4);
                    slog(ctx->svc, "VP_GXM_TEXUPLOAD linear=%d texel0=0x%08x\n",
                         linear != NULL ? 1 : 0, t0);
                }
                {
                    VPGxmDrawRequest req;
                    memset(&req, 0, sizeof(req));
                    req.vertices = (const void *)(uintptr_t)cpu;
                    req.hash = draw.shaders[0].hash;
                    req.vs_token = draw.shaders[0].hash;
                    req.fs_token = draw.shaders[1].hash;
                    req.viewport_transform = decode->state.sub[ctx->sub].value[0x64bu] & 1u;
                    memcpy(req.attributes, &decode->state.sub[ctx->sub].value[0x458u],
                           sizeof(req.attributes));
                    if (game_draw_count <= 2)
                        slog(ctx->svc, "VP_GPU_VIEWPORT transform=%u\n", req.viewport_transform);
                    req.count = draw.vertex_count;
                    req.stride = draw.streams[0].stride;
                    req.screen_width = draw.targets[0].width;
                    req.screen_height = draw.targets[0].height;
                    for (unsigned st = 0; st < 2u; st++) {
                        unsigned gpu_stage = st == 0u ? 0u : 4u;
                        for (unsigned bank = 0; bank < VP_GPU_CBUF_SLOTS; bank++) {
                            const vp_gpu_cbuf_binding *b =
                                &decode->state.sub[ctx->sub].cbuf[gpu_stage][bank];
                            uint32_t ptr = 0;
                            if (b->enabled && b->size != 0u &&
                                gpu_va_to_cpu(ctx->session, b->address, b->size, &ptr)) {
                                req.stage_cbuf[st][bank].data = (const void *)(uintptr_t)ptr;
                                req.stage_cbuf[st][bank].size = b->size;
                                if (game_draw_count == 1 && bank >= 3u && bank <= 6u) {
                                    char path[96];
                                    FILE *dump;
                                    snprintf(path, sizeof(path), "ux0:data/mhgu-vita-port/m2-%u-%u.bin", st, bank);
                                    dump = fopen(path, "wb");
                                    if (dump != NULL) {
                                        fwrite((const void *)(uintptr_t)ptr, 1, b->size, dump);
                                        fclose(dump);
                                    }
                                }
                                if (game_draw_count <= 2)
                                    slog(ctx->svc, "VP_GPU_CBUF stage=%u bank=%u va=0x%llx size=%u\n",
                                         gpu_stage, bank, (unsigned long long)b->address, b->size);
                            }
                        }
                    }
                    if (frag_uniforms != NULL) {
                        memcpy(req.uniforms, frag_uniforms, sizeof(req.uniforms));
                        req.has_uniforms = 1;
                    }
                    req.texels = linear;
                    req.tex_width = tex_w;
                    req.tex_height = tex_h;
                    memcpy(req.sampler, sampler, sizeof(sampler));
                    if (game_draw_count == 1) {
                        FILE *dump = fopen("ux0:data/mhgu-vita-port/m2-vertices.bin", "wb");
                        if (dump != NULL) {
                            fwrite(req.vertices, 1, bytes, dump);
                            fclose(dump);
                        }
                    }
                    if (game_draw_count == 1) {
                        FILE *sample = fopen("ux0:data/mhgu-vita-port/m2-sample.json", "wb");
                        if (sample != NULL) {
                            fprintf(sample, "{\"hash\":%u,\"indexed\":false,\"topology\":%u,\"stride\":%u,\"count\":%u,\"viewport_transform\":%u,\"width\":%u,\"height\":%u,\"texture_width\":%u,\"texture_height\":%u,\"sampler\":[%u,%u]}\n",
                                    req.hash, draw.draw_begin, req.stride, req.count,
                                    req.viewport_transform, req.screen_width, req.screen_height,
                                    tex_w, tex_h, sampler[0], sampler[1]);
                            fclose(sample);
                        }
                    }
                    rc = guest_gxm_request(ctx->svc,0,&req,0);
                    center = req.center;
                    nonzero = req.nonzero;
                }
                slog(ctx->svc, "VP_GXM_GAME_DRAW rc=%d va=0x%llx bytes=%u start=%u count=%u "
                     "stride=%u rt=%ux%u cbuf=0x%llx size=%u fs0=%.3f,%.3f,%.3f,%.3f "
                     "tic=%d tex=0x%llx %ux%u center=0x%08x nonzero=%u\n",
                     rc, (unsigned long long)va, bytes, draw.vertex_start,
                     draw.vertex_count, draw.streams[0].stride,
                     draw.targets[0].width, draw.targets[0].height,
                     (unsigned long long)cb_va, decode->cbuf_size[0],
                     uniforms[0], uniforms[1], uniforms[2], uniforms[3],
                     tic_ok, (unsigned long long)tex_va, tex_w, tex_h,
                     center, nonzero);
                free(linear);
            } else {
                slog(ctx->svc, "VP_GXM_GAME_DRAW skip va=0x%llx bytes=%u mapped=0\n",
                     (unsigned long long)va, bytes);
            }
        }
    }
    if (!ctx->log) return;
    slog(ctx->svc,
        "VP_GPU_DRAW fd=%u submission=%u draw=%u sub=%u indexed=%d topo=%u start=%u count=%u index_count=%u "
        "rt0=0x%llx rt0_w=%u rt0_h=%u fmt=%u vp=%ux%u s0=0x%llx stride=%u "
        "st1=0x%x@0x%llx st5=0x%x@0x%llx\n",
        ctx->fd, ctx->submission, draw.index, (unsigned)ctx->sub, draw.indexed,
        draw.draw_begin, draw.vertex_start, draw.vertex_count,
        draw.indexed ? draw.index_count : 0u,
        (unsigned long long)draw.targets[0].address, draw.targets[0].width,
        draw.targets[0].height, draw.targets[0].format,
        draw.viewport_width, draw.viewport_height,
        (unsigned long long)draw.streams[0].address, draw.streams[0].stride,
        draw.shaders[0].hash, (unsigned long long)draw.shaders[0].address,
        draw.shaders[1].hash, (unsigned long long)draw.shaders[1].address);
}

/* The command worker consumes one complete GPFIFO entry at a time. The
 * collector writes directly into the decoder-owned command buffer; a failed
 * malformed tail never dispatches the prefix it managed to decode. */

/* Deterministic input replay for M3 scene reachability. The file is a plain
 * text list of `at_ms buttons [lx ly]` full states using the reference
 * runtime's XInput bit values; it is converted to the Npad bits the title
 * reads from shared memory. Without the file the SceCtrl path is unchanged.
 * This replaces the controller interface, not any game logic. */
#define VP_HID_REPLAY_LIMIT 2048u
typedef struct {
    uint32_t at_ms;
    uint32_t buttons;
    int32_t lx;
    int32_t ly;
} VPHidReplayEvent;
static VPHidReplayEvent hid_replay[VP_HID_REPLAY_LIMIT];
static unsigned hid_replay_count;
static int hid_replay_state = -1;
static unsigned hid_replay_applied = 0xffffffffu;

static uint64_t hid_replay_npad(uint32_t buttons) {
    uint64_t npad = 0;
    if (buttons & 0x2000u) npad |= 1ull << 0;  /* Xbox B -> Switch A */
    if (buttons & 0x1000u) npad |= 1ull << 1;  /* Xbox A -> Switch B */
    if (buttons & 0x8000u) npad |= 1ull << 2;  /* Xbox Y -> Switch X */
    if (buttons & 0x4000u) npad |= 1ull << 3;  /* Xbox X -> Switch Y */
    if (buttons & 0x0040u) npad |= 1ull << 4;  /* LStick click -> LStick */
    if (buttons & 0x0080u) npad |= 1ull << 5;  /* RStick click -> RStick */
    if (buttons & 0x0100u) npad |= 1ull << 6;  /* LB -> L */
    if (buttons & 0x0200u) npad |= 1ull << 7;  /* RB -> R */
    if (buttons & 0x0001u) npad |= 1ull << 13; /* D-pad up */
    if (buttons & 0x0002u) npad |= 1ull << 15; /* D-pad down */
    if (buttons & 0x0004u) npad |= 1ull << 12; /* D-pad left */
    if (buttons & 0x0008u) npad |= 1ull << 14; /* D-pad right */
    if (buttons & 0x0010u) npad |= 1ull << 10; /* Start -> Plus */
    if (buttons & 0x0020u) npad |= 1ull << 11; /* Back -> Minus */
    return npad;
}

/* Explicit test-controller mailbox. It replaces pad input only when enabled;
 * it never changes game state. A sequence is consumed once, and every command
 * expires after at most 30 seconds even if the host stops sending releases.
 * Decimal fields: sequence, XInput buttons, left X/Y, duration in milliseconds.
 * Polling is bounded to 10 Hz; inactive commands leave physical/replay input. */
static int hid_live_input(VPGuestSvc *svc, uint64_t now_ms,
                          uint64_t *buttons, int32_t *lx, int32_t *ly) {
    static int enabled = -1;
    static uint64_t next_poll, expires;
    static unsigned last_sequence, held_buttons;
    static int held_x, held_y;
    FILE *file;
    if (enabled < 0) {
        file = fopen("ux0:data/mhgu-vita-port/input-live.enable", "rb");
        enabled = file != NULL;
        if (file) fclose(file);
        if (enabled) slog(svc, "VP_HID_LIVE enabled=1\n");
    }
    if (!enabled) return 0;
    if (now_ms >= next_poll) {
        unsigned sequence, mask, duration;
        int x, y;
        next_poll = now_ms + 100u;
        file = fopen("ux0:data/mhgu-vita-port/input-live.txt", "rb");
        if (file) {
            int fields = fscanf(file, "%u %u %d %d %u", &sequence, &mask, &x, &y, &duration);
            fclose(file);
            if (fields == 5 && sequence != 0u && sequence != last_sequence &&
                mask <= 65535u && x >= -32767 && x <= 32767 &&
                y >= -32767 && y <= 32767 && duration <= 30000u) {
                last_sequence = sequence;
                held_buttons = mask; held_x = x; held_y = y;
                expires = now_ms + duration;
                slog(svc, "VP_HID_LIVE seq=%u buttons=0x%x l=(%d,%d) duration=%u at_ms=%llu\n",
                     sequence, mask, x, y, duration, (unsigned long long)now_ms);
            }
        }
    }
    if (expires && now_ms >= expires) {
        slog(svc, "VP_HID_LIVE release seq=%u at_ms=%llu\n", last_sequence,
             (unsigned long long)now_ms);
        held_buttons = 0; held_x = held_y = 0; expires = 0;
    }
    *buttons = hid_replay_npad(held_buttons);
    *lx = held_x; *ly = held_y;
    return expires != 0u;
}

/* SceCtrl reports sticks as 0..255 with 128 centered. The title's npad state
 * uses signed axes centered at 0 (positive X right, positive Y up), so the
 * host Y axis (0 at top) is inverted. A small dead zone keeps a resting
 * controller from producing stick bits; the full-scale range is clamped to
 * the reference runtime's HID_JOYSTICK_MAX (0x7fff). */
#define VP_HID_CTRL_AXIS_CENTER 128
#define VP_HID_CTRL_AXIS_DEADZONE 8
#define VP_HID_CTRL_NAV_DEADZONE 48

static int32_t hid_ctrl_axis(unsigned char value, int invert) {
    int32_t centered = (int32_t)value - VP_HID_CTRL_AXIS_CENTER;
    int32_t scaled;
    if (invert) centered = -centered;
    if (centered >= -VP_HID_CTRL_AXIS_DEADZONE &&
        centered <= VP_HID_CTRL_AXIS_DEADZONE)
        return 0;
    scaled = centered * 258; /* 127 * 258 = 32766, -128 * 258 clamps below */
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32767) scaled = -32767;
    return scaled;
}

static uint64_t hid_ctrl_npad(uint32_t buttons) {
    uint64_t npad = 0;
    /* Preserve positions: east/south/north/west = Nintendo A/B/X/Y. */
    if (buttons & SCE_CTRL_CIRCLE) npad |= 1ull << 0;
    if (buttons & SCE_CTRL_CROSS) npad |= 1ull << 1;
    if (buttons & SCE_CTRL_TRIANGLE) npad |= 1ull << 2;
    if (buttons & SCE_CTRL_SQUARE) npad |= 1ull << 3;
    if (buttons & SCE_CTRL_L3) npad |= 1ull << 4;
    if (buttons & SCE_CTRL_R3) npad |= 1ull << 5;
    if (buttons & SCE_CTRL_L1) npad |= 1ull << 6;
    if (buttons & SCE_CTRL_R1) npad |= 1ull << 7;
    if (buttons & SCE_CTRL_L2) npad |= 1ull << 8;
    if (buttons & SCE_CTRL_R2) npad |= 1ull << 9;
    if (buttons & SCE_CTRL_UP) npad |= 1ull << 13;
    if (buttons & SCE_CTRL_DOWN) npad |= 1ull << 15;
    if (buttons & SCE_CTRL_LEFT) npad |= 1ull << 12;
    if (buttons & SCE_CTRL_RIGHT) npad |= 1ull << 14;
    if (buttons & SCE_CTRL_START) npad |= 1ull << 10;
    if (buttons & SCE_CTRL_SELECT) npad |= 1ull << 11;
    return npad;
}

/* Keyboard navigation uses the left stick as a second D-pad with a larger
 * dead zone than gameplay so a resting stick cannot move the selection. */
static uint32_t hid_ctrl_stick_dirs(const SceCtrlData *pad) {
    uint32_t dirs = 0;
    int32_t lx = (int32_t)pad->lx - VP_HID_CTRL_AXIS_CENTER;
    int32_t ly = (int32_t)pad->ly - VP_HID_CTRL_AXIS_CENTER;
    if (lx <= -VP_HID_CTRL_NAV_DEADZONE) dirs |= SCE_CTRL_LEFT;
    else if (lx >= VP_HID_CTRL_NAV_DEADZONE) dirs |= SCE_CTRL_RIGHT;
    if (ly <= -VP_HID_CTRL_NAV_DEADZONE) dirs |= SCE_CTRL_UP;
    else if (ly >= VP_HID_CTRL_NAV_DEADZONE) dirs |= SCE_CTRL_DOWN;
    return dirs;
}

/* The reference HID frontend sets both the analog axes and the digital
 * stick-direction bits in npad_buttons. Menus commonly read the latter, so
 * derive them here with the same half-deflection threshold. */
static uint64_t hid_stick_bits(int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    const int32_t threshold = 0x4000;
    uint64_t bits = 0;
    if (lx < -threshold) bits |= 1ull << 16; /* StickLLeft */
    if (ly > threshold) bits |= 1ull << 17;  /* StickLUp */
    if (lx > threshold) bits |= 1ull << 18;  /* StickLRight */
    if (ly < -threshold) bits |= 1ull << 19; /* StickLDown */
    if (rx < -threshold) bits |= 1ull << 20; /* StickRLeft */
    if (ry > threshold) bits |= 1ull << 21;  /* StickRUp */
    if (rx > threshold) bits |= 1ull << 22;  /* StickRRight */
    if (ry < -threshold) bits |= 1ull << 23; /* StickRDown */
    return bits;
}

/* Switch the pad to analog reporting once. Both the gameplay path and the
 * inline keyboard read the same SceCtrl sample. */
static void hid_ctrl_ensure_mode(VPGuestSvc *svc) {
    static int configured;
    int rc;
    if (configured) return;
    configured = 1;
    rc = sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    slog(svc, "VP_HID_CTRL_MODE mode=analog rc=%d\n", rc);
    rc = sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG);
    slog(svc, "VP_HID_CTRL_MODE mode=analog-ext rc=%d mapping=positional-abxy shoulders=L1-R1-ZL-ZR\n", rc);
}

static void hid_replay_load(VPGuestSvc *svc) {
    FILE *file;
    if (hid_replay_state >= 0) return;
    hid_replay_state = 0;
    file = fopen("ux0:data/mhgu-vita-port/input-replay.txt", "rb");
    if (file == NULL) return;
    while (hid_replay_count < VP_HID_REPLAY_LIMIT) {
        unsigned at_ms = 0, buttons = 0;
        int lx = 0, ly = 0;
        int got = fscanf(file, "%u %u %d %d", &at_ms, &buttons, &lx, &ly);
        if (got < 2) break;
        hid_replay[hid_replay_count].at_ms = at_ms;
        hid_replay[hid_replay_count].buttons = buttons;
        hid_replay[hid_replay_count].lx = lx;
        hid_replay[hid_replay_count].ly = ly;
        hid_replay_count++;
    }
    fclose(file);
    if (hid_replay_count == 0) return;
    hid_replay_state = 1;
    slog(svc, "VP_HID_REPLAY events=%u\n", hid_replay_count);
}

/* A terminal released state ends a finite replay. Otherwise the last state
 * intentionally remains held for older diagnostic replays. Use this in both
 * npad and the inline keyboard so neither can retain replay input focus after
 * an automated setup followed by interactive play. */
static int hid_replay_active(VPGuestSvc *svc, uint64_t now_ms) {
    const VPHidReplayEvent *last;
    hid_replay_load(svc);
    if (hid_replay_state != 1) return 0;
    last = &hid_replay[hid_replay_count - 1u];
    if (now_ms >= last->at_ms && last->buttons == 0u &&
        last->lx == 0 && last->ly == 0) {
        hid_replay_state = 2;
        slog(svc, "VP_HID_REPLAY_END at_ms=%u now_ms=%llu source=scectrl\n",
             last->at_ms, (unsigned long long)now_ms);
        return 0;
    }
    return 1;
}

/* Write a minimal nn::hid npad state into the title's 0x40000-byte shared
 * memory. The locked SDK reads it directly (detail::GetNpadStates), so the
 * layout is the Switch SharedMemoryFormat: npad entries start at 0x9A00,
 * ten entries of 0x5000 bytes, each with a 17-entry, 0x30-byte-stride Lifo
 * per style (FullKey at +0x28, Handheld at +0x378). A Lifo header is 0x20
 * bytes: timestamp, total count, tail, count. Each entry is a u64 sampling
 * number followed by the 0x28-byte NPadGenericState. The offsets were
 * confirmed against the SDK disassembly: FullKey reads tail/count at
 * internal_state+0x38/+0x40 and state at +0x50. */
#define VP_HID_NPAD_BASE 0x9A00u
#define VP_HID_NPAD_ENTRY_SIZE 0x5000u
#define VP_HID_FULLKEY_LIFO 0x28u
#define VP_HID_HANDHELD_LIFO 0x378u
#define VP_HID_DEVICE_TYPE 0x4188u
#define VP_HID_SYSTEM_PROPERTIES 0x4190u
#define VP_HID_LIFO_HEADER 0x20u
#define VP_HID_LIFO_ENTRY 0x30u
#define VP_HID_LIFO_ENTRIES 17u

static void hid_npad_entry_init(uint8_t *shm, uint32_t index,
                                uint32_t style_tag, uint32_t device_type,
                                uint64_t system_properties) {
    uint8_t *base = shm + VP_HID_NPAD_BASE + index * VP_HID_NPAD_ENTRY_SIZE;
    /* style_tag, assignment mode (Dual), colour attributes (Ok). */
    *(uint32_t *)(base + 0x00) = style_tag;
    *(uint32_t *)(base + 0x04) = 0;
    *(uint32_t *)(base + 0x08) = 0;
    *(uint32_t *)(base + 0x14) = 0;
    *(uint32_t *)(base + VP_HID_DEVICE_TYPE) = device_type;
    *(uint64_t *)(base + VP_HID_SYSTEM_PROPERTIES) = system_properties;
}

static void hid_lifo_write(uint8_t *shm, uint32_t index, uint32_t lifo_offset,
                           uint64_t buttons, int32_t lx, int32_t ly,
                           int32_t rx, int32_t ry, uint64_t now_ms) {
    uint8_t *lifo = shm + VP_HID_NPAD_BASE + index * VP_HID_NPAD_ENTRY_SIZE +
                    lifo_offset;
    int64_t *header = (int64_t *)lifo;
    int64_t total = header[1], tail = header[2], count = header[3];
    uint64_t sampling = 1;
    uint8_t *entry;

    if (total != (int64_t)VP_HID_LIFO_ENTRIES) {
        total = (int64_t)VP_HID_LIFO_ENTRIES;
        tail = 0;
        count = 0;
    }
    if (count > 0) {
        uint8_t *previous = lifo + VP_HID_LIFO_HEADER +
                            (size_t)tail * VP_HID_LIFO_ENTRY;
        sampling = *(uint64_t *)previous + 1u;
    }
    tail = (tail + 1) % (int64_t)VP_HID_LIFO_ENTRIES;
    if (count < (int64_t)VP_HID_LIFO_ENTRIES - 1) count++;
    entry = lifo + VP_HID_LIFO_HEADER + (size_t)tail * VP_HID_LIFO_ENTRY;
    *(uint64_t *)(entry + 0x00) = sampling;   /* AtomicStorage sampling */
    *(uint64_t *)(entry + 0x08) = sampling;   /* NPadGenericState sampling */
    *(uint64_t *)(entry + 0x10) = buttons;
    *(int32_t *)(entry + 0x18) = lx;
    *(int32_t *)(entry + 0x1C) = ly;
    *(int32_t *)(entry + 0x20) = rx;
    *(int32_t *)(entry + 0x24) = ry;
    *(uint32_t *)(entry + 0x28) = 1u; /* connection_status: connected */
    *(uint32_t *)(entry + 0x2C) = 0;
    header[0] = (int64_t)now_ms;
    header[1] = total;
    header[2] = tail;
    header[3] = count;
}

/* The title binds one style-set update event per npad id at startup and its
 * input threads then park on those events indefinitely. The reference HID
 * server signals them when a style set changes; this platform has no style
 * changes, so without a signal the threads never re-read the shared memory.
 * Wake them when the replay state actually changes. This is the event
 * plumbing around the same shared-memory state, not a second input path. */
static void hid_signal_style_events(VPGuestSvc *svc) {
    unsigned si, i;
    for (si = 0; si < VP_GUEST_SESSION_LIMIT; si++) {
        VPGuestSession *st = &svc->sessions[si];
        for (i = 0; i < st->style_event_count; i++) {
            VPGuestHandle *slot = find_handle(svc, st->style_events[i].handle);
            if (slot != NULL && slot->kind == 3) slot->signaled = 1;
        }
    }
}

/* While the inline software keyboard is shown it owns input focus: the real
 * applet receives the pad and the title does not. The loader polls the pad
 * for the keyboard itself in swkbd_poll_active, so masking the npad state
 * here only stops the game from also reacting to the same presses. */
static int swkbd_has_input_focus(const VPGuestSvc *svc) {
    for (unsigned i = 0; i < VP_GUEST_SESSION_LIMIT; ++i) {
        const VPGuestSession *s = &svc->sessions[i];
        if (s->swkbd.object && s->swkbd.state == VP_SWKBD_STATE_SHOWN) return 1;
    }
    return 0;
}

static void vp_hid_fill_state(VPGuestSvc *svc) {
    SceCtrlData pad;
    uint8_t *shm;
    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    const char *source = "scectrl";
    unsigned replay_index = 0xffffffffu;
    uint64_t now_ms;
    static uint64_t written_buttons;
    static int32_t written_lx, written_ly, written_rx, written_ry;
    static int written_valid;
    static uint32_t ctrl_last_buttons;
    static int32_t ctrl_last_lx, ctrl_last_ly, ctrl_last_rx, ctrl_last_ry;
    static int ctrl_last_valid;
    static unsigned ctrl_logs;
    static int capture_checked;
    static FILE *capture_file;
    if (svc->hid_shared_addr == 0) return;
    now_ms = ((uint64_t)sceKernelGetProcessTimeWide() - svc->tick_origin_us) / 1000u;
    if (!capture_checked) {
        FILE *marker = fopen("ux0:data/mhgu-vita-port/input-capture.enable", "rb");
        capture_checked = 1;
        if (marker) {
            fclose(marker);
            capture_file = fopen("ux0:data/mhgu-vita-port/input-capture.csv", "wb");
            if (capture_file) {
                fprintf(capture_file, "process_us,game_ms,source,npad,lx,ly,rx,ry\n");
                slog(svc, "VP_HID_CAPTURE enabled=1 path=input-capture.csv\n");
            }
        }
    }
    if (hid_live_input(svc, now_ms, &buttons, &lx, &ly)) {
        source = "live-test";
    } else if (hid_replay_active(svc, now_ms)) {
        source = "replay";
        for (unsigned i = 0; i < hid_replay_count; i++) {
            if (hid_replay[i].at_ms <= now_ms) replay_index = i;
            else break;
        }
        if (replay_index != 0xffffffffu) {
            buttons = hid_replay_npad(hid_replay[replay_index].buttons);
            lx = hid_replay[replay_index].lx;
            ly = hid_replay[replay_index].ly;
            rx = 0;
            ry = 0;
            if (replay_index != hid_replay_applied) {
                hid_replay_applied = replay_index;
                slog(svc, "VP_HID_REPLAY_APPLY index=%u at_ms=%u buttons=0x%x npad=0x%llx\n",
                     replay_index, hid_replay[replay_index].at_ms,
                     hid_replay[replay_index].buttons,
                     (unsigned long long)buttons);
            }
        }
    } else {
        /* Request analog axes once; without this some hosts leave lx/ly at 0
         * and a resting pad would look like full left/up. */
        hid_ctrl_ensure_mode(svc);
        memset(&pad, 0, sizeof(pad));
        pad.lx = pad.ly = pad.rx = pad.ry = 128;
        if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) < 0) return;
        buttons = hid_ctrl_npad(pad.buttons);
        lx = hid_ctrl_axis(pad.lx, 0);
        ly = hid_ctrl_axis(pad.ly, 1);
        rx = hid_ctrl_axis(pad.rx, 0);
        ry = hid_ctrl_axis(pad.ry, 1);
        if (!ctrl_last_valid || ctrl_last_buttons != pad.buttons ||
            ctrl_last_lx != lx || ctrl_last_ly != ly ||
            ctrl_last_rx != rx || ctrl_last_ry != ry) {
            ctrl_last_valid = 1;
            ctrl_last_buttons = pad.buttons;
            ctrl_last_lx = lx;
            ctrl_last_ly = ly;
            ctrl_last_rx = rx;
            ctrl_last_ry = ry;
            if (ctrl_logs < 512u) {
                ctrl_logs++;
                slog(svc, "VP_HID_CTRL buttons=0x%08x npad=0x%llx "
                     "l=(%d,%d) r=(%d,%d)\n",
                     pad.buttons, (unsigned long long)buttons, lx, ly, rx, ry);
            }
        }
    }
    buttons |= hid_stick_bits(lx, ly, rx, ry);
    if (swkbd_has_input_focus(svc)) {
        buttons = 0;
        lx = 0;
        ly = 0;
        rx = 0;
        ry = 0;
    }
    shm = (uint8_t *)(uintptr_t)svc->hid_shared_addr;
    /* The title reports OperationMode=handheld and polls npad id 0x20 as
     * well as player 1, so keep both entries populated. Only the matching
     * style tag is advertised; the other style's Lifo is still written in
     * case the SDK asks for it under the same id. */
    hid_npad_entry_init(shm, 0, 1u /* FullKey */, 1u /* device fullkey */,
                        (1ull << 11) | (1ull << 13) | (1ull << 14));
    hid_npad_entry_init(shm, 8, 2u /* Handheld */, 0xcu /* left+right */,
                        (1ull << 11) | (1ull << 13) | (1ull << 14) |
                        (1ull << 15));
    hid_lifo_write(shm, 0, VP_HID_FULLKEY_LIFO, buttons, lx, ly, rx, ry, now_ms);
    hid_lifo_write(shm, 0, VP_HID_HANDHELD_LIFO, buttons, lx, ly, rx, ry, now_ms);
    hid_lifo_write(shm, 8, VP_HID_FULLKEY_LIFO, buttons, lx, ly, rx, ry, now_ms);
    hid_lifo_write(shm, 8, VP_HID_HANDHELD_LIFO, buttons, lx, ly, rx, ry, now_ms);
    if (!written_valid || written_buttons != buttons ||
        written_lx != lx || written_ly != ly ||
        written_rx != rx || written_ry != ry) {
        written_valid = 1;
        written_buttons = buttons;
        written_lx = lx;
        written_ly = ly;
        written_rx = rx;
        written_ry = ry;
        if (capture_file) {
            fprintf(capture_file, "%llu,%llu,%s,0x%llx,%d,%d,%d,%d\n",
                    (unsigned long long)sceKernelGetProcessTimeWide(),
                    (unsigned long long)now_ms, source,
                    (unsigned long long)buttons, lx, ly, rx, ry);
            fflush(capture_file);
        }
        hid_signal_style_events(svc);
    }
    if (svc->module_bases[3] != 0u) {
        /* Diagnostic only: read the SDK's npad pointer table and the state
         * the writer just produced. module 3 + 0x8e4e30 is the table base
         * used by detail::GetNpadStates (entries at +0x40+index*4). */
        uint32_t table = svc->module_bases[3] + 0x8e4e30u + 0x40u;
        uint32_t p0 = *(volatile uint32_t *)(uintptr_t)table;
        uint32_t p8 = *(volatile uint32_t *)(uintptr_t)(table + 8u * 4u);
        uint32_t expect0 = svc->hid_shared_addr + VP_HID_NPAD_BASE;
        uint32_t expect8 = expect0 + 8u * VP_HID_NPAD_ENTRY_SIZE;
        static unsigned diag_fills;
        static uint32_t last0, last8;
        if (diag_fills < 8u || p0 != last0 || p8 != last8) {
            uint8_t *b0 = (uint8_t *)(uintptr_t)expect0;
            uint8_t *b8 = (uint8_t *)(uintptr_t)expect8;
            int64_t t0 = *(int64_t *)(b0 + VP_HID_FULLKEY_LIFO + 0x10);
            int64_t c0 = *(int64_t *)(b0 + VP_HID_FULLKEY_LIFO + 0x18);
            int64_t t8 = *(int64_t *)(b8 + VP_HID_HANDHELD_LIFO + 0x10);
            int64_t c8 = *(int64_t *)(b8 + VP_HID_HANDHELD_LIFO + 0x18);
            uint64_t btn0 = *(uint64_t *)(b0 + VP_HID_FULLKEY_LIFO + 0x20 +
                                          (size_t)t0 * VP_HID_LIFO_ENTRY + 0x10);
            uint64_t btn8 = *(uint64_t *)(b8 + VP_HID_HANDHELD_LIFO + 0x20 +
                                          (size_t)t8 * VP_HID_LIFO_ENTRY + 0x10);
            uint8_t *e0 = b0 + VP_HID_FULLKEY_LIFO + 0x20 +
                          (size_t)t0 * VP_HID_LIFO_ENTRY;
            uint8_t *e8 = b8 + VP_HID_HANDHELD_LIFO + 0x20 +
                          (size_t)t8 * VP_HID_LIFO_ENTRY;
            diag_fills++;
            last0 = p0;
            last8 = p8;
            slog(svc, "VP_HID_LAYOUT p0=0x%08x expect0=0x%08x p8=0x%08x expect8=0x%08x "
                 "style0=0x%x style8=0x%x dev0=0x%x dev8=0x%x "
                 "tail0=%d count0=%d btn0=0x%llx stick0=(%d,%d,%d,%d) "
                 "tail8=%d count8=%d btn8=0x%llx stick8=(%d,%d,%d,%d)\n",
                 p0, expect0, p8, expect8,
                 *(uint32_t *)(b0), *(uint32_t *)(b8),
                 *(uint32_t *)(b0 + VP_HID_DEVICE_TYPE),
                 *(uint32_t *)(b8 + VP_HID_DEVICE_TYPE),
                 (int)t0, (int)c0, (unsigned long long)btn0,
                 *(int32_t *)(e0 + 0x18), *(int32_t *)(e0 + 0x1C),
                 *(int32_t *)(e0 + 0x20), *(int32_t *)(e0 + 0x24),
                 (int)t8, (int)c8, (unsigned long long)btn8,
                 *(int32_t *)(e8 + 0x18), *(int32_t *)(e8 + 0x1C),
                 *(int32_t *)(e8 + 0x20), *(int32_t *)(e8 + 0x24));
        }
    }
    if (hid_replay_state == 1 && replay_index != 0xffffffffu &&
        hid_replay_applied == replay_index) {
        static unsigned fill_logs;
        if (fill_logs < 3u) {
            fill_logs++;
            slog(svc, "VP_HID_FILL n=%u source=%s buttons=0x%llx "
                 "stick=(%d,%d,%d,%d) npad0_style=0x%x npad8_style=0x%x\n",
                 fill_logs, source, (unsigned long long)buttons,
                 lx, ly, rx, ry,
                 *(uint32_t *)(shm + VP_HID_NPAD_BASE),
                 *(uint32_t *)(shm + VP_HID_NPAD_BASE +
                               8u * VP_HID_NPAD_ENTRY_SIZE));
        }
    }
}

static void vp_guest_gpu_decode_entry_locked(VPGuestSvc *svc, VPGuestSession *state,
        VPGuestGpuChannel *channel, uint32_t fd, uint32_t submission,
        uint32_t cpu, uint32_t words, int log) {
    VPGpuDecode *decode = channel->gpu_decode;
    vp_gpu_decode_stats stats;
    int perf = svc_play_perf_enabled();
    uint64_t phase = perf ? sceKernelGetProcessTimeWide() : 0u;
    uint64_t phase_end;
    size_t k = 0;
    size_t write_count = 0;
    unsigned layout_consumed=0u;
    int bad;

    vp_hid_fill_state(svc);
    if (decode == NULL) {
        decode = (VPGpuDecode *)calloc(1, sizeof(*decode));
        if (decode == NULL) {
            if (perf)
                gpu_cpu_perf.entry_setup_us +=
                    sceKernelGetProcessTimeWide() - phase;
            return;
        }
        vp_macro_table_init(&decode->macros);
        vp_macro_upload_state_init(&decode->upload);
        vp_mme_shared_init(&decode->shared);
        vp_gpu_state_init(&decode->state);
        decode->binds[0].stage = 1u;
        decode->binds[1].stage = 5u;
        channel->gpu_decode = decode;
    }
    bad = !words || words > 0x1fffffu || cpu == 0u ||
          !vp_guest_svc_readable(svc, cpu, words * 4u) ||
          vp_gpu_command_buffer_reserve(&decode->commands, words) < 0;
    if (perf) {
        phase_end = sceKernelGetProcessTimeWide();
        gpu_cpu_perf.entry_setup_us += phase_end - phase;
        phase = phase_end;
    }
    if (bad) {
        ++svc->ipc_unsupported;
        slog(svc,"VP_GPU_ENTRY_REJECT fd=%u submission=%u words=%u reason=allocation-or-size\n",fd,submission,words);
        return;
    }
    /* Snapshot the whole entry before decode/dispatch. The guest command RAM
     * stays the producer's; decoder state never borrows a mutable pointer. */
    memcpy(decode->commands.words,(const void *)(uintptr_t)cpu,
           (size_t)words * sizeof(uint32_t));
    if (!(++decode->layout_report_entries&1023u)) {
        vp_engine_layout_report(svc->log);
        vp_engine_full_report(svc->log);
    }
    if (perf) {
        phase_end = sceKernelGetProcessTimeWide();
        gpu_cpu_perf.command_copy_us += phase_end - phase;
        gpu_cpu_perf.command_words += words;
        phase = phase_end;
    }
    if (vp_gpu_decode_collect(decode->commands.words, words,
                              decode->commands.writes, decode->commands.capacity,
                              &stats) != 0) {
        if (perf) {
            phase_end = sceKernelGetProcessTimeWide();
            gpu_cpu_perf.command_collect_us += phase_end - phase;
        }
        ++svc->ipc_unsupported;
        slog(svc,"VP_GPU_ENTRY_REJECT fd=%u submission=%u words=%u reason=decode writes=%u\n",
             fd,submission,words,(unsigned)stats.writes);
        return;
    }
    write_count = stats.writes;
    if (perf) {
        phase_end = sceKernelGetProcessTimeWide();
        gpu_cpu_perf.command_collect_us += phase_end - phase;
        gpu_cpu_perf.decoded_writes += write_count;
        phase = phase_end;
    }
    VPGpuSendContext direct = {0};
    direct.decode = decode;
    direct.svc = svc;
    direct.session = state;
    direct.fd = fd;
    direct.submission = submission;
    direct.log = log;
    while (k < write_count) {
        vp_gpu_write write = decode->commands.writes[k];
        if (write.method==VP_ENGINE_LAYOUT_METHOD &&
            (write.value&0xffff0000u)==VP_ENGINE_LAYOUT_TOKEN) {
            if (vp_engine_layout_consume(write.value,write.sub,&decode->state,&decode->shared)) {
                ++svc->ipc_unsupported;
                slog(svc,"VP_ENGINE_LAYOUT_REJECT token=%08x sub=%u fd=%u submission=%u\n",
                    write.value,write.sub,fd,submission);
                return;
            }
            layout_consumed++;
            k++;
            continue;
        }
        if (write.method < 0xE00u) {
            vp_macro_upload_apply(&decode->macros, &decode->upload,
                                  write.method, write.value);
            direct.sub = write.sub;
            gpu_state_send(&direct, write.method, write.value);
            if (write.sub < VP_GPU_SUBCHANNELS) {
                if (write.method == 0x35eu) decode->indexed[write.sub] = 0;
                if (write.method == 0x5f8u) decode->indexed[write.sub] = 1;
                if (write.method == 0x8e0u) decode->cbuf_size[write.sub] = write.value;
                if (write.method == 0x8e1u) decode->cbuf_high[write.sub] = write.value;
                if (write.method == 0x8e2u) decode->cbuf_low[write.sub] = write.value;
            }
            k++;
            continue;
        }
        if ((write.method & 1u) != 0u) {
            k++;
            continue;
        }
        {
            size_t param_count = 1;
            size_t index = (size_t)((write.method - 0xE00u) >> 1);
            VPGpuSendContext ctx = {0};
            vp_mme_result result;

            decode->commands.params[0] = write.value;
            k++;
            while (k < write_count &&
                   decode->commands.writes[k].method == (uint16_t)(write.method + 1u)) {
                decode->commands.params[param_count++] = decode->commands.writes[k].value;
                k++;
            }
            if (write.method == 0xE18u && param_count >= 5u &&
                (decode->commands.params[0] == 1u || decode->commands.params[0] == 5u)) {
                size_t slot = decode->commands.params[0] == 1u ? 0u : 1u;
                decode->binds[slot].stage = decode->commands.params[0];
                decode->binds[slot].hash = decode->commands.params[1];
                decode->binds[slot].address =
                    ((uint64_t)(decode->commands.params[4] >> 24) << 32) | decode->commands.params[2];
            }
            if (index >= decode->macros.position_count) continue;
            memset(&result, 0, sizeof(result));
            result.supported = 1;
            ctx.decode = decode;
            ctx.svc = svc;
            ctx.session = state;
            ctx.sub = write.sub;
            ctx.fd = fd;
            ctx.submission = submission;
            ctx.log = log;
            vp_mme_execute(&decode->macros, decode->macros.positions[index],
                           decode->commands.params, param_count, &decode->shared,
                           gpu_state_send, &ctx, &result);
            direct.upload_size = 0u;
        }
    }
    vp_engine_layout_note_consumed(layout_consumed);
    if (perf) {
        phase_end = sceKernelGetProcessTimeWide();
        gpu_cpu_perf.command_dispatch_us += phase_end - phase;
        phase = phase_end;
    }
    frame_finish_submission(svc, fd, submission);
    if (perf)
        gpu_cpu_perf.submission_finish_us +=
            sceKernelGetProcessTimeWide() - phase;
}
typedef struct {
    VPGuestSvc *svc;
    VPGuestSession *state;
    VPGuestGpuChannel *channel;
    uint32_t fd, submission, cpu, words, token[2];
    /* B112 wall-clock handoff: submitting timestamp before enqueue and the
     * worker callback's final timestamp. The submitter reads callback_end_us
     * only after vp_gxm_worker_execute signals completion. */
    uint64_t queued_us, callback_end_us;
    int log;
} VPGpuDecodeWork;

static void gpu_decode_on_worker(void *opaque) {
    VPGpuDecodeWork *work = opaque;
    uint32_t released[2];
    int perf = svc_play_perf_enabled();
    /* B104: this is the only Session.state_lock handoff on the inline path.
     * It also restores the submitting guest thread's handle/TLS, so the whole
     * decoder entry, including its inline GXM requests, keeps that identity
     * until the release below. */
    uint64_t acquire_started = perf ? sceKernelGetProcessTimeWide() : 0u;
    if (perf && work->queued_us && acquire_started >= work->queued_us)
        gpu_cpu_perf.worker_queue_us += acquire_started - work->queued_us;
    device_wait_end(work->svc, work->token);
    uint64_t started = perf ? sceKernelGetProcessTimeWide() : 0u;
    if (acquire_started)
        gpu_cpu_perf.segment_acquire_us += started - acquire_started;
    vp_guest_gpu_decode_entry_locked(work->svc, work->state, work->channel,
        work->fd, work->submission, work->cpu, work->words, work->log);
    if (started) {
        gpu_cpu_perf.entries++;
        gpu_cpu_perf.decode_us += sceKernelGetProcessTimeWide() - started;
        if ((gpu_cpu_perf.entries & 255u) == 0u)
            slog(work->svc, "VP_GPU_CPU_PERF t_us=%llu entries=%llu draws=%llu "
                 "decode_incl_us=%llu draw_prepare_incl_us=%llu "
                 "gxm_call_incl_us=%llu device_release_us=%llu "
                 "gxm_request_core_us=%llu device_reacquire_us=%llu "
                 "segment_acquire_us=%llu segment_release_us=%llu "
                 "entry_setup_us=%llu command_copy_us=%llu "
                 "command_collect_us=%llu command_dispatch_us=%llu "
                 "submission_finish_us=%llu command_words=%llu "
                 "decoded_writes=%llu gate_wait_us=%llu worker_queue_us=%llu "
                 "worker_postwake_us=%llu caller_reacquire_us=%llu "
                 "external_release_us=%llu external_core_us=%llu "
                 "external_reacquire_us=%llu\n",
                 (unsigned long long)sceKernelGetProcessTimeWide(),
                 (unsigned long long)gpu_cpu_perf.entries,
                 (unsigned long long)gpu_cpu_perf.draws,
                 (unsigned long long)gpu_cpu_perf.decode_us,
                 (unsigned long long)gpu_cpu_perf.draw_prepare_us,
                 (unsigned long long)gpu_cpu_perf.gxm_call_us,
                 (unsigned long long)gpu_cpu_perf.device_release_us,
                 (unsigned long long)gpu_cpu_perf.gxm_request_core_us,
                 (unsigned long long)gpu_cpu_perf.device_reacquire_us,
                 (unsigned long long)gpu_cpu_perf.segment_acquire_us,
                 (unsigned long long)gpu_cpu_perf.segment_release_us,
                 (unsigned long long)gpu_cpu_perf.entry_setup_us,
                 (unsigned long long)gpu_cpu_perf.command_copy_us,
                 (unsigned long long)gpu_cpu_perf.command_collect_us,
                 (unsigned long long)gpu_cpu_perf.command_dispatch_us,
                 (unsigned long long)gpu_cpu_perf.submission_finish_us,
                 (unsigned long long)gpu_cpu_perf.command_words,
                 (unsigned long long)gpu_cpu_perf.decoded_writes,
                 (unsigned long long)gpu_cpu_perf.gate_wait_us,
                 (unsigned long long)gpu_cpu_perf.worker_queue_us,
                 (unsigned long long)gpu_cpu_perf.worker_postwake_us,
                 (unsigned long long)gpu_cpu_perf.caller_reacquire_us,
                 (unsigned long long)gpu_cpu_perf.external_release_us,
                 (unsigned long long)gpu_cpu_perf.external_core_us,
                 (unsigned long long)gpu_cpu_perf.external_reacquire_us);
    }
    uint64_t release_started = perf ? sceKernelGetProcessTimeWide() : 0u;
    device_wait_begin(work->svc, released);
    if (release_started)
        gpu_cpu_perf.segment_release_us +=
            sceKernelGetProcessTimeWide() - release_started;
    /* Final worker-side observation. The submitter turns this into the
     * callback-end-to-submit-return (post work wake) wall segment. */
    if (perf) work->callback_end_us = sceKernelGetProcessTimeWide();
}

static int vp_guest_gpu_decode_entry(VPGuestSvc *svc, VPGuestSession *state,
        VPGuestGpuChannel *channel, uint32_t fd, uint32_t submission,
        uint32_t cpu, uint32_t words, int log) {
    if (gpu_gate_enter(svc)<0) {
        ++svc->ipc_unsupported;
        slog(svc,"VP_GPU_GATE_FAILED fd=%u submission=%u\n",fd,submission);
        return -1;
    }
    display_bridge_init(svc);
    if (!display_bridge_enabled && frame_mode_state != 1) {
        vp_guest_gpu_decode_entry_locked(svc, state, channel, fd, submission, cpu, words, log);
        sceKernelUnlockMutex(gpu_execution_gate, 1);
        return 0;
    }
    int perf = svc_play_perf_enabled();
    VPGpuDecodeWork work = {svc, state, channel, fd, submission, cpu, words,
                            {0, 0}, 0u, 0u, log};
    /* The caller keeps the GPU gate until the worker returns. Release service
     * state while waiting; the worker acquires it once for the whole decode,
     * including the inline GXM requests, and releases it when the entry ends. */
    device_wait_begin(svc, work.token);
    work.queued_us = perf ? sceKernelGetProcessTimeWide() : 0u;
    int rc = vp_gxm_worker_execute(gpu_decode_on_worker, &work);
    if (perf) {
        uint64_t returned = sceKernelGetProcessTimeWide();
        if (work.callback_end_us && returned >= work.callback_end_us)
            gpu_cpu_perf.worker_postwake_us += returned - work.callback_end_us;
    }
    uint64_t reacquire_started = perf ? sceKernelGetProcessTimeWide() : 0u;
    device_wait_end(svc, work.token);
    if (reacquire_started)
        gpu_cpu_perf.caller_reacquire_us +=
            sceKernelGetProcessTimeWide() - reacquire_started;
    sceKernelUnlockMutex(gpu_execution_gate,1);
    if (rc < 0) {
        ++svc->ipc_unsupported;
        slog(svc, "VP_GPU_EXECUTE_FAILED fd=%u submission=%u rc=%d\n", fd, submission, rc);
    }
    return rc;
}

static uint32_t create_handle(VPGuestSvc *svc, uint32_t kind, const char *name, uint32_t arg) {
    for (unsigned i = 0; i < VP_GUEST_HANDLE_LIMIT; ++i) {
        VPGuestHandle *slot = &svc->handles[i];
        if (slot->kind) continue;
        memset(slot, 0, sizeof(*slot));
        slot->kind = kind;
        slot->arg = arg;
        for (unsigned j = 0; j < 8 && name[j]; ++j) slot->name[j] = name[j];
        if (kind == 2) {
            for (unsigned s = 0; s < VP_GUEST_SESSION_LIMIT; ++s) {
                if (svc->sessions[s].refs) continue;
                memset(&svc->sessions[s], 0, sizeof(svc->sessions[s]));
                svc->sessions[s].refs = 1;
                slot->session = s + 1;
                break;
            }
            if (!slot->session) {
                slot->kind = 0;
                slog(svc, "SVC_SESSION_TABLE_FULL kind=%u name=%s limit=%u\n",
                        kind, name, VP_GUEST_SESSION_LIMIT);
                sflush(svc);
                return 0;
            }
        }
        return VP_GUEST_HANDLE_FIRST + i;
    }
    slog(svc, "SVC_HANDLE_TABLE_FULL kind=%u name=%s limit=%u\n",
            kind, name, VP_GUEST_HANDLE_LIMIT);
    sflush(svc);
    return 0;
}

static VPGuestSession *handle_session(VPGuestSvc *svc, VPGuestHandle *handle) {
    if (!handle || handle->kind != 2 || !handle->session) return NULL;
    return &svc->sessions[handle->session - 1];
}

/* HIPC CloneCurrentObject binds a second client handle to the same server
 * session. Both handles must observe one domain object table and one set of
 * service-side state, so the session is shared and reference-counted. */
static uint32_t clone_handle(VPGuestSvc *svc, const VPGuestHandle *source) {
    if (source->kind != 2 || !source->session) return 0;
    for (unsigned i = 0; i < VP_GUEST_HANDLE_LIMIT; ++i) {
        VPGuestHandle *slot = &svc->handles[i];
        if (slot->kind) continue;
        memset(slot, 0, sizeof(*slot));
        slot->kind = 2;
        slot->session = source->session;
        for (unsigned j = 0; j < 8 && source->name[j]; ++j) slot->name[j] = source->name[j];
        ++svc->sessions[source->session - 1].refs;
        return VP_GUEST_HANDLE_FIRST + i;
    }
    slog(svc, "SVC_HANDLE_TABLE_FULL kind=clone name=%s limit=%u\n",
            source->name, VP_GUEST_HANDLE_LIMIT);
    sflush(svc);
    return 0;
}

/* Swkbd protocol constants observed in the reviewed inline-keyboard request
 * and reply layouts. The loader speaks this protocol itself because no applet
 * process runs; the meanings come from the public Horizon library-applet ABI
 * and the request bytes the title actually sends. */

/* Encode the model's UTF-16LE keyboard text as UTF-8 for the *Utf8 reply
 * variants. The inline protocol keeps the same 0x3ec-byte text field; only
 * its interpretation and the reported length change. */
static uint32_t swkbd_utf8_encode(const uint8_t *src, uint32_t bytes,
                                  uint8_t *out, uint32_t capacity) {
    uint32_t o = 0;
    for (uint32_t i = 0; i + 1u < bytes; i += 2u) {
        uint32_t cp = (uint32_t)src[i] | ((uint32_t)src[i + 1u] << 8);
        if (cp >= 0xd800u && cp < 0xdc00u && i + 3u < bytes) {
            uint32_t lo = (uint32_t)src[i + 2u] | ((uint32_t)src[i + 3u] << 8);
            if (lo >= 0xdc00u && lo < 0xe000u) {
                cp = 0x10000u + ((cp - 0xd800u) << 10) + (lo - 0xdc00u);
                i += 2u;
            }
        }
        uint8_t tmp[4];
        uint32_t n;
        if (cp < 0x80u) {
            tmp[0] = (uint8_t)cp; n = 1u;
        } else if (cp < 0x800u) {
            tmp[0] = (uint8_t)(0xc0u | (cp >> 6));
            tmp[1] = (uint8_t)(0x80u | (cp & 0x3fu)); n = 2u;
        } else if (cp < 0x10000u) {
            tmp[0] = (uint8_t)(0xe0u | (cp >> 12));
            tmp[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
            tmp[2] = (uint8_t)(0x80u | (cp & 0x3fu)); n = 3u;
        } else {
            tmp[0] = (uint8_t)(0xf0u | (cp >> 18));
            tmp[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3fu));
            tmp[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
            tmp[3] = (uint8_t)(0x80u | (cp & 0x3fu)); n = 4u;
        }
        if (o + n > capacity) break;
        memcpy(out + o, tmp, n);
        o += n;
    }
    return o;
}

/* Allocate the lowest free domain object id instead of a monotonically
 * increasing one. The client closes objects during its normal lifecycle, and
 * the 64-entry table must be reusable or a long keyboard session (one reply
 * storage per pop) exhausts it. Object 1 is the converted root. */
static uint32_t alloc_object_id(VPGuestSession *state) {
    for (uint32_t id = 2; id < VP_GUEST_DOMAIN_OBJECT_LIMIT; ++id) {
        if (state->objects[id] != VP_GUEST_OBJECT_NONE) continue;
        state->lapplet_event[id] = 0;
        state->lapplet_id[id] = 0;
        state->lapplet_mode[id] = 0;
        state->lapplet_started[id] = 0;
        state->lapplet_in_data[id] = 0;
        state->lapplet_pop_event[id] = 0;
        state->lapplet_interactive_event[id] = 0;
        state->lapplet_complete[id] = 0;
        state->storage_parent[id] = 0;
        state->save_objects[id] = NULL;
        if (state->objects[id] == VP_GUEST_OBJECT_NONE) {
            state->storages[id].data = NULL;
            state->storages[id].size = 0;
            state->storages[id].pushed = 0;
            state->storages[id].source_address = 0;
        }
        return id;
    }
    return 0;
}


/* Queue one inline-keyboard reply for PopInteractiveOutData. The reply is the
 * exact layout the applet protocol defines: SwkbdState, SwkbdReplyType, then
 * for text replies a fixed 0x3ec-byte UTF-16 or 0x7d4-byte UTF-8 buffer followed by the argument
 * struct. Queueing signals the pop event when the guest already holds it. */
static int swkbd_queue_reply(VPGuestSvc *svc, VPGuestSession *state,
                             uint32_t accessor, uint32_t type) {
    uint32_t size = 8;
    int utf8 = type == VP_SWKBD_REPLY_CHANGED_STRING_UTF8
        || type == VP_SWKBD_REPLY_DECIDED_ENTER_UTF8;
    uint32_t text_capacity = utf8 ? 0x7d4u : VP_SWKBD_UTF16_REPLY_BYTES;
    int text_reply = type == VP_SWKBD_REPLY_CHANGED_STRING
        || type == VP_SWKBD_REPLY_CHANGED_STRING_V2
        || type == VP_SWKBD_REPLY_CHANGED_STRING_UTF8
        || type == VP_SWKBD_REPLY_DECIDED_ENTER
        || type == VP_SWKBD_REPLY_DECIDED_ENTER_UTF8;
    if (type == VP_SWKBD_REPLY_CHANGED_STRING || type == VP_SWKBD_REPLY_CHANGED_STRING_UTF8)
        size += text_capacity + 0x10;
    else if (type == VP_SWKBD_REPLY_CHANGED_STRING_V2)
        size += text_capacity + 0x10 + 1;
    else if (type == VP_SWKBD_REPLY_DECIDED_ENTER || type == VP_SWKBD_REPLY_DECIDED_ENTER_UTF8)
        size += text_capacity + 4;
    else if (type == VP_SWKBD_REPLY_FINISHED_INIT)
        size = 9;
    if (state->swkbd.reply_count >= VP_SWKBD_REPLY_QUEUE) return -1;
    uint8_t *reply = (uint8_t *)calloc(1, size);
    if (!reply) return -1;
    put32((uint32_t)(uintptr_t)reply, state->swkbd.state);
    put32((uint32_t)(uintptr_t)reply + 4, type);
    if (text_reply) {
        uint32_t arg = (uint32_t)(uintptr_t)reply + 8 + text_capacity;
        uint32_t units;
        if (utf8) {
            (void)swkbd_utf8_encode(state->swkbd.text, state->swkbd.text_len,
                                      reply + 8, text_capacity);
            units = state->swkbd.text_len / 2u;
        } else {
            uint32_t text_bytes = state->swkbd.text_len;
            if (text_bytes > VP_SWKBD_UTF16_REPLY_BYTES)
                text_bytes = VP_SWKBD_UTF16_REPLY_BYTES;
            memcpy(reply + 8, state->swkbd.text, text_bytes);
            units = text_bytes / 2u;
        }
        put32(arg, units);
        if (type != VP_SWKBD_REPLY_DECIDED_ENTER
            && type != VP_SWKBD_REPLY_DECIDED_ENTER_UTF8) {
            put32(arg + 4, 0xffffffffu); /* dictionary start: none */
            put32(arg + 8, 0xffffffffu); /* dictionary end: none */
            put32(arg + 0xc, state->swkbd.cursor);
        }
    }
    state->swkbd.replies[state->swkbd.reply_count].data = reply;
    state->swkbd.replies[state->swkbd.reply_count].size = size;
    ++state->swkbd.reply_count;
    if (accessor < VP_GUEST_DOMAIN_OBJECT_LIMIT) {
        VPGuestHandle *event = find_handle(svc, state->lapplet_interactive_event[accessor]);
        if (event) event->signaled = 1;
    }
    slog(svc, "SVC_IPC_SWKBD_REPLY accessor=%u type=%u state=%u size=0x%x text_chars=%u utf8=%u queued=%u\n",
            accessor, type, state->swkbd.state, size, state->swkbd.text_len / 2u,
            state->swkbd.utf8_mode, state->swkbd.reply_count);
    return 0;
}

/* Register a host-side IStorage carrying a queued swkbd reply and return its
 * domain object id. PopInteractiveOutData hands this id to the client, which
 * reads the bytes through IStorageAccessor; nothing is mapped into the guest. */
static uint32_t swkbd_reply_storage(VPGuestSvc *svc, VPGuestSession *state,
                                    uint8_t *bytes, uint32_t size) {
    uint32_t id = alloc_object_id(state);
    if (!id) return 0;
    state->objects[id] = VP_GUEST_OBJECT_STORAGE;
    state->storages[id].data = bytes;
    state->storages[id].size = size;
    state->storages[id].pushed = 0;
    (void)svc;
    return id;
}

/* The root domain object implements the interface of the named service the
 * session was connected to. Unreviewed names stay object 0 so every call on
 * them is reported as unsupported instead of being answered generically. */
/* Minimal inline-keyboard frontend. The applet process does not run here, so
 * the loader is the applet's frontend: it draws the keyboard into the indirect
 * layer image the title maps for GetIndirectLayerImageMap and turns physical
 * pad edges into the inline protocol's replies. The layout is deliberately
 * small (A-Z, 0-9, space, period, hyphen, DEL, OK); the text model is UTF-16
 * like the protocol, and pad-generated characters are ASCII. This is real
 * input and a real drawn image, not a test pattern. */
#define VP_SWKBD_KEY_CHARS 39u
static const char swkbd_key_chars[VP_SWKBD_KEY_CHARS + 1u] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .-";

/* 5x7 row-major glyphs, low five bits per row. Only the characters the
 * keyboard can emit (and the labels DEL/OK) are defined. */
static const uint8_t swkbd_font[128][7] = {
    ['A'] = {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11},
    ['B'] = {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},
    ['C'] = {0x0e,0x11,0x10,0x10,0x10,0x11,0x0e},
    ['D'] = {0x1e,0x11,0x11,0x11,0x11,0x11,0x1e},
    ['E'] = {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f},
    ['F'] = {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
    ['G'] = {0x0e,0x11,0x10,0x17,0x11,0x11,0x0f},
    ['H'] = {0x11,0x11,0x11,0x1f,0x11,0x11,0x11},
    ['I'] = {0x0e,0x04,0x04,0x04,0x04,0x04,0x0e},
    ['J'] = {0x07,0x02,0x02,0x02,0x02,0x12,0x0c},
    ['K'] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    ['L'] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
    ['M'] = {0x11,0x1b,0x15,0x15,0x11,0x11,0x11},
    ['N'] = {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    ['O'] = {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e},
    ['P'] = {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},
    ['Q'] = {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d},
    ['R'] = {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
    ['S'] = {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e},
    ['T'] = {0x1f,0x04,0x04,0x04,0x04,0x04,0x04},
    ['U'] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0e},
    ['V'] = {0x11,0x11,0x11,0x11,0x11,0x0a,0x04},
    ['W'] = {0x11,0x11,0x11,0x15,0x15,0x1b,0x11},
    ['X'] = {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11},
    ['Y'] = {0x11,0x11,0x0a,0x04,0x04,0x04,0x04},
    ['Z'] = {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f},
    ['0'] = {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e},
    ['1'] = {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
    ['2'] = {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f},
    ['3'] = {0x1f,0x02,0x04,0x02,0x01,0x11,0x0e},
    ['4'] = {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02},
    ['5'] = {0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e},
    ['6'] = {0x06,0x08,0x10,0x1e,0x11,0x11,0x0e},
    ['7'] = {0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
    ['8'] = {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},
    ['9'] = {0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c},
    ['.'] = {0x00,0x00,0x00,0x00,0x00,0x0c,0x0c},
    ['-'] = {0x00,0x00,0x00,0x1f,0x00,0x00,0x00},
    [' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

static uint32_t swkbd_rgb(unsigned r, unsigned g, unsigned b) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xff000000u;
}

static void swkbd_rect(uint32_t *dst, uint32_t stride_words, uint32_t width,
                       uint32_t height, int x0, int y0, int rw, int rh,
                       uint32_t color) {
    if (x0 < 0) { rw += x0; x0 = 0; }
    if (y0 < 0) { rh += y0; y0 = 0; }
    if (rw <= 0 || rh <= 0) return;
    if ((uint32_t)x0 >= width || (uint32_t)y0 >= height) return;
    if ((uint32_t)(x0 + rw) > width) rw = (int)(width - (uint32_t)x0);
    if ((uint32_t)(y0 + rh) > height) rh = (int)(height - (uint32_t)y0);
    for (int y = y0; y < y0 + rh; ++y) {
        uint32_t *row = dst + (uint32_t)y * stride_words;
        for (int x = x0; x < x0 + rw; ++x) row[x] = color;
    }
}

static void swkbd_glyph(uint32_t *dst, uint32_t stride_words, uint32_t width,
                        uint32_t height, int x, int y, unsigned scale,
                        uint32_t color, char c) {
    const uint8_t *g = swkbd_font[(unsigned char)c & 0x7fu];
    for (unsigned row = 0; row < 7u; ++row)
        for (unsigned col = 0; col < 5u; ++col)
            if (g[row] & (1u << (4u - col)))
                swkbd_rect(dst, stride_words, width, height,
                           x + (int)(col * scale), y + (int)(row * scale),
                           (int)scale, (int)scale, color);
}

static void swkbd_text(uint32_t *dst, uint32_t stride_words, uint32_t width,
                       uint32_t height, int x, int y, unsigned scale,
                       uint32_t color, const char *text, unsigned chars) {
    for (unsigned i = 0; i < chars; ++i) {
        int gx = x + (int)(i * 6u * scale);
        if (gx + (int)(5u * scale) >= (int)width) break;
        swkbd_glyph(dst, stride_words, width, height, gx, y, scale, color, text[i]);
    }
}

static VPGuestSession *swkbd_active_session(VPGuestSvc *svc) {
    for (unsigned i = 0; i < VP_GUEST_SESSION_LIMIT; ++i) {
        VPGuestSession *s = &svc->sessions[i];
        if (s->swkbd.object && s->swkbd.state == VP_SWKBD_STATE_SHOWN) return s;
    }
    return NULL;
}

/* Draw the keyboard into the layer image. Called for every image map while the
 * keyboard is shown; rendering is skipped until an input or state change. */
static void swkbd_render_image(VPGuestSession *state, void *buffer,
                               uint32_t width, uint32_t height, uint32_t stride) {
    uint32_t *dst = (uint32_t *)buffer;
    uint32_t stride_words = stride / 4u;
    const uint32_t bg = swkbd_rgb(0x10, 0x18, 0x20);
    const uint32_t panel = swkbd_rgb(0xb0, 0xc0, 0xd0);
    const uint32_t panel_sel = swkbd_rgb(0xf0, 0x90, 0x30);
    const uint32_t ink = swkbd_rgb(0x10, 0x10, 0x10);
    const uint32_t white = swkbd_rgb(0xf0, 0xf0, 0xf0);
    unsigned m = width / 40u;
    unsigned text_h = height / 6u;
    unsigned tscale = height / 80u;
    unsigned grid_y, cell_w, cell_h;
    uint32_t sel = state->swkbd.selection;

    if (m < 4u) m = 4u;
    if (tscale < 2u) tscale = 2u;
    if (sel > 40u) sel = 0u;
    swkbd_rect(dst, stride_words, width, height, 0, 0, (int)width, (int)height, bg);
    swkbd_rect(dst, stride_words, width, height, (int)m, (int)(m / 2u),
               (int)(width - 2u * m), (int)text_h, swkbd_rgb(0x20, 0x28, 0x38));
    if (state->swkbd.text_len >= 2u) {
        uint32_t chars = state->swkbd.text_len / 2u;
        char line[0x1f6];
        for (uint32_t i = 0; i < chars && i < sizeof(line); ++i) {
            uint32_t cp = (uint32_t)state->swkbd.text[i * 2u]
                | ((uint32_t)state->swkbd.text[i * 2u + 1u] << 8);
            line[i] = cp < 0x80u ? (char)cp : '?';
        }
        swkbd_text(dst, stride_words, width, height, (int)(m * 2u),
                   (int)(m / 2u + text_h / 2u - 3u * tscale), tscale, white,
                   line, chars < sizeof(line) ? chars : (unsigned)sizeof(line));
        swkbd_rect(dst, stride_words, width, height,
                   (int)(m * 2u + chars * 6u * tscale), (int)(m / 2u + text_h / 2u),
                   2, (int)(7u * tscale), white);
    } else {
        swkbd_text(dst, stride_words, width, height, (int)(m * 2u),
                   (int)(m / 2u + text_h / 2u - 3u * tscale), tscale,
                   swkbd_rgb(0x80, 0x80, 0x80), "NAME", 4);
    }
    grid_y = text_h + m;
    cell_w = (width - 2u * m) / 13u;
    cell_h = (height - grid_y - m) / 4u;
    for (uint32_t i = 0; i < VP_SWKBD_KEY_CHARS; ++i) {
        unsigned row = i / 13u, col = i % 13u;
        int x = (int)(m + col * cell_w);
        int y = (int)(grid_y + row * cell_h);
        unsigned scale = (cell_w - 8u) / 6u;
        unsigned vs = (cell_h - 8u) / 9u;
        if (vs < scale) scale = vs;
        if (scale < 1u) scale = 1u;
        swkbd_rect(dst, stride_words, width, height,
                   x + 2, y + 2, (int)cell_w - 4, (int)cell_h - 4,
                   i == sel ? panel_sel : panel);
        swkbd_glyph(dst, stride_words, width, height,
                    x + (int)(cell_w - 5u * scale) / 2,
                    y + (int)(cell_h - 7u * scale) / 2,
                    scale, ink, swkbd_key_chars[i]);
    }
    {
        int y = (int)(grid_y + 3u * cell_h);
        int half = (int)(6u * cell_w);
        unsigned scale = (cell_h - 8u) / 9u;
        if (scale < 1u) scale = 1u;
        swkbd_rect(dst, stride_words, width, height, (int)m + 2, y + 2,
                   half - 4, (int)cell_h - 4, sel == 39u ? panel_sel : panel);
        swkbd_rect(dst, stride_words, width, height, (int)m + half + 2, y + 2,
                   (int)(width - 2u * m - (unsigned)half) - 4,
                   (int)cell_h - 4, sel == 40u ? panel_sel : panel);
        swkbd_text(dst, stride_words, width, height,
                   (int)(m + (unsigned)half / 2u - 9u * scale),
                   y + (int)(cell_h - 7u * scale) / 2, scale, ink, "DEL", 3);
        swkbd_text(dst, stride_words, width, height,
                   (int)(m + (unsigned)half + (width - 2u * m - (unsigned)half) / 2u
                         - 6u * scale),
                   y + (int)(cell_h - 7u * scale) / 2, scale, ink, "OK", 2);
    }
}

/* The title stops mapping the indirect keyboard image as soon as the
 * applet's Hidden reply is consumed. Clear the last mapped image ourselves so
 * Vita3K cannot keep compositing the final opaque keyboard frame. */
static void swkbd_clear_rendered_image(VPGuestSvc *svc,VPGuestSession *state,
                                       const char *reason) {
    uint64_t bytes=(uint64_t)state->swkbd.render_width*
                   state->swkbd.render_height*4u;
    uint32_t address=state->swkbd.render_addr;
    if (!address || !bytes || bytes>0xffffffffu ||
        !vp_guest_svc_writable(svc,address,(uint32_t)bytes)) {
        slog(svc,"SVC_SWKBD_LAYER_CLEAR reason=%s address=0x%08x width=%u height=%u result=unmapped\n",
             reason,address,state->swkbd.render_width,state->swkbd.render_height);
        return;
    }
    memset((void *)(uintptr_t)address,0,(size_t)bytes);
    slog(svc,"SVC_SWKBD_LAYER_CLEAR reason=%s address=0x%08x width=%u height=%u bytes=%u result=transparent\n",
         reason,address,state->swkbd.render_width,state->swkbd.render_height,(unsigned)bytes);
    state->swkbd.render_addr=0;
    state->swkbd.render_width=0;
    state->swkbd.render_height=0;
    state->swkbd.render_dirty=1;
}

/* Turn pad press edges into keyboard edits and inline replies. Called from the
 * applet channel handlers, which run on the applet session's own thread. */
static void swkbd_poll_active(VPGuestSvc *svc) {
    VPGuestSession *state = swkbd_active_session(svc);
    SceCtrlData pad;
    uint32_t pressed, sel, row, col;
    uint32_t stick_dirs;
    const char *source = "scectrl";
    int changed = 0, decide = 0, moved = 0;

    if (!state || state->swkbd.decided) return;
    memset(&pad, 0, sizeof(pad));
    pad.lx = pad.ly = pad.rx = pad.ry = 128;
    uint64_t ms=((uint64_t)sceKernelGetProcessTimeWide()-svc->tick_origin_us)/1000u;
    if (hid_replay_active(svc, ms)) {
        uint32_t buttons=0;
        source = "replay";
        for (unsigned i=0;i<hid_replay_count && hid_replay[i].at_ms<=ms;i++)
            buttons=hid_replay[i].buttons;
        if (buttons&0x2000u) pad.buttons|=SCE_CTRL_CIRCLE;
        if (buttons&0x1000u) pad.buttons|=SCE_CTRL_CROSS;
        if (buttons&0x4000u) pad.buttons|=SCE_CTRL_SQUARE;
        if (buttons&0x8000u) pad.buttons|=SCE_CTRL_TRIANGLE;
        if (buttons&0x0001u) pad.buttons|=SCE_CTRL_UP;
        if (buttons&0x0002u) pad.buttons|=SCE_CTRL_DOWN;
        if (buttons&0x0004u) pad.buttons|=SCE_CTRL_LEFT;
        if (buttons&0x0008u) pad.buttons|=SCE_CTRL_RIGHT;
        if (buttons&0x0010u) pad.buttons|=SCE_CTRL_START;
    } else {
        hid_ctrl_ensure_mode(svc);
        if (sceCtrlPeekBufferPositiveExt2(0,&pad,1)<0) return;
    }
    stick_dirs = hid_ctrl_stick_dirs(&pad);
    pressed = (pad.buttons | stick_dirs) & ~state->swkbd.input_last;
    state->swkbd.input_last = pad.buttons | stick_dirs;
    if (!pressed) return;
    sel = state->swkbd.selection;
    if (sel > 40u) sel = 0u;
    row = sel < VP_SWKBD_KEY_CHARS ? sel / 13u : 3u;
    col = sel < VP_SWKBD_KEY_CHARS ? sel % 13u : sel - VP_SWKBD_KEY_CHARS;
    if (pressed & SCE_CTRL_LEFT) {
        if (row < 3u) col = (col + 12u) % 13u;
        else col = 0u;
        moved = 1;
    }
    if (pressed & SCE_CTRL_RIGHT) {
        if (row < 3u) col = (col + 1u) % 13u;
        else col = 1u;
        moved = 1;
    }
    if (pressed & SCE_CTRL_UP) {
        if (row > 0u) --row;
        if (row == 3u && col > 1u) col = 1u;
        moved = 1;
    }
    if (pressed & SCE_CTRL_DOWN) {
        if (row < 3u) ++row;
        if (row == 3u && col > 1u) col = 1u;
        moved = 1;
    }
    if (moved) {
        state->swkbd.selection = row < 3u ? row * 13u + col
                                          : VP_SWKBD_KEY_CHARS + (col ? 1u : 0u);
        state->swkbd.render_dirty = 1;
        sel = state->swkbd.selection;
    }
    if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_SQUARE)) {
        if (state->swkbd.text_len >= 2u && state->swkbd.cursor > 0u) {
            state->swkbd.text_len -= 2u;
            state->swkbd.cursor--;
            changed = 1;
        }
    }
    if (pressed & SCE_CTRL_CIRCLE) {
        if (sel == VP_SWKBD_KEY_CHARS + 1u) {
            decide = 1;
        } else if (sel == VP_SWKBD_KEY_CHARS) {
            if (state->swkbd.text_len >= 2u && state->swkbd.cursor > 0u) {
                state->swkbd.text_len -= 2u;
                state->swkbd.cursor--;
                changed = 1;
            }
        } else if (sel < VP_SWKBD_KEY_CHARS
                   && (!state->swkbd.max_chars || state->swkbd.text_len/2u<state->swkbd.max_chars)
                   && state->swkbd.text_len + 2u <= sizeof(state->swkbd.text)) {
            char c = swkbd_key_chars[sel];
            state->swkbd.text[state->swkbd.text_len] = (uint8_t)c;
            state->swkbd.text[state->swkbd.text_len + 1u] = 0;
            state->swkbd.text_len += 2u;
            state->swkbd.cursor = state->swkbd.text_len / 2u;
            changed = 1;
        }
    }
    if (pressed & (SCE_CTRL_START | SCE_CTRL_TRIANGLE)) decide = 1;
    if (changed) {
        uint32_t type = state->swkbd.utf8_mode ? VP_SWKBD_REPLY_CHANGED_STRING_UTF8
            : state->swkbd.changed_v2 ? VP_SWKBD_REPLY_CHANGED_STRING_V2
                                      : VP_SWKBD_REPLY_CHANGED_STRING;
        state->swkbd.render_dirty = 1;
        if (swkbd_queue_reply(svc, state, state->swkbd.object, type) < 0)
            slog(svc, "SVC_IPC_SWKBD_QUEUE_FULL accessor=%u type=%u\n",
                 state->swkbd.object, type);
    }
    if (decide && state->swkbd.text_len/2u<state->swkbd.min_chars) decide=0;
    if (decide) {
        uint32_t type = state->swkbd.utf8_mode
            ? VP_SWKBD_REPLY_DECIDED_ENTER_UTF8
            : VP_SWKBD_REPLY_DECIDED_ENTER;
        state->swkbd.decided = 1;
        state->swkbd.render_dirty = 1;
        if (swkbd_queue_reply(svc, state, state->swkbd.object, type) < 0)
            slog(svc, "SVC_IPC_SWKBD_QUEUE_FULL accessor=%u type=%u\n",
                 state->swkbd.object, type);
        state->swkbd.state=VP_SWKBD_STATE_DISAPPEARING;
        swkbd_queue_reply(svc,state,state->swkbd.object,VP_SWKBD_REPLY_DEFAULT);
        state->swkbd.state=VP_SWKBD_STATE_HIDDEN;
        swkbd_queue_reply(svc,state,state->swkbd.object,VP_SWKBD_REPLY_DEFAULT);
        /* The title's last GetIndirectLayerImageMap can be its final request:
         * after it consumes the hidden reply, it stops polling the applet.
         * Clear the mapped layer now, while the image backing is still live,
         * rather than waiting for Finalize after the renderer has stopped
         * sampling it. */
        swkbd_clear_rendered_image(svc,state,"decision");
    }
    slog(svc, "SVC_IPC_SWKBD_INPUT source=%s buttons=0x%08x dirs=0x%08x "
         "sel=%u chars=%u changed=%d decided=%d utf8=%u state=%u\n",
         source, pad.buttons, stick_dirs, state->swkbd.selection,
         state->swkbd.text_len / 2u, changed, decide,
         state->swkbd.utf8_mode, state->swkbd.state);
}

/* Diagnostic self-test for the keyboard frontend. With the marker present,
 * render a sample keyboard into a 960x544 RGBA image and write it out with
 * pixel statistics. This exercises the same renderer the layer map uses; it
 * does not fabricate a game reply or a visible frame. */
static void swkbd_selftest(VPGuestSvc *svc, VPGuestSession *state) {
    static const uint8_t sample[] = {'A', 0, 'B', 0, 'C', 0};
    FILE *marker = fopen("ux0:data/mhgu-vita-port/swkbd-selftest.enable", "rb");
    uint32_t width = 960u, height = 544u, stride = 960u * 4u;
    uint8_t *image;
    uint32_t saved_state, saved_selection, saved_len, saved_cursor;
    uint8_t saved_text[8];
    unsigned nz = 0, max = 0;
    if (!marker) return;
    fclose(marker);
    image = (uint8_t *)calloc(1, (size_t)stride * height);
    if (!image) return;
    saved_state = state->swkbd.state;
    saved_selection = state->swkbd.selection;
    saved_len = state->swkbd.text_len;
    saved_cursor = state->swkbd.cursor;
    memcpy(saved_text, state->swkbd.text, sizeof(saved_text));
    state->swkbd.state = VP_SWKBD_STATE_SHOWN;
    state->swkbd.selection = 5u;
    state->swkbd.text_len = 6u;
    memset(state->swkbd.text, 0, 8);
    memcpy(state->swkbd.text, sample, sizeof(sample));
    state->swkbd.cursor = 3u;
    swkbd_render_image(state, image, width, height, stride);
    state->swkbd.state = saved_state;
    state->swkbd.selection = saved_selection;
    state->swkbd.text_len = saved_len;
    state->swkbd.cursor = saved_cursor;
    memcpy(state->swkbd.text, saved_text, sizeof(saved_text));
    for (uint32_t i = 0; i < width * height; ++i) {
        const uint8_t *p = image + (size_t)i * 4u;
        unsigned m = p[0] > p[1] ? p[0] : p[1];
        if (p[2] > m) m = p[2];
        if (m > 8u) ++nz;
        if (m > max) max = m;
    }
    {
        FILE *out = fopen("ux0:data/mhgu-vita-port/swkbd-selftest.rgba", "wb");
        if (out) {
            fwrite(image, 4u, (size_t)width * height, out);
            fclose(out);
        }
    }
    free(image);
    slog(svc, "SVC_IPC_SWKBD_SELFTEST image=%ux%u nonzero=%u max=%u file=ux0:data/mhgu-vita-port/swkbd-selftest.rgba\n",
         width, height, nz, max);
}

static uint8_t root_interface(const char *name) {
    if (!strcmp(name, "appletOE")) return VP_GUEST_OBJECT_APPLICATION_PROXY_SERVICE;
    if (!strcmp(name, "pctl")) return VP_GUEST_OBJECT_PARENTAL_CONTROL_SERVICE_FACTORY;
    if (!strcmp(name, "pctl:s")) return VP_GUEST_OBJECT_PARENTAL_CONTROL_SERVICE;
    if (!strcmp(name, "pctl:a")) return VP_GUEST_OBJECT_PARENTAL_CONTROL_SERVICE;
    if (!strcmp(name, "pctl:r")) return VP_GUEST_OBJECT_PARENTAL_CONTROL_SERVICE;
    if (!strcmp(name, "aoc:u")) return VP_GUEST_OBJECT_AOC_SERVICE;
    if (!strcmp(name, "time:u")) return VP_GUEST_OBJECT_TIME_SERVICE;
    if (!strcmp(name, "fsp-srv")) return VP_GUEST_OBJECT_FILE_SYSTEM_PROXY;
    if (!strcmp(name, "friend:u") || !strcmp(name, "friend:m")
        || !strcmp(name, "friend:s") || !strcmp(name, "friend:v"))
        return VP_GUEST_OBJECT_FRIEND_SERVICE;
    if (!strcmp(name, "bcat:u")) return VP_GUEST_OBJECT_BCAT_SERVICE_CREATOR;
    return VP_GUEST_OBJECT_NONE;
}

/* Open the RomFS read backing on first use. The three paths are generated
 * data installed beside the title; when they are absent the storage calls
 * fail with a logged reason instead of pretending the image is empty. */
static int ensure_storage(VPGuestSvc *svc) {
    if (svc->storage.open) return 0;
    if (vp_guest_storage_open(&svc->storage, VP_GUEST_STORAGE_INDEX_PATH,
                              VP_GUEST_STORAGE_MAP_PATH, VP_GUEST_STORAGE_TREE_PATH) != 0) {
        slog(svc, "SVC_STORAGE_OPEN result=failed index=%s map=%s tree=%s error=%s\n",
             VP_GUEST_STORAGE_INDEX_PATH, VP_GUEST_STORAGE_MAP_PATH,
             VP_GUEST_STORAGE_TREE_PATH, svc->storage.error);
        return -1;
    }
    slog(svc, "SVC_STORAGE_OPEN result=ok image_size=%llu data_offset=%llu files=%u tree_bytes=%llu index=%s map=%s tree=%s\n",
         (unsigned long long)svc->storage.image_size,
         (unsigned long long)svc->storage.data_offset,
         svc->storage.file_count,
         (unsigned long long)svc->storage.tree_bytes,
         VP_GUEST_STORAGE_INDEX_PATH, VP_GUEST_STORAGE_MAP_PATH,
         VP_GUEST_STORAGE_TREE_PATH);
    return 0;
}

static int connect_named_port(VPGuestSvc *svc, VPBridgeFrame *f) {    /* 32-bit veneer: r0 = out Handle*, r1 = name; result r0, handle r1. The
     * original continuation stores r1 through the saved out pointer. */
    uint32_t out = f->regs[0], name_address = f->regs[1];
    char name[9] = {0};
    uint32_t result = 0, handle = 0;
    if (!vp_guest_svc_writable(svc, out, 4) || !vp_guest_svc_readable(svc, name_address, 1))
        return -141;
    for (unsigned i = 0; i < 8; ++i) {
        if (!vp_guest_svc_readable(svc, name_address + i, 1)) return -141;
        name[i] = (char)*(const uint8_t *)(uintptr_t)(name_address + i);
        if (!name[i]) break;
    }
    if (strcmp(name, "sm:")) {
        /* The only named port this loader provides. Other names stay errors,
         * never unconditional success handles. */
        result = SVC_RESULT_INVALID_NAME;
    } else {
        handle = create_handle(svc, 1, name, 0);
        if (!handle) result = SVC_RESULT_OUT_OF_MEMORY;
    }
    f->regs[0] = result;
    f->regs[1] = handle;
    slog(svc, "SVC_CONNECT_NAMED name=\"%s\" handle=0x%08x result=0x%08x\n",
            name, handle, result);
    sflush(svc);
    return 0;
}

static int close_handle(VPGuestSvc *svc, VPBridgeFrame *f) {
    uint32_t handle = f->regs[0];
    VPGuestHandle *slot = find_handle(svc, handle);
    if (!slot) {
        f->regs[0] = SVC_RESULT_INVALID_HANDLE;
    } else {
        if (slot->kind == 2 && slot->session) {
            VPGuestSession *state = &svc->sessions[slot->session - 1];
            if (state->refs) --state->refs;
            if (!state->refs) {
                if (state->audio_sink_owned && svc->audio_sink_owner == (void *)state) {
                    vp_audio_sink_stop();
                    svc->audio_sink_owner = NULL;
                }
                if (svc->audio_update_session == slot->session) {
                    svc->audio_update_thread_handle = 0;
                    svc->audio_update_session = 0;
                }
                /* Audio events outlive this service session in the handle
                 * table. Clear their owner tag so a later ResetSignal cannot
                 * use a reused session's cadence state. */
                for (unsigned i = 0; i < VP_GUEST_HANDLE_LIMIT; ++i) {
                    if (svc->handles[i].audio_owner_session == slot->session)
                        svc->handles[i].audio_owner_session = 0;
                }
                if (state->audio_mixer) {
                    vp_audio_renderer_destroy((VPAudioRenderer *)state->audio_mixer);
                    state->audio_mixer = NULL;
                }
                release_all_save_objects(state);
                vp_media_video_forget_session(state);
                memset(state, 0, sizeof(*state));
            }
        }
        slot->kind = 0;
        slot->session = 0;
        f->regs[0] = 0;
    }
    if (!slot) {
        slog(svc, "SVC_CLOSE_HANDLE handle=0x%08x result=0x%08x\n",
             handle, f->regs[0]);
        sflush(svc);
    }
    return 0;
}

/* Reads the FspPath carried by HIPC buffer `index`. The generated client
 * declares a 0x301-byte buffer; the string must contain a NUL inside the
 * declared region, exactly like nn::fs's own ValidatePath. */
static uint32_t save_read_path(VPGuestSvc *svc, const VPGuestHipcLayout *layout,
                               uint32_t index, char *path, uint32_t capacity) {
    uint32_t size = 0, address = 0;
    hipc_read_buffer(layout, index, &size, &address);
    if (!size || size > 0x301u || !address || capacity < 0x302u)
        return VP_GUEST_SAVE_INVALID_PATH;
    if (!vp_guest_svc_readable(svc, address, size))
        return VP_GUEST_SAVE_INVALID_PATH;
    if (!memchr((const void *)(uintptr_t)address, 0, size))
        return VP_GUEST_SAVE_INVALID_PATH;
    memcpy(path, (const void *)(uintptr_t)address, size);
    path[size] = 0;
    return VP_GUEST_SAVE_OK;
}

/* The CMIF raw payload starts after the 16-byte CMIF header and is bounded by
 * the HIPC raw-data word count (header word 1, bits 0-9). Domain request
 * headers do not change that count. */
static int save_raw_available(uint32_t header1, uint32_t offset,
                              uint32_t width) {
    uint32_t words = header1 & 0x3ffu;
    if (words < 4u) return 0;
    uint32_t bytes = words * 4u - 0x10u;
    return offset <= bytes && width <= bytes - offset;
}

/* Executes one command on a save IFileSystem, IFile or IDirectory domain
 * object. Returns 0 when the command was answered (the reply is written) and
 * 1 for a command number this layer does not model. */
static int save_object_command(VPGuestSvc *svc, VPBridgeFrame *f,
                               VPGuestSession *state, uint32_t handle,
                               const char *service, uint32_t object_id,
                               unsigned interface, uint32_t command,
                               uint32_t buffer, uint32_t header0,
                               uint32_t header1, uint32_t request_meta) {
    (void)f;
    VPGuestSaveObject *object = state->save_objects[object_id];
    if (!object) return 1;
    VPGuestHipcLayout layout;
    hipc_layout(buffer, header0, header1, &layout);
    uint32_t raw = request_meta + 0x10u;
    char path[0x302];
    char path2[0x302];
    char detail[192];
    const char *call = NULL;
    uint32_t result = VP_GUEST_SAVE_OK;
    uint32_t out_object = 0;
    uint32_t out_words[48];
    uint32_t out_count = 0;
    int unsupported = 0;
    path[0] = 0;
    path2[0] = 0;
    (void)path2;
    memset(out_words, 0, sizeof(out_words));
    detail[0] = 0;

    save_lock(svc);
    if (interface == VP_GUEST_OBJECT_FS_SAVE) {
        switch (command) {
        case 0: { /* CreateFile(path, s32 option, s64 size) */
            call = "CreateFile";
            int32_t option = save_raw_available(header1, 0, 4)
                                 ? (int32_t)get32(raw) : 0;
            int64_t size = save_raw_available(header1, 8, 8)
                               ? (int64_t)get64(raw + 8) : -1;
            result = save_read_path(svc, &layout, 0, path, sizeof(path));
            if (!result && !save_raw_available(header1, 8, 8))
                result = VP_GUEST_SAVE_INVALID_ARGUMENT;
            if (!result)
                result = vp_guest_save_create_file(object, path, size);
            snprintf(detail, sizeof(detail), "option=0x%x size=%lld",
                     (unsigned)option, (long long)size);
            break;
        }
        case 1: /* DeleteFile(path) */
            call = "DeleteFile";
            result = save_read_path(svc, &layout, 0, path, sizeof(path));
            if (!result) result = vp_guest_save_delete_file(object, path);
            break;
        case 2: /* CreateDirectory(path) */
            call = "CreateDirectory";
            result = save_read_path(svc, &layout, 0, path, sizeof(path));
            if (!result) result = vp_guest_save_create_directory(object, path);
            break;
        case 3: /* DeleteDirectory(path) */
        case 4: /* DeleteDirectoryRecursively(path) */
            call = command == 3 ? "DeleteDirectory"
                                : "DeleteDirectoryRecursively";
            result = save_read_path(svc, &layout, 0, path, sizeof(path));
            if (!result)
                result = vp_guest_save_delete_directory(object, path,
                                                        command == 4);
            break;
        case 5: /* RenameFile(old, new) */
        case 6: /* RenameDirectory(old, new) */
            call = command == 5 ? "RenameFile" : "RenameDirectory";
            result = save_read_path(svc, &layout, 0, path, sizeof(path));
            if (!result)
                result = save_read_path(svc, &layout, 1, path2, sizeof(path2));
            if (!result)
                result = vp_guest_save_rename(object, path, path2,
                                              command == 6);
            break;
        case 7: { /* GetEntryType(path) -> Out<u32> */
            call = "GetEntryType";
            uint32_t type = 0;
            result = save_read_path(svc, &layout, 0, path, sizeof(path));
            if (!result) result = vp_guest_save_entry_type(object, path, &type);
            out_words[0] = type;
            out_count = 1;
            snprintf(detail, sizeof(detail), "type=%u", type);
            break;
        }
        case 8: { /* OpenFile(path, s32 mode) -> OutInterface<IFile> */
            call = "OpenFile";
            uint32_t mode = save_raw_available(header1, 0, 4)
                                ? get32(raw) : 0xffffffffu;
            VPGuestSaveObject opened;
            result = save_read_path(svc, &layout, 0, path, sizeof(path));
            if (!result) result = vp_guest_save_open_file(object, path, mode,
                                                          &opened);
            if (!result) {
                uint32_t new_id = 0;
                VPGuestSaveObject *file = create_save_object(
                    state, VP_GUEST_OBJECT_FS_SAVE_FILE, &new_id);
                if (file) {
                    *file = opened;
                    out_object = new_id;
                } else {
                    vp_guest_save_release(&opened);
                    result = VP_GUEST_SAVE_IO_ERROR;
                }
            }
            snprintf(detail, sizeof(detail), "mode=0x%x out_object=%u",
                     mode, out_object);
            break;
        }
        case 9: { /* OpenDirectory(path, s32 mode) -> OutInterface<IDirectory> */
            call = "OpenDirectory";
            uint32_t mode = save_raw_available(header1, 0, 4)
                                ? get32(raw) : 0xffffffffu;
            VPGuestSaveObject opened;
            result = save_read_path(svc, &layout, 0, path, sizeof(path));
            if (!result) result = vp_guest_save_list_directory(object, path,
                                                               mode, &opened);
            if (!result) {
                uint32_t new_id = 0;
                VPGuestSaveObject *directory = create_save_object(
                    state, VP_GUEST_OBJECT_FS_SAVE_DIR, &new_id);
                if (directory) {
                    *directory = opened;
                    out_object = new_id;
                } else {
                    vp_guest_save_release(&opened);
                    result = VP_GUEST_SAVE_IO_ERROR;
                }
            }
            snprintf(detail, sizeof(detail), "mode=0x%x out_object=%u",
                     mode, out_object);
            break;
        }
        case 10: /* Commit() */
            call = "Commit";
            /* Writes are completed by the close inside each write request;
             * there is no write-back cache to flush. */
            result = VP_GUEST_SAVE_OK;
            snprintf(detail, sizeof(detail), "model=direct-write-no-writeback-cache");
            break;
        case 11:   /* GetFreeSpaceSize(path) -> Out<s64> */
        case 12: { /* GetTotalSpaceSize(path) -> Out<s64> */
            call = command == 11 ? "GetFreeSpaceSize" : "GetTotalSpaceSize";
            uint64_t free_bytes = 0, total_bytes = 0;
            result = save_read_path(svc, &layout, 0, path, sizeof(path));
            if (!result)
                result = vp_guest_save_free_space(object, &free_bytes,
                                                  &total_bytes);
            uint64_t value = command == 11 ? free_bytes : total_bytes;
            out_words[0] = (uint32_t)value;
            out_words[1] = (uint32_t)(value >> 32);
            out_count = 2;
            snprintf(detail, sizeof(detail), "value=%llu",
                     (unsigned long long)value);
            break;
        }
        case 13: /* CleanDirectoryRecursively(path) */
            call = "CleanDirectoryRecursively";
            result = save_read_path(svc, &layout, 0, path, sizeof(path));
            if (!result) result = vp_guest_save_clean_directory(object, path);
            break;
        case 14: { /* GetFileTimeStampRaw(path) -> Out<FileTimeStampRaw> */
            call = "GetFileTimeStampRaw";
            uint64_t stamp[3] = {0, 0, 0};
            result = save_read_path(svc, &layout, 0, path, sizeof(path));
            if (!result)
                result = vp_guest_save_file_timestamp(object, path, stamp);
            for (unsigned i = 0; i < 3; ++i) {
                out_words[i * 2u] = (uint32_t)stamp[i];
                out_words[i * 2u + 1u] = (uint32_t)(stamp[i] >> 32);
            }
            out_count = 8;
            break;
        }
        case 15: /* QueryEntry: the reference server does not register it. */
            call = "QueryEntry";
            result = VP_GUEST_SAVE_NOT_IMPLEMENTED;
            snprintf(detail, sizeof(detail),
                     "model=reference-server-leaves-command-unimplemented");
            break;
        case 16: { /* GetFileSystemAttribute() -> Out<FileSystemAttribute> */
            call = "GetFileSystemAttribute";
            /* Layout matches the reviewed FS attribute structure: defined
             * flags at 0/1, u32 maxima at 0x28/0x2c. The save store exposes
             * 0x40-byte entry names, as the reference reports. */
            uint8_t *bytes = (uint8_t *)out_words;
            bytes[0] = 1;
            bytes[1] = 1;
            uint32_t name_max = 0x40;
            memcpy(bytes + 0x28, &name_max, 4);
            memcpy(bytes + 0x2c, &name_max, 4);
            out_count = 48;
            snprintf(detail, sizeof(detail), "entry_name_max=0x40");
            break;
        }
        default:
            unsupported = 1;
            break;
        }
    } else if (interface == VP_GUEST_OBJECT_FS_SAVE_FILE) {
        switch (command) {
        case 0: { /* Read(option, Out<s64> size, offset, OutBuffer, s64 size) */
            call = "Read";
            uint32_t option = save_raw_available(header1, 0, 4)
                                  ? get32(raw) : 0;
            int64_t offset = save_raw_available(header1, 8, 8)
                                 ? (int64_t)get64(raw + 8) : -1;
            int64_t size = save_raw_available(header1, 16, 8)
                               ? (int64_t)get64(raw + 16) : -1;
            uint32_t buffer_size = 0, buffer_address = 0;
            hipc_write_buffer(&layout, 0, &buffer_size, &buffer_address);
            uint64_t returned = 0;
            if (size < 0 || (uint64_t)size > buffer_size) {
                result = VP_GUEST_SAVE_INVALID_SIZE;
            } else if (!save_raw_available(header1, 16, 8)) {
                result = VP_GUEST_SAVE_INVALID_ARGUMENT;
            } else if (size &&
                       (!buffer_address ||
                        !vp_guest_svc_writable(svc, buffer_address,
                                               (uint32_t)size))) {
                result = VP_GUEST_SAVE_INVALID_ARGUMENT;
            } else {
                result = vp_guest_save_read_file(
                    object, offset, (void *)(uintptr_t)buffer_address,
                    (uint32_t)size, &returned);
            }
            out_words[0] = (uint32_t)returned;
            out_words[1] = (uint32_t)(returned >> 32);
            out_count = 2;
            snprintf(detail, sizeof(detail),
                     "option=0x%x offset=%lld size=%lld capacity=%u returned=%llu",
                     option, (long long)offset, (long long)size, buffer_size,
                     (unsigned long long)returned);
            break;
        }
        case 1: { /* Write(InBuffer, option, offset, s64 size) */
            call = "Write";
            uint32_t option = save_raw_available(header1, 0, 4)
                                  ? get32(raw) : 0;
            int64_t offset = save_raw_available(header1, 8, 8)
                                 ? (int64_t)get64(raw + 8) : -1;
            int64_t size = save_raw_available(header1, 16, 8)
                               ? (int64_t)get64(raw + 16) : -1;
            uint32_t buffer_size = 0, buffer_address = 0;
            hipc_read_buffer(&layout, 0, &buffer_size, &buffer_address);
            if (size < 0 || (uint64_t)size > buffer_size) {
                result = VP_GUEST_SAVE_INVALID_SIZE;
            } else if (!save_raw_available(header1, 16, 8)) {
                result = VP_GUEST_SAVE_INVALID_ARGUMENT;
            } else if (size &&
                       (!buffer_address ||
                        !vp_guest_svc_readable(svc, buffer_address,
                                               (uint32_t)size))) {
                result = VP_GUEST_SAVE_INVALID_ARGUMENT;
            } else {
                result = vp_guest_save_write_file(
                    object, offset, (const void *)(uintptr_t)buffer_address,
                    (uint32_t)size);
            }
            snprintf(detail, sizeof(detail),
                     "option=0x%x offset=%lld size=%lld capacity=%u",
                     option, (long long)offset, (long long)size, buffer_size);
            break;
        }
        case 2: /* Flush() */
            call = "Flush";
            result = vp_guest_save_flush_file(object);
            break;
        case 3: { /* SetSize(s64 size) */
            call = "SetSize";
            int64_t size = save_raw_available(header1, 0, 8)
                               ? (int64_t)get64(raw) : -1;
            if (!save_raw_available(header1, 0, 8))
                result = VP_GUEST_SAVE_INVALID_ARGUMENT;
            else
                result = vp_guest_save_set_file_size(object, size);
            snprintf(detail, sizeof(detail), "size=%lld", (long long)size);
            break;
        }
        case 4: { /* GetSize() -> Out<s64> */
            call = "GetSize";
            uint64_t size = 0;
            result = vp_guest_save_file_size(object, &size);
            out_words[0] = (uint32_t)size;
            out_words[1] = (uint32_t)(size >> 32);
            out_count = 2;
            snprintf(detail, sizeof(detail), "size=%llu",
                     (unsigned long long)size);
            break;
        }
        default:
            unsupported = 1;
            break;
        }
    } else { /* VP_GUEST_OBJECT_FS_SAVE_DIR */
        switch (command) {
        case 0: { /* Read() -> Out<s64> count, OutArray<DirectoryEntry> */
            call = "Read";
            uint32_t buffer_size = 0, buffer_address = 0;
            uint64_t count = 0;
            hipc_write_buffer(&layout, 0, &buffer_size, &buffer_address);
            if (buffer_size && !buffer_address)
                result = VP_GUEST_SAVE_INVALID_ARGUMENT;
            else if (buffer_size && !vp_guest_svc_writable(
                         svc, buffer_address, buffer_size))
                result = VP_GUEST_SAVE_INVALID_ARGUMENT;
            else
                result = vp_guest_save_directory_read(
                    object, (void *)(uintptr_t)buffer_address, buffer_size,
                    &count);
            out_words[0] = (uint32_t)count;
            out_words[1] = (uint32_t)(count >> 32);
            out_count = 2;
            snprintf(detail, sizeof(detail),
                     "capacity=%u count=%llu cursor=%u/%u", buffer_size,
                     (unsigned long long)count, object->cursor,
                     object->entry_count);
            break;
        }
        case 1: { /* GetEntryCount() -> Out<s64> */
            call = "GetEntryCount";
            uint64_t count = 0;
            result = vp_guest_save_directory_count(object, &count);
            out_words[0] = (uint32_t)count;
            out_words[1] = (uint32_t)(count >> 32);
            out_count = 2;
            snprintf(detail, sizeof(detail), "count=%llu",
                     (unsigned long long)count);
            break;
        }
        default:
            unsupported = 1;
            break;
        }
    }
    save_unlock(svc);
    if (unsupported) return 1;
    if (out_object) {
        reply_domain_out_object(buffer, out_object);
    } else if (out_count) {
        reply_domain_out_words(buffer, out_words, out_count, result);
    } else {
        reply_domain_out_raw(buffer, 0, 0, 0, result);
    }
    slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=%s path=\"%s\"%s%s result=0x%08x\n",
            handle, service, object_id, interface, command, call,
            path[0] && interface != VP_GUEST_OBJECT_FS_SAVE_FILE
                && interface != VP_GUEST_OBJECT_FS_SAVE_DIR
                ? path : object->path,
            detail[0] ? " " : "", detail, result);
    return 0;
}

/* Raw-data (CMIF header) position for a type-4 HIPC message, following the
 * locked SDK's HipcMessageReader::Initialize (sdk+0x49e74): two base words,
 * then special-header/pid/copy/move words, then the A/B/W/X descriptor
 * classes; the CMIF header is 16-byte aligned from there. */
static uint32_t hipc_raw_data_start(uint32_t buffer, uint32_t size,
                                    uint32_t *layout_words) {
    if (size < 8u) return 0;
    uint32_t header0 = get32(buffer);
    uint32_t header1 = get32(buffer + 4);
    uint32_t words = 2u;
    if (header1 & 0x80000000u) {
        if (size < 0xcu) return 0;
        uint32_t special = get32(buffer + 8);
        words = 3u;
        if (special & 1u) words += 2u;          /* Pid (u64) */
        words += (special >> 1) & 0xfu;         /* copy-handle words */
        words += (special >> 5) & 0xfu;         /* move-handle words */
    }
    words += ((header0 >> 16) & 0xfu) * 2u;     /* A descriptors */
    words += ((header0 >> 20) & 0xfu) * 3u;     /* B descriptors */
    words += ((header0 >> 24) & 0xfu) * 3u;     /* W descriptors */
    words += ((header0 >> 28) & 0xfu) * 3u;     /* X descriptors */
    if (words > size / 4u) return 0;
    if (layout_words) *layout_words = words;
    return (buffer + words * 4u + 0xfu) & ~0xfu;
}

static int send_sync_request(VPGuestSvc *svc, VPBridgeFrame *f, int with_buffer) {
    /* svc 0x21 takes only the session handle; the kernel reads the calling
     * thread's IPC buffer. svc 0x22 receives (buffer, size, handle) directly.
     * Both reach the same reviewed HIPC/CMIF message in guest memory. */
    uint32_t handle = with_buffer ? f->regs[2] : f->regs[0];
    uint32_t buffer = with_buffer ? f->regs[0] : svc->tls_buffer;
    uint32_t size = with_buffer ? f->regs[1] : 0x100u;
    VPGuestHandle *session = find_handle(svc, handle);
    if (!session) {
        f->regs[0] = SVC_RESULT_INVALID_HANDLE;
        /* f->sp is the SendSyncRequest wrapper's frame, so [sp+4] is the
         * caller's return address. Recorded while tracing which client path
         * can send on a null session handle; the SDK aborts on its own IPC
         * assertion right after this call. */
        uint32_t caller = vp_guest_svc_readable(svc, f->sp + 4, 4) ? get32(f->sp + 4) : 0;
        slog(svc, "SVC_IPC_REJECT handle=0x%08x result=0x%08x caller=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x r8=0x%08x r9=0x%08x r10=0x%08x r11=0x%08x r12=0x%08x\n",
                handle, f->regs[0], caller, f->regs[4], f->regs[5], f->regs[6],
                f->regs[7], f->regs[8], f->regs[9], f->regs[10], f->regs[11], f->regs[12]);
        /* Temporary diagnostic, bound to this loader's fixed sdk placement
         * (MODULE index=3 base=0x810cf000): NvOsDrv keeps two INvDrvServices
         * proxy pointers at sdk+0x91fba4 / sdk+0x91fba8. Dump both objects so
         * the unconnected one and its session slot are identified. */
        {
            const uint32_t nvosdrv_globals = 0x819EEBA4u;
            if (vp_guest_svc_readable(svc, nvosdrv_globals, 8u)) {
                uint32_t g4 = get32(nvosdrv_globals), g8 = get32(nvosdrv_globals + 4);
                slog(svc, "SVC_IPC_REJECT_NVOSDRV g4=0x%08x g8=0x%08x\n", g4, g8);
                for (unsigned i = 0; i < 2; ++i) {
                    uint32_t obj = i ? g8 : g4;
                    if (obj && vp_guest_svc_readable(svc, obj, 0x20u))
                        slog(svc, "SVC_IPC_REJECT_NVOSDRV_OBJ which=%u at=0x%08x words=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                                i, obj, get32(obj), get32(obj + 4), get32(obj + 8),
                                get32(obj + 0xc), get32(obj + 0x10), get32(obj + 0x14),
                                get32(obj + 0x18), get32(obj + 0x1c));
                }
            }
        }
        for (unsigned row = 0; row < 32; ++row) {
            uint32_t at = f->sp + row * 32u;
            if (!vp_guest_svc_readable(svc, at, 32u)) break;
            slog(svc, "SVC_IPC_REJECT_STACK +0x%03x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    row * 32u, get32(at), get32(at + 4), get32(at + 8), get32(at + 0xc),
                    get32(at + 0x10), get32(at + 0x14), get32(at + 0x18), get32(at + 0x1c));
        }
        /* The SDK aborts on a null session handle before this call returns to
         * the game; walk the frame chain so the failing client owner and the
         * game-side caller are both on record. */
        uint32_t fp = vp_guest_svc_readable(svc, f->sp, 4) ? get32(f->sp) : 0;
        for (unsigned depth = 0; depth < 12 && fp; ++depth) {
            if (!vp_guest_svc_readable(svc, fp, 8)) break;
            slog(svc, "SVC_IPC_REJECT_FRAME depth=%u fp=0x%08x saved_lr=0x%08x\n",
                    depth, fp, get32(fp + 4));
            fp = get32(fp);
        }
        return 0;
    }
    if (!buffer || size < 0x20 || size > 0x1000
        || !vp_guest_svc_readable(svc, buffer, size) || !vp_guest_svc_writable(svc, buffer, size)) {
        slog(svc, "SVC_IPC_SHAPE_REJECT handle=0x%08x buffer=0x%08x size=0x%x readable=%d writable=%d\n",
                handle, buffer, size,
                buffer ? vp_guest_svc_readable(svc, buffer, size) : 0,
                buffer ? vp_guest_svc_writable(svc, buffer, size) : 0);
        sflush(svc);
        return -141;
    }
    ++svc->ipc_requests;
    VPGuestSession *state = handle_session(svc, session);
    uint32_t header0 = get32(buffer), header1 = get32(buffer + 4);
    uint32_t message_type = header0 & 0xffffu;
    uint32_t scan_meta = 0;
    for (uint32_t off = 0; off + 0x10 <= size; off += 4)
        if (get32(buffer + off) == 0x49434653u) { scan_meta = buffer + off; break; }
    /* The per-thread IPC buffer is reused, and the SDK only rewrites the
     * descriptor/special words in front of the new CMIF header, so a scan can
     * hit a stale SFCI from an earlier, smaller request. In probe66 the
     * GetWorkBufferSize header survived at word 4 of OpenAudioRenderer while
     * the real header had moved to word 8. Derive the header position from
     * the HIPC descriptor layout instead. Transport control messages keep the
     * scan: their "command" is not a CMIF command. */
    uint32_t request_meta = scan_meta;
    uint32_t layout_raw = 0;
    if (message_type == 4u) {
        uint32_t layout_words = 0;
        layout_raw = hipc_raw_data_start(buffer, size, &layout_words);
        if (layout_raw) {
            uint32_t cmif = layout_raw;
            /* Domain sessions prepend a 16-byte domain context (target object
             * id at +4) between the raw start and the CMIF header. */
            if (state && state->domain_state == 2) cmif += 0x10u;
            if (cmif >= buffer && cmif - buffer + 0x10u <= size
                && get32(cmif) == 0x49434653u)
                request_meta = cmif;
        }
    }
    if (scan_meta != request_meta
        && (!strcmp(session->name, "audren:u") || !strcmp(session->name, "aud-dev"))) {
        int audio_meta = state != NULL;
        if (audio_meta) ++state->audio_meta_count;
        if (!audio_meta || vp_audio_log_cadence_should_emit(
                &state->audio_meta_log, 4u, 64u))
            slog(svc, "SVC_IPC_AUDIO_META scan=0x%08x layout=0x%08x used=0x%08x hdr=0x%08x,0x%08x thread=0x%08x count=%u log=%u/%u\n",
                    scan_meta, layout_raw, request_meta, header0, header1,
                    svc->current_thread_handle,
                    state ? state->audio_meta_count : 0u,
                    state ? state->audio_meta_log.emitted : 0u,
                    state ? state->audio_meta_log.seen : 0u);
    }
    uint32_t version = request_meta ? get32(request_meta + 4) : 0;
    uint32_t command = request_meta ? get32(request_meta + 8) : 0;
    uint32_t token = request_meta ? get32(request_meta + 0xc) : 0;
    VPAudioIpcClass audio_class = vp_audio_ipc_classify(session->name,
                                                        message_type, command);
    int renderer_update = audio_class == VP_AUDIO_IPC_RENDERER_UPDATE;
    int audio_cmif_ready = request_meta && !(request_meta & 0xfu);
    /* RequestUpdate is the audio clock (~65-200 calls/s). Take the entry
     * timestamp only on the real renderer update path; no other service
     * pays for the clock read. */
    uint64_t audio_request_us = renderer_update && audio_cmif_ready
        ? sceKernelGetProcessTimeWide() : 0;
    if (state && audio_class != VP_AUDIO_IPC_NONE)
        ++state->audio_ipc_class_counts[audio_class];
    /* run10 shows the manager's GetAudioDeviceServiceWithRevisionInfo
     * (audren:u command 4) and the device's GetActiveAudioDeviceName
     * (aud-dev commands 3/10/13) also being issued once per audio update.
     * They keep their real handlers and counters; only their successful
     * per-request lines are bounded to the first calls plus a sparse
     * heartbeat. Other manager/device commands, malformed messages and all
     * transport/control failures keep their individual lines. */
    int audio_fast_known = state && vp_audio_ipc_known_fast(
        session->name, message_type, command, audio_cmif_ready);
    if (state && audio_cmif_ready) {
        if (audio_class >= VP_AUDIO_IPC_MANAGER_OPEN
            && audio_class <= VP_AUDIO_IPC_MANAGER_OTHER && command < 8u)
            ++state->audio_manager_cmd_counts[command];
        if (audio_class == VP_AUDIO_IPC_DEVICE && command < 16u)
            ++state->audio_device_cmd_counts[command];
    }
    /* sm:GetService("audren:u") is issued from the same renderer thread in the
     * real game. Detect the exact request shape here so both the generic
     * pre-request line and the handler success line can be bounded; any
     * malformed shape or another thread keeps the normal lines. */
    int sm_audio_getservice_name = 0;
    if (session->kind == 1 && !strcmp(session->name, "sm:")
        && audio_cmif_ready && command == 1u
        && vp_guest_svc_readable(svc, request_meta + 0x10u, 8u)) {
        static const char audren_name[8] = {'a','u','d','r','e','n',':','u'};
        sm_audio_getservice_name =
            memcmp((const void *)(uintptr_t)(request_meta + 0x10u),
                   audren_name, 8u) == 0;
    }
    int sm_audio_getservice_owned = sm_audio_getservice_name
        && vp_audio_ipc_thread_owned(svc->current_thread_handle,
                                     svc->audio_update_thread_handle);
    /* Keep the per-request line for the audren:u manager and aud-dev device
     * sessions except the two known per-update calls above; their handlers
     * emit the bounded line (with per-command counters). The renderer's
     * RequestUpdate has its own bounded line in the command-4 handler.
     * audren:u command 4 is GetAudioDeviceServiceWithRevisionInfo, not an
     * update. Transport control/close messages carry no CMIF command and are
     * classified separately, so a stale command word cannot suppress them. */
    int log_request = vp_audio_ipc_is_session(session->name)
        && !renderer_update && !audio_fast_known;
    if (log_request)
        slog(svc, "SVC_IPC_AUDIO_REQUEST handle=0x%08x name=%s kind=%u type=%u size=%u hdr=0x%08x,0x%08x command=%u class=%s thread=0x%08x\n",
                handle, session->name, session->kind, message_type, size,
                header0, header1, command,
                vp_audio_ipc_class_name(audio_class),
                svc->current_thread_handle);
    /* Audio requests are flushed so the last received message survives an
     * emulator abort; the file path is otherwise block-buffered. */
    if (log_request)
        sflush(svc);
    /* Diagnostic for the OpenAudioRenderer special-header layout: M5 needs
     * the exact word positions of the pid and the two copy handles because
     * the CMIF header's alignment (and therefore its distance from them)
     * depends on whether the TLS buffer is 0 or 8 mod 16. */
    if (!strcmp(session->name, "audren:u") && header1 == 0x8000001au) {
        for (uint32_t off = 0; off < 0x80u && off + 0x20u <= size; off += 0x20u)
            slog(svc, "SVC_IPC_AUDIO_OPEN_WORDS +0x%02x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    off, get32(buffer + off), get32(buffer + off + 4),
                    get32(buffer + off + 8), get32(buffer + off + 0xc),
                    get32(buffer + off + 0x10), get32(buffer + off + 0x14),
                    get32(buffer + off + 0x18), get32(buffer + off + 0x1c));
    }
    /* One summary line per CMIF request. HIPC control messages (type 5) and
     * session-close messages (type 2) are routine transport work; only their
     * failures are logged below. The renderer's RequestUpdate and the two
     * known per-update manager/device calls are high frequency and are
     * reported by their own bounded lines (first calls plus sparse
     * heartbeat); their malformed/unsupported paths still log individually.
     * Per-request byte/stack/frame dumps were used to reconstruct request
     * shapes during bring-up and are not part of the steady-state log. */
    if ((message_type == 4u || message_type == 6u)
        && !(renderer_update && audio_cmif_ready)
        && !audio_fast_known
        && !sm_audio_getservice_owned)
        slog(svc, "SVC_IPC handle=0x%08x kind=%u name=%s buffer=0x%08x size=%u hdr=0x%08x,0x%08x meta=0x%08x version=0x%08x command=%u token=0x%08x thread=0x%08x\n",
                handle, session->kind, session->name, buffer, size, header0, header1,
                request_meta, version, command, token, svc->current_thread_handle);
    if (message_type == 2) {
        /* HIPC control handshake (session close). The kernel completes this
         * without a CMIF reply; the following CloseHandle releases the slot. */
        f->regs[0] = 0;
        sflush(svc);
        return 0;
    }
    if (message_type == 5) {
        /* Transport control requests. A kernel reply rewrites the HIPC header
         * and places the CMIF reply where the SDK's HipcMessageReader derives
         * it: the reply header declares no descriptors, so raw data starts at
         * word 2 (buffer+8) and the reader aligns the CMIF header up from
         * there, align16(buffer + 8). This must not be simplified to
         * align16(buffer): the TLS command buffer is only 8-byte aligned and
         * different threads get either 0 or 8 mod 16. The audio worker thread
         * showed buffer=0x8108c1f0 (0 mod 16), where align16(buffer) put SFCO
         * on top of the reply header; HipcMessageReader::Initialize then read
         * the SFCO magic as descriptor counts, computed a 332-byte message and
         * tripped QueryPointerBufferSize's size assert (AbortImpl in
         * nn::audio::GetActiveAudioDeviceName's Process call, terminate result
         * 0x2a2). Raw data is the 4-word CMIF header plus one payload word. */
        uint32_t response_meta = (buffer + 8u + 0xfu) & ~0xfu;
        if (!state) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_CONTROL_NO_SESSION handle=0x%08x command=%u kind=%u\n",
                    handle, command, session->kind);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        if (command == 4) {
            /* CloneCurrentObjectEx: same server session on a second native
             * handle, with the reviewed one-move-handle HIPC reply. Used by
             * the SDK's IPC client before handing the session to another
             * thread; the handle shares the domain object table and service
             * state exactly like CloneCurrentObject. */
            uint32_t clone = clone_handle(svc, session);
            if (!clone) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_CLONE_LIMIT handle=0x%08x service=%s\n", handle, session->name);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            uint32_t handle_offset = (3u + 2u) * 4u;
            uint32_t clone_meta = (buffer + handle_offset + 4u + 0xfu) & ~0xfu;
            put32(buffer, 0);
            put32(buffer + 4, 0x8000000Au); /* raw words, one move handle */
            /* Header2 bit0 declares the Pid descriptor (words 2..4), bits 5-8
             * declare one copy handle (word 5). GetOutNativeHandles reads a
             * copy handle from the reader's +0x38 base offset, which is 5 only
             * with this word; header2 = 3 declares the handle under the move
             * class instead and the SDK stores offset 0 (a null handle). This
             * is the layout already proven by CloneCurrentObject. */
            put32(buffer + 8, 0x21u);
            put32(buffer + 0xc, 0);
            put32(buffer + 0x10, 0);
            put32(buffer + handle_offset, clone);
            put32(clone_meta, 0x4F434653u);
            put32(clone_meta + 4, 0);
            put32(clone_meta + 8, 0);
            put32(clone_meta + 0xc, 0);
            f->regs[0] = 0;
            int audio_control = vp_audio_ipc_is_session(session->name);
            if (audio_control) ++state->audio_control_count;
            if (!audio_control || vp_audio_log_cadence_should_emit(
                    &state->audio_control_log, 4u, 64u)) {
                slog(svc, "SVC_IPC_CLONE_EX handle=0x%08x service=%s clone=0x%08x session=%u refs=%u meta=0x%08x result=0 count=%u log=%u/%u\n",
                        handle, session->name, clone, session->session,
                        svc->sessions[session->session - 1].refs, clone_meta,
                        state->audio_control_count,
                        state->audio_control_log.emitted,
                        state->audio_control_log.seen);
                sflush(svc);
            }
            return 0;
        }
        if (command == 2 && state->domain_state == 2) {
            /* HIPC control command 2 CloneCurrentObject (verified at the
             * hipc client helper around sdk+0xa7b84): the SDK expects one out
             * native handle and the CMIF success header. The clone is a second
             * client handle on this same server session, so the two handles
             * share the domain object table and service state. The response
             * uses the reviewed one-move-handle HIPC header: handle word 5 and
             * the CMIF reply at align16(buffer + 6*4 + 0xf). */
            uint32_t clone = clone_handle(svc, session);
            if (!clone) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_CLONE_LIMIT handle=0x%08x service=%s\n", handle, session->name);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            uint32_t handle_offset = (3u + 2u) * 4u;
            uint32_t clone_meta = (buffer + handle_offset + 4u + 0xfu) & ~0xfu;
            put32(buffer, 0);
            put32(buffer + 4, 0x8000000Au); /* raw words, one move handle */
            put32(buffer + 8, 0x21u);       /* pid present, one out handle */
            put32(buffer + handle_offset, clone);
            put32(clone_meta, 0x4F434653u);
            put32(clone_meta + 4, 0);
            put32(clone_meta + 8, 0);
            put32(clone_meta + 0xc, 0);
            f->regs[0] = 0;
            int audio_control = vp_audio_ipc_is_session(session->name);
            if (audio_control) ++state->audio_control_count;
            if (!audio_control || vp_audio_log_cadence_should_emit(
                    &state->audio_control_log, 4u, 64u)) {
                slog(svc, "SVC_IPC_CLONE handle=0x%08x service=%s clone=0x%08x session=%u refs=%u meta=0x%08x result=0 count=%u log=%u/%u\n",
                        handle, session->name, clone, session->session,
                        svc->sessions[session->session - 1].refs, clone_meta,
                        state->audio_control_count,
                        state->audio_control_log.emitted,
                        state->audio_control_log.seen);
                sflush(svc);
            }
            return 0;
        }
        if (command == 3) {
            /* QueryPointerBufferSize. The 32-bit SDK sends InArray/OutArray
             * arguments through the HIPC pointer buffer, sized by this u16.
             * The locked SDK also relocates small MapAlias InBuffer/OutBuffer
             * arguments into that budget: it zeroes the A/B descriptor and
             * names the same guest address in the parallel X entry (read) or
             * the C entry after the raw data (write). hipc_read_buffer and
             * hipc_write_buffer resolve both forms directly in the client's
             * address space, so the declared size only gates which buffers
             * the client relocates. The reference service manager answers
             * 0x8000; nn::fs declares 0x301-byte path buffers (the adapter's
             * stack copy of a mount path), so a smaller answer makes the
             * client reject the call with its own 0x11A0B before any IPC. */
            put32(buffer, 0);      /* no handles and no descriptors */
            put32(buffer + 4, 5);  /* raw data: CMIF header + u16 size */
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            put32(response_meta + 0x10, VP_GUEST_POINTER_BUFFER_BYTES & 0xffffu);
            state->domain_state = 1;
            f->regs[0] = 0;
            int audio_control = vp_audio_ipc_is_session(session->name);
            if (audio_control) ++state->audio_control_count;
            if (!audio_control || vp_audio_log_cadence_should_emit(
                    &state->audio_control_log, 4u, 64u)) {
                slog(svc, "SVC_IPC_CONTROL_POINTER handle=0x%08x name=%s size=%u meta=0x%08x count=%u log=%u/%u\n",
                        handle, session->name, VP_GUEST_POINTER_BUFFER_BYTES & 0xffffu,
                        response_meta, state->audio_control_count,
                        state->audio_control_log.emitted,
                        state->audio_control_log.seen);
                sflush(svc);
            }
            return 0;
        }
        if (command == 0 && state->domain_state == 1) {
            /* ConvertSessionToDomain: a CMIF domain-manager request. The reply
             * carries the domain object id as the first payload word; the
             * native transport only needs an opaque nonzero id per session. */
            put32(buffer, 0);
            put32(buffer + 4, 5);  /* raw data: CMIF header + domain id */
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            put32(response_meta + 0x10, 1);
            state->domain_state = 2;
            state->next_object_id = 2;
            memset(state->objects, 0, sizeof(state->objects));
            state->objects[1] = root_interface(session->name);
            f->regs[0] = 0;
            int audio_control = vp_audio_ipc_is_session(session->name);
            if (audio_control) ++state->audio_control_count;
            if (!audio_control || vp_audio_log_cadence_should_emit(
                    &state->audio_control_log, 4u, 64u)) {
                slog(svc, "SVC_IPC_CONVERT_DOMAIN handle=0x%08x name=%s domain=1 result=0 meta=0x%08x count=%u log=%u/%u\n",
                        handle, session->name, response_meta,
                        state->audio_control_count,
                        state->audio_control_log.emitted,
                        state->audio_control_log.seen);
                sflush(svc);
            }
            return 0;
        }
        ++svc->ipc_unsupported;
        slog(svc, "SVC_IPC_CONTROL_UNSUPPORTED handle=0x%08x command=%u state=%u\n",
                handle, command, state->domain_state);
        sflush(svc);
        return VP_BRIDGE_UNSUPPORTED;
    }
    if (message_type == 4 && session->kind == 2 && state->domain_state == 2
        && !(header0 & 0xfff00000u) && !(header1 & 0x80000000u)) {
        /* CmifDomainClientMessage::ProxyBaseObject::~ProxyBaseObject sends a
         * domain close message instead of a CMIF request: the 16-byte domain
         * context at align16(buffer+8+0xf) carries word 2 and the target
         * object id at +4, with no CMIF header after it. Generated request
         * processors write word 1 (and the CMIF payload size in word 2) in
         * the same context, so the low byte identifies the two. The check must
         * run before the SFCI scan: the session buffer is reused and a stale
         * SFCI from an earlier, larger request can survive past the offset the
         * close overwrites, which previously sent a close down the request
         * path with object 0. Messages with descriptors keep the scan because
         * their context is placed after the descriptor region. */
        uint32_t close_meta = (buffer + 8u + 0xfu) & ~0xfu;
        if (get32(close_meta) == 2u) {
            uint32_t closed = get32(close_meta + 4);
            if (closed < VP_GUEST_DOMAIN_OBJECT_LIMIT) {
                /* A storage that PushInData handed to an applet stays alive as
                 * the applet's pending input even after the client closes its
                 * domain handle; an unconsumed one releases its host buffer. */
                if (state->objects[closed] == VP_GUEST_OBJECT_STORAGE
                    && state->storages[closed].data && !state->storages[closed].pushed) {
                    free(state->storages[closed].data);
                    state->storages[closed].data = NULL;
                    state->storages[closed].size = 0;
                }
                if (state->objects[closed] == VP_GUEST_OBJECT_STORAGE_ACCESSOR)
                    state->storage_parent[closed] = 0;
                if (state->save_objects[closed])
                    release_save_object(state, closed);
                state->objects[closed] = VP_GUEST_OBJECT_NONE;
            }
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            int audio_close = vp_audio_ipc_is_session(session->name);
            if (audio_close) ++state->audio_close_count;
            if (!audio_close || closed >= VP_GUEST_DOMAIN_OBJECT_LIMIT
                || vp_audio_log_cadence_should_emit(
                       &state->audio_close_log, 4u, 64u)) {
                slog(svc, "SVC_IPC_DOMAIN_CLOSE handle=0x%08x service=%s object=%u result=0 valid=%u count=%u log=%u/%u\n",
                        handle, session->name, closed,
                        closed < VP_GUEST_DOMAIN_OBJECT_LIMIT ? 1u : 0u,
                        state->audio_close_count,
                        state->audio_close_log.emitted,
                        state->audio_close_log.seen);
                sflush(svc);
            }
            return 0;
        }
    }
    if (message_type == 4 && session->kind == 2 && state->domain_state == 2
        && request_meta && request_meta >= buffer + 0x10) {
        /* Every reviewed type-4 CMIF call on a domain session carries the
         * target object id in the 16-byte domain request header. A received
         * header without room for it is a different transport shape and must
         * not be guessed. */
        uint32_t object_id = get32(request_meta - 0xc);
        unsigned interface = object_id < VP_GUEST_DOMAIN_OBJECT_LIMIT
            ? state->objects[object_id] : VP_GUEST_OBJECT_NONE;
        unsigned next_interface = VP_GUEST_OBJECT_NONE;
        const char *call = NULL;
        if (interface == VP_GUEST_OBJECT_APPLICATION_PROXY_SERVICE && command == 0) {
            next_interface = VP_GUEST_OBJECT_APPLICATION_PROXY;
            call = "OpenApplicationProxy";
        } else if (interface == VP_GUEST_OBJECT_APPLICATION_PROXY && command == 20) {
            /* nn::am::service::IApplicationProxy command 20 is
             * GetApplicationFunctions: no input arguments, one out object. */
            next_interface = VP_GUEST_OBJECT_APPLICATION_FUNCTIONS;
            call = "GetApplicationFunctions";
        } else if (interface == VP_GUEST_OBJECT_APPLICATION_PROXY && command == 11) {
            /* nn::am::service::IAppletProxyCommon command 11 is
             * GetLibraryAppletCreator: no input arguments, one out object. */
            next_interface = VP_GUEST_OBJECT_LIBRARY_APPLET_CREATOR;
            call = "GetLibraryAppletCreator";
        } else if (interface == VP_GUEST_OBJECT_APPLICATION_PROXY && command == 0) {
            /* nn::am::service::IAppletProxyCommon command 0 is
             * GetCommonStateGetter: no input arguments, one out object. */
            next_interface = VP_GUEST_OBJECT_COMMON_STATE_GETTER;
            call = "GetCommonStateGetter";
        } else if (interface == VP_GUEST_OBJECT_APPLICATION_PROXY
                   && (command == 1 || command == 2 || command == 3
                       || command == 4 || command == 0xa || command == 0x3e8)) {
            /* Remaining IAppletProxyCommon object getters, each verified at
             * its own generated wrapper: 1 GetSelfController, 2
             * GetWindowController, 3 GetAudioController, 4
             * GetDisplayController, 10 GetProcessWindingController, 1000
             * GetDebugFunctions. All take no input and return one object. */
            static const struct {
                uint32_t command;
                uint8_t interface;
                const char *name;
            } getters[] = {
                {1, VP_GUEST_OBJECT_SELF_CONTROLLER, "GetSelfController"},
                {2, VP_GUEST_OBJECT_WINDOW_CONTROLLER, "GetWindowController"},
                {3, VP_GUEST_OBJECT_AUDIO_CONTROLLER, "GetAudioController"},
                {4, VP_GUEST_OBJECT_DISPLAY_CONTROLLER, "GetDisplayController"},
                {0xa, VP_GUEST_OBJECT_PROCESS_WINDING_CONTROLLER, "GetProcessWindingController"},
                {0x3e8, VP_GUEST_OBJECT_DEBUG_FUNCTIONS, "GetDebugFunctions"},
            };
            for (unsigned i = 0; i < sizeof(getters) / sizeof(getters[0]); ++i) {
                if (getters[i].command != command) continue;
                next_interface = getters[i].interface;
                call = getters[i].name;
                break;
            }
        } else if (interface == VP_GUEST_OBJECT_WINDOW_CONTROLLER && command == 1) {
            /* nn::am::service::IWindowController command 1 is
             * GetAppletResourceUserId (verified at wrapper 0xd8608): one out
             * raw u64 and no input. The native platform is a single applet,
             * so its resource user id is a stable nonzero loader-owned value
             * that is also the process id this service layer reports. */
            uint64_t user_id = svc->process_id;
            reply_domain_out_u64(buffer, user_id);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetAppletResourceUserId value=%llu result=0 meta=0x%08x\n",
                    handle, session->name, object_id, interface, command,
                    (unsigned long long)user_id, domain_response_meta(buffer));
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_COMMON_STATE_GETTER && command == 0) {
            /* nn::am::service::ICommonStateGetter command 0 is GetEventHandle
             * (verified at wrapper 0xd6fa4): one out native handle, no input.
             * The handle is a loader-owned event object; this layer does not
             * synthesize applet state changes, so nothing signals it. */
            uint32_t event = create_handle(svc, 3, "applet", 0);
            if (!event) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_DOMAIN_HANDLE_LIMIT handle=0x%08x object=%u\n", handle, object_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            VPGuestHandle *event_slot = find_handle(svc, event);
            event_slot->signaled = 1; /* initial FocusStateChanged notification */
            state->applet_event = event;
            state->applet_queue[0] = APPLET_MESSAGE_FOCUS_STATE_CHANGED;
            state->applet_queue_head = 0;
            state->applet_messages = 1;
            reply_domain_out_handle(buffer, event);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetEventHandle event=0x%08x result=0 meta=0x%08x\n",
                    handle, session->name, object_id, interface, command, event,
                    (buffer + 0x37u) & ~0xfu);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_COMMON_STATE_GETTER && command == 1) {
            /* nn::am::service::ICommonStateGetter command 1 is ReceiveMessage
             * (verified at wrapper 0xd6fb0): one out raw AppletMessage. The
             * modeled queue holds the initial FocusStateChanged event; once
             * drained the reviewed empty-queue result is returned. */
            uint32_t result = 0, message = 0;
            if (state->applet_messages) {
                message = state->applet_queue[state->applet_queue_head];
                state->applet_queue_head = (state->applet_queue_head + 1u) % 8u;
                --state->applet_messages;
            } else result = APPLET_RESULT_NO_MESSAGES;
            reply_domain_out_raw(buffer, message, 0, 1, result);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=ReceiveMessage message=0x%x result=0x%08x\n",
                    handle, session->name, object_id, interface, command, message, result);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_COMMON_STATE_GETTER && command == 5) {
            /* nn::am::service::ICommonStateGetter command 5 is
             * GetOperationMode (verified at wrapper 0xd6fe0): one out raw u8.
             * The native platform is a handheld Vita, so the modeled operation
             * mode is Handheld (0), never a docked state this host lacks. */
            reply_domain_out_raw(buffer, 0, 0, 1, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetOperationMode value=0 result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_COMMON_STATE_GETTER && command == 6) {
            /* nn::am::service::ICommonStateGetter command 6 is
             * GetPerformanceMode (verified at wrapper 0xd6fec): one out raw
             * u32. The loader's host runs the application at Vita clocks; no
             * boost-mode budget is modeled, so the reported mode is Normal. */
            reply_domain_out_raw(buffer, 0, 0, 1, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetPerformanceMode value=0 result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_COMMON_STATE_GETTER && command == 9) {
            /* nn::am::service::ICommonStateGetter command 9 is
             * GetCurrentFocusState (verified at wrapper 0xd7010): one out
             * raw u8. The modeled application is in focus. */
            reply_domain_out_raw(buffer, APPLET_FOCUS_STATE_IN_FOCUS, 0, 1, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetCurrentFocusState value=%u result=0\n",
                    handle, session->name, object_id, interface, command, APPLET_FOCUS_STATE_IN_FOCUS);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_WINDOW_CONTROLLER
                   && (command == 0xa || command == 0xb || command == 0xc)) {
            /* nn::am::service::IWindowController foreground policy, verified
             * at the generated wrappers: 10 AcquireForegroundRights,
             * 11 ReleaseForegroundRights, 12 RejectToChangeIntoBackground.
             * This platform hosts exactly one native application, so the
             * foreground is always available; the calls are recorded as the
             * reviewed platform state transitions rather than stubbed. */
            const char *name = command == 0xa ? "AcquireForegroundRights"
                : command == 0xb ? "ReleaseForegroundRights"
                : "RejectToChangeIntoBackground";
            if (command == 0xa) state->foreground_rights = 1;
            else if (command == 0xb) state->foreground_rights = 0;
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=%s held=%u result=0\n",
                    handle, session->name, object_id, interface, command, name,
                    state->foreground_rights);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_SELF_CONTROLLER
                   && (command == 0 || command == 1 || command == 2)) {
            /* nn::am::service::ISelfController commands 0 Exit, 1 LockExit and
             * 2 UnlockExit. The reviewed reference terminates the process on
             * Exit, on LockExit while an exit is already requested, and on
             * UnlockExit while an exit is requested. The exit request itself is
             * raised by the platform Exit AppletMessage. The reply is still
             * written so the guest veneer returns normally; the main thread
             * ends the session at its next service call. */
            const char *exit_name = command == 0 ? "Exit"
                : command == 1 ? "LockExit" : "UnlockExit";
            if (command == 0) {
                state->exit_requested = 1;
                state->process_exit_requested = 1;
            } else if (command == 1) {
                if (state->exit_requested) state->process_exit_requested = 1;
                else ++state->exit_lock_depth;
            } else {
                if (state->exit_lock_depth) --state->exit_lock_depth;
                if (state->exit_requested) state->process_exit_requested = 1;
            }
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=%s depth=%u exit_requested=%u process_exit=%u result=0\n",
                    handle, session->name, object_id, interface, command, exit_name,
                    state->exit_lock_depth, state->exit_requested,
                    state->process_exit_requested);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_SELF_CONTROLLER && command == 11) {
            /* nn::am::service::ISelfController command 11 is
             * SetOperationModeChangedNotification (libnx
             * _appletSetOperationModeChangedNotification): one raw u8 and no
             * output. The host's operation mode never changes, so the
             * requested notification is recorded and cannot fire. */
            state->operation_mode_notification = (uint8_t)get32(request_meta + 0x10);
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=SetOperationModeChangedNotification enabled=%u result=0\n",
                    handle, session->name, object_id, interface, command,
                    state->operation_mode_notification);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_SELF_CONTROLLER && command == 12) {
            /* nn::am::service::ISelfController command 12 is
             * SetPerformanceModeChangedNotification (libnx
             * _appletSetPerformanceModeChangedNotification): one raw u8 and
             * no output. The host runs at fixed Vita clocks, so no
             * performance-mode transition exists to notify. */
            state->performance_mode_notification = (uint8_t)get32(request_meta + 0x10);
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=SetPerformanceModeChangedNotification enabled=%u result=0\n",
                    handle, session->name, object_id, interface, command,
                    state->performance_mode_notification);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_SELF_CONTROLLER && command == 14) {
            /* nn::am::service::ISelfController command 14 is
             * SetRestartMessageEnabled (libnx appletSetRestartMessageEnabled):
             * one raw u8 and no output. The native host has no restart-message
             * UI, so the flag is platform state with no observable effect. */
            state->restart_message_enabled = (uint8_t)get32(request_meta + 0x10);
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=SetRestartMessageEnabled enabled=%u result=0\n",
                    handle, session->name, object_id, interface, command,
                    state->restart_message_enabled);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_SELF_CONTROLLER && command == 40) {
            /* nn::am::service::ISelfController command 40 is
             * CreateManagedDisplayLayer (verified at wrapper 0xd7d34: no
             * input, one out raw u64). The returned LayerId is what the
             * application later passes to the display service's OpenLayer.
             * This host has no compositor, but the id must name a real layer
             * object so OpenLayer can accept exactly the layer the applet
             * side created; ids are allocated from a single counter. */
            uint64_t layer = ++svc->managed_layers;
            svc->managed_layer_id = layer;
            reply_domain_out_raw(buffer, (uint32_t)layer, (uint32_t)(layer >> 32), 2, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=CreateManagedDisplayLayer layer=0x%llx result=0\n",
                    handle, session->name, object_id, interface, command,
                    (unsigned long long)layer);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_SELF_CONTROLLER && command == 9) {
            /* nn::am::service::ISelfController command 9 is
             * GetLibraryAppletLaunchableEvent: one out native handle. The
             * reference keeps one event per applet service instance, signals it
             * on this query and returns the same event on every call. This
             * platform has no launcher process, but the notification is what
             * the SDK's StartLibraryApplet waits for before it starts an
             * applet, so the event is created once and signalled per query. */
            if (!state->lapplet_launch_event) {
                state->lapplet_launch_event = create_handle(svc, 3, "libappl", 0);
                if (!state->lapplet_launch_event) {
                    ++svc->ipc_unsupported;
                    slog(svc, "SVC_IPC_DOMAIN_HANDLE_LIMIT handle=0x%08x object=%u\n", handle, object_id);
                    sflush(svc);
                    return VP_BRIDGE_UNSUPPORTED;
                }
            }
            uint32_t launch_event = state->lapplet_launch_event;
            VPGuestHandle *launch_slot = find_handle(svc, launch_event);
            if (launch_slot) launch_slot->signaled = 1;
            reply_domain_out_handle(buffer, launch_event);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetLibraryAppletLaunchableEvent event=0x%08x model=no-library-applet result=0 meta=0x%08x\n",
                    handle, session->name, object_id, interface, command, launch_event,
                    (buffer + 0x37u) & ~0xfu);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_SELF_CONTROLLER && command == 10) {
            /* nn::am::service::ISelfController command 10 is
             * SetScreenShotPermission (libnx appletSetScreenShotPermission):
             * one raw u32 and no output. This host does not capture screen
             * shots, so the permission is recorded as platform state. */
            state->screenshot_permission = get32(request_meta + 0x10);
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=SetScreenShotPermission permission=%u result=0\n",
                    handle, session->name, object_id, interface, command,
                    state->screenshot_permission);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_SELF_CONTROLLER && command == 13) {
            /* nn::am::service::ISelfController command 13 is
             * SetFocusHandlingMode (verified at wrapper 0xd7cbc): three raw
             * u8 flags and no output. There is no competing applet and focus
             * never leaves this application, so the flags are recorded for
             * the platform trace and cannot change observable behavior. */
            state->focus_handling[0] = (uint8_t)get32(request_meta + 0x10);
            state->focus_handling[1] = (uint8_t)get32(request_meta + 0x11);
            state->focus_handling[2] = (uint8_t)get32(request_meta + 0x12);
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=SetFocusHandlingMode mode=%u,%u,%u result=0\n",
                    handle, session->name, object_id, interface, command,
                    state->focus_handling[0], state->focus_handling[1], state->focus_handling[2]);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_SELF_CONTROLLER && command == 16) {
            /* nn::am::service::ISelfController command 16 is
             * SetOutOfFocusSuspending (verified at wrapper 0xd7cf8): one raw
             * u8 and no output. Focus never leaves this application, so the
             * flag is recorded but has no platform effect. */
            state->out_of_focus_suspending = (uint8_t)get32(request_meta + 0x10);
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=SetOutOfFocusSuspending enabled=%u result=0\n",
                    handle, session->name, object_id, interface, command,
                    state->out_of_focus_suspending);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_PARENTAL_CONTROL_SERVICE_FACTORY
                   && (command == 0 || command == 1)) {
            /* nn::pctl IParentalControlServiceFactory commands 0 CreateService
             * and 1 CreateServiceWithoutInitialize (the latter verified at
             * wrapper 0x17aaf4): one raw u64 and one out object
             * (IParentalControlService). The native platform has no parental
             * control server; the service object returned here implements the
             * queries this title actually issues and reports the rest. */
            next_interface = VP_GUEST_OBJECT_PARENTAL_CONTROL_SERVICE;
            call = command == 1 ? "CreateServiceWithoutInitialize" : "CreateService";
        } else if (interface == VP_GUEST_OBJECT_PARENTAL_CONTROL_SERVICE && command == 1) {
            /* nn::pctl::detail::ipc::IParentalControlService command 1 is
             * Initialize (verified at wrapper 0x17aca4): one raw u64 and no
             * output. This platform has no parental-control backend, so the
             * service keeps its default (no restrictions) state; the raw
             * process id is recorded in the trace. */
            uint64_t process = (uint64_t)get32(request_meta + 0x10)
                | ((uint64_t)get32(request_meta + 0x14) << 32);
            state->parental_initialized = 1;
            state->parental_process = process;
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=Initialize process=0x%llx result=0\n",
                    handle, session->name, object_id, interface, command,
                    (unsigned long long)process);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_PARENTAL_CONTROL_SERVICE && command == 1001) {
            /* nn::pctl::detail::ipc::IParentalControlService command 1001 is
             * CheckFreeCommunicationPermission (Switchbrew). It returns only a
             * Result: success means free communication is permitted, failure
             * means a parental-control restriction blocks it. This platform
             * has no parental-control backend and therefore no PIN or
             * restriction settings, which is the no-restriction case the
             * reviewed emulator model treats as permitted. The service never
             * fabricates a restriction for a title the user launched
             * directly. */
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=CheckFreeCommunicationPermission initialized=%u model=no-parental-settings-permission-granted result=0\n",
                    handle, session->name, object_id, interface, command,
                    state->parental_initialized);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_FILE_SYSTEM_PROXY && command == 1) {
            /* nn::fssrv::sf::IFileSystemProxy command 1 is SetCurrentProcess
             * (verified at the generated processor with one in raw u64 and no
             * output, called from nn::fs::detail::GetFileSystemProxyServiceObject
             * with a zero process id). It binds the process identity the FS
             * server uses for permission checks to this session. */
            uint64_t process = (uint64_t)get32(request_meta + 0x10)
                | ((uint64_t)get32(request_meta + 0x14) << 32);
            state->fs_process = process;
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=SetCurrentProcess process=0x%llx result=0\n",
                    handle, session->name, object_id, interface, command,
                    (unsigned long long)process);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_FILE_SYSTEM_PROXY
                   && (command == 1004 || command == 1005)) {
            /* IFileSystemProxy commands 1004 SetGlobalAccessLogMode (one raw
             * u32, no output) and 1005 GetGlobalAccessLogMode (no input, one
             * out u32), verified against the reviewed Horizon FS service
             * table. This loader has no FS access log backend, so the mode it
             * reports is exactly the value the guest set. */
            if (command == 1004) {
                state->fs_access_log_mode = get32(request_meta + 0x10);
                reply_domain_out_raw(buffer, 0, 0, 0, 0);
            } else {
                reply_domain_out_raw(buffer, state->fs_access_log_mode, 0, 1, 0);
            }
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=%s mode=%u result=0\n",
                    handle, session->name, object_id, interface, command,
                    command == 1004 ? "SetGlobalAccessLogMode" : "GetGlobalAccessLogMode",
                    state->fs_access_log_mode);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_FILE_SYSTEM_PROXY && command == 200) {
            /* IFileSystemProxy command 200 is OpenDataStorageByCurrentProcess
             * (reviewed Horizon FS service table): no input, one out IStorage
             * object for the current process's data content. */
            next_interface = VP_GUEST_OBJECT_FS_STORAGE;
            call = "OpenDataStorageByCurrentProcess";
        } else if (interface == VP_GUEST_OBJECT_FILE_SYSTEM_PROXY && command == 202) {
            /* IFileSystemProxy command 202 is OpenDataStorageByDataId (libnx
             * fsOpenDataStorageByDataId). The guest's CMIF payload carries the
             * storage id at +0x10 and the data id u64 at +0x18. BuiltInSystem
             * data titles are firmware content this loader cannot include, so
             * they are modeled as an explicitly empty storage; every other
             * data-id storage is reported missing instead of fabricated. */
            uint32_t storage_id = get32(request_meta + 0x10) & 0xffu;
            uint64_t data_id = (uint64_t)get32(request_meta + 0x18)
                | ((uint64_t)get32(request_meta + 0x1c) << 32);
            if (storage_id == 3 && data_id == 0x0100000000000823ull && empty_romfs()) {
                next_interface = VP_GUEST_OBJECT_FS_STORAGE_EMPTY;
                call = "OpenDataStorageByDataId";
                slog(svc, "SVC_FS_DATA_STORAGE handle=0x%08x data_id=0x%llx storage=%u model=empty-system-data reason=loader-has-no-firmware-data\n",
                        handle, (unsigned long long)data_id, storage_id);
            } else {
                reply_domain_out_raw(buffer, 0, 0, 0, FS_RESULT_STORAGE_FAILED);
                f->regs[0] = 0;
                slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=OpenDataStorageByDataId storage=%u data_id=0x%llx result=0x%08x available=0\n",
                        handle, session->name, object_id, interface, command,
                        storage_id, (unsigned long long)data_id, FS_RESULT_STORAGE_FAILED);
                sflush(svc);
                return 0;
            }
        } else if ((interface == VP_GUEST_OBJECT_FS_STORAGE
                    || interface == VP_GUEST_OBJECT_FS_STORAGE_EMPTY) && command == 0) {
            /* nn::fs::IStorage command 0 is Read (generated processor at
             * sdk+0x5fea0: InRawArgumentInfo<8,8,0>, one buffer argument, then
             * InRawArgumentInfo<8,8,8>). The payload carries the u64 storage
             * offset and the u64 byte count; HIPC word 2 is the guest buffer
             * size and word 3 its address. The server fills the buffer and
             * returns only the CMIF result. */
            uint64_t offset = (uint64_t)get32(request_meta + 0x10)
                | ((uint64_t)get32(request_meta + 0x14) << 32);
            uint64_t size = (uint64_t)get32(request_meta + 0x18)
                | ((uint64_t)get32(request_meta + 0x1c) << 32);
            uint32_t descriptor_bytes = get32(buffer + 8);
            uint32_t descriptor_address = get32(buffer + 0xc);
            uint32_t result = 0;
            const char *reason = NULL;
            const uint8_t *empty_image = interface == VP_GUEST_OBJECT_FS_STORAGE_EMPTY
                ? empty_romfs() : NULL;
            if (size > 0xFFFFFFFFu || size > descriptor_bytes) {
                result = FS_RESULT_STORAGE_FAILED;
                reason = "guest buffer is smaller than the requested read";
            } else if (size && !vp_guest_svc_writable(svc, descriptor_address, (uint32_t)size)) {
                result = FS_RESULT_STORAGE_FAILED;
                reason = "guest buffer is not writable";
            } else if (empty_image) {
                if (offset > offline_system_bytes || size > offline_system_bytes - offset) {
                    result = FS_RESULT_STORAGE_FAILED;
                    reason = "empty system-data read beyond the zero-entry image";
                } else {
                    memcpy((void *)(uintptr_t)descriptor_address, empty_image + offset,
                           (uint32_t)size);
                }
            } else if (ensure_storage(svc) != 0) {
                result = FS_RESULT_STORAGE_FAILED;
                reason = svc->storage.error;
            } else {
                vp_guest_storage_set_owner(&svc->storage,
                                           svc->current_thread_handle);
                int read_status = vp_guest_storage_read(&svc->storage, offset,
                                                        descriptor_address, (uint32_t)size);
                if (read_status != VP_GUEST_STORAGE_OK) {
                    result = FS_RESULT_STORAGE_FAILED;
                    reason = read_status == VP_GUEST_STORAGE_BUSY
                        ? "storage read is already active"
                        : svc->storage.error;
                }
            }
            reply_domain_out_raw(buffer, 0, 0, 0, result);
            f->regs[0] = 0;
            const int mp4_path = strstr(svc->storage.last_path, ".mp4") != NULL;
            if (result == 0 && mp4_path)
                vp_native_movie_note_path(svc->storage.last_path);
            if (result || svc->storage.reads < STORAGE_DETAIL_LIMIT
                || (mp4_path && storage_mp4_detail_logs < 8u)) {
                slog(svc, "SVC_STORAGE_READ handle=0x%08x object=%u offset=%llu size=%llu result=0x%08x path=%s error=%s\n",
                        handle, object_id, (unsigned long long)offset,
                        (unsigned long long)size, result, svc->storage.last_path,
                        reason ? reason : "");
                if (mp4_path && storage_mp4_detail_logs < 8u)
                    ++storage_mp4_detail_logs;
            } else if (svc->storage.reads % STORAGE_SUMMARY_INTERVAL == 0) {
                slog(svc, "SVC_STORAGE_SUMMARY reads=%llu bytes=%llu opens=%llu errors=%llu generations=%llu tree_opens=%llu tree_closes=%llu tree_retires=%llu busy=%llu result=0\n",
                        (unsigned long long)svc->storage.reads,
                        (unsigned long long)svc->storage.bytes,
                        (unsigned long long)svc->storage.file_opens,
                        (unsigned long long)svc->storage.errors,
                        (unsigned long long)svc->storage.fd_generation,
                        (unsigned long long)svc->storage.tree_opens,
                        (unsigned long long)svc->storage.tree_closes,
                        (unsigned long long)svc->storage.tree_retires,
                        (unsigned long long)svc->storage.concurrent_reads);
            }
            /* The diagnostic exists only for a tree-file open/stat/size/read
             * failure and is cleared by every successful read. Log it only
             * beside a failed SVC_STORAGE_READ so successes stay quiet. */
            if (result && svc->storage.diagnostic.operation
                    != VP_GUEST_STORAGE_DIAG_NONE) {
                const VPGuestStorageDiagnostic *diag = &svc->storage.diagnostic;
                slog(svc, "SVC_STORAGE_DIAGNOSTIC op=%s fd=%d open_rc=%d stat_rc=%d open_attempted=%u stat_attempted=%u observed_valid=%u read_attempted=%u expected=%llu observed=%llu io_rc=%d offset=%llu length=%u owner=0x%08x generation=%llu open_seq=%llu close_seq=%llu retire_seq=%llu closes_since_open=%llu retires_since_open=%llu close_attempted=%u close_rc=%d retire_attempted=%u probe_attempted=%u probe_valid=%u probe_ebadfd=%u probe_rc=%d probe_size=%llu concurrent=%u path=%s\n",
                        vp_guest_storage_diag_operation_name(diag->operation),
                        diag->fd, diag->open_rc, diag->stat_rc,
                        diag->open_attempted, diag->stat_attempted,
                        diag->observed_valid, diag->read_attempted,
                        (unsigned long long)diag->expected_size,
                        (unsigned long long)diag->observed_size,
                        diag->io_rc, (unsigned long long)diag->file_offset,
                        diag->read_length, diag->owner,
                        (unsigned long long)diag->cache_generation,
                        (unsigned long long)diag->open_seq,
                        (unsigned long long)diag->close_seq,
                        (unsigned long long)diag->retire_seq,
                        (unsigned long long)diag->closes_since_open,
                        (unsigned long long)diag->retires_since_open,
                        diag->close_attempted, diag->close_rc,
                        diag->retire_attempted,
                        diag->fd_probe_attempted, diag->fd_probe_valid,
                        diag->fd_probe_ebadfd,
                        diag->fd_probe_rc,
                        (unsigned long long)diag->fd_probe_size,
                        diag->concurrent, svc->storage.last_path);
            }
            sflush(svc);
            return 0;
        } else if ((interface == VP_GUEST_OBJECT_FS_STORAGE
                    || interface == VP_GUEST_OBJECT_FS_STORAGE_EMPTY) && command == 4) {
            /* nn::fs::IStorage command 4 is GetSize: no input, one out raw
             * u64. The value is the size of the virtual RomFS image, which
             * the RomFS mount checks before reading metadata and file data.
             * The empty system-data storage reports a real zero size. */
            uint32_t result = 0;
            uint64_t image_size = 0;
            if (interface == VP_GUEST_OBJECT_FS_STORAGE_EMPTY) {
                (void)empty_romfs();
                image_size = offline_system_bytes;
            } else if (ensure_storage(svc) != 0) {
                result = FS_RESULT_STORAGE_FAILED;
            } else image_size = vp_guest_storage_size(&svc->storage);
            reply_domain_out_raw(buffer, (uint32_t)image_size,
                                 (uint32_t)(image_size >> 32), 2, result);
            f->regs[0] = 0;
            slog(svc, "SVC_STORAGE_SIZE handle=0x%08x object=%u size=%llu result=0x%08x error=%s\n",
                    handle, object_id, (unsigned long long)image_size, result,
                    result ? svc->storage.error : "");
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_FILE_SYSTEM_PROXY && command == 51) {
            /* IFileSystemProxy command 51 OpenSaveDataFileSystem: one u8
             * space id, padding, then a 0x40-byte SaveDataAttribute (program
             * id u64, user id, system id u64, type, rank, index). The loader
             * owns one user save store per attribute below
             * ux0:data/mhgu-vita-port/savedata. The returned IFileSystem runs
             * the original title's file operations against that directory;
             * a mount that cannot be created is reported as an nn::fs error
             * and no object is returned. */
            uint32_t space = get32(request_meta + 0x10) & 0xffu;
            uint32_t attr = request_meta + 0x18;
            uint64_t program = (uint64_t)get32(attr)
                | ((uint64_t)get32(attr + 4) << 32);
            uint64_t system = (uint64_t)get32(attr + 0x18)
                | ((uint64_t)get32(attr + 0x1c) << 32);
            uint32_t type = get32(attr + 0x20) & 0xffu;
            uint32_t rank = (get32(attr + 0x20) >> 8) & 0xffu;
            uint32_t index = (get32(attr + 0x20) >> 16) & 0xffffu;
            uint8_t uid[16];
            for (unsigned i = 0; i < 4; ++i) {
                uint32_t word = get32(attr + 0x08u + i * 4u);
                uid[i * 4u + 0u] = (uint8_t)word;
                uid[i * 4u + 1u] = (uint8_t)(word >> 8);
                uid[i * 4u + 2u] = (uint8_t)(word >> 16);
                uid[i * 4u + 3u] = (uint8_t)(word >> 24);
            }
            VPGuestSaveObject mounted;
            save_lock(svc);
            uint32_t mount_result = vp_guest_save_mount(&mounted, program, uid,
                                                        type, rank, index);
            save_unlock(svc);
            slog(svc, "SVC_IPC_FS_SAVE_REQ space=%u program=0x%llx uid=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x system=0x%llx type=%u rank=%u index=%u root=%s result=0x%08x\n",
                    space, (unsigned long long)program,
                    uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6], uid[7],
                    uid[8], uid[9], uid[10], uid[11], uid[12], uid[13], uid[14], uid[15],
                    (unsigned long long)system, type, rank, index,
                    mount_result ? "" : mounted.path, mount_result);
            if (mount_result == 0) {
                uint32_t new_id = 0;
                VPGuestSaveObject *object = create_save_object(
                    state, VP_GUEST_OBJECT_FS_SAVE, &new_id);
                if (object) {
                    *object = mounted;
                    reply_domain_out_object(buffer, new_id);
                    f->regs[0] = 0;
                    slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=OpenSaveDataFileSystem out_object=%u out_interface=%u result=0\n",
                            handle, session->name, object_id, interface, command,
                            new_id, VP_GUEST_OBJECT_FS_SAVE);
                    sflush(svc);
                    return 0;
                }
                vp_guest_save_release(&mounted);
                mount_result = VP_GUEST_SAVE_IO_ERROR;
                slog(svc, "SVC_IPC_SAVE_OBJECT_LIMIT handle=0x%08x object=%u\n",
                        handle, object_id);
            }
            reply_domain_out_raw(buffer, 0, 0, 0, mount_result);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=OpenSaveDataFileSystem result=0x%08x model=save-backend-error\n",
                    handle, session->name, object_id, interface, command, mount_result);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_FS_SAVE
                   || interface == VP_GUEST_OBJECT_FS_SAVE_FILE
                   || interface == VP_GUEST_OBJECT_FS_SAVE_DIR) {
            if (save_object_command(svc, f, state, handle, session->name,
                                    object_id, interface, command, buffer,
                                    header0, header1, request_meta) == 0) {
                f->regs[0] = 0;
                sflush(svc);
                return 0;
            }
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_DOMAIN_UNSUPPORTED handle=0x%08x service=%s object=%u interface=%u command=%u model=save-object-unmodeled\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        } else if (interface == VP_GUEST_OBJECT_APPLICATION_FUNCTIONS && command == 22) {
            /* nn::am::service::IApplicationFunctions command 22 is
             * SetTerminateResult (verified at wrapper 0xdeb88, command 0x16):
             * one raw u32 and no output. The native process model keeps the
             * value so an exit report can carry exactly what the guest set;
             * it does not fabricate process teardown here. */
            uint32_t terminate = get32(request_meta + 0x10);
            state->terminate_result = terminate;
            /* Temporary: tie the terminate value to the guest code that set
             * it. The SDK proxy runs inside this SVC; the deciding caller is
             * still on the thread's stack. */
            slog(svc, "SVC_IPC_TERMINATE_CONTEXT sp=0x%08x lr=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x r8=0x%08x r9=0x%08x\n",
                    f->sp, f->lr, f->regs[4], f->regs[5], f->regs[6],
                    f->regs[7], f->regs[8], f->regs[9]);
            for (unsigned row = 0; row < 24; ++row) {
                uint32_t at = f->sp + row * 32u;
                if (!vp_guest_svc_readable(svc, at, 32u)) break;
                slog(svc, "SVC_IPC_TERMINATE_STACK +0x%03x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                        row * 32u, get32(at), get32(at + 4), get32(at + 8),
                        get32(at + 0xc), get32(at + 0x10), get32(at + 0x14),
                        get32(at + 0x18), get32(at + 0x1c));
            }
            {
                uint32_t tfp = vp_guest_svc_readable(svc, f->sp, 4) ? get32(f->sp) : 0;
                for (unsigned depth = 0; depth < 12 && tfp; ++depth) {
                    if (!vp_guest_svc_readable(svc, tfp, 8)) break;
                    slog(svc, "SVC_IPC_TERMINATE_FRAME depth=%u fp=0x%08x saved_lr=0x%08x\n",
                            depth, tfp, get32(tfp + 4));
                    tfp = get32(tfp);
                }
            }
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=SetTerminateResult value=0x%08x result=0 hdr=0x%08x,0x%08x meta=0x%08x\n",
                    handle, session->name, object_id, interface, command, terminate,
                    get32(buffer), get32(buffer + 4), domain_response_meta(buffer));
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_APPLICATION_FUNCTIONS && command == 20) {
            /* nn::am::service::IApplicationFunctions command 20 is
             * EnsureSaveData(const Uid*): one 16-byte Uid input and one out
             * raw u64 size. The reference creates the user's account save and
             * reports zero bytes. This loader owns one user but has no save
             * filesystem backend yet, so it can only report the fresh-save
             * state: zero bytes for the loader user. It does not claim that
             * files are mountable or persistent; the following
             * OpenSaveDataFileSystem (fsp-srv command 51) is left explicitly
             * unsupported and stops that worker. */
            uint8_t uid[16];
            for (unsigned i = 0; i < 4; ++i) {
                uint32_t word = get32(request_meta + 0x10u + i * 4u);
                uid[i * 4u + 0u] = (uint8_t)word;
                uid[i * 4u + 1u] = (uint8_t)(word >> 8);
                uid[i * 4u + 2u] = (uint8_t)(word >> 16);
                uid[i * 4u + 3u] = (uint8_t)(word >> 24);
            }
            uint32_t result = memcmp(uid, loader_user_id, sizeof(uid)) == 0
                                  ? 0u : 0xFFFFFFFFu;
            reply_domain_out_raw(buffer, 0, 0, 2, result);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=EnsureSaveData uid=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x size=0 model=fresh-save-view-no-filesystem-backend result=0x%08x\n",
                    handle, session->name, object_id, interface, command,
                    uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6], uid[7],
                    uid[8], uid[9], uid[10], uid[11], uid[12], uid[13], uid[14], uid[15],
                    result);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_APPLICATION_FUNCTIONS && command == 21) {
            /* nn::am::service::IApplicationFunctions command 21 is
             * GetDesiredLanguage (verified at _nn_sf_sync_GetDesiredLanguage,
             * command 0x15, one Out<nn::settings::LanguageCode>). LanguageCode
             * is an eight-byte NUL-terminated code (LanguageCode::Make zeroes
             * bytes 0..7). This platform has no system-settings database, so it
             * models the US title's default console language, en-US. */
            reply_domain_out_raw(buffer, 0x552d6e65u /* "en-U" */, 0x00000053u /* "S\0\0\0" */, 2, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetDesiredLanguage value=en-US result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_TIME_SERVICE && command == 0) {
            /* nn::timesrv::detail::service::IStaticService command 0 is
             * GetStandardUserSystemClock (verified at the generated sync
             * wrapper _nn_sf_sync_GetStandardUserSystemClock): no input and one
             * out object of interface ISystemClock. */
            next_interface = VP_GUEST_OBJECT_TIME_SYSTEM_CLOCK;
            call = "GetStandardUserSystemClock";
        } else if (interface == VP_GUEST_OBJECT_TIME_SERVICE
                   && (command == 1 || command == 2 || command == 3 || command == 4)) {
            /* Remaining IStaticService clock getters verified at their own
             * generated sync wrappers: 1 GetStandardNetworkSystemClock and
             * 4 GetStandardLocalSystemClock return ISystemClock, 2
             * GetStandardSteadyClock returns ISteadyClock, 3
             * GetTimeZoneService returns ITimeZoneService. All take no input
             * and return one out object. */
            static const struct {
                uint32_t command;
                uint8_t interface;
                const char *name;
            } getters[] = {
                {1, VP_GUEST_OBJECT_TIME_SYSTEM_CLOCK, "GetStandardNetworkSystemClock"},
                {2, VP_GUEST_OBJECT_TIME_STEADY_CLOCK, "GetStandardSteadyClock"},
                {3, VP_GUEST_OBJECT_TIME_ZONE_SERVICE, "GetTimeZoneService"},
                {4, VP_GUEST_OBJECT_TIME_SYSTEM_CLOCK, "GetStandardLocalSystemClock"},
            };
            for (unsigned i = 0; i < sizeof(getters) / sizeof(getters[0]); ++i) {
                if (getters[i].command != command) continue;
                next_interface = getters[i].interface;
                call = getters[i].name;
                break;
            }
        } else if (interface == VP_GUEST_OBJECT_TIME_ZONE_SERVICE && command == 4) {
            /* ITimeZoneService command 4 is LoadTimeZoneRule: one 0x24-byte
             * LocationName in raw data and a 0x4000-byte out buffer descriptor
             * that receives the rule. The generated client treats the rule as
             * opaque and passes it back to ToCalendarTime, so this platform
             * returns its own zeroed UTC rule blob and performs the conversion
             * server-side. */
            VPGuestHipcLayout layout;
            uint32_t rule_bytes = 0, rule_address = 0;
            hipc_layout(buffer, header0, header1, &layout);
            hipc_write_buffer(&layout, 0, &rule_bytes, &rule_address);
            uint32_t result = 0;
            const char *location = (const char *)(uintptr_t)(request_meta + 0x10);
            if (memcmp(location, "Etc/UTC", 7) || location[7] != '\0') {
                result = (989u << 9) | 116u; /* TimeZoneNotFound */
            } else if (rule_bytes < 0x4000u
                || !vp_guest_svc_writable(svc, rule_address, 0x4000u)) {
                result = VP_TIME_INVALID_ARGUMENT;
            } else {
                memset((void *)(uintptr_t)rule_address, 0, 0x4000u);
            }
            reply_domain_out_raw(buffer, 0, 0, 0, result);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=LoadTimeZoneRule location=%.36s out=0x%08x/%u result=0x%08x model=utc-zero-rule\n",
                    handle, session->name, object_id, interface, command,
                    (const char *)(uintptr_t)(request_meta + 0x10),
                    rule_address, rule_bytes, result);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_TIME_ZONE_SERVICE &&
                   (command == 100 || command == 101)) {
            /* The locked SDK processor at sdk+0x19c4e4 declares out raw
             * <8,align2,offset0>, <24,align4,offset8>, in s64 and a 0x4000
             * map-alias rule. Only the UTC rule issued by LoadTimeZoneRule
             * is supported; never silently interpret an arbitrary rule as UTC. */
            int64_t seconds = (int64_t)((uint64_t)get32(request_meta + 0x10)
                | ((uint64_t)get32(request_meta + 0x14) << 32));
            uint32_t words[8] = {0}, result = 0;
            if (command == 100) {
                VPGuestHipcLayout layout;
                uint32_t rule_size = 0, rule_address = 0;
                hipc_layout(buffer, header0, header1, &layout);
                hipc_read_buffer(&layout, 0, &rule_size, &rule_address);
                if (rule_size != 0x4000u ||
                    !vp_guest_svc_readable(svc, rule_address, rule_size)) {
                    result = VP_TIME_INVALID_ARGUMENT;
                } else {
                    const uint8_t *rule = (const uint8_t *)(uintptr_t)rule_address;
                    for (uint32_t i = 0; i < rule_size; ++i) {
                        if (rule[i]) { result = VP_TIME_INVALID_ARGUMENT; break; }
                    }
                }
            }
            if (!result) result = vp_time_calendar_utc(seconds, words);
            reply_domain_out_words(buffer, words, 8, result);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_TIME_CALENDAR command=%u seconds=%lld result=0x%08x model=UTC year=%u month=%u day=%u\n",
                 command, (long long)seconds, result,
                 words[0] & 0xffffu, (words[0] >> 16) & 0xffu, words[0] >> 24);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_TIME_ZONE_SERVICE && command == 0) {
            /* nn::timesrv::detail::service::ITimeZoneService command 0 is
             * GetDeviceLocationName: no input, one 0x24-byte LocationName.
             * The native host has no console timezone database, so it models
             * a single explicit UTC location (no DST rule to invent). */
            uint32_t words[9] = {0};
            memcpy(words, "Etc/UTC", 7);
            reply_domain_out_words(buffer, words, 9, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetDeviceLocationName value=Etc/UTC result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_TIME_ZONE_SERVICE && command == 2) {
            /* ITimeZoneService command 2 is GetTotalLocationNameCount: no
             * input, one out u32. The modeled location list contains only the
             * UTC entry reported by GetDeviceLocationName. */
            reply_domain_out_raw(buffer, 1, 0, 1, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetTotalLocationNameCount value=1 result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_TIME_SERVICE && command == 0x12c) {
            /* nn::timesrv::detail::service::IStaticService command 300 is
             * CalculateMonotonicSystemClockBaseTimePoint (verified at
             * _nn_sf_sync_CalculateMonotonicSystemClockBaseTimePoint, which
             * sends one raw SystemClockContext and reads one out u64). The
             * result is the POSIX second at steady time zero:
             * context.offset - context.steady_time_point/1e9, the bias the
             * client's MonotonicSystemClock adds to steady seconds. */
            uint32_t in = request_meta + 0x10;
            int64_t offset = (int64_t)((uint64_t)get32(in) | ((uint64_t)get32(in + 4) << 32));
            int64_t base = offset - (int64_t)(((uint64_t)get32(in + 8)
                | ((uint64_t)get32(in + 0xc) << 32)) / 1000000000u);
            reply_domain_out_u64(buffer, (uint64_t)base);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=CalculateMonotonicSystemClockBaseTimePoint offset=%lld base=%lld result=0\n",
                    handle, session->name, object_id, interface, command,
                    (long long)offset, (long long)base);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_TIME_SYSTEM_CLOCK && command == 0) {
            /* nn::timesrv::detail::service::ISystemClock command 0 is
             * GetCurrentTime (verified at _nn_sf_sync_GetCurrentTime): no input,
             * one out raw u64 PosixTime. The native platform supplies host UTC
             * seconds, a real wall clock instead of a fixed guest constant. */
            uint64_t now = (uint64_t)time(NULL);
            reply_domain_out_u64(buffer, now);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetCurrentTime value=%llu result=0\n",
                    handle, session->name, object_id, interface, command,
                    (unsigned long long)now);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_TIME_SYSTEM_CLOCK && command == 2) {
            /* nn::timesrv::detail::service::ISystemClock command 2 is
             * GetSystemClockContext (verified at _nn_sf_sync_GetSystemClockContext
             * and processor OutRawArgumentInfo<32,8>): no input, one out raw
             * SystemClockContext = {s64 offset, SteadyClockTimePoint{ s64
             * time_point, 16-byte ClockSourceId }}. offset is the POSIX second
             * corresponding to time_point, so the SDK's correction math
             * (offset + steady delta) tracks the same wall clock. */
            uint64_t steady = steady_ns(svc);
            int64_t offset = (int64_t)time(NULL) - (int64_t)(steady / 1000000000u);
            uint32_t words[8] = {
                (uint32_t)offset, (uint32_t)((uint64_t)offset >> 32),
                (uint32_t)steady, (uint32_t)(steady >> 32),
            };
            memcpy(&words[4], clock_source_id, sizeof(clock_source_id));
            reply_domain_out_words(buffer, words, 8, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetSystemClockContext offset=%lld steady=%llu result=0\n",
                    handle, session->name, object_id, interface, command,
                    (long long)offset, (unsigned long long)steady);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_TIME_STEADY_CLOCK && command == 0) {            /* nn::timesrv::detail::service::ISteadyClock command 0 is
             * GetCurrentTimePoint (verified at _nn_sf_sync_GetCurrentTimePoint
             * and processor OutRawArgumentInfo<24,8>): no input, one out raw
             * SteadyClockTimePoint = {s64 nanoseconds, 16-byte ClockSourceId}.
             * Both clocks derive from the loader's host-monotonic tick base. */
            uint64_t steady = steady_ns(svc);
            uint32_t words[6] = {
                (uint32_t)steady, (uint32_t)(steady >> 32),
            };
            memcpy(&words[2], clock_source_id, sizeof(clock_source_id));
            reply_domain_out_words(buffer, words, 6, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetCurrentTimePoint steady=%llu result=0\n",
                    handle, session->name, object_id, interface, command,
                    (unsigned long long)steady);
            sflush(svc);
            return 0;
        } else if (!strcmp(session->name, "acc:u0")
                   && interface == VP_GUEST_OBJECT_NONE && command == 100) {
            /* nn::account::detail::IAccountServiceForApplication command 100
             * is InitializeApplicationInfo (reference ACC_U0). The loader is
             * the launched application, so the application-info object exists
             * and the reference path succeeds. The ARP launch property is not
             * available in this environment; the initialization state is
             * recorded explicitly and no per-call success is invented for any
             * later command that needs that identity. */
            state->acc_initialized = 1;
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=InitializeApplicationInfo result=0 model=loader-application-identity\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (!strcmp(session->name, "acc:u0")
                   && interface == VP_GUEST_OBJECT_NONE && command == 0) {
            /* nn::account IAccountServiceForApplication command 0 is
             * GetUserCount (Out<s32>). The loader models exactly one account:
             * the preselected user reported by PopLaunchParameter and used by
             * GetProfile/GetAccountId. Reporting zero here contradicted that
             * identity, and the title's error path launched applet 0xE
             * (Error) when it compared the two. */
            reply_domain_out_raw(buffer, 1, 0, 1, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetUserCount value=1 model=loader-preselected-user\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (!strcmp(session->name, "acc:u0")
                   && interface == VP_GUEST_OBJECT_NONE && command == 1) {
            /* nn::account::detail::IAccountServiceForApplication command 1 is
             * GetUserExistence(const Uid*): the payload is the 16-byte Uid and
             * the answer is one bool. The loader owns exactly one user, the
             * launch parameter's preselected Uid (see GetUserCount and the
             * List* commands); every other Uid gets the truthful false answer
             * instead of a fabricated account. The SDK issues this from a
             * worker thread during account setup, so leaving it unsupported
             * stopped that thread even though the main path continued. */
            uint8_t uid[16];
            for (unsigned i = 0; i < 4; ++i) {
                uint32_t word = get32(request_meta + 0x10u + i * 4u);
                uid[i * 4u + 0u] = (uint8_t)word;
                uid[i * 4u + 1u] = (uint8_t)(word >> 8);
                uid[i * 4u + 2u] = (uint8_t)(word >> 16);
                uid[i * 4u + 3u] = (uint8_t)(word >> 24);
            }
            uint32_t exists = memcmp(uid, loader_user_id, sizeof(uid)) == 0 ? 1u : 0u;
            reply_domain_out_raw(buffer, exists, 0, 1, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetUserExistence uid=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x exists=%u model=loader-preselected-user result=0\n",
                    handle, session->name, object_id, interface, command,
                    uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6], uid[7],
                    uid[8], uid[9], uid[10], uid[11], uid[12], uid[13], uid[14], uid[15],
                    exists);
            sflush(svc);
            return 0;
        } else if (!strcmp(session->name, "acc:u0")
                   && interface == VP_GUEST_OBJECT_NONE
                   && (command == 2 || command == 3)) {
            /* nn::account IAccountServiceForApplication commands 2
             * ListAllUsers and 3 ListOpenUsers both take an OutArray<Uid>.
             * The loader owns exactly one user, the launch parameter's
             * preselected Uid, and that user is the one open handle created
             * by account::Initialize; both commands therefore report the same
             * single identity. The SDK's ListAllUsers wrapper scans the
             * returned array for the first all-zero entry to derive the count,
             * so the Uid bytes are the authoritative result; no extra Uid is
             * fabricated. */
            VPGuestHipcLayout layout;
            hipc_layout(buffer, header0, header1, &layout);
            uint32_t out_size = 0, out_address = 0;
            hipc_write_buffer(&layout, 0, &out_size, &out_address);
            uint32_t result = 0;
            if (out_size < 16u || out_size > 0x400u || !out_address
                || !vp_guest_svc_writable(svc, out_address, out_size)) {
                result = SVC_RESULT_INVALID_COMBINATION;
            } else {
                /* The SDK's ListAllUsers wrapper walks the caller's array
                 * until the first all-zero Uid, so every slot the service
                 * does not fill must be a defined empty entry. Zeros here
                 * mean "no user at this index", not a fabricated account. */
                memset((void *)(uintptr_t)out_address, 0, out_size);
                memcpy((void *)(uintptr_t)out_address, loader_user_id, 16u);
            }
            fprintf(svc->log, "SVC_IPC_ACC_USERLIST command=%u buf=0x%08x hdr=0x%08x,0x%08x x=%u a=%u b=%u w=%u c=%u out=0x%08x size=0x%x result=0x%08x\n",
                    command, buffer, header0, header1, layout.x_count,
                    layout.a_count, layout.b_count, layout.w_count,
                    layout.c_count, out_address, out_size, result);
            reply_domain_out_raw(buffer, 1, 0, 2, result);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=%s count=1 model=loader-preselected-user result=0x%08x\n",
                    handle, session->name, object_id, interface, command,
                    command == 2 ? "ListAllUsers" : "ListOpenUsers", result);
            sflush(svc);
            return 0;
        } else if (!strcmp(session->name, "acc:u0")
                   && interface == VP_GUEST_OBJECT_NONE && command == 101) {
            /* nn::account::detail::IAccountServiceForApplication command 101 is
             * GetBaasAccountManagerForApplication. The reference pushes a new
             * IManagerForApplication domain object backed by the profile
             * manager. The loader has no profile database; the manager proxy
             * reports the same fixed preselected user that PopLaunchParameter
             * supplied, so the identity the SDK already consumed stays
             * consistent across both services. */
            uint32_t new_id = state->next_object_id;
            if (new_id >= VP_GUEST_DOMAIN_OBJECT_LIMIT) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_DOMAIN_OBJECT_LIMIT handle=0x%08x service=%s object=%u call=GetBaasAccountManagerForApplication\n",
                        handle, session->name, object_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            state->next_object_id = new_id + 1;
            state->objects[new_id] = VP_GUEST_OBJECT_ACC_MANAGER_FOR_APP;
            reply_domain_out_object(buffer, new_id);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetBaasAccountManagerForApplication out_object=%u init=%u model=loader-preselected-user result=0\n",
                    handle, session->name, object_id, interface, command, new_id,
                    state->acc_initialized);
            sflush(svc);
            return 0;
        } else if (!strcmp(session->name, "acc:u0")
                   && interface == VP_GUEST_OBJECT_NONE && command == 5) {
            /* nn::account::detail::IAccountServiceForApplication command 5 is
             * GetProfile(const Uid*): one out IProfile for that user. The
             * reviewed reference creates the interface for any Uid and only
             * reports the unknown user when its Get/GetBase is called, so the
             * Uid is recorded here instead of rejecting the object creation.
             * The loader owns one profile identity; other Uids reach a real
             * unknown-user answer below. */
            uint8_t uid[16];
            for (unsigned i = 0; i < 4; ++i) {
                uint32_t word = get32(request_meta + 0x10u + i * 4u);
                uid[i * 4u + 0u] = (uint8_t)word;
                uid[i * 4u + 1u] = (uint8_t)(word >> 8);
                uid[i * 4u + 2u] = (uint8_t)(word >> 16);
                uid[i * 4u + 3u] = (uint8_t)(word >> 24);
            }
            /* Temporary diagnosis: capture the request before the reply
             * overwrites the same IPC buffer. */
            fprintf(svc->log, "SVC_IPC_ACC_GETPROFILE_REQ buffer=0x%08x meta=0x%08x hdr=0x%08x,0x%08x",
                    buffer, request_meta, get32(buffer), get32(buffer + 4));
            for (unsigned d = 0; d < 20; ++d) {
                uint32_t at = buffer + d * 4u;
                if (!vp_guest_svc_readable(svc, at, 4)) break;
                uint32_t v; memcpy(&v, (void *)(uintptr_t)at, 4);
                fprintf(svc->log, " %08x", v);
            }
            fputc('\n', svc->log);
            uint32_t new_id = state->next_object_id;
            if (new_id >= VP_GUEST_DOMAIN_OBJECT_LIMIT) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_DOMAIN_OBJECT_LIMIT handle=0x%08x service=%s object=%u call=GetProfile\n",
                        handle, session->name, object_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            state->next_object_id = new_id + 1;
            state->objects[new_id] = VP_GUEST_OBJECT_ACC_PROFILE;
            memcpy(state->acc_profile_uid[new_id], uid, sizeof(uid));
            reply_domain_out_object(buffer, new_id);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetProfile uid=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x out_object=%u model=%s result=0\n",
                    handle, session->name, object_id, interface, command,
                    uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6], uid[7],
                    uid[8], uid[9], uid[10], uid[11], uid[12], uid[13], uid[14], uid[15],
                    new_id,
                    memcmp(uid, loader_user_id, sizeof(uid)) == 0
                        ? "loader-preselected-user" : "unmodeled-user");
            if (memcmp(uid, loader_user_id, sizeof(uid)) != 0) {
                /* Temporary diagnosis: identify which SDK/game path asks for
                 * an unmodeled Uid (the uid came back as zeros). */
                fprintf(svc->log, "SVC_IPC_ACC_GETPROFILE_STACK pc=0x%08x lr=0x%08x sp=0x%08x",
                        f->pc, f->lr, f->sp);
                for (unsigned d = 0; d < 13; ++d)
                    fprintf(svc->log, " r%u=%08x", d, f->regs[d]);
                for (unsigned d = 0; d < 32; ++d) {
                    uint32_t at = f->sp + d * 4u;
                    if (!vp_guest_svc_readable(svc, at, 4)) break;
                    uint32_t v; memcpy(&v, (void *)(uintptr_t)at, 4);
                    fprintf(svc->log, " %08x", v);
                }
                fputc('\n', svc->log);
                fprintf(svc->log, "SVC_IPC_ACC_GETPROFILE_BUF buffer=0x%08x meta=0x%08x",
                        buffer, request_meta);
                for (unsigned d = 0; d < 16; ++d) {
                    uint32_t at = buffer + d * 4u;
                    if (!vp_guest_svc_readable(svc, at, 4)) break;
                    uint32_t v; memcpy(&v, (void *)(uintptr_t)at, 4);
                    fprintf(svc->log, " %08x", v);
                }
                fputc('\n', svc->log);
                uint32_t cache = f->regs[8] + 0xd0u;
                if (vp_guest_svc_readable(svc, cache, 16)) {
                    fprintf(svc->log, "SVC_IPC_ACC_GETPROFILE_CACHE at=0x%08x", cache);
                    for (unsigned d = 0; d < 4; ++d) {
                        uint32_t v; memcpy(&v, (void *)(uintptr_t)(cache + d * 4u), 4);
                        fprintf(svc->log, " %08x", v);
                    }
                    fputc('\n', svc->log);
                }
                uint32_t fp = vp_guest_svc_readable(svc, f->sp, 4) ? get32(f->sp) : 0;
                for (unsigned depth = 0; depth < 24 && fp; ++depth) {
                    if (!vp_guest_svc_readable(svc, fp, 8)) break;
                    uint32_t saved_fp = get32(fp), saved_lr = get32(fp + 4);
                    fprintf(svc->log, "SVC_IPC_ACC_GETPROFILE_FRAME depth=%u fp=0x%08x saved_lr=0x%08x\n",
                            depth, fp, saved_lr);
                    if (saved_fp <= fp) break;
                    fp = saved_fp;
                }
            }
            sflush(svc);
            return 0;
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_ACC_PROFILE && (command == 0 || command == 1)) {
            /* nn::account::profile::IProfile commands 0 Get and 1 GetBase.
             * Both return the 0x38-byte ProfileBase {Uid, u64 creation time,
             * 32-byte name}; Get separately fills the caller's 0x80-byte
             * UserData buffer. The loader owns one fixed profile: the launch
             * parameter uid, creation time zero (the reference default for a
             * created user) and a display name that identifies the platform.
             * Any other Uid resolves to the reference's unknown-user error
             * (ResultUnknown in acc.cpp) with no payload; no profile data is
             * fabricated for a user the platform does not own. */
            uint8_t *profile_uid = state->acc_profile_uid[object_id];
            if (memcmp(profile_uid, loader_user_id, sizeof(loader_user_id)) != 0) {
                reply_domain_out_raw(buffer, 0, 0, 0, 0xFFFFFFFFu);
                f->regs[0] = 0;
                slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=%s uid=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x result=0xffffffff model=unknown-user-reference-unknown\n",
                        handle, session->name, object_id, interface, command,
                        command == 0 ? "Get" : "GetBase",
                        profile_uid[0], profile_uid[1], profile_uid[2], profile_uid[3],
                        profile_uid[4], profile_uid[5], profile_uid[6], profile_uid[7],
                        profile_uid[8], profile_uid[9], profile_uid[10], profile_uid[11],
                        profile_uid[12], profile_uid[13], profile_uid[14], profile_uid[15]);
                sflush(svc);
                return 0;
            }
            if (command == 0) {
                VPGuestHipcLayout layout;
                hipc_layout(buffer, header0, header1, &layout);
                uint32_t out_size = 0, out_address = 0;
                hipc_write_buffer(&layout, 0, &out_size, &out_address);
                uint32_t write_size = out_size < 0x80u ? out_size : 0x80u;
                if (write_size) {
                    if (!vp_guest_svc_writable(svc, out_address, write_size)) {
                        ++svc->ipc_unsupported;
                        slog(svc, "SVC_IPC_ACC_PROFILE_INVALID handle=0x%08x object=%u size=0x%x address=0x%08x\n",
                                handle, object_id, out_size, out_address);
                        sflush(svc);
                        return VP_BRIDGE_UNSUPPORTED;
                    }
                    memset((void *)(uintptr_t)out_address, 0, write_size);
                }
            }
            uint32_t words[14];
            memset(words, 0, sizeof(words));
            memcpy(words, loader_user_id, sizeof(loader_user_id));
            memcpy((uint8_t *)words + 0x18, loader_profile_name,
                   sizeof(loader_profile_name) - 1u);
            reply_domain_out_words(buffer, words, 14, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=%s name=%s timer=0 user_data=0x80 model=loader-profile result=0\n",
                    handle, session->name, object_id, interface, command,
                    command == 0 ? "Get" : "GetBase", loader_profile_name);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_ACC_PROFILE
                   && (command == 10 || command == 11)) {
            /* IProfile command 10 GetImageSize and 11 LoadImage. The modeled
             * profile has no icon image at all, so the size is zero and no
             * bytes are written. This is a real answer about this profile, not
             * a substitute for a missing service: the reference's own fallback
             * image is console content this loader does not ship. */
            reply_domain_out_raw(buffer, 0, 0, 1, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=%s size=0 model=no-profile-image result=0\n",
                    handle, session->name, object_id, interface, command,
                    command == 10 ? "GetImageSize" : "LoadImage");
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_ACC_MANAGER_FOR_APP && command == 0) {
            /* IManagerForApplication command 0 is CheckAvailability. The
             * reference answers false: this platform has no Nintendo Account
             * backend, so no account operation that needs one is available. */
            reply_domain_out_raw(buffer, 0, 0, 1, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=CheckAvailability value=0 model=no-account-backend result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_ACC_MANAGER_FOR_APP && command == 1) {
            /* IManagerForApplication command 1 is GetAccountId: the 64-bit
             * account id of the last opened user (reference:
             * Common::UUID::Hash of the profile manager's last user). */
            uint64_t account_id = loader_user_hash();
            reply_domain_out_u64(buffer, account_id);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetAccountId account_id=0x%016llx model=loader-preselected-user result=0\n",
                    handle, session->name, object_id, interface, command,
                    (unsigned long long)account_id);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_ACC_MANAGER_FOR_APP && command == 2) {
            /* IManagerForApplication command 2 is EnsureIdTokenCacheAsync. The
             * reference returns an IAsyncContext whose token-cache task has
             * already completed (there is no account backend to talk to), so
             * the completion event is signaled from creation. The context is
             * one per loader session and every request to it reports done. */
            uint32_t new_id = state->next_object_id;
            if (new_id >= VP_GUEST_DOMAIN_OBJECT_LIMIT) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_DOMAIN_OBJECT_LIMIT handle=0x%08x service=%s object=%u call=EnsureIdTokenCacheAsync\n",
                        handle, session->name, object_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            if (!state->acc_async_event) {
                state->acc_async_event = create_handle(svc, 3, "accasync", 0);
                if (!state->acc_async_event) {
                    ++svc->ipc_unsupported;
                    slog(svc, "SVC_IPC_DOMAIN_HANDLE_LIMIT handle=0x%08x service=%s object=%u call=EnsureIdTokenCacheAsync\n",
                            handle, session->name, object_id);
                    sflush(svc);
                    return VP_BRIDGE_UNSUPPORTED;
                }
                find_handle(svc, state->acc_async_event)->signaled = 1;
            }
            state->next_object_id = new_id + 1;
            state->objects[new_id] = VP_GUEST_OBJECT_ACC_ASYNC_CONTEXT;
            reply_domain_out_object(buffer, new_id);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=EnsureIdTokenCacheAsync out_object=%u event=0x%08x model=empty-token-cache-already-complete result=0\n",
                    handle, session->name, object_id, interface, command, new_id,
                    state->acc_async_event);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_ACC_MANAGER_FOR_APP && command == 3) {
            /* IManagerForApplication command 3 is LoadIdTokenCache: one out
             * u32 cache size. No ID token has ever been cached, so the size is
             * zero. The reference's async context answers the same value. */
            reply_domain_out_raw(buffer, 0, 0, 1, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=LoadIdTokenCache size=0 model=empty-token-cache result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_ACC_MANAGER_FOR_APP && command == 130) {
            /* IManagerForApplication command 130 is
             * GetNintendoAccountUserResourceCacheForApplication. The reference
             * zero-fills the 0x68-byte NAS user-resource cache in out buffer 0
             * (and any second out buffer the client provides) and returns the
             * last opened user's account id. Without an account backend every
             * cache field is empty/zero, which is the state of a user whose
             * resource cache has never been fetched. */
            VPGuestHipcLayout layout;
            hipc_layout(buffer, header0, header1, &layout);
            uint32_t out_size = 0, out_address = 0;
            hipc_write_buffer(&layout, 0, &out_size, &out_address);
            if (out_size < 0x68u
                || !vp_guest_svc_writable(svc, out_address, 0x68u)) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_ACC_CACHE_INVALID handle=0x%08x object=%u size=0x%x address=0x%08x\n",
                        handle, object_id, out_size, out_address);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            memset((void *)(uintptr_t)out_address, 0, 0x68u);
            uint32_t second_size = 0, second_address = 0;
            hipc_write_buffer(&layout, 1, &second_size, &second_address);
            if (second_size) {
                if (!vp_guest_svc_writable(svc, second_address, second_size)) {
                    ++svc->ipc_unsupported;
                    slog(svc, "SVC_IPC_ACC_CACHE_INVALID handle=0x%08x object=%u index=1 size=0x%x address=0x%08x\n",
                            handle, object_id, second_size, second_address);
                    sflush(svc);
                    return VP_BRIDGE_UNSUPPORTED;
                }
                memset((void *)(uintptr_t)second_address, 0, second_size);
            }
            uint64_t account_id = loader_user_hash();
            reply_domain_out_u64(buffer, account_id);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetNintendoAccountUserResourceCacheForApplication cache_size=0x68 second_size=0x%x account_id=0x%016llx model=empty-user-resource-cache result=0\n",
                    handle, session->name, object_id, interface, command,
                    second_size, (unsigned long long)account_id);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_ACC_MANAGER_FOR_APP && command == 160) {
            /* IManagerForApplication command 160 is StoreOpenContext. The
             * reference persists the opened-user list through the profile
             * manager; this loader keeps no profile store, so there is nothing
             * to write. The reported result records that the state was
             * already durable (the fixed user is the only account). */
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=StoreOpenContext model=no-profile-store result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_ACC_ASYNC_CONTEXT && command == 0) {
            /* acc IAsyncContext command 0 is GetSystemEvent: one copied
             * completion-event handle. The token-cache task is complete, so
             * the event is signaled (manual-clear, like every KEvent). */
            if (!state->acc_async_event) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_ACC_ASYNC_STATE handle=0x%08x object=%u command=0\n",
                        handle, object_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            reply_domain_out_handle(buffer, state->acc_async_event);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetSystemEvent event=0x%08x model=token-cache-complete result=0\n",
                    handle, session->name, object_id, interface, command,
                    state->acc_async_event);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_ACC_ASYNC_CONTEXT && command == 1) {
            /* IAsyncContext command 1 is Cancel. The task already completed,
             * so cancelling changes nothing and succeeds. */
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=Cancel model=token-cache-complete result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_ACC_ASYNC_CONTEXT && command == 2) {
            /* IAsyncContext command 2 is HasDone: out bool. */
            reply_domain_out_raw(buffer, 1, 0, 1, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=HasDone value=1 model=token-cache-complete result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_ACC_ASYNC_CONTEXT && command == 3) {
            /* IAsyncContext command 3 is GetResult: the response result field
             * itself is the task result (0 = success). */
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetResult result=0 model=token-cache-complete\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_FRIEND_SERVICE && command == 0) {
            /* nn::friend::IFriendService command 0 is GetCompletionEvent (one
             * out copy handle). The reference creates the event once per
             * service instance; no friend operation is ever issued on this
             * platform, so it stays unsignaled and the same loader-owned
             * event is returned on every call. */
            if (!state->friend_completion_event) {
                state->friend_completion_event = create_handle(svc, 3, "friend", 0);
                if (!state->friend_completion_event) {
                    ++svc->ipc_unsupported;
                    slog(svc, "SVC_IPC_DOMAIN_HANDLE_LIMIT handle=0x%08x service=%s object=%u\n",
                            handle, session->name, object_id);
                    sflush(svc);
                    return VP_BRIDGE_UNSUPPORTED;
                }
            }
            reply_domain_out_handle(buffer, state->friend_completion_event);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetCompletionEvent event=0x%08x model=no-friend-operation\n",
                    handle, session->name, object_id, interface, command,
                    state->friend_completion_event);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_APPLICATION_FUNCTIONS
                   && command == 1) {
            /* nn::am::service::IApplicationFunctions command 1 is
             * PopLaunchParameter (reference returns an Out<SharedPointer<
             * IStorage>> from one of two launch-parameter channels). The
             * AccountPreselectedUser channel is the launcher's hand-off to the
             * application: a 0x88-byte LaunchParameterAccountPreselectedUser
             * record (magic 0xC79497CA, selected flag, 16-byte user UUID) that
             * the SDK's TryPopPreselectedUser parses before
             * nn::account::OpenPreselectedUser. This loader is the launcher, so
             * it supplies that one record with a fixed, logged user identity;
             * the channel is empty afterwards and for the UserChannel kind,
             * where the reviewed answer is AM/2 ResultNoDataInChannel. */
            uint32_t kind = get32(request_meta + 0x10);
            /* LaunchParameterKind: 1 UserChannel, 2 AccountPreselectedUser. */
            if (kind < 1u || kind > 2u) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_DOMAIN_UNSUPPORTED handle=0x%08x service=%s object=%u interface=%u command=%u kind=%u\n",
                        handle, session->name, object_id, interface, command, kind);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            if (kind == 2u && !state->preselected_user_popped
                && state->next_object_id < VP_GUEST_DOMAIN_OBJECT_LIMIT) {
                uint32_t new_id = state->next_object_id;
                uint8_t *bytes = (uint8_t *)calloc(1, 0x88u);
                if (bytes) {
                    uint32_t magic = 0xC79497CAu; /* LaunchParameterAccountPreselectedUser magic */
                    uint32_t selected = 1u;       /* is_account_selected */
                    memcpy(bytes, &magic, 4);
                    memcpy(bytes + 4, &selected, 4);
                    /* Modeled launcher user id; no profile backend exists on
                     * this platform, so the value is fixed and reported. */
                    memcpy(bytes + 8, loader_user_id, sizeof(loader_user_id));
                    state->next_object_id = new_id + 1;
                    state->objects[new_id] = VP_GUEST_OBJECT_STORAGE;
                    state->storages[new_id].data = bytes;
                    state->storages[new_id].size = 0x88u;
                    state->storages[new_id].pushed = 0;
                    state->preselected_user_popped = 1;
                    reply_domain_out_object(buffer, new_id);
                    f->regs[0] = 0;
                    slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=PopLaunchParameter kind=2 out_object=%u size=0x88 result=0 model=loader-preselected-user user=56502d4c6f616465-52000000-00000001\n",
                            handle, session->name, object_id, interface, command,
                            new_id);
                    sflush(svc);
                    return 0;
                }
            }
            reply_domain_out_raw(buffer, 0, 0, 0, 0x480u);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=PopLaunchParameter kind=%u result=0x00000480 model=empty-channel\n",
                    handle, session->name, object_id, interface, command, kind);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_BCAT_SERVICE_CREATOR && command == 0) {
            /* nn::bcat::detail::IServiceCreator command 0 CreateBcatService:
             * one out object. The service exists in every process; the
             * delivery backend is the null backend (no BCAT server). */
            next_interface = VP_GUEST_OBJECT_BCAT_SERVICE;
            call = "CreateBcatService";
        } else if (interface == VP_GUEST_OBJECT_BCAT_SERVICE
                   && (command == 10100 || command == 10101)) {
            /* IBcatService RequestSyncDeliveryCache /
             * RequestSyncDeliveryCacheWithDirectoryName: one out progress
             * object. With no BCAT backend the sync finishes immediately
             * with zero downloaded bytes and a signalled update event,
             * which is the exact state the null backend produces. */
            if (!state->bcat_progress_event) {
                state->bcat_progress_event = create_handle(svc, 3, "bcat-prog", 0);
                if (!state->bcat_progress_event) {
                    ++svc->ipc_unsupported;
                    slog(svc, "SVC_IPC_BCAT_EVENT_LIMIT handle=0x%08x object=%u\n",
                            handle, object_id);
                    sflush(svc);
                    return VP_BRIDGE_UNSUPPORTED;
                }
            }
            VPGuestHandle *progress_event = find_handle(svc, state->bcat_progress_event);
            if (progress_event) progress_event->signaled = 1;
            next_interface = VP_GUEST_OBJECT_BCAT_PROGRESS;
            call = command == 10100 ? "RequestSyncDeliveryCache"
                : "RequestSyncDeliveryCacheWithDirectoryName";
        } else if (interface == VP_GUEST_OBJECT_BCAT_PROGRESS && command == 0) {
            /* IDeliveryCacheProgressService command 0 GetEvent: one copy
             * handle to the update event. */
            if (!state->bcat_progress_event) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_BCAT_NO_PROGRESS handle=0x%08x object=%u\n",
                        handle, object_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            reply_domain_out_handle(buffer, state->bcat_progress_event);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetEvent event=0x%08x result=0 meta=0x%08x\n",
                    handle, session->name, object_id, interface, command,
                    state->bcat_progress_event, (buffer + 0x37u) & ~0xfu);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_BCAT_PROGRESS && command == 1) {
            /* IDeliveryCacheProgressService command 1 GetImpl: OutLargeData
             * carrying the 0x200-byte DeliveryCacheProgressImpl. The null
             * backend's FinishDownload(Success) state is status Done, result
             * 0 and all byte counters zero. */
            VPGuestHipcLayout layout;
            hipc_layout(buffer, header0, header1, &layout);
            uint32_t out_size = 0, out_address = 0;
            hipc_write_buffer(&layout, 0, &out_size, &out_address);
            if (!out_size && layout.x_count) {
                uint32_t word0 = get32(layout.x_base);
                out_size = word0 >> 16;
                out_address = get32(layout.x_base + 4u);
            }
            if (out_size < 0x200u
                || !vp_guest_svc_writable(svc, out_address, out_size)) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_BCAT_PROGRESS_INVALID size=0x%x address=0x%08x\n",
                        out_size, out_address);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            memset((void *)(uintptr_t)out_address, 0, 0x200u);
            put32(out_address + 0x00u, 9u); /* DeliveryCacheProgressStatus::Done */
            put32(out_address + 0x04u, 0u); /* ResultSuccess */
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetImpl status=Done result=0 meta=0x%08x\n",
                    handle, session->name, object_id, interface, command,
                    domain_response_meta(buffer));
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_BCAT_SERVICE && command == 30100) {
            /* IBcatService SetPassphrase(u64 application_id, InBuffer
             * passphrase). The null backend stores nothing; the call's
             * argument validation is still the service contract. */
            uint64_t application_id = (uint64_t)get32(request_meta + 0x10u)
                | ((uint64_t)get32(request_meta + 0x14u) << 32);
            VPGuestHipcLayout layout;
            hipc_layout(buffer, header0, header1, &layout);
            uint32_t in_size = 0, in_address = 0;
            hipc_read_buffer(&layout, 0, &in_size, &in_address);
            uint32_t result = application_id && in_size <= 0x40u ? 0u : 0x27Au;
            reply_domain_out_raw(buffer, 0, 0, 0, result);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=SetPassphrase application_id=0x%llx size=0x%x model=null-backend result=0x%08x\n",
                    handle, session->name, object_id, interface, command,
                    (unsigned long long)application_id, in_size, result);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_BCAT_SERVICE && command == 30300) {
            /* IBcatService RegisterSystemApplicationDeliveryTasks: the
             * reference has no observable backend effect and succeeds. */
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=RegisterSystemApplicationDeliveryTasks model=null-backend result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_BCAT_SERVICE && command == 90201) {
            /* IBcatService ClearDeliveryCacheStorage(u64 application_id):
             * the null backend clears trivially; zero is invalid. */
            uint64_t application_id = (uint64_t)get32(request_meta + 0x10u)
                | ((uint64_t)get32(request_meta + 0x14u) << 32);
            uint32_t result = application_id ? 0u : 0x27Au;
            reply_domain_out_raw(buffer, 0, 0, 0, result);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=ClearDeliveryCacheStorage application_id=0x%llx model=null-backend result=0x%08x\n",
                    handle, session->name, object_id, interface, command,
                    (unsigned long long)application_id, result);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_CREATOR && command == 0) {
            /* ILibraryAppletCreator command 0 CreateLibraryApplet(
             * AppletId, LibraryAppletMode): one out accessor object. The
             * locked application creates the software-keyboard applet during
             * initialization, so that object is modeled. No library-applet
             * process exists on this platform: the accessor can be observed,
             * but Start and the data channels are refused instead of being
             * faked. Any other applet id stops the run. */
            uint32_t applet_id = get32(request_meta + 0x10u);
            uint32_t applet_mode = get32(request_meta + 0x14u);
            if (applet_id != 0x11u) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_APPLET_UNSUPPORTED id=0x%x mode=%u\n", applet_id, applet_mode);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            uint32_t new_id = alloc_object_id(state);
            if (!new_id) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_DOMAIN_OBJECT_LIMIT handle=0x%08x object=%u\n", handle, object_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            state->objects[new_id] = VP_GUEST_OBJECT_LIBRARY_APPLET_ACCESSOR;
            state->lapplet_id[new_id] = applet_id;
            state->lapplet_mode[new_id] = applet_mode;
            state->lapplet_event[new_id] = 0;
            /* The reference signals the launchable event once the applet has
             * been created ("applet is created, can now be launched"); the SDK
             * waits on it in StartLibraryApplet before the accessor Start. */
            if (state->lapplet_launch_event) {
                VPGuestHandle *launch_slot = find_handle(svc, state->lapplet_launch_event);
                if (launch_slot) launch_slot->signaled = 1;
            }
            reply_domain_out_object(buffer, new_id);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=CreateLibraryApplet applet=0x%x mode=%u out_object=%u model=no-applet-process result=0 meta=0x%08x\n",
                    handle, session->name, object_id, interface, command, applet_id,
                    applet_mode, new_id, domain_response_meta(buffer));
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_CREATOR && command == 10) {
            /* ILibraryAppletCreator command 10 CreateStorage(s64 size): one
             * out IStorage object. The bytes are a host-side zero-filled
             * buffer owned by this server session; the accessor copies them
             * to and from the client's HIPC buffers, so nothing is mapped
             * into the guest address space. The SDK's
             * nn::la::CommonArgumentsWriter and PushToInChannel use this path
             * to hand the keyboard applet its input storages. */
            int64_t storage_size = (int64_t)((uint64_t)get32(request_meta + 0x10u)
                | ((uint64_t)get32(request_meta + 0x14u) << 32));
            uint32_t new_id = alloc_object_id(state);
            if (storage_size <= 0 || storage_size > 0x01000000
                || !new_id) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_STORAGE_INVALID handle=0x%08x object=%u size=0x%llx next_object=%u\n",
                        handle, object_id, (unsigned long long)storage_size, new_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            uint8_t *bytes = (uint8_t *)malloc((size_t)storage_size);
            if (!bytes) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_STORAGE_OOM handle=0x%08x object=%u size=0x%llx\n",
                        handle, object_id, (unsigned long long)storage_size);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            memset(bytes, 0, (size_t)storage_size);
            state->objects[new_id] = VP_GUEST_OBJECT_STORAGE;
            state->storages[new_id].data = bytes;
            state->storages[new_id].size = (uint32_t)storage_size;
            state->storages[new_id].pushed = 0;
            reply_domain_out_object(buffer, new_id);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=CreateStorage size=0x%x out_object=%u result=0 meta=0x%08x\n",
                    handle, session->name, object_id, interface, command,
                    (uint32_t)storage_size, new_id, domain_response_meta(buffer));
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_STORAGE && command == 0) {
            /* IStorage command 0 Open: one out IStorageAccessor bound to this
             * storage. OpenTransferStorage (command 1) is not part of the
             * launched title's keyboard path and stays unsupported. */
            if (!state->storages[object_id].data) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_STORAGE_OPEN_INVALID handle=0x%08x object=%u\n",
                        handle, object_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            uint32_t new_id = alloc_object_id(state);
            if (!new_id) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_DOMAIN_OBJECT_LIMIT handle=0x%08x object=%u call=Open\n",
                        handle, object_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            state->objects[new_id] = VP_GUEST_OBJECT_STORAGE_ACCESSOR;
            state->storage_parent[new_id] = object_id;
            reply_domain_out_object(buffer, new_id);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=StorageOpen size=0x%x out_object=%u result=0\n",
                    handle, session->name, object_id, interface, command,
                    state->storages[object_id].size, new_id);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_STORAGE_ACCESSOR
                   && (command == 0 || command == 10 || command == 11)) {
            /* IStorageAccessor GetSize (0), Write (10) and Read (11). The
             * data transfer is a HIPC InBuffer/OutBuffer plus the s64 offset
             * in raw data; offsets are checked against the storage size and
             * an out-of-range access returns AM ResultInvalidOffset
             * (0x80 | 503<<9) instead of silently truncating. */
            uint32_t storage_id = state->storage_parent[object_id];
            if (storage_id >= VP_GUEST_DOMAIN_OBJECT_LIMIT
                || !state->storages[storage_id].data) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_STORAGE_ACCESSOR_INVALID handle=0x%08x object=%u\n",
                        handle, object_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            uint32_t storage_size = state->storages[storage_id].size;
            if (command == 0) {
                reply_domain_out_u64(buffer, storage_size);
                f->regs[0] = 0;
                slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=StorageGetSize size=0x%x result=0\n",
                        handle, session->name, object_id, interface, command, storage_size);
                sflush(svc);
                return 0;
            }
            int64_t offset = (int64_t)((uint64_t)get32(request_meta + 0x10u)
                | ((uint64_t)get32(request_meta + 0x14u) << 32));
            VPGuestHipcLayout layout;
            hipc_layout(buffer, header0, header1, &layout);
            uint32_t io_size = 0, io_address = 0;
            if (command == 10) hipc_read_buffer(&layout, 0, &io_size, &io_address);
            else hipc_write_buffer(&layout, 0, &io_size, &io_address);
            uint32_t result = 0;
            if (offset < 0 || (uint64_t)offset + io_size > storage_size
                || (io_size && !vp_guest_svc_readable(svc, io_address, io_size))
                || (command == 11 && io_size
                    && !vp_guest_svc_writable(svc, io_address, io_size))) {
                result = 0x3EE80u; /* AM ResultInvalidOffset */
            } else if (io_size) {
                if (command == 10) {
                    memcpy(state->storages[storage_id].data + (uint32_t)offset,
                           (const void *)(uintptr_t)io_address, io_size);
                } else {
                    memcpy((void *)(uintptr_t)io_address,
                           state->storages[storage_id].data + (uint32_t)offset, io_size);
                }
            }
            if (command == 11 && io_size == 0x88u && offset == 0) {
                /* Temporary diagnosis: show the launch-parameter record the
                 * guest actually received. */
                const uint8_t *b = state->storages[storage_id].data;
                slog(svc, "SVC_IPC_STORAGE_READ_HEAD storage=%u size=0x88 magic=%02x%02x%02x%02x selected=%02x%02x%02x%02x uid=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                        storage_id, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15],
                        b[16], b[17], b[18], b[19], b[20], b[21], b[22], b[23]);
                fprintf(svc->log, "SVC_IPC_STORAGE_READ_DST at=0x%08x size=0x%x", io_address, io_size);
                for (unsigned d = 0; d < 8; ++d) {
                    uint32_t at = io_address + d * 4u;
                    if (!vp_guest_svc_readable(svc, at, 4)) break;
                    uint32_t v; memcpy(&v, (void *)(uintptr_t)at, 4);
                    fprintf(svc->log, " %08x", v);
                }
                fputc('\n', svc->log);
            }
            reply_domain_out_raw(buffer, 0, 0, 0, result);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=%s storage=%u offset=%lld size=0x%x result=0x%08x\n",
                    handle, session->name, object_id, interface, command,
                    command == 10 ? "StorageWrite" : "StorageRead", storage_id,
                    (long long)offset, io_size, result);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_ACCESSOR && command == 10) {
            /* ILibraryAppletAccessor command 10 Start: the SDK asks the applet
             * to run. No applet process exists on this platform, so the call
             * succeeds at the protocol level and the accessor records that it
             * was started; no state change is fabricated. */
            state->lapplet_started[object_id] = 1;
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=Start model=no-applet-process result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_ACCESSOR && command == 100) {
            /* ILibraryAppletAccessor command 100 PushInData(SharedPointer<
             * IStorage>): the input object id is the first raw data word and
             * the storage must already exist in this domain. The storage is
             * retained as the applet's pending input; nothing consumes it
             * because no applet process runs. */
            uint32_t storage_id = get32(request_meta + 0x10u);
            if (storage_id >= VP_GUEST_DOMAIN_OBJECT_LIMIT
                || state->objects[storage_id] != VP_GUEST_OBJECT_STORAGE
                || !state->storages[storage_id].data) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_APPLET_PUSH_INVALID handle=0x%08x object=%u storage=%u\n",
                        handle, object_id, storage_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            state->storages[storage_id].pushed = 1;
            ++state->lapplet_in_data[object_id];
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=PushInData storage=%u size=0x%x count=%u model=no-applet-process result=0\n",
                    handle, session->name, object_id, interface, command, storage_id,
                    state->storages[storage_id].size, state->lapplet_in_data[object_id]);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_ACCESSOR && command == 101) {
            /* PopOutData: no applet process produces normal-channel output
             * here, so the broker is empty and the reviewed server answer is
             * AM ResultNoDataInChannel (0x80 | 2<<9). Inline-keyboard replies
             * travel on the interactive channel and are handled below. */
            reply_domain_out_raw(buffer, 0, 0, 0, 0x480u);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=PopOutData result=0x00000480 model=no-applet-output\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_ACCESSOR && command == 104) {
            /* PopInteractiveOutData: return the oldest queued swkbd reply as
             * a new IStorage object. An empty queue is the protocol's
             * ResultNoDataInChannel, not a fabricated empty storage. The
             * title polls this channel at frame rate while the keyboard is
             * shown, so this is also where physical pad edges are turned into
             * ChangedString/DecidedEnter replies. */
            if (state->swkbd.object == object_id)
                swkbd_poll_active(svc);
            if (state->swkbd.object != object_id || !state->swkbd.reply_count) {
                /* DecidedEnter hides the inline keyboard; it does not exit
                 * the applet. The client issues Finalize after processing its
                 * text callback. Do not signal applet termination here. */
                reply_domain_out_raw(buffer, 0, 0, 0, 0x480u);
                f->regs[0] = 0;
                slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=PopInteractiveOutData result=0x00000480 queued=%u model=swkbd-inline-protocol\n",
                        handle, session->name, object_id, interface, command,
                        state->swkbd.reply_count);
                sflush(svc);
                return 0;
            }
            uint8_t *reply_data = state->swkbd.replies[0].data;
            uint32_t reply_size = state->swkbd.replies[0].size;
            for (uint32_t i = 1; i < state->swkbd.reply_count; ++i)
                state->swkbd.replies[i - 1] = state->swkbd.replies[i];
            --state->swkbd.reply_count;
            uint32_t storage_id = swkbd_reply_storage(svc, state, reply_data, reply_size);
            if (!storage_id) {
                free(reply_data);
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_APPLET_STORAGE_LIMIT handle=0x%08x object=%u next_object=%u\n",
                        handle, object_id, state->next_object_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            reply_domain_out_object(buffer, storage_id);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=PopInteractiveOutData storage=%u size=0x%x type=%u state=%u queued=%u model=swkbd-inline-protocol result=0 meta=0x%08x\n",
                    handle, session->name, object_id, interface, command, storage_id,
                    reply_size, get32((uint32_t)(uintptr_t)reply_data + 4),
                    get32((uint32_t)(uintptr_t)reply_data), state->swkbd.reply_count,
                    domain_response_meta(buffer));
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_ACCESSOR && command == 103) {
            /* PushInteractiveInData(IStorage): the inline software keyboard
             * protocol. The applet process is not executed here; the loader
             * parses the request the title wrote into the storage, maintains
             * the SwkbdState every reply reports, and queues the replies the
             * protocol defines. No user input is invented: text-echo replies
             * are only produced for a request the title itself sent while the
             * keyboard is shown. */
            uint32_t storage_id = get32(request_meta + 0x10u);
            if (storage_id >= VP_GUEST_DOMAIN_OBJECT_LIMIT
                || state->objects[storage_id] != VP_GUEST_OBJECT_STORAGE
                || !state->storages[storage_id].data
                || state->storages[storage_id].size < 4u
                || state->lapplet_id[object_id] != 0x11u) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_APPLET_INTERACTIVE_INVALID handle=0x%08x object=%u applet=0x%x storage=%u\n",
                        handle, object_id, state->lapplet_id[object_id], storage_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            if (state->swkbd.object && state->swkbd.object != object_id) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_APPLET_SECOND_KEYBOARD handle=0x%08x object=%u active=%u\n",
                        handle, object_id, state->swkbd.object);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            if (!state->swkbd.object) {
                state->swkbd.object = object_id;
                state->swkbd.state = VP_SWKBD_STATE_NOT_INITIALIZED;
            }
            state->storages[storage_id].pushed = 1;
            const uint8_t *request = state->storages[storage_id].data;
            uint32_t request_size = state->storages[storage_id].size;
            const uint8_t *arg = request + 4;
            uint32_t request_command = get32((uint32_t)(uintptr_t)request);
            fprintf(svc->log, "SVC_IPC_APPLET_INTERACTIVE_HEAD storage=%u size=0x%x words",
                    storage_id, request_size);
            for (unsigned d = 0; d < 64u && (d + 1u) * 4u <= request_size; ++d)
                fprintf(svc->log, " %08x", get32((uint32_t)(uintptr_t)request + d * 4u));
            uint32_t source = state->storages[storage_id].source_address;
            if (source && vp_guest_svc_readable(svc, source, 64u)) {
                fprintf(svc->log, " source=0x%08x source_now", source);
                for (unsigned d = 0; d < 16u; ++d)
                    fprintf(svc->log, " %08x", get32(source + d * 4u));
            }
            fputc('\n', svc->log);
            uint32_t text_chars = 0;
            uint64_t flags = 0;
            int queued = 0;
            if (request_command == 0x0au) {
                /* Calc: common header at arg+0, layout-specific text and
                 * cursor offsets follow the SwkbdAppletVersion the title
                 * selected through the storage size it allocated. */
                if (request_size < 4u + 0x18u) goto swkbd_invalid;
                uint32_t calc_size = get32((uint32_t)(uintptr_t)arg + 4) & 0xffffu;
                uint32_t text_off, cursor_off;
                if (calc_size == 0x4a0u) {
                    text_off = 0x68; cursor_off = 0x1c;
                } else if (calc_size == 0x4e8u) {
                    text_off = 0x90; cursor_off = 0x8c;
                } else {
                    goto swkbd_invalid;
                }
                if (request_size < 4u + text_off + 0x3f4u) goto swkbd_invalid;
                /* The Calc argument carries the requested reply encoding.
                 * UTF-8 mode must be answered with the *Utf8 reply types and
                 * UTF-8 text, not the UTF-16 variants. */
                {
                    uint32_t utf8_off = calc_size == 0x4a0u ? 0x45cu : 0x484u;
                    if (request_size >= 4u + utf8_off + 1u)
                        state->swkbd.utf8_mode = arg[utf8_off] ? 1u : 0u;
                }
                flags = (uint64_t)get32((uint32_t)(uintptr_t)arg + 8)
                    | ((uint64_t)get32((uint32_t)(uintptr_t)arg + 0xc) << 32);
                state->swkbd.calc_size = calc_size;
                if (flags&4u) {
                    uint32_t limits=calc_size==0x4a0u?0x40u:0x38u;
                    state->swkbd.max_chars=get32((uint32_t)(uintptr_t)arg+limits);
                    state->swkbd.min_chars=get32((uint32_t)(uintptr_t)arg+limits+4u);
                    if (state->swkbd.min_chars==0xffffffffu) state->swkbd.min_chars=0u;
                    slog(svc,"VP_SWKBD_LIMITS max=%u min=%u\n",state->swkbd.max_chars,state->swkbd.min_chars);
                }
                if (flags & 8u) {
                    text_chars = 0;
                    while (text_chars < 0x1fau
                           && (arg[text_off + text_chars * 2u]
                               || arg[text_off + text_chars * 2u + 1u]))
                        ++text_chars;
                    uint32_t text_bytes = text_chars * 2u;
                    if (text_bytes > VP_SWKBD_UTF16_REPLY_BYTES)
                        text_bytes = VP_SWKBD_UTF16_REPLY_BYTES;
                    memcpy(state->swkbd.text, arg + text_off, text_bytes);
                    state->swkbd.text_len = text_bytes;
                }
                if (flags & 0x10u)
                    state->swkbd.cursor = get32((uint32_t)(uintptr_t)arg + cursor_off);
                if (state->swkbd.state == VP_SWKBD_STATE_NOT_INITIALIZED && (flags & 1u)) {
                    state->swkbd.decided = 0;
                    state->swkbd.text_len = 0;
                    state->swkbd.cursor = 0;
                    state->swkbd.selection = 0;
                    state->swkbd.input_last = 0;
                    state->lapplet_complete[object_id] = 0;
                    if (state->lapplet_event[object_id]) {
                        VPGuestHandle *changed = find_handle(svc,
                            state->lapplet_event[object_id]);
                        if (changed != NULL) changed->signaled = 0;
                    }
                    state->swkbd.state = VP_SWKBD_STATE_HIDDEN;
                    if (swkbd_queue_reply(svc, state, object_id, VP_SWKBD_REPLY_DEFAULT) < 0)
                        goto swkbd_queue_full;
                    if (swkbd_queue_reply(svc, state, object_id,
                                          VP_SWKBD_REPLY_FINISHED_INIT) < 0)
                        goto swkbd_queue_full;
                    queued += 2;
                    swkbd_selftest(svc, state);
                }
                if (!(flags & 1u) && (flags & (8u | 0x10u))) {
                    if (state->swkbd.state == VP_SWKBD_STATE_SHOWN) {
                        uint32_t type = state->swkbd.utf8_mode
                            ? VP_SWKBD_REPLY_CHANGED_STRING_UTF8
                            : state->swkbd.changed_v2
                                ? VP_SWKBD_REPLY_CHANGED_STRING_V2
                                : VP_SWKBD_REPLY_CHANGED_STRING;
                        if (swkbd_queue_reply(svc, state, object_id, type) < 0)
                            goto swkbd_queue_full;
                        ++queued;
                    } else {
                        slog(svc, "SVC_IPC_SWKBD_TEXT_WHILE_HIDDEN accessor=%u chars=%u cursor=%u state=%u model=stored-no-reply\n",
                                object_id, text_chars, state->swkbd.cursor,
                                state->swkbd.state);
                    }
                }
                if (state->swkbd.state == VP_SWKBD_STATE_HIDDEN && (flags & 4u)) {
                    state->swkbd.state = VP_SWKBD_STATE_APPEARING;
                    if (swkbd_queue_reply(svc, state, object_id, VP_SWKBD_REPLY_DEFAULT) < 0)
                        goto swkbd_queue_full;
                    /* The reference applet reports Shown immediately after it
                     * asks the frontend to show the keyboard. This loader is
                     * the frontend: GetIndirectLayerImageMap fills the
                     * applet's image with the keyboard the loader draws, and
                     * PopInteractiveOutData polls the physical pad. */
                    state->swkbd.state = VP_SWKBD_STATE_SHOWN;
                    state->swkbd.decided = 0;
                    state->swkbd.input_last = 0;
                    state->swkbd.render_dirty = 1;
                    if (swkbd_queue_reply(svc, state, object_id, VP_SWKBD_REPLY_DEFAULT) < 0)
                        goto swkbd_queue_full;
                    queued += 2;
                }
                if (state->swkbd.state == VP_SWKBD_STATE_SHOWN && (flags & 0x80u)) {
                    state->swkbd.state = VP_SWKBD_STATE_DISAPPEARING;
                    if (swkbd_queue_reply(svc, state, object_id, VP_SWKBD_REPLY_DEFAULT) < 0)
                        goto swkbd_queue_full;
                    state->swkbd.state = VP_SWKBD_STATE_HIDDEN;
                    state->swkbd.decided = 0;
                    state->swkbd.render_dirty = 1;
                    if (swkbd_queue_reply(svc, state, object_id, VP_SWKBD_REPLY_DEFAULT) < 0)
                        goto swkbd_queue_full;
                    swkbd_clear_rendered_image(svc,state,"hide");
                    queued += 2;
                }
                if (state->swkbd.state <= VP_SWKBD_STATE_HIDDEN && (flags & 0x40u)) {
                    if (swkbd_queue_reply(svc, state, object_id,
                                          VP_SWKBD_REPLY_UNSET_CUSTOMIZE_DIC) < 0)
                        goto swkbd_queue_full;
                    ++queued;
                }
                if (state->swkbd.state <= VP_SWKBD_STATE_HIDDEN && (flags & 0x400u)) {
                    if (swkbd_queue_reply(svc, state, object_id,
                                          VP_SWKBD_REPLY_RELEASED_USER_WORD_INFO) < 0)
                        goto swkbd_queue_full;
                    ++queued;
                }
                slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=PushInteractiveInData storage=%u size=0x%x calc=0x%x flags=0x%llx chars=%u cursor=%u state=%u utf8=%u replies=%u model=swkbd-inline-protocol result=0\n",
                        handle, session->name, object_id, interface, command,
                        storage_id, request_size, state->swkbd.calc_size,
                        (unsigned long long)flags, text_chars, state->swkbd.cursor,
                        state->swkbd.state, state->swkbd.utf8_mode, queued);
                reply_domain_out_raw(buffer, 0, 0, 0, 0);
                f->regs[0] = 0;
                sflush(svc);
                return 0;
            } else if (request_command == 0x4u) {
                /* Finalize: the applet resets to NotInitialized, reports it,
                 * and exits. DecidedEnter only hides the inline keyboard;
                 * completion and the state-changed
                 * event follow the applet's exit, not a synthesized result. */
                swkbd_clear_rendered_image(svc,state,"finalize");
                state->swkbd.state = VP_SWKBD_STATE_NOT_INITIALIZED;
                state->swkbd.decided = 0;
                state->swkbd.render_dirty = 1;
                if (swkbd_queue_reply(svc, state, object_id, VP_SWKBD_REPLY_DEFAULT) < 0)
                    goto swkbd_queue_full;
                state->lapplet_complete[object_id] = 1;
                VPGuestHandle *changed = find_handle(svc, state->lapplet_event[object_id]);
                if (changed) changed->signaled = 1;
                slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=PushInteractiveInData storage=%u command=Finalize complete=1 model=swkbd-inline-protocol result=0\n",
                        handle, session->name, object_id, interface, command, storage_id);
                reply_domain_out_raw(buffer, 0, 0, 0, 0);
                f->regs[0] = 0;
                sflush(svc);
                return 0;
            } else if (request_command == 0x6u) {
                if (swkbd_queue_reply(svc, state, object_id,
                                      VP_SWKBD_REPLY_RELEASED_USER_WORD_INFO) < 0)
                    goto swkbd_queue_full;
                slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=PushInteractiveInData command=SetUserWordInfo model=word-info-not-stored result=0\n",
                        handle, session->name, object_id, interface, command);
                reply_domain_out_raw(buffer, 0, 0, 0, 0);
                f->regs[0] = 0;
                sflush(svc);
                return 0;
            } else if (request_command == 0x7u) {
                slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=PushInteractiveInData command=SetCustomizeDic model=custom-dictionary-not-stored result=0\n",
                        handle, session->name, object_id, interface, command);
                reply_domain_out_raw(buffer, 0, 0, 0, 0);
                f->regs[0] = 0;
                sflush(svc);
                return 0;
            } else if (request_command == 0x43444a4eu
                       && state->storages[storage_id].source_address
                       && request_size >= 0x20u
                       && get32((uint32_t)(uintptr_t)request + 4u) == 0x00000200u
                       && get32((uint32_t)(uintptr_t)request + 8u) == 0x03000000u) {
                /* Customized-dictionary transfer. The title reads an iwnn
                 * dictionary from its RomFS (the observed bytes match
                 * nativeNX/system/swkbd/iwnn/eng/dictionary_rabbit0_eng.a,
                 * magic "NJDC"), places it in a TransferMemory, creates an
                 * IStorage through CreateHandleStorage, and pushes that
                 * storage to the swkbd applet separately from the request
                 * channel: the first word is the dictionary magic, not a
                 * SwkbdRequestCommand. The reference applet consumes the
                 * dictionary for word prediction and sends no interactive
                 * reply, so the modeled applet records the transfer and
                 * answers success without inventing one. Prediction data is
                 * not consumed by this model. */
                uint32_t header_size = get32((uint32_t)(uintptr_t)request + 4u);
                uint32_t version = __builtin_bswap32(
                    get32((uint32_t)(uintptr_t)request + 8u));
                uint32_t entries = __builtin_bswap32(
                    get32((uint32_t)(uintptr_t)request + 0x10u));
                slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=PushInteractiveInData storage=%u size=0x%x command=SetCustomizeDicDictionary magic=NJDC header=0x%x version=%u entries=%u model=swkbd-dictionary-transfer data-not-consumed result=0\n",
                        handle, session->name, object_id, interface, command,
                        storage_id, request_size, header_size, version, entries);
                reply_domain_out_raw(buffer, 0, 0, 0, 0);
                f->regs[0] = 0;
                sflush(svc);
                return 0;
            } else if (request_command == 0xdu || request_command == 0xeu) {
                uint32_t flag = request_size >= 5u ? request[4] : 0u;
                if (request_command == 0xdu) state->swkbd.changed_v2 = flag ? 1u : 0u;
                else state->swkbd.moved_v2 = flag ? 1u : 0u;
                slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=PushInteractiveInData command=%s flag=%u model=reply-version-recorded result=0\n",
                        handle, session->name, object_id, interface, command,
                        request_command == 0xdu ? "SetChangedStringV2Flag"
                                                : "SetMovedCursorV2Flag", flag);
                reply_domain_out_raw(buffer, 0, 0, 0, 0);
                f->regs[0] = 0;
                sflush(svc);
                return 0;
            }
swkbd_invalid:
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_APPLET_INTERACTIVE_SHAPE handle=0x%08x object=%u storage=%u size=0x%x command=0x%x\n",
                    handle, object_id, storage_id, request_size,
                    request_size >= 4u ? request_command : 0u);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
swkbd_queue_full:
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_SWKBD_QUEUE_FULL handle=0x%08x object=%u queued=%u\n",
                    handle, object_id, state->swkbd.reply_count);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_ACCESSOR
                   && (command == 105 || command == 106)) {
            /* GetPopOutDataEvent / GetPopInteractiveOutDataEvent: the broker
             * owns one event per channel. It never fires without an applet,
             * exactly like the persistent unsignalled completion events. */
            uint32_t *slot = command == 105
                ? &state->lapplet_pop_event[object_id]
                : &state->lapplet_interactive_event[object_id];
            if (!*slot) {
                *slot = create_handle(svc, 3,
                    command == 105 ? "appl-pop" : "appl-ipop", 0);
                if (!*slot) {
                    ++svc->ipc_unsupported;
                    slog(svc, "SVC_IPC_DOMAIN_HANDLE_LIMIT handle=0x%08x object=%u\n",
                            handle, object_id);
                    sflush(svc);
                    return VP_BRIDGE_UNSUPPORTED;
                }
            }
            reply_domain_out_handle(buffer, *slot);
            f->regs[0] = 0;
            /* A reply queued before the guest asked for the event must still
             * be observable: the event is level-signaled like the broker's. */
            if (command == 106 && state->swkbd.object == object_id
                && state->swkbd.reply_count) {
                VPGuestHandle *event = find_handle(svc, *slot);
                if (event) event->signaled = 1;
            }
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=%s event=0x%08x model=applet-broker-event result=0 meta=0x%08x\n",
                    handle, session->name, object_id, interface, command,
                    command == 105 ? "GetPopOutDataEvent" : "GetPopInteractiveOutDataEvent",
                    *slot, (buffer + 0x37u) & ~0xfu);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_ACCESSOR && command == 0) {
            /* ILibraryAppletAccessor command 0 GetAppletStateChangedEvent:
             * one copy handle. The event fires when the modeled applet exits
             * (only the inline keyboard has an exit model); an applet with no
             * process leaves it unsignalled. If the exit already happened
             * before the guest first asked for the event, the event is
             * created signalled so the guest can observe the state change. */
            if (!state->lapplet_event[object_id]) {
                state->lapplet_event[object_id] = create_handle(svc, 3, "lappl-ev", 0);
                if (!state->lapplet_event[object_id]) {
                    ++svc->ipc_unsupported;
                    slog(svc, "SVC_IPC_DOMAIN_HANDLE_LIMIT handle=0x%08x object=%u\n", handle, object_id);
                    sflush(svc);
                    return VP_BRIDGE_UNSUPPORTED;
                }
            }
            if (state->lapplet_complete[object_id]) {
                VPGuestHandle *event = find_handle(svc, state->lapplet_event[object_id]);
                if (event) event->signaled = 1;
            }
            reply_domain_out_handle(buffer, state->lapplet_event[object_id]);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetAppletStateChangedEvent event=0x%08x model=applet-not-started result=0 meta=0x%08x\n",
                    handle, session->name, object_id, interface, command,
                    state->lapplet_event[object_id], (buffer + 0x37u) & ~0xfu);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_ACCESSOR && command == 1) {
            /* IsCompleted: true only after the modeled inline keyboard
             * processed its Finalize request and exited, mirroring the real
             * applet's lifecycle. Any other applet has no process here. */
            uint32_t completed = state->lapplet_complete[object_id] ? 1u : 0u;
            reply_domain_out_raw(buffer, completed, 0, 1, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=IsCompleted completed=%u model=applet-lifecycle result=0\n",
                    handle, session->name, object_id, interface, command, completed);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_ACCESSOR && command == 30) {
            /* GetResult: the applet's terminate result; no applet process has
             * run, so the reference's default success value is reported. */
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetResult result=0 model=applet-not-started\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_ACCESSOR && command == 60) {
            /* PresetLibraryAppletGpuTimeSliceZero: no arguments, no effect
             * without an applet process. */
            reply_domain_out_raw(buffer, 0, 0, 0, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=PresetLibraryAppletGpuTimeSliceZero model=no-applet-process result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_ACCESSOR && command == 160) {
            /* GetIndirectLayerConsumerHandle: the client needs a non-zero
             * value to treat as valid. No applet process or indirect layer
             * exists here, so report the reference's documented sentinel and
             * say so; this is not a usable layer handle. */
            reply_domain_out_u64(buffer, 0xDEADBEEFu);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=GetIndirectLayerConsumerHandle value=0xdeadbeef model=no-applet-process result=0\n",
                    handle, session->name, object_id, interface, command);
            sflush(svc);
            return 0;
        } else if (interface == VP_GUEST_OBJECT_LIBRARY_APPLET_CREATOR && command == 12) {
            /* ILibraryAppletCreator command 12 CreateHandleStorage(s64 size,
             * InCopyHandle<TransferMemory>): one out IStorage backed by the
             * transfer memory. The CMIF raw data carries the size; the copy
             * handle sits in the HIPC handle array described by the special
             * word. The storage copies the transfer memory's current bytes,
             * which is what the applet would read; no applet process exists,
             * so the accessor remains the only consumer. */
            int64_t storage_size = (int64_t)((uint64_t)get32(request_meta + 0x10u)
                | ((uint64_t)get32(request_meta + 0x14u) << 32));
            uint32_t special = (header1 & 0x80000000u) ? get32(buffer + 8u) : 0;
            uint32_t copy_count = (special >> 1) & 0xfu;
            uint32_t move_count = (special >> 5) & 0xfu;
            uint32_t handle_words = 3u + ((special & 1u) ? 2u : 0u);
            uint32_t tmem = copy_count || move_count
                ? get32(buffer + handle_words * 4u) : 0;
            VPGuestHandle *transfer = find_handle(svc, tmem);
            uint32_t new_id = alloc_object_id(state);
            if (storage_size <= 0 || storage_size > 0x01000000
                || !transfer || transfer->kind != VP_GUEST_HANDLE_KIND_TRANSFER
                || (uint32_t)storage_size > transfer->transfer_size
                || !new_id
                || !vp_guest_svc_readable(svc, transfer->transfer_address,
                                          (uint32_t)storage_size)) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_DOMAIN_UNSUPPORTED handle=0x%08x service=%s object=%u interface=%u command=%u size=0x%llx special=0x%x copy=%u move=%u tmem=0x%08x transfer=%u hdr1=0x%08x\n",
                        handle, session->name, object_id, interface, command,
                        (unsigned long long)storage_size, special, copy_count, move_count,
                        tmem, transfer ? transfer->kind : 0, header1);
                for (unsigned w = 0; w < 8; ++w)
                    slog(svc, "SVC_IPC_DOMAIN_UNSUPPORTED_WORD +0x%02x=0x%08x\n",
                            w * 4u, get32(buffer + w * 4u));
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            uint8_t *bytes = (uint8_t *)malloc((size_t)storage_size);
            if (!bytes) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_STORAGE_OOM handle=0x%08x object=%u size=0x%llx\n",
                        handle, object_id, (unsigned long long)storage_size);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            memcpy(bytes, (const void *)(uintptr_t)transfer->transfer_address,
                   (size_t)storage_size);
            state->objects[new_id] = VP_GUEST_OBJECT_STORAGE;
            state->storages[new_id].data = bytes;
            state->storages[new_id].size = (uint32_t)storage_size;
            state->storages[new_id].pushed = 0;
            state->storages[new_id].source_address = transfer->transfer_address;
            fprintf(svc->log, "SVC_IPC_STORAGE_SOURCE address=0x%08x copy=",
                    transfer->transfer_address);
            for (unsigned d = 0; d < 8 && vp_guest_svc_readable(svc, transfer->transfer_address + d * 4u, 4); ++d)
                fprintf(svc->log, " %08x", get32(transfer->transfer_address + d * 4u));
            fputc('\n', svc->log);
            reply_domain_out_object(buffer, new_id);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=CreateHandleStorage size=0x%x transfer=0x%08x out_object=%u model=transfer-memory-copy result=0 meta=0x%08x\n",
                    handle, session->name, object_id, interface, command,
                    (uint32_t)storage_size, tmem, new_id,
                    domain_response_meta(buffer));
            sflush(svc);
            return 0;
        }
        if (next_interface != VP_GUEST_OBJECT_NONE) {
            uint32_t new_id = state->next_object_id;
            if (new_id >= VP_GUEST_DOMAIN_OBJECT_LIMIT) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_DOMAIN_OBJECT_LIMIT handle=0x%08x object=%u call=%s\n",
                        handle, object_id, call);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            state->next_object_id = new_id + 1;
            state->objects[new_id] = (uint8_t)next_interface;
            reply_domain_out_object(buffer, new_id);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_DOMAIN_CALL handle=0x%08x service=%s object=%u interface=%u command=%u call=%s out_object=%u out_interface=%u result=0 meta=0x%08x\n",
                    handle, session->name, object_id, interface, command, call,
                    new_id, next_interface, domain_response_meta(buffer));
            sflush(svc);
            return 0;
        }
        ++svc->ipc_unsupported;
        slog(svc, "SVC_IPC_DOMAIN_UNSUPPORTED handle=0x%08x service=%s object=%u interface=%u command=%u\n",
                handle, session->name, object_id, interface, command);
        sflush(svc);
        return VP_BRIDGE_UNSUPPORTED;
    }
    if (session->kind == 1 && request_meta && !(request_meta & 0xfu)) {
        if (command == 0) {
            /* nn::sm::Initialize first sends the sm user-interface command with
             * no arguments; its generated client method expects only a
             * successful response header. */
            uint32_t response_meta = response_meta_for(buffer);
            if (!response_meta) { ++svc->ipc_unsupported; return VP_BRIDGE_UNSUPPORTED; }
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 8, 0);
            f->regs[0] = 0; /* SVC result; the veneer resumes with this in r0 */
            slog(svc, "SVC_IPC_RESPONSE service=%s command=%u result=0\n", session->name, command);
            sflush(svc);
            return 0;
        }
        if (command == 1) {
            /* nn::sm::GetService: the in-raw 8-byte name follows the request
             * CMIF header. The reply declares one out handle (word2 bits 5-8)
             * which the client reads from the HIPC handle array. */
            char name[9] = {0};
            for (unsigned i = 0; i < 8; ++i) name[i] = (char)*(const uint8_t *)(uintptr_t)(request_meta + 0x10 + i);
            uint32_t service = create_handle(svc, 2, name, 0);
            if (!service) {
                put32(request_meta, 0x4F434653u);
                put32(request_meta + 8, SVC_RESULT_OUT_OF_MEMORY);
                f->regs[0] = 0;
                slog(svc, "SVC_IPC_GET_SERVICE name=\"%s\" result=0x%08x\n", name, SVC_RESULT_OUT_OF_MEMORY);
                sflush(svc);
                return 0;
            }
            /* Response header: send_pid present (word2), one r4 out handle at
             * word 5 (after the 3-word header, 2-word lr array), CMIF reply at
             * word 6.  The reader aligns that meta to buffer+0x18. */
            uint32_t handle_offset = (3u + 2u) * 4u;
            uint32_t response_meta = (buffer + handle_offset + 4u + 0xfu) & ~0xfu;
            put32(buffer + 4, 0x8000000Au);
            put32(buffer + 8, 0x21u); /* lr array present, one out handle */
            put32(buffer + handle_offset, service);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 8, 0);
            f->regs[0] = 0;
            int audio_service = strcmp(name, "audren:u") == 0;
            int audio_owned = audio_service && vp_audio_ipc_thread_owned(
                svc->current_thread_handle, svc->audio_update_thread_handle);
            if (audio_service) {
                ++svc->audio_sm_getservice_count;
                if (!audio_owned) ++svc->audio_log_unowned_sm_getservice;
            }
            if (!audio_service || !audio_owned
                || vp_audio_log_cadence_should_emit(
                       &svc->audio_sm_getservice_log, 4u, 64u)) {
                slog(svc, "SVC_IPC_GET_SERVICE name=\"%s\" handle=0x%08x handle_word=%u meta=0x%08x result=0 owner=%s count=%u log=%u/%u\n",
                        name, service, handle_offset / 4, response_meta,
                        audio_owned ? "renderer-thread"
                            : audio_service ? "unowned" : "n/a",
                        audio_service ? svc->audio_sm_getservice_count : 0u,
                        svc->audio_sm_getservice_log.emitted,
                        svc->audio_sm_getservice_log.seen);
                sflush(svc);
            }
            return 0;
        }
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "apm") && command == 0) {
        /* nn::apm IApmService command 0 OpenSession: one out session handle
         * to an ISession, reached as a native move handle (the same HIPC
         * reply shape as sm:GetService and hid CreateAppletResource). The
         * session carries the loader's modeled performance state. */
        uint32_t apm_session = create_handle(svc, 2, "apm-ses", 0);
        if (!apm_session) {
            uint32_t error_meta = (buffer + (3u + 2u) * 4u + 4u + 0xfu) & ~0xfu;
            put32(buffer, 0);
            put32(buffer + 4, 0x8000000Au);
            put32(buffer + 8, 0x21u);
            put32(buffer + 0xc, 0);
            put32(buffer + 0x10, 0);
            put32(buffer + 0x14, 0);
            put32(error_meta, 0x4F434653u);
            put32(error_meta + 8, SVC_RESULT_OUT_OF_MEMORY);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_APM_SESSION_LIMIT\n");
            sflush(svc);
            return 0;
        }
        uint32_t handle_offset = (3u + 2u) * 4u;
        uint32_t response_meta = (buffer + handle_offset + 4u + 0xfu) & ~0xfu;
        put32(buffer, 0);
        put32(buffer + 4, 0x8000000Au);
        put32(buffer + 8, 0x21u); /* Pid word, one move handle */
        put32(buffer + 0xc, 0);
        put32(buffer + 0x10, 0);
        put32(buffer + handle_offset, apm_session);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_APM command=0 call=OpenSession session=0x%08x meta=0x%08x result=0\n",
                apm_session, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "apm-ses")) {
        /* nn::apm ISession. The loader has no clock or thermal control, so
         * SetPerformanceConfiguration records the requested configuration and
         * the getter returns it; the reference controller's own clock hook is
         * a no-op. SetCpuOverclockEnabled is the same: the flag is recorded
         * and changes nothing because no clock is controlled here. */
        uint32_t mode = get32(request_meta + 0x10);
        if (command == 0 || command == 1) {
            if (mode > 1u) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_APM_INVALID_MODE command=%u mode=%d\n",
                        command, (int32_t)mode);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            if (command == 0) {
                uint32_t config = get32(request_meta + 0x14);
                if (mode == 0) state->apm_config_normal = config;
                else state->apm_config_boost = config;
                uint32_t response_meta = plain_response_meta(buffer);
                put32(buffer, 0);
                put32(buffer + 4, 4);
                put32(response_meta, 0x4F434653u);
                put32(response_meta + 4, 0);
                put32(response_meta + 8, 0);
                put32(response_meta + 0xc, 0);
                f->regs[0] = 0;
                slog(svc, "SVC_IPC_APM command=0 call=SetPerformanceConfiguration mode=%u config=0x%08x model=no-clock-control result=0\n",
                        mode, config);
                sflush(svc);
                return 0;
            }
            uint32_t config = mode == 1 ? state->apm_config_boost : state->apm_config_normal;
            /* PerformanceConfiguration::Config7 is the reference default. */
            if (!config) config = 0x00020003u;
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, 5);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            put32(response_meta + 0x10, config);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_APM command=1 call=GetPerformanceConfiguration mode=%u config=0x%08x model=recorded-or-default result=0\n",
                    mode, config);
            sflush(svc);
            return 0;
        }
        if (command == 2) {
            uint32_t enabled = get32(request_meta + 0x10) & 1u;
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, 4);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_APM command=2 call=SetCpuOverclockEnabled enabled=%u model=no-clock-control result=0\n",
                    enabled);
            sflush(svc);
            return 0;
        }
        ++svc->ipc_unsupported;
        slog(svc, "SVC_IPC_APM_UNSUPPORTED command=%u\n", command);
        sflush(svc);
        return VP_BRIDGE_UNSUPPORTED;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid") && command == 0) {
        /* nn::hid IHidServer command 0 CreateAppletResource(u64 aruid). The
         * locked proxy is stateless (MessageType 4), so the returned
         * IAppletResource reaches the client as a native copy handle to a new
         * server-side sub-session, not as a domain object. The loader creates
         * that second named session and returns its handle with the reviewed
         * one-copy-handle HIPC reply already used by GetService and
         * CloneCurrentObject. The resource's own commands are modeled
         * separately as they appear; an unknown one stops the run. */
        uint64_t aruid = (uint64_t)get32(request_meta + 0x10)
            | ((uint64_t)get32(request_meta + 0x14) << 32);
        uint32_t resource = create_handle(svc, 2, "hid-res", 0);
        if (!resource) {
            put32(request_meta, 0x4F434653u);
            put32(request_meta + 8, SVC_RESULT_OUT_OF_MEMORY);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_HID_RESOURCE_LIMIT aruid=0x%llx\n",
                    (unsigned long long)aruid);
            sflush(svc);
            return 0;
        }
        uint32_t handle_offset = (3u + 2u) * 4u;
        uint32_t response_meta = (buffer + handle_offset + 4u + 0xfu) & ~0xfu;
        put32(buffer, 0);
        put32(buffer + 4, 0x8000000Au);
        put32(buffer + 8, 0x21u); /* Pid word, one copy handle */
        put32(buffer + 0xc, 0);
        put32(buffer + 0x10, 0);
        put32(buffer + handle_offset, resource);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        {
            VPGuestSession *resource_state = handle_session(svc, find_handle(svc, resource));
            if (resource_state) resource_state->hid_aruid = aruid;
        }
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_HID command=0 call=CreateAppletResource aruid=0x%llx resource=0x%08x meta=0x%08x result=0\n",
                (unsigned long long)aruid, resource, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid") && command == 100u) {
        /* IHidServer SetSupportedNpadStyleSet(NpadStyleSet, u32 pad, u64
         * aruid): the client declares which controller styles it accepts.
         * Recording the request is the whole server-side effect here; no
         * controller is attached, so nothing becomes visible in the hid
         * shared memory. */
        uint32_t style_set = get32(request_meta + 0x10);
        uint64_t aruid = (uint64_t)get32(request_meta + 0x18)
            | ((uint64_t)get32(request_meta + 0x1c) << 32);
        uint32_t response_meta = response_meta_for(buffer);
        if (!response_meta) { ++svc->ipc_unsupported; return VP_BRIDGE_UNSUPPORTED; }
        put32(buffer, 0);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 8, 0);
        state->npad_style_aruid = aruid;
        state->npad_style_set = style_set;
        state->npad_style_set_valid = 1;
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_HID command=100 call=SetSupportedNpadStyleSet aruid=0x%llx style=0x%x model=no-controllers result=0 meta=0x%08x\n",
                (unsigned long long)aruid, style_set, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid") && command == 102u) {
        /* IHidServer SetSupportedNpadIdType(ClientAppletResourceUserId,
         * InArray<NpadIdType, BufferAttr_HipcPointer>): the raw payload is the
         * u64 aruid and the id list travels in the HIPC pointer buffer as an
         * X descriptor, not in the message. The reference server stores the
         * list per aruid as the input routing policy; with no controllers
         * attached nothing can be reported through it, so recording the exact
         * ids is the whole server-side effect and the zeroed hid shared
         * memory stays accurate. A malformed list or an id outside the
         * NpadIdType values stops the run instead of being approximated. */
        uint64_t aruid = (uint64_t)get32(request_meta + 0x10)
            | ((uint64_t)get32(request_meta + 0x14) << 32);
        VPGuestHipcLayout layout;
        hipc_layout(buffer, header0, header1, &layout);
        uint32_t list_size = 0, list_address = 0;
        hipc_read_buffer(&layout, 0, &list_size, &list_address);
        if (!list_size || (list_size & 3u)
            || list_size > VP_GUEST_NPAD_ID_LIMIT * 4u
            || !vp_guest_svc_readable(svc, list_address, list_size)) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_HID_IDTYPE_INVALID aruid=0x%llx size=%u address=0x%08x x=%u a=%u\n",
                    (unsigned long long)aruid, list_size, list_address,
                    layout.x_count, layout.a_count);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        uint32_t count = list_size / 4u;
        uint32_t ids[VP_GUEST_NPAD_ID_LIMIT];
        char id_text[96];
        id_text[0] = '\0';
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t id = get32(list_address + i * 4u);
            if (id > 7u && id != 0x10u && id != 0x20u) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_HID_IDTYPE_UNSUPPORTED_ID aruid=0x%llx index=%u id=0x%x\n",
                        (unsigned long long)aruid, i, id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            ids[i] = id;
            snprintf(id_text + strlen(id_text), sizeof(id_text) - strlen(id_text),
                     i ? ",0x%x" : "0x%x", id);
        }
        state->npad_id_aruid = aruid;
        state->npad_id_count = count;
        memcpy(state->npad_ids, ids, count * 4u);
        uint32_t response_meta = plain_response_meta(buffer);
        put32(buffer, 0);
        put32(buffer + 4, 4); /* raw words: CMIF header only */
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_HID command=102 call=SetSupportedNpadIdType aruid=0x%llx count=%u ids=%s model=no-controllers result=0 meta=0x%08x\n",
                (unsigned long long)aruid, count, id_text, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid") && command == 120u) {
        /* IHidServer SetNpadJoyHoldType(NpadJoyHoldType hold_type, u64 aruid):
         * Vertical = 0, Horizontal = 1. The setting changes how a detached
         * Joy-Con pair would be reported; with no controllers attached the
         * request has no observable effect, so the exact value is recorded
         * and the zeroed hid shared memory stays accurate. Any other value
         * is the crash path in the reference server and is refused here. */
        uint32_t hold_type = get32(request_meta + 0x10);
        uint64_t aruid = (uint64_t)get32(request_meta + 0x18)
            | ((uint64_t)get32(request_meta + 0x1c) << 32);
        uint32_t response_meta = response_meta_for(buffer);
        if (hold_type > 1u) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_HID_JOYHOLD_INVALID aruid=0x%llx hold_type=%u\n",
                    (unsigned long long)aruid, hold_type);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        if (!response_meta) { ++svc->ipc_unsupported; return VP_BRIDGE_UNSUPPORTED; }
        put32(buffer, 0);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 8, 0);
        state->npad_joy_hold_aruid = aruid;
        state->npad_joy_hold_type = hold_type;
        state->npad_joy_hold_valid = 1;
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_HID command=120 call=SetNpadJoyHoldType aruid=0x%llx hold_type=%u model=no-controllers result=0 meta=0x%08x\n",
                (unsigned long long)aruid, hold_type, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid") && command == 121u) {
        /* IHidServer GetNpadJoyHoldType(u64 aruid) -> Out<s64>. The locked
         * client accepts only 0 (Vertical) or 1 (Horizontal) and aborts on
         * anything else. The server state is the value recorded by
         * SetNpadJoyHoldType; before that request the reference default is
         * Vertical. This request carries a special header, so the reply writes
         * its own plain HIPC header instead of inheriting the request's. */
        uint64_t aruid = (uint64_t)get32(request_meta + 0x10)
            | ((uint64_t)get32(request_meta + 0x14) << 32);
        uint64_t hold_type = state->npad_joy_hold_valid
            ? state->npad_joy_hold_type : 0u;
        uint32_t response_meta = plain_response_meta(buffer);
        put32(buffer, 0);
        put32(buffer + 4, 6); /* CMIF header plus the two-word s64 output */
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        put32(response_meta + 0x10, (uint32_t)hold_type);
        put32(response_meta + 0x14, (uint32_t)(hold_type >> 32));
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_HID command=121 call=GetNpadJoyHoldType aruid=0x%llx hold_type=%u model=recorded-or-default result=0 meta=0x%08x\n",
                (unsigned long long)aruid, (unsigned)hold_type, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid")
        && (command == 66u || command == 67u)) {
        /* IHidServer StartSixAxisSensor/StopSixAxisSensor(SixAxisSensorHandle,
         * u64 aruid). The handle is {u8 npad_style_index, u8 npad_id, u8
         * device_index, u8 pad}; the reference server validates the npad id
         * and device index, then flips the sensor's enabled flag without
         * requiring the controller to be connected. This platform has no
         * controllers, so the flag is recorded per handle and no samples are
         * produced into the zeroed hid shared memory. */
        uint32_t handle = get32(request_meta + 0x10);
        uint32_t npad_type = handle & 0xffu;
        uint32_t npad_id = (handle >> 8) & 0xffu;
        uint32_t device_index = (handle >> 16) & 0xffu;
        uint64_t aruid = (uint64_t)get32(request_meta + 0x18)
            | ((uint64_t)get32(request_meta + 0x1c) << 32);
        uint32_t response_meta = response_meta_for(buffer);
        if (!(npad_id <= 7u || npad_id == 0x10u || npad_id == 0x20u)
            || device_index >= 3u) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_HID_SIXAXIS_INVALID aruid=0x%llx handle=0x%08x npad_type=%u npad_id=0x%x device=%u\n",
                    (unsigned long long)aruid, handle, npad_type, npad_id,
                    device_index);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        if (!response_meta) { ++svc->ipc_unsupported; return VP_BRIDGE_UNSUPPORTED; }
        int32_t slot = -1;
        for (uint32_t i = 0; i < state->sixaxis_count; ++i)
            if (state->sixaxis[i].npad_type == npad_type
                && state->sixaxis[i].npad_id == npad_id
                && state->sixaxis[i].device_index == device_index
                && state->sixaxis[i].aruid == aruid)
                slot = (int32_t)i;
        if (slot < 0) {
            if (state->sixaxis_count >= VP_GUEST_SIXAXIS_LIMIT) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_HID_SIXAXIS_LIMIT aruid=0x%llx handle=0x%08x count=%u\n",
                        (unsigned long long)aruid, handle, state->sixaxis_count);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            slot = (int32_t)state->sixaxis_count++;
            state->sixaxis[slot].npad_type = npad_type;
            state->sixaxis[slot].npad_id = npad_id;
            state->sixaxis[slot].device_index = device_index;
            state->sixaxis[slot].aruid = aruid;
        }
        state->sixaxis[slot].enabled = command == 66u;
        put32(buffer, 0);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 8, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_HID command=%u call=%s aruid=0x%llx npad_type=%u npad_id=0x%x device=%u enabled=%u model=no-controllers result=0 meta=0x%08x\n",
                command, command == 66u ? "StartSixAxisSensor" : "StopSixAxisSensor",
                (unsigned long long)aruid, npad_type, npad_id, device_index,
                state->sixaxis[slot].enabled, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid") && command == 106u) {
        /* IHidServer AcquireNpadStyleSetUpdateEventHandle(OutCopyHandle<
         * KReadableEvent>, NpadIdType npad_id, u64 aruid, u64 unknown): the
         * reference creates one auto-clear readable event per {aruid, npad
         * id} on first use and signals it on every acquire, then returns it
         * as a copy handle. The event exists so the client can wait for
         * style-set changes; nothing here can change a style set (there are
         * no controllers), but the acquire-time signal is real and wakes a
         * wait exactly once, matching the reference. The aruid must be the
         * one that CreateAppletResource/ActivateNpad registered. */
        uint32_t npad_id = get32(request_meta + 0x10);
        uint64_t aruid = (uint64_t)get32(request_meta + 0x18)
            | ((uint64_t)get32(request_meta + 0x1c) << 32);
        uint64_t unknown = (uint64_t)get32(request_meta + 0x20)
            | ((uint64_t)get32(request_meta + 0x24) << 32);
        if (!(npad_id <= 7u || npad_id == 0x10u || npad_id == 0x20u)
            || !state->npad_activated || state->npad_aruid != aruid) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_HID_STYLE_EVENT_UNREVIEWED npad_id=0x%x aruid=0x%llx unknown=0x%llx activated=%u expected=0x%llx\n",
                    npad_id, (unsigned long long)aruid,
                    (unsigned long long)unknown, state->npad_activated,
                    (unsigned long long)state->npad_aruid);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        uint32_t event = 0;
        for (uint32_t i = 0; i < state->style_event_count; ++i)
            if (state->style_events[i].npad_id == npad_id
                && state->style_events[i].aruid == aruid)
                event = state->style_events[i].handle;
        if (!event) {
            if (state->style_event_count >= VP_GUEST_STYLE_EVENT_LIMIT) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_HID_STYLE_EVENT_LIMIT npad_id=0x%x aruid=0x%llx count=%u\n",
                        npad_id, (unsigned long long)aruid, state->style_event_count);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            event = create_handle(svc, 3, "hid-style", npad_id);
            if (!event) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_HID_STYLE_EVENT_HANDLES npad_id=0x%x\n", npad_id);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            state->style_events[state->style_event_count].aruid = aruid;
            state->style_events[state->style_event_count].npad_id = npad_id;
            state->style_events[state->style_event_count].handle = event;
            ++state->style_event_count;
        }
        VPGuestHandle *event_slot = find_handle(svc, event);
        if (event_slot) event_slot->signaled = 1;
        uint32_t handle_offset = (3u + 2u) * 4u;
        uint32_t response_meta = (buffer + handle_offset + 4u + 0xfu) & ~0xfu;
        put32(buffer, 0);
        put32(buffer + 4, 0x8000000Au); /* raw words, one handle */
        /* The locked 32-bit SF client reads this Out<NativeHandle> from the
         * move array (header2 bits 1-4), the same reader path that delivers
         * IAppletResource::GetSharedMemoryHandle. Declaring it as a copy
         * handle leaves the move array empty and the client attaches 0. */
        put32(buffer + 8, 0x03u);       /* Pid word, one move handle */
        put32(buffer + 0xc, 0);
        put32(buffer + 0x10, 0);
        put32(buffer + handle_offset, event);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_HID command=106 call=AcquireNpadStyleSetUpdateEventHandle npad_id=0x%x aruid=0x%llx unknown=0x%llx event=0x%08x signaled=1 meta=0x%08x result=0\n",
                npad_id, (unsigned long long)aruid, (unsigned long long)unknown,
                event, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid") && command == 200u) {
        /* IHidServer GetVibrationDeviceInfo(Out<VibrationDeviceInfo>,
         * VibrationDeviceHandle): the out value is the 8-byte raw struct
         * {u32 type, u32 position}. The reference derives the type from the
         * npad style index (Fullkey/Handheld/Joycon* -> linear resonant
         * actuator, GameCube -> GcErm, N64 -> N64, otherwise unknown) and the
         * position from the device index for the linear actuators. That is a
         * static property of the requested device handle, not of any attached
         * hardware, so the exact reference mapping is returned. */
        uint32_t handle = get32(request_meta + 0x10);
        uint32_t npad_type = handle & 0xffu;
        uint32_t npad_id = (handle >> 8) & 0xffu;
        uint32_t device_index = (handle >> 16) & 0xffu;
        uint32_t type = 0, position = 0;
        int style_ok = npad_type == 3u || (npad_type >= 4u && npad_type <= 8u)
            || npad_type == 13u || npad_type == 32u || npad_type == 33u;
        if (!style_ok || !(npad_id <= 7u || npad_id == 0x10u || npad_id == 0x20u)
            || device_index >= 3u) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_HID_VIBINFO_INVALID handle=0x%08x npad_type=%u npad_id=0x%x device=%u\n",
                    handle, npad_type, npad_id, device_index);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        if (npad_type >= 3u && npad_type <= 7u) {
            type = 1; /* LinearResonantActuator */
            position = device_index == 0u ? 1u
                : device_index == 1u ? 2u : 0u;
        } else if (npad_type == 8u) {
            type = 2; /* GcErm */
        } else if (npad_type == 13u) {
            type = 3; /* N64 */
        }
        uint32_t response_meta = plain_response_meta(buffer);
        put32(buffer, 0);
        put32(buffer + 4, 6); /* raw words: CMIF header + 8-byte info */
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        put32(response_meta + 0x10, type);
        put32(response_meta + 0x14, position);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_HID command=200 call=GetVibrationDeviceInfo handle=0x%08x npad_type=%u npad_id=0x%x device=%u type=%u position=%u result=0 meta=0x%08x\n",
                handle, npad_type, npad_id, device_index, type, position,
                response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid") && command == 206u) {
        /* IHidServer SendVibrationValues(ClientAppletResourceUserId,
         * InArray<VibrationDeviceHandle, BufferAttr_HipcPointer>,
         * InArray<VibrationValue, BufferAttr_HipcPointer>): the handles and
         * values travel as two X descriptors. The reference server validates
         * that the two arrays have matching counts, checks each handle against
         * the activated device list, and forwards the value to the actuator.
         * This platform has no haptic device: the validated request and its
         * values are recorded, nothing is actuated, and the call succeeds with
         * the same no-device effect the reference has when no controller is
         * attached. A malformed or unactivated handle is rejected instead of
         * being silently accepted. */
        VPGuestHipcLayout layout;
        hipc_layout(buffer, header0, header1, &layout);
        uint32_t handle_size = 0, handle_address = 0;
        uint32_t value_size = 0, value_address = 0;
        hipc_read_buffer(&layout, 0, &handle_size, &handle_address);
        hipc_read_buffer(&layout, 1, &value_size, &value_address);
        uint32_t count = handle_size / 4u;
        if (!handle_size || (handle_size & 3u) || !count || count > VP_GUEST_VIBRATION_LIMIT
            || value_size != count * 16u
            || !vp_guest_svc_readable(svc, handle_address, handle_size)
            || !vp_guest_svc_readable(svc, value_address, value_size)) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_HID_VIBRATE_INVALID handles=%u@0x%08x values=%u@0x%08x\n",
                    handle_size, handle_address, value_size, value_address);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        char handle_text[96];
        handle_text[0] = '\0';
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t handle = get32(handle_address + i * 4u);
            uint32_t npad_type = handle & 0xffu;
            uint32_t npad_id = (handle >> 8) & 0xffu;
            uint32_t device_index = (handle >> 16) & 0xffu;
            int active = 0;
            for (uint32_t v = 0; v < svc->vibration_count; ++v)
                if (svc->vibration[v].npad_type == npad_type
                    && svc->vibration[v].npad_id == npad_id
                    && svc->vibration[v].device_index == device_index)
                    active = 1;
            if (!active) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_HID_VIBRATE_INACTIVE handle=0x%08x npad_type=%u npad_id=0x%x device=%u\n",
                        handle, npad_type, npad_id, device_index);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            snprintf(handle_text + strlen(handle_text),
                     sizeof(handle_text) - strlen(handle_text),
                     i ? ",0x%08x" : "0x%08x", handle);
        }
        /* VibrationValue is {f32 amplitude_low, f32 frequency_low,
         * f32 amplitude_high, f32 frequency_high}. Record the first value in
         * the log so the request is evidence even though no actuator exists. */
        slog(svc, "SVC_IPC_HID command=206 call=SendVibrationValues count=%u handles=%s amplitude_bits=0x%08x/0x%08x frequency_bits=0x%08x/0x%08x model=no-haptic-device result=0\n",
                count, handle_text,
                get32(value_address), get32(value_address + 8u),
                get32(value_address + 4u), get32(value_address + 12u));
        uint32_t response_meta = plain_response_meta(buffer);
        put32(buffer, 0);
        put32(buffer + 4, 4);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid") && command == 203u) {
        /* IHidServer CreateActiveVibrationDeviceList: no input, one out
         * interface. On a stateless non-domain session the out object is a
         * native copy handle, exactly like CreateAppletResource. The returned
         * IActiveVibrationDeviceList is served by the hid-vib branch below;
         * this platform has no haptic hardware, so activating a device is
         * only a validated list append there. */
        uint32_t list = create_handle(svc, 2, "hid-vib", 0);
        if (!list) {
            put32(request_meta, 0x4F434653u);
            put32(request_meta + 8, SVC_RESULT_OUT_OF_MEMORY);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_HID_RESOURCE_LIMIT call=CreateActiveVibrationDeviceList\n");
            sflush(svc);
            return 0;
        }
        uint32_t handle_offset = (3u + 2u) * 4u;
        uint32_t response_meta = (buffer + handle_offset + 4u + 0xfu) & ~0xfu;
        put32(buffer, 0);
        put32(buffer + 4, 0x8000000Au); /* raw words, one copy handle */
        put32(buffer + 8, 0x21u);       /* Pid word, one copy handle */
        put32(buffer + 0xc, 0);
        put32(buffer + 0x10, 0);
        put32(buffer + handle_offset, list);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_HID command=203 call=CreateActiveVibrationDeviceList list=0x%08x meta=0x%08x result=0\n",
                list, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid-vib") && command == 0) {
        /* IActiveVibrationDeviceList command 0 ActivateVibrationDevice: one
         * 4-byte VibrationDeviceHandle {u8 npad_style_index, u8 npad_id, u8
         * device_index, u8 pad}. The reference validates the style, npad id
         * and device index, activates the device and appends it to the list
         * unless already present. No haptic device exists here, so the
         * validated list is the whole effect; an out-of-range handle stops
         * the run instead of being silently added. */
        uint32_t handle = get32(request_meta + 0x10);
        uint32_t npad_type = handle & 0xffu;
        uint32_t npad_id = (handle >> 8) & 0xffu;
        uint32_t device_index = (handle >> 16) & 0xffu;
        int style_ok = npad_type == 3u || (npad_type >= 4u && npad_type <= 8u)
            || npad_type == 13u || npad_type == 32u || npad_type == 33u;
        if (!style_ok || !(npad_id <= 7u || npad_id == 0x10u || npad_id == 0x20u)
            || device_index >= 3u) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_HID_VIBRATION_INVALID handle=0x%08x npad_type=%u npad_id=0x%x device=%u\n",
                    handle, npad_type, npad_id, device_index);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        int present = 0;
        for (uint32_t i = 0; i < svc->vibration_count; ++i)
            if (svc->vibration[i].npad_type == npad_type
                && svc->vibration[i].npad_id == npad_id
                && svc->vibration[i].device_index == device_index)
                present = 1;
        if (!present) {
            if (svc->vibration_count >= VP_GUEST_VIBRATION_LIMIT) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_HID_VIBRATION_LIMIT handle=0x%08x count=%u\n",
                        handle, svc->vibration_count);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            svc->vibration[svc->vibration_count].npad_type = npad_type;
            svc->vibration[svc->vibration_count].npad_id = npad_id;
            svc->vibration[svc->vibration_count].device_index = device_index;
            ++svc->vibration_count;
        }
        uint32_t response_meta = plain_response_meta(buffer);
        put32(buffer, 0);
        put32(buffer + 4, 4); /* raw words: CMIF header only */
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_HID command=0 call=ActivateVibrationDevice handle=0x%08x npad_type=%u npad_id=0x%x device=%u active=%u result=0 meta=0x%08x\n",
                handle, npad_type, npad_id, device_index, svc->vibration_count,
                response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid")
        && (command == 103u || command == 104u)) {
        /* IHidServer ActivateNpad/DeactivateNpad(ClientAppletResourceUserId).
         * The reference server activates the npad resources for the aruid;
         * this platform has no controllers, so activation succeeds and the
         * zeroed hid shared memory is left untouched: nothing is connected
         * to report. The call is recorded so the point where a future
         * Vita-input-to-npad model must take over is visible. */
        uint64_t aruid = (uint64_t)get32(request_meta + 0x10)
            | ((uint64_t)get32(request_meta + 0x14) << 32);
        uint32_t response_meta = response_meta_for(buffer);
        if (!response_meta) { ++svc->ipc_unsupported; return VP_BRIDGE_UNSUPPORTED; }
        put32(buffer, 0); /* response message, no descriptors */
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 8, 0);
        state->npad_activated = command == 103u;
        state->npad_aruid = aruid;
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_HID command=%u call=%s aruid=0x%llx model=no-controllers result=0 meta=0x%08x\n",
                command, command == 103u ? "ActivateNpad" : "DeactivateNpad",
                (unsigned long long)aruid, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid")
        && (command == 1u || command == 11u || command == 21u || command == 31u)) {
        /* IHidServer ActivateDebugPad/ActivateTouchScreen/ActivateMouse/
         * ActivateKeyboard(ClientAppletResourceUserId). The reference server
         * marks the device active for the aruid and starts reporting it
         * through the shared memory. No input source is wired to this
         * platform yet, so the device exists and is activated but never
         * produces state: the zeroed shared memory is its exact state. The
         * call is recorded so the point where Vita input must take over is
         * visible. */
        const char *call = command == 1u ? "ActivateDebugPad"
            : command == 11u ? "ActivateTouchScreen"
            : command == 21u ? "ActivateMouse" : "ActivateKeyboard";
        uint64_t aruid = (uint64_t)get32(request_meta + 0x10)
            | ((uint64_t)get32(request_meta + 0x14) << 32);
        uint32_t response_meta = response_meta_for(buffer);
        if (!response_meta) { ++svc->ipc_unsupported; return VP_BRIDGE_UNSUPPORTED; }
        put32(buffer, 0); /* response message, no descriptors */
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 8, 0);
        if (command == 11u) state->touch_screen_activated = 1;
        else if (command == 21u) state->mouse_activated = 1;
        else if (command == 31u) state->keyboard_activated = 1;
        else state->debug_pad_activated = 1;
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_HID command=%u call=%s aruid=0x%llx model=no-input-device result=0 meta=0x%08x\n",
                command, call, (unsigned long long)aruid, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "hid-res") && command == 0) {
        /* nn::hid IAppletResource command 0 GetSharedMemoryHandle. The
         * server's AppletResource owns one KSharedMemory per aruid, created
         * on demand with sizeof(hid::detail::SharedMemoryFormat) = 0x40000
         * bytes and zero-filled by the kernel; the client receives it as a
         * native copy handle and maps it read-only itself. No input devices
         * exist in this platform, so the zeroed object is its exact state:
         * nothing server-side ever writes it. The handle is created once and
         * returned again on repeated calls, matching the singleton object. */
        if (!state->hid_shared_handle) {
            state->hid_shared_handle = create_handle(svc, VP_GUEST_HANDLE_KIND_SHARED, "hid-shm", 0);
            if (!state->hid_shared_handle) {
                put32(request_meta, 0x4F434653u);
                put32(request_meta + 8, SVC_RESULT_OUT_OF_MEMORY);
                f->regs[0] = 0;
                slog(svc, "SVC_IPC_HID call=GetSharedMemoryHandle aruid=0x%llx result=0x%08x\n",
                        (unsigned long long)state->hid_aruid, SVC_RESULT_OUT_OF_MEMORY);
                sflush(svc);
                return 0;
            }
            VPGuestHandle *shared = find_handle(svc, state->hid_shared_handle);
            shared->shared_size = VP_GUEST_HID_SHARED_BYTES;
        }
        uint32_t handle_offset = (3u + 2u) * 4u;
        uint32_t response_meta = (buffer + handle_offset + 4u + 0xfu) & ~0xfu;
        put32(buffer, 0);
        put32(buffer + 4, 0x8000000Au);
        /* IAppletResource::GetSharedMemoryHandle declares one *move* handle:
         * the locked client reads the returned handle from the move array
         * (HipcMessageReader offset 0x34, header2 bits 1-4), unlike the
         * session-returning sm:GetService which uses the copy array. */
        put32(buffer + 8, 0x03u); /* Pid word, one move handle */
        put32(buffer + 0xc, 0);
        put32(buffer + 0x10, 0);
        put32(buffer + handle_offset, state->hid_shared_handle);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_HID call=GetSharedMemoryHandle aruid=0x%llx resource_handle=0x%08x shared=0x%08x size=0x%x model=zeroed-object meta=0x%08x result=0\n",
                (unsigned long long)state->hid_aruid, handle, state->hid_shared_handle,
                VP_GUEST_HID_SHARED_BYTES, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "lm") && command == 0) {
        /* nn::lm::Initialize opens the log service with a no-argument,
         * no-output command. Reply with the CMIF success header only. */
        uint32_t response_meta = response_meta_for(buffer);
        if (!response_meta) { ++svc->ipc_unsupported; return VP_BRIDGE_UNSUPPORTED; }
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 8, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_LM_INIT result=0 meta=0x%08x\n", response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && (state->vi_role == VP_GUEST_VI_RELAY || state->vi_role == VP_GUEST_VI_INDIRECT)
        && command == 3) {
        /* nns::hosbinder::IHOSBinderDriver command 3 TransactParcelAuto
         * (wrapper 0x1ef2cc): i32 binder id, u32 transaction code, the
         * serialized android Parcel in a send buffer and the caller's reply
         * Parcel buffer in a receive buffer. The transaction codes are the
         * shared IGraphicBufferProducer numbers, reviewed at the reference
         * BnGraphicBufferProducer::onTransact: 1 RequestBuffer, 3 DequeueBuffer,
         * 7 QueueBuffer, 8 CancelBuffer, 9 Query(int what), 10 Connect
         * (nullable IBinder listener, int api, bool producerControlledByApp),
         * 11 Disconnect(int api), 14 SetPreallocatedBuffer(int slot,
         * NvGraphicBuffer). This platform has no compositor, so the buffer
         * ring is modeled as slot bookkeeping over the preallocated nvmap
         * buffers: Query answers only values this model defines, dequeue hands
         * out a matching free slot with a no-fence, and queue/cancel move it
         * through the ring without displaying or signalling anything. Any
         * other producer operation stops the run instead of returning a
         * fabricated success. */
        uint32_t id = get32(request_meta + 0x10);
        uint32_t code = get32(request_meta + 0x14);
        VPGuestHipcLayout layout;
        hipc_layout(buffer, header0, header1, &layout);
        uint32_t send_count = layout.a_count;
        uint32_t receive_count = layout.b_count;
        uint32_t in_size, in_address, out_size, out_address;
        hipc_read_buffer(&layout, 0, &in_size, &in_address);
        hipc_write_buffer(&layout, 0, &out_size, &out_address);
        uint32_t result = 0;
        uint32_t reply_bytes = 0;
        uint32_t connect_api = 0, connect_controlled = 0;
        if (id != state->binder_handle) {
            result = SVC_RESULT_INVALID_HANDLE;
        } else if (code != 1u && code != 3u && code != 7u && code != 8u
                   && code != 9u && code != 10u && code != 11u && code != 14u) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_BINDER_UNSUPPORTED command=3 id=%u code=%u in=0x%x out=0x%x\n",
                    id, code, in_size, out_size);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        } else if (send_count != 1u || receive_count != 1u || !in_size
                   || in_size > 0x1000u || !out_size || out_size > 0x1000u
                   || !vp_guest_svc_readable(svc, in_address, in_size)
                   || !vp_guest_svc_writable(svc, out_address, out_size)) {
            result = SVC_RESULT_INVALID_COMBINATION;
        } else {
            /* The InBuffer starts with nn::vi's serialized Parcel header
             * {data size, data offset, objects size, objects offset}; the
             * data is android::Parcel::writeInterfaceToken output: the strict
             * mode policy word, then a String16 token, then the arguments. */
            uint32_t data_size = get32(in_address);
            uint32_t data_offset = get32(in_address + 4);
            uint32_t objects_size = get32(in_address + 8);
            uint32_t objects_offset = get32(in_address + 0xc);
            if (data_offset != 0x10u || data_size < 0x50u
                || data_size + objects_size + 0x10u > in_size
                || (objects_size && objects_offset + objects_size > in_size)) {
                result = SVC_RESULT_INVALID_COMBINATION;
            } else {
                uint32_t token_length = get32(in_address + data_offset + 4);
                static const char token[] = "android.gui.IGraphicBufferProducer";
                int match = token_length == sizeof(token) - 1;
                for (uint32_t i = 0; match && i < token_length; ++i) {
                    uint32_t c = get32(in_address + data_offset + 8 + i * 2) & 0xffffu;
                    if (c != (uint8_t)token[i]) match = 0;
                }
                uint32_t args = data_offset + 8u + ((5u + token_length * 2u) & ~3u);
                uint32_t arg_bytes =
                    code == 3u ? 20u
                    : code == 7u ? 4u + 84u
                    : code == 8u ? 4u + 8u + 36u
                    : code == 10u ? 12u
                    : code == 14u ? 4u + 0x178u /* slot, valid, size, buffer */
                    : 4u;
                if (!match || objects_size
                    || data_size < args - data_offset + arg_bytes) {
                    result = SVC_RESULT_INVALID_COMBINATION;
                } else if (code == 10u) {
                    uint32_t token_object = get32(in_address + args);
                    uint32_t api = get32(in_address + args + 4);
                    uint32_t controlled = get32(in_address + args + 8);
                    connect_api = api;
                    connect_controlled = controlled;
                    if (token_object != 0u || api < 1u || api > 4u
                        || controlled > 1u) {
                        result = SVC_RESULT_INVALID_COMBINATION;
                    } else {
                        /* QueueBufferOutput {width, height, transformHint,
                         * numPendingBuffers} followed by the connect status.
                         * The dimensions are the docked 1920x1080 the locked
                         * title renders to and match the modeled display. */
                        put32(out_address + 0x00, 0x14u); /* data size */
                        put32(out_address + 0x04, 0x10u); /* data offset */
                        put32(out_address + 0x08, 0u);    /* objects size */
                        put32(out_address + 0x0c, 0x24u); /* objects offset */
                        put32(out_address + 0x10, 1920u);
                        put32(out_address + 0x14, 1080u);
                        put32(out_address + 0x18, 0u);
                        put32(out_address + 0x1c, 0u);
                        put32(out_address + 0x20, 0u);
                        state->binder_connected = 1;
                        state->binder_api = api;
                        reply_bytes = 0x14u;
                    }
                } else if (code == 9u) {
                    /* Query(what): the NX NativeWindow enum from the
                     * reviewed BufferQueueProducer. Width/Height/Format and
                     * the queue counters are the values this platform can
                     * state; the rest have no model and stop the run. */
                    uint32_t what = get32(in_address + args);
                    const char *name = "unknown";
                    uint32_t value = 0;
                    int known = 1;
                    switch (what) {
                    case 0:
                        name = "Width";
                        value = state->binder_width ? state->binder_width : 1920u;
                        break;
                    case 1:
                        name = "Height";
                        value = state->binder_height ? state->binder_height : 1080u;
                        break;
                    case 2:
                        name = "Format";
                        value = state->binder_format ? state->binder_format : 1u;
                        break;
                    case 3: name = "MinUndequeuedBuffers"; value = 0; break;
                    case 9: name = "ConsumerRunningBehind"; value = 0; break;
                    case 10: name = "ConsumerUsageBits"; value = 0; break;
                    case 11: name = "StickyTransform"; value = 0; break;
                    default: known = 0; break;
                    }
                    if (!known) {
                        ++svc->ipc_unsupported;
                        slog(svc, "SVC_IPC_BINDER_QUERY_UNSUPPORTED id=%u what=%u\n",
                                id, what);
                        sflush(svc);
                        return VP_BRIDGE_UNSUPPORTED;
                    }
                    /* Query reply payload: the value word followed by the
                     * status word, matching the connect convention. */
                    put32(out_address + 0x00, 8u);    /* data size */
                    put32(out_address + 0x04, 0x10u); /* data offset */
                    put32(out_address + 0x08, 0u);    /* objects size */
                    put32(out_address + 0x0c, 0x18u); /* objects offset */
                    put32(out_address + 0x10, value);
                    put32(out_address + 0x14, 0u);
                    connect_api = what;
                    reply_bytes = 8u;
                    slog(svc, "SVC_IPC_BINDER command=3 call=query id=%u what=%s value=%u reply=%u result=0\n",
                            id, name, value, reply_bytes);
                } else if (code == 14u) {
                    /* SetPreallocatedBuffer(slot, NvGraphicBuffer): the guest
                     * hands the composer one nvmap-backed buffer per slot.
                     * The parcel encodes {i32 slot, bool valid, s64 flattened
                     * size, NvGraphicBuffer}; a valid buffer updates the
                     * producer's default geometry exactly like the reviewed
                     * composer. No pixels are displayed or copied: this is
                     * the slot bookkeeping a later display path must consume. */
                    int32_t slot = (int32_t)get32(in_address + args);
                    uint32_t valid = *(const uint8_t *)(uintptr_t)(in_address + args + 4);
                    uint64_t flattened = (uint64_t)get32(in_address + args + 8)
                        | ((uint64_t)get32(in_address + args + 0xc) << 32);
                    uint32_t status = 0;
                    if (slot < 0 || slot >= (int32_t)VP_GUEST_BINDER_SLOT_LIMIT) {
                        status = (uint32_t)-22; /* android BAD_VALUE */
                    } else if (valid && flattened != 0x16cu) {
                        status = (uint32_t)-22;
                    } else if (valid) {
                        uint32_t b = in_address + args + 0x10;
                        uint32_t width = get32(b + 0x04);
                        uint32_t height = get32(b + 0x08);
                        uint32_t stride = get32(b + 0x0c);
                        uint32_t format = get32(b + 0x10);
                        uint32_t usage = get32(b + 0x14);
                        uint32_t buffer_id = get32(b + 0x2c);
                        uint32_t handle = get32(b + 0x74);
                        uint32_t offset = get32(b + 0x78);
                        uint32_t i;
                        for (i = 0; i < state->binder_slot_count; ++i)
                            if (state->binder_slots[i].slot == slot) break;
                        if (i == state->binder_slot_count) {
                            if (state->binder_slot_count >= VP_GUEST_BINDER_SLOT_LIMIT) {
                                ++svc->ipc_unsupported;
                                slog(svc, "SVC_IPC_BINDER_SLOT_LIMIT id=%u count=%u\n",
                                        id, state->binder_slot_count);
                                sflush(svc);
                                return VP_BRIDGE_UNSUPPORTED;
                            }
                            ++state->binder_slot_count;
                        }
                        state->binder_slots[i].slot = slot;
                        state->binder_slots[i].width = width;
                        state->binder_slots[i].height = height;
                        state->binder_slots[i].stride = stride;
                        state->binder_slots[i].format = format;
                        state->binder_slots[i].usage = usage;
                        state->binder_slots[i].buffer_id = buffer_id;
                        state->binder_slots[i].handle = handle;
                        state->binder_slots[i].offset = offset;
                        for (uint32_t w = 0; w < 0x16cu / 4u; ++w)
                            state->binder_slots[i].buffer[w] = get32(b + w * 4u);
                        state->binder_slots[i].has_buffer = 1;
                        state->binder_slots[i].state = 0;
                        {
                            uint64_t gva = binder_slot_gpu_va(svc, state, i);
                            slog(svc, "VP_DISPLAY_BRIDGE_SLOT slot=%d handle=%u offset=0x%x gpu_va=0x%llx valid=%u\n",
                                 slot, handle, offset, (unsigned long long)gva,
                                 state->binder_slots[i].gpu_va_valid);
                            if (display_bridge_enabled) {
                                /* The same nvmap allocation can be mapped more
                                 * than once in the GPU address space. Dump
                                 * every candidate so a wrong pick is visible
                                 * instead of guessed. */
                                for (unsigned si = 0; si < VP_GUEST_SESSION_LIMIT; ++si) {
                                    VPGuestSession *cand = &svc->sessions[si];
                                    VPGuestNvMapHandle *nh = nvmap_find(cand, handle);
                                    if (nh != NULL)
                                        slog(svc, "VP_DISPLAY_BRIDGE_NVMAP slot=%d sess=%u addr=0x%x size=0x%x allocated=%u\n",
                                             slot, si, nh->address, nh->size, nh->allocated);
                                    for (uint32_t mi = 0; mi < cand->gpu_as.mapping_count; ++mi) {
                                        if (cand->gpu_as.mappings[mi].handle != handle) continue;
                                        slog(svc, "VP_DISPLAY_BRIDGE_MAP slot=%d sess=%u m=%u dev=0x%x gpu=0x%llx size=0x%llx fixed=%u\n",
                                             slot, si, mi,
                                             cand->gpu_as.mappings[mi].device_address,
                                             (unsigned long long)cand->gpu_as.mappings[mi].offset,
                                             (unsigned long long)cand->gpu_as.mappings[mi].size,
                                             cand->gpu_as.mappings[mi].fixed);
                                    }
                                }
                            }
                        }
                        if (width && height && format) {
                            state->binder_width = width;
                            state->binder_height = height;
                            state->binder_format = format;
                        }
                        slog(svc, "SVC_IPC_BINDER command=3 call=set-preallocated-buffer id=%u slot=%d width=%u height=%u stride=%u format=%u usage=0x%x buffer_id=%u handle=%u offset=0x%x slots=%u result=0\n",
                                id, slot, width, height, stride, format, usage,
                                buffer_id, handle, offset, state->binder_slot_count);
                    } else {
                        slog(svc, "SVC_IPC_BINDER command=3 call=set-preallocated-buffer id=%u slot=%d empty result=0\n",
                                id, slot);
                    }
                    put32(out_address + 0x00, 4u);    /* data size */
                    put32(out_address + 0x04, 0x10u); /* data offset */
                    put32(out_address + 0x08, 0u);    /* objects size */
                    put32(out_address + 0x0c, 0x14u); /* objects offset */
                    put32(out_address + 0x10, status);
                    connect_api = (uint32_t)slot;
                    reply_bytes = 4u;
                } else if (code == 1u) {
                    /* RequestBuffer(slot): return the NvGraphicBuffer the
                     * guest preallocated for that slot. The image is the
                     * guest's own struct, recorded verbatim. */
                    uint32_t slot = get32(in_address + args);
                    uint32_t status = 0;
                    uint32_t i;
                    for (i = 0; i < state->binder_slot_count; ++i)
                        if ((uint32_t)state->binder_slots[i].slot == slot) break;
                    if (i == state->binder_slot_count
                        || !state->binder_slots[i].has_buffer) {
                        status = (uint32_t)-22; /* android BAD_VALUE */
                    } else {
                        put32(out_address + 0x10, 1u);       /* object present */
                        put32(out_address + 0x14, 0x16cu);   /* flattened size */
                        put32(out_address + 0x18, 0u);
                        for (uint32_t w = 0; w < 0x16cu / 4u; ++w)
                            put32(out_address + 0x1c + w * 4u,
                                  state->binder_slots[i].buffer[w]);
                        reply_bytes = 4u + 8u + 0x16cu;
                        slog(svc, "SVC_IPC_BINDER command=3 call=request-buffer id=%u slot=%u buffer_id=%u result=0\n",
                                id, slot, state->binder_slots[i].buffer_id);
                    }
                    put32(out_address + 0x00,
                          status ? 4u : 4u + 8u + 0x16cu);
                    put32(out_address + 0x04, 0x10u);
                    put32(out_address + 0x08, 0u);
                    put32(out_address + 0x0c, 0x10u + reply_bytes);
                    if (status) {
                        put32(out_address + 0x10, status);
                        reply_bytes = 4u;
                    }
                    connect_api = slot;
                } else if (code == 3u) {
                    /* DequeueBuffer(async, width, height, format, usage): hand
                     * the guest a free preallocated slot and a no-fence. The
                     * producer ring is modeled honestly: a slot is only given
                     * out when a preallocated buffer matches the requested
                     * geometry; a mismatch would require allocating a new
                     * nvmap buffer, which this loader does not do. */
                    uint32_t async = get32(in_address + args);
                    uint32_t width = get32(in_address + args + 4);
                    uint32_t height = get32(in_address + args + 8);
                    uint32_t format = get32(in_address + args + 0x0c);
                    uint32_t usage = get32(in_address + args + 0x10);
                    uint32_t i, chosen = VP_GUEST_BINDER_SLOT_LIMIT;
                    for (i = 0; i < state->binder_slot_count; ++i) {
                        if (!state->binder_slots[i].has_buffer) continue;
                        if (state->binder_slots[i].state != 0u) continue;
                        if (width && state->binder_slots[i].width != width) continue;
                        if (height && state->binder_slots[i].height != height) continue;
                        if (format && state->binder_slots[i].format != format) continue;
                        if (usage & ~state->binder_slots[i].usage) continue;
                        chosen = i;
                        break;
                    }
                    if (chosen == VP_GUEST_BINDER_SLOT_LIMIT) {
                        /* The producer has queued every slot and now asks for
                         * another buffer. On the target this DequeueBuffer
                         * blocks until the display consumer presents the
                         * oldest queued buffer and releases it. No display
                         * hardware exists here, so the consumer model releases
                         * the oldest matching queued slot at this point. No
                         * pixels are scanned out and no vsync timing is
                         * claimed; only the ring's release side is modeled. */
                        uint32_t oldest = VP_GUEST_BINDER_SLOT_LIMIT;
                        for (i = 0; i < state->binder_slot_count; ++i) {
                            if (!state->binder_slots[i].has_buffer) continue;
                            if (state->binder_slots[i].state != 2u) continue;
                            if (width && state->binder_slots[i].width != width) continue;
                            if (height && state->binder_slots[i].height != height) continue;
                            if (format && state->binder_slots[i].format != format) continue;
                            if (usage & ~state->binder_slots[i].usage) continue;
                            if (oldest == VP_GUEST_BINDER_SLOT_LIMIT
                                || state->binder_slots[i].queue_seq
                                   < state->binder_slots[oldest].queue_seq)
                                oldest = i;
                        }
                        if (oldest != VP_GUEST_BINDER_SLOT_LIMIT) {
                            display_bridge_init(svc);
                            if (display_bridge_enabled) {
                                uint64_t gva = binder_slot_gpu_va(svc, state, oldest);
                                ++display_present_attempts;
                                if (gva == 0u) {
                                    ++display_present_miss_va;
                                    if (display_present_miss_va <= 16u)
                                        slog(svc, "VP_DISPLAY_BRIDGE_MISS reason=no-va slot=%d handle=%u offset=0x%x\n",
                                             state->binder_slots[oldest].slot,
                                             state->binder_slots[oldest].handle,
                                             state->binder_slots[oldest].offset);
                                } else {
                                    int prc = guest_gxm_request(svc,3,NULL,gva);
                                    if (prc == 0) {
                                        ++display_present_ok;
                                    } else {
                                        ++display_present_miss_present;
                                        if (display_present_miss_present <= 16u)
                                            slog(svc, "VP_DISPLAY_BRIDGE_MISS reason=present va=0x%llx rc=%d\n",
                                                 (unsigned long long)gva, prc);
                                    }
                                }
                                if ((display_present_attempts % 512u) == 0u)
                                    slog(svc, "VP_DISPLAY_BRIDGE_SUMMARY attempts=%u ok=%u miss_va=%u miss_present=%u\n",
                                         display_present_attempts, display_present_ok,
                                         display_present_miss_va,
                                         display_present_miss_present);
                            }
                            state->binder_slots[oldest].state = 0u;
                            ++state->binder_presents;
                            chosen = oldest;
                            slog(svc, "SVC_IPC_BINDER_PRESENT id=%u slot=%d seq=%u presents=%u model=consumer-releases-oldest-queued\n",
                                    id, state->binder_slots[oldest].slot,
                                    state->binder_slots[oldest].queue_seq,
                                    state->binder_presents);
                        }
                    }
                    if (chosen == VP_GUEST_BINDER_SLOT_LIMIT) {
                        ++svc->ipc_unsupported;
                        slog(svc, "SVC_IPC_BINDER_DEQUEUE_UNSUPPORTED id=%u async=%u w=%u h=%u format=%u usage=0x%x slots=%u\n",
                                id, async, width, height, format, usage,
                                state->binder_slot_count);
                        sflush(svc);
                        return VP_BRIDGE_UNSUPPORTED;
                    }
                    if (!width) width = state->binder_slots[chosen].width;
                    if (!height) height = state->binder_slots[chosen].height;
                    if (!format) format = state->binder_slots[chosen].format;
                    state->binder_slots[chosen].state = 1u;
                    state->binder_slots[chosen].fence_id = (uint32_t)-1;
                    state->binder_slots[chosen].fence_value = 0;
                    ++state->binder_dequeues;
                    put32(out_address + 0x00, 0x34u); /* data size */
                    put32(out_address + 0x04, 0x10u);
                    put32(out_address + 0x08, 0u);
                    put32(out_address + 0x0c, 0x44u);
                    put32(out_address + 0x10, state->binder_slots[chosen].slot);
                    put32(out_address + 0x14, 1u);      /* fence object present */
                    put32(out_address + 0x18, 36u);     /* flattened Fence size */
                    put32(out_address + 0x1c, 0u);
                    put32(out_address + 0x20, 0u);      /* num_fences */
                    for (uint32_t f = 0; f < 4; ++f) {
                        put32(out_address + 0x24 + f * 8u, (uint32_t)-1);
                        put32(out_address + 0x28 + f * 8u, 0u);
                    }
                    connect_api = (uint32_t)state->binder_slots[chosen].slot;
                    reply_bytes = 0x34u;
                    slog(svc, "SVC_IPC_BINDER command=3 call=dequeue-buffer id=%u slot=%d w=%u h=%u format=%u usage=0x%x fence=none dequeues=%u result=0\n",
                            id, state->binder_slots[chosen].slot, width, height,
                            format, usage, state->binder_dequeues);
                } else if (code == 7u) {
                    /* QueueBuffer(slot, QueueBufferInput): the guest finished
                     * rendering the slot. The composer is modeled as a slot
                     * ring only; pixels and fences are recorded, nothing is
                     * displayed or signalled. */
                    uint32_t slot = get32(in_address + args);
                    uint32_t status = 0;
                    uint32_t fence_count = get32(in_address + args + 4u + 0x30u);
                    uint32_t fence_id = get32(in_address + args + 4u + 0x34u);
                    uint32_t fence_value = get32(in_address + args + 4u + 0x38u);
                    uint32_t i;
                    for (i = 0; i < state->binder_slot_count; ++i)
                        if ((uint32_t)state->binder_slots[i].slot == slot) break;
                    if (i == state->binder_slot_count
                        || state->binder_slots[i].state != 1u) {
                        status = (uint32_t)-22; /* BAD_VALUE: slot not dequeued */
                    } else {
                        state->binder_slots[i].state = 2u;
                        state->binder_slots[i].queue_seq = ++state->binder_queue_seq;
                        state->binder_slots[i].fence_id = fence_id;
                        state->binder_slots[i].fence_value = fence_value;
                        ++state->binder_queues;
                    }
                    put32(out_address + 0x00, 0x14u); /* output + status */
                    put32(out_address + 0x04, 0x10u);
                    put32(out_address + 0x08, 0u);
                    put32(out_address + 0x0c, 0x24u);
                    put32(out_address + 0x10, state->binder_width ? state->binder_width : 1920u);
                    put32(out_address + 0x14, state->binder_height ? state->binder_height : 1080u);
                    put32(out_address + 0x18, 0u); /* transform hint */
                    put32(out_address + 0x1c, state->binder_queues);
                    put32(out_address + 0x20, status);
                    connect_api = slot;
                    reply_bytes = 0x14u;
                    slog(svc, "SVC_IPC_BINDER command=3 call=queue-buffer id=%u slot=%u fences=%u fence=%u:%u queues=%u result=0 status=0x%08x\n",
                            id, slot, fence_count, fence_id, fence_value,
                            state->binder_queues, status);
                } else if (code == 8u) {
                    /* CancelBuffer(slot, fence): return the slot to the free
                     * ring without display. */
                    uint32_t slot = get32(in_address + args);
                    uint32_t status = 0;
                    uint32_t i;
                    for (i = 0; i < state->binder_slot_count; ++i)
                        if ((uint32_t)state->binder_slots[i].slot == slot) break;
                    if (i == state->binder_slot_count
                        || state->binder_slots[i].state == 0u) {
                        status = (uint32_t)-22;
                    } else {
                        state->binder_slots[i].state = 0u;
                    }
                    put32(out_address + 0x00, 4u);
                    put32(out_address + 0x04, 0x10u);
                    put32(out_address + 0x08, 0u);
                    put32(out_address + 0x0c, 0x14u);
                    put32(out_address + 0x10, status);
                    connect_api = slot;
                    reply_bytes = 4u;
                    slog(svc, "SVC_IPC_BINDER command=3 call=cancel-buffer id=%u slot=%u result=0 status=0x%08x\n",
                            id, slot, status);
                } else {
                    /* disconnect(api): detach the producer from the modeled
                     * display. The android Parcel reply carries the status_t;
                     * the loader clears the connect record only when the api
                     * matches the one that connected. */
                    uint32_t api = get32(in_address + args);
                    uint32_t status = 0;
                    connect_api = api;
                    if (!state->binder_connected || api != state->binder_api)
                        status = (uint32_t)-22; /* android BAD_VALUE */
                    else
                        state->binder_connected = 0;
                    put32(out_address + 0x00, 4u);    /* data size */
                    put32(out_address + 0x04, 0x10u); /* data offset */
                    put32(out_address + 0x08, 0u);    /* objects size */
                    put32(out_address + 0x0c, 0x14u); /* objects offset */
                    put32(out_address + 0x10, status);
                    reply_bytes = 4u;
                }
            }
        }
        uint32_t response_meta = (buffer + 8u + 0xfu) & ~0xfu;
        put32(buffer, 0);
        put32(buffer + 4, 4u); /* raw words: CMIF header only */
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, result);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_BINDER command=3 call=%s id=%u code=%u api=%u controlled=%u connected=%u reply=%u result=0x%08x meta=0x%08x\n",
                code == 1u ? "request-buffer"
                    : code == 3u ? "dequeue-buffer"
                    : code == 7u ? "queue-buffer"
                    : code == 8u ? "cancel-buffer"
                    : code == 9u ? "query"
                    : code == 10u ? "connect"
                    : code == 14u ? "set-preallocated-buffer" : "disconnect",
                id, code, connect_api, connect_controlled, state->binder_connected,
                reply_bytes, result, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && (state->vi_role == VP_GUEST_VI_RELAY || state->vi_role == VP_GUEST_VI_INDIRECT)
        && command == 1) {
        /* nns::hosbinder::IHOSBinderDriver command 1 AdjustRefcount (verified
         * at wrapper 0x1ef284): i32 binder id, i32 addend, i32 type. The
         * display service's binder handles are objects of this platform, so
         * the guest's strong/weak counts are tracked here and an unknown id
         * is rejected instead of silently accepted. */
        uint32_t id = get32(request_meta + 0x10);
        int32_t add = (int32_t)get32(request_meta + 0x14);
        uint32_t type = get32(request_meta + 0x18);
        uint32_t result = 0;
        if (id != state->binder_handle) {
            result = SVC_RESULT_INVALID_HANDLE;
        } else if (type == 0) {
            state->binder_strong_refs += add;
        } else if (type == 1) {
            state->binder_weak_refs += add;
        } else {
            result = SVC_RESULT_INVALID_ENUM;
        }
        uint32_t response_meta = (buffer + 8u + 0xfu) & ~0xfu;
        put32(buffer, header0);
        put32(buffer + 4, 4u); /* raw words: CMIF header only */
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, result);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_BINDER command=1 call=AdjustRefcount id=%u add=%d type=%u strong=%d weak=%d result=0x%08x meta=0x%08x\n",
                id, add, type, state->binder_strong_refs, state->binder_weak_refs, result, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && (state->vi_role == VP_GUEST_VI_RELAY || state->vi_role == VP_GUEST_VI_INDIRECT)
        && command == 2) {
        /* nns::hosbinder::IHOSBinderDriver command 2 GetNativeHandle
         * (verified at wrapper 0x1ef2a8): i32 binder id, u32 type, one out
         * NativeHandle. The locked Surface constructor asks the display
         * producer for type 15 and keeps the returned kernel object; this
         * platform models it as an auto-clear event owned by the guest, with
         * no signal source until a display flip exists. Unknown ids and
         * unreviewed handle types are rejected rather than fabricated. */
        uint32_t id = get32(request_meta + 0x10);
        uint32_t type = get32(request_meta + 0x14);
        uint32_t result = 0;
        uint32_t handle = 0;
        if (id != state->binder_handle) {
            result = SVC_RESULT_INVALID_HANDLE;
        } else if (type != 15u) {
            result = SVC_RESULT_INVALID_ENUM;
        } else {
            handle = create_handle(svc, 3, "dispdrv", 0);
            if (!handle) result = SVC_RESULT_OUT_OF_MEMORY;
        }
        uint32_t response_meta = (buffer + 0x18u + 0xfu) & ~0xfu;
        put32(buffer, 0);
        put32(buffer + 4, 0x80000000u | ((response_meta + 0x10u - (buffer + 8u)) / 4u));
        put32(buffer + 8, handle ? 0x21u : 0x1u); /* Pid, one move handle */
        put32(buffer + 0xc, 0);
        put32(buffer + 0x10, 0);
        if (handle) put32(buffer + 0x14, handle);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, result);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_BINDER command=2 call=GetNativeHandle id=%u type=%u handle=0x%08x result=0x%08x meta=0x%08x model=guest-event\n",
                id, type, handle, result, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && state->vi_role == VP_GUEST_VI_APPLICATION && command == 2020) {
        /* IApplicationDisplayService command 2020 OpenLayer: inputs are the
         * display name, the u64 LayerId the applet side returned from
         * CreateManagedDisplayLayer, and the AppletResourceUserId; the one
         * receive-map buffer holds the serialized android NativeWindow
         * Parcel and the out raw u64 is its byte size. This host has no
         * compositor, but the Parcel names the "dispdrv" binder service that
         * nn::vi::Initialize registered over the relay IHOSBinderDriver, so
         * the guest's later producer transactions reach this service layer
         * with a real transport rather than a fabricated local object. */
        uint32_t descriptor = 8;
        if (header1 & 0x80000000u) {
            uint32_t special = get32(buffer + 8);
            descriptor += 4;
            if (special & 1u) descriptor += 8;
            descriptor += ((special >> 1) & 0xfu) * 4u;
            descriptor += ((special >> 5) & 0xfu) * 4u;
        }
        descriptor += ((header0 >> 16) & 0xfu) * 8u; /* pointer descriptors */
        descriptor += ((header0 >> 20) & 0xfu) * 4u; /* send descriptors */
        uint32_t capacity = get32(buffer + descriptor);
        uint32_t address = get32(buffer + descriptor + 4);
        char name[0x41];
        memcpy(name, (const void *)(uintptr_t)(request_meta + 0x10), 0x40);
        name[0x40] = 0;
        uint64_t layer = (uint64_t)get32(request_meta + 0x50)
            | ((uint64_t)get32(request_meta + 0x54) << 32);
        uint64_t aruid = (uint64_t)get32(request_meta + 0x58)
            | ((uint64_t)get32(request_meta + 0x5c) << 32);
        uint32_t result = 0;
        if (((header0 >> 24) & 0xfu) != 1 || !capacity || capacity < 0x3c
            || strcmp(name, "Default") || !layer || layer > svc->managed_layers
            || !vp_guest_svc_writable(svc, address, 0x3c)) {
            result = SVC_RESULT_INVALID_COMBINATION;
        } else {
            /* android::Parcel::ipcSetDataReference layout used by
             * nn::vi::native::ParcelIo::OpenParcel: a 0x10-byte header with
             * data size/offset and objects size/offset. This libbinder's
             * Parcel::readStrongBinder feeds the current data position
             * straight to unflatten_binder, so the data starts with one
             * 0x28-byte flat object whose Switch extension carries the C
             * service name used by HOSServiceManager; the objects array
             * holds the object's offset within the data. */
            uint8_t *parcel = (uint8_t *)(uintptr_t)address;
            memset(parcel, 0, 0x3c);
            put32(address + 0x00, 0x28u); /* data size */
            put32(address + 0x04, 0x10u); /* data offset */
            put32(address + 0x08, 4u);    /* objects size in bytes */
            put32(address + 0x0c, 0x38u); /* objects offset */
            put32(address + 0x10, 2u);    /* flat object: HANDLE */
            put32(address + 0x18, 1u);    /* binder handle passed to TransactParcel */
            memcpy(parcel + 0x28, "dispdrv", 8);
            put32(address + 0x38, 0u);    /* object offset inside data */
            state->layer_opened = 1;
            state->layer_id = layer;
        }
        uint32_t raw_start = descriptor + 12u;
        uint32_t response_meta = (buffer + raw_start + 0xfu) & ~0xfu;
        put32(buffer, header0);
        put32(buffer + 4, 0x80000000u | ((response_meta + 0x18u - (buffer + raw_start)) / 4u));
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, result);
        put32(response_meta + 0xc, 0);
        put32(response_meta + 0x10, result ? 0u : 0x3cu);
        put32(response_meta + 0x14, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_VI command=2020 call=OpenLayer name=\"%s\" layer=0x%llx aruid=0x%llx buffer=0x%08x capacity=%u parcel_size=%u result=0x%08x meta=0x%08x\n",
                name, (unsigned long long)layer, (unsigned long long)aruid,
                address, capacity, result ? 0u : 0x3cu, result, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && state->vi_role == VP_GUEST_VI_APPLICATION && command == 2101) {
        /* IApplicationDisplayService command 2101 SetLayerScalingMode
         * (wrapper 0x1eef1c, args u32 ScalingMode at raw offset 0 and u64
         * LayerId at raw offset 8). The host has no compositor, so this
         * records the requested mode for the layer the guest opened and
         * acknowledges the call; no pixels are scaled or presented. */
        uint32_t mode = get32(request_meta + 0x10);
        uint64_t layer = (uint64_t)get32(request_meta + 0x18)
            | ((uint64_t)get32(request_meta + 0x1c) << 32);
        uint32_t result = 0;
        if (!state->layer_opened || layer != state->layer_id) {
            result = SVC_RESULT_INVALID_COMBINATION;
        } else {
            state->layer_scaling_mode = mode;
        }
        put32(buffer, 0);
        put32(buffer + 4, (request_meta - (buffer + 8u)) / 4u + 4u);
        put32(request_meta, 0x4F434653u);
        put32(request_meta + 4, 0);
        put32(request_meta + 8, result);
        put32(request_meta + 0xc, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_VI command=2101 call=SetLayerScalingMode layer=0x%llx opened=%u mode=%u result=0x%08x meta=0x%08x\n",
                (unsigned long long)layer, state->layer_opened, mode, result, request_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && state->vi_role == VP_GUEST_VI_APPLICATION && command == 2460) {
        /* IApplicationDisplayService command 2460
         * GetIndirectLayerImageRequiredMemoryInfo(width,height): out two s64.
         * The reference sizes an RGBA8 image aligned up to 0x20000 and reports
         * 0x1000 alignment; the SDK allocates that much guest memory for the
         * software keyboard's indirect layer, so the arithmetic is the
         * reference's, not a placeholder. */
        int64_t width = (int64_t)((uint64_t)get32(request_meta + 0x10)
            | ((uint64_t)get32(request_meta + 0x14) << 32));
        int64_t height = (int64_t)((uint64_t)get32(request_meta + 0x18)
            | ((uint64_t)get32(request_meta + 0x1c) << 32));
        uint32_t result = 0;
        uint64_t size = 0, alignment = 0x1000;
        if (width <= 0 || height <= 0 || width > 0x4000 || height > 0x4000) {
            result = SVC_RESULT_INVALID_COMBINATION;
        } else {
            uint64_t texture_size = (uint64_t)width * (uint64_t)height * 4u;
            size = (texture_size + 0x20000u - 1u) / 0x20000u * 0x20000u;
        }
        put32(buffer, 0);
        put32(buffer + 4, (request_meta - (buffer + 8u)) / 4u + 4u + 4u);
        put32(request_meta, 0x4F434653u);
        put32(request_meta + 4, 0);
        put32(request_meta + 8, result);
        put32(request_meta + 0xc, 0);
        put32(request_meta + 0x10, (uint32_t)size);
        put32(request_meta + 0x14, (uint32_t)(size >> 32));
        put32(request_meta + 0x18, (uint32_t)alignment);
        put32(request_meta + 0x1c, (uint32_t)(alignment >> 32));
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_VI command=2460 call=GetIndirectLayerImageRequiredMemoryInfo width=%lld height=%lld size=0x%llx alignment=0x%llx result=0x%08x meta=0x%08x\n",
                (long long)width, (long long)height, (unsigned long long)size,
                (unsigned long long)alignment, result, request_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && state->vi_role == VP_GUEST_VI_APPLICATION && command == 2450) {
        /* IApplicationDisplayService command 2450 GetIndirectLayerImageMap
         * (raw: width s64, height s64, consumer handle u64, aruid u64; out
         * size s64, out stride s64; one mapped-buffer descriptor carries the
         * image destination). The caller wants the image rendered by a
         * library applet's indirect layer -- in this title, the inline
         * software keyboard. This loader is the applet's frontend: while the
         * keyboard is shown it draws the keyboard state into the mapped RGBA8
         * image (linear, stride*height). When no keyboard is active the buffer
         * is the defined empty frame, and the log says which of the two was
         * produced so no screenshot can be misattributed. */
        int64_t width = (int64_t)get64(request_meta + 0x10);
        int64_t height = (int64_t)get64(request_meta + 0x18);
        uint64_t consumer = get64(request_meta + 0x20);
        uint64_t aruid = get64(request_meta + 0x28);
        VPGuestHipcLayout layout;
        uint32_t buffer_size = 0, buffer_address = 0;
        uint64_t size = 0, stride = 0;
        uint32_t result = 0;
        VPGuestSession *keyboard = NULL;
        hipc_layout(buffer, header0, header1, &layout);
        hipc_write_buffer(&layout, 0, &buffer_size, &buffer_address);
        if (width <= 0 || height <= 0 || width > 0x4000 || height > 0x4000) {
            result = SVC_RESULT_INVALID_COMBINATION;
        } else {
            stride = (uint64_t)width * 4u;
            size = stride * (uint64_t)height;
            if (!buffer_address || buffer_size < size || size > 0x10000000u
                || !vp_guest_svc_writable(svc, buffer_address, (uint32_t)size)) {
                result = SVC_RESULT_INVALID_COMBINATION;
            } else if ((keyboard = swkbd_active_session(svc)) != NULL) {
                /* The inline keyboard is the frontend this loader provides;
                 * draw its current state into the layer image. Re-render only
                 * when an input/state change or a different image is mapped. */
                if (keyboard->swkbd.render_dirty
                    || keyboard->swkbd.render_addr != buffer_address
                    || keyboard->swkbd.render_width != (uint32_t)width
                    || keyboard->swkbd.render_height != (uint32_t)height) {
                    swkbd_render_image(keyboard, (void *)(uintptr_t)buffer_address,
                                       (uint32_t)width, (uint32_t)height,
                                       (uint32_t)stride);
                    keyboard->swkbd.render_addr = buffer_address;
                    keyboard->swkbd.render_width = (uint32_t)width;
                    keyboard->swkbd.render_height = (uint32_t)height;
                    keyboard->swkbd.render_dirty = 0;
                }
            } else {
                memset((void *)(uintptr_t)buffer_address, 0, (size_t)size);
            }
        }
        if (result) { size = 0; stride = 0; }
        /* The sf client derives the response CMIF header solely from the HIPC
         * header this server writes, not from the request's descriptor
         * layout. With header0=0 it reports raw_offset=2 words and looks for
         * SFCO at align16(buffer+8). Writing the reply at the request's own
         * raw offset (word 10 here, behind the special header and the
         * mapped-buffer descriptor) leaves SFCO where the client does not
         * look; its processor then returns 0x1a80a, which is constructed at
         * sdk+0x1f4338 when the word at the derived offset is not "SFCO".
         * The out payload is two s64 values after the 4-word CMIF header. */
        uint32_t response_meta = plain_response_meta(buffer);
        put32(buffer, 0);
        put32(buffer + 4, 8);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, result);
        put32(response_meta + 0xc, 0);
        put32(response_meta + 0x10, (uint32_t)size);
        put32(response_meta + 0x14, (uint32_t)(size >> 32));
        put32(response_meta + 0x18, (uint32_t)stride);
        put32(response_meta + 0x1c, (uint32_t)(stride >> 32));
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_VI command=2450 call=GetIndirectLayerImageMap width=%lld height=%lld consumer=0x%llx aruid=0x%llx buffer=0x%08x capacity=%u size=0x%llx stride=0x%llx content=%s model=%s result=0x%08x meta=0x%08x request_meta=0x%08x\n",
                (long long)width, (long long)height, (unsigned long long)consumer,
                (unsigned long long)aruid, buffer_address, buffer_size,
                (unsigned long long)size, (unsigned long long)stride,
                keyboard ? "keyboard-image" : "zero-filled",
                keyboard ? "loader-keyboard" : "no-applet-render", result,
                response_meta, request_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && state->vi_role == VP_GUEST_VI_APPLICATION && command == 1010) {
        /* IApplicationDisplayService command 1010 OpenDisplay: input one
         * DisplayName, output u64 display id. The loader models exactly the
         * one "Default" display it reported from ListDisplays. */
        char name[0x41];
        memcpy(name, (const void *)(uintptr_t)(request_meta + 0x10), 0x40);
        name[0x40] = 0;
        uint32_t result = !strcmp(name, "Default") ? 0 : SVC_RESULT_INVALID_COMBINATION;
        put32(buffer, 0);
        put32(buffer + 4, (request_meta - (buffer + 8u)) / 4u + 4u + 2u);
        put32(request_meta, 0x4F434653u);
        put32(request_meta + 4, 0);
        put32(request_meta + 8, result);
        put32(request_meta + 0xc, 0);
        put32(request_meta + 0x10, 1); /* modeled Default display id */
        put32(request_meta + 0x14, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_VI command=1010 call=OpenDisplay name=\"%s\" display=1 result=0x%08x meta=0x%08x\n",
                name, result, request_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && state->vi_role == VP_GUEST_VI_APPLICATION && command == 1000) {
        /* IApplicationDisplayService command 1000 ListDisplays: one OutArray
         * of 0x60-byte DisplayInfo records plus the returned count. The
         * modeled target has the single "Default" display; its reported
         * maximum layer size is the docked 1920x1080 the locked title also
         * renders to in the reference captures. */
        uint32_t descriptor = (header0 & 0x01000000u) != 0;
        uint32_t capacity = descriptor ? get32(buffer + 8) / 0x60u : 0;
        uint32_t address = descriptor ? get32(buffer + 0xc) : 0;
        uint32_t result = 0;
        uint64_t count = 0;
        if (!descriptor || !capacity || !vp_guest_svc_writable(svc, address, 0x60)) {
            result = SVC_RESULT_INVALID_COMBINATION;
        } else {
            uint8_t *info = (uint8_t *)(uintptr_t)address;
            uint64_t values[3] = {1, 1920, 1080}; /* layer max, width, height */
            memset(info, 0, 0x60);
            memcpy(info, "Default", 8);
            info[0x40] = 1; /* HasLayerLimit */
            memcpy(info + 0x48, &values[0], 8);
            memcpy(info + 0x50, &values[1], 8);
            memcpy(info + 0x58, &values[2], 8);
            count = 1;
        }
        put32(buffer, descriptor ? 0x01000004u : 0u);
        put32(buffer + 4, (request_meta - (buffer + 8u)) / 4u + 4u + 2u);
        put32(request_meta, 0x4F434653u);
        put32(request_meta + 4, 0);
        put32(request_meta + 8, result);
        put32(request_meta + 0xc, 0);
        put32(request_meta + 0x10, (uint32_t)count);
        put32(request_meta + 0x14, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_VI command=1000 call=ListDisplays capacity=%u count=%u result=0x%08x meta=0x%08x\n",
                capacity, (unsigned)count, result, request_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && state->vi_role == VP_GUEST_VI_APPLICATION
        && command >= 0x64 && command <= 0x67) {
        /* IApplicationDisplayService commands 100..103 return the relay
         * (IHOSBinderDriver), system display, manager display and indirect
         * transaction services. Each is an out object; the loader represents
         * it as another session handle and tags the role for dispatch. */
        static const struct { uint32_t vi_role; const char *name; } roles[] = {
            {VP_GUEST_VI_RELAY, "GetRelayService"},
            {VP_GUEST_VI_SYSTEM, "GetSystemDisplayService"},
            {VP_GUEST_VI_MANAGER, "GetManagerDisplayService"},
            {VP_GUEST_VI_INDIRECT, "GetIndirectDisplayTransactionService"},
        };
        uint32_t index = command - 0x64u;
        uint32_t object = create_handle(svc, 2, "vi:obj", 0);
        VPGuestSession *ostate = object ? handle_session(svc, find_handle(svc, object)) : NULL;
        if (!ostate) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_VI_OBJECT_LIMIT command=%u\n", command);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        ostate->vi_role = roles[index].vi_role;
        if (ostate->vi_role == VP_GUEST_VI_RELAY
            || ostate->vi_role == VP_GUEST_VI_INDIRECT) {
            ostate->binder_handle = 1; /* producer handle in the OpenLayer Parcel */
        }
        uint32_t handle_offset = (3u + 2u) * 4u;
        uint32_t response_meta = (buffer + handle_offset + 4u + 0xfu) & ~0xfu;
        put32(buffer, 0);
        put32(buffer + 4, 0x8000000Au); /* raw words, one move handle */
        put32(buffer + 8, 0x21u);       /* Pid + one move handle */
        put32(buffer + 0xc, 0);
        put32(buffer + 0x10, 0);
        put32(buffer + handle_offset, object);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_VI_OBJECT command=%u call=%s object=0x%08x role=%u meta=0x%08x result=0\n",
                command, roles[index].name, object, roles[index].vi_role, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "vi:m") && command == 2) {
        /* nn::visrv::sf::IManagerRootService command 2 GetDisplayService:
         * input one u32 service type, output one IApplicationDisplayService
         * session. The loader has no separate server process, so the display
         * service is a second handle on a fresh session state; its commands
         * are handled by the display branch below. */
        uint32_t service_type = get32(request_meta + 0x10);
        uint32_t display = create_handle(svc, 2, "vi:disp", 0);
        VPGuestSession *dstate = display ? handle_session(svc, find_handle(svc, display)) : NULL;
        if (!dstate) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_VI_DISPLAY_LIMIT handle=0x%08x\n", handle);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        dstate->display_service = service_type ? service_type : 1;
        dstate->vi_role = VP_GUEST_VI_APPLICATION;
        uint32_t handle_offset = (3u + 2u) * 4u;
        uint32_t response_meta = (buffer + handle_offset + 4u + 0xfu) & ~0xfu;
        put32(buffer, 0);
        put32(buffer + 4, 0x8000000Au); /* raw words, one move handle */
        put32(buffer + 8, 0x21u);       /* Pid + one move handle */
        put32(buffer + 0xc, 0);
        put32(buffer + 0x10, 0);
        put32(buffer + handle_offset, display);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_VI_GET_DISPLAY_SERVICE type=%u display=0x%08x meta=0x%08x result=0\n",
                dstate->display_service, display, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strncmp(session->name, "nvdrv", 5)) {
        /* nn::nvdrv root session. Command 3 Initialize takes (process handle,
         * transfer memory handle, Size) and returns only Err. The copy-handle
         * array starts at word 3 unless the special header carries a PID.
         * The transfer memory was created by svc 0x15 in this same process,
         * so the loader-side server can reach the same pages later; this
         * command only binds the handle and its declared size. */
        uint32_t special = (header1 & 0x80000000u) ? get32(buffer + 8) : 0;
        uint32_t copies = (special >> 1) & 0xfu;
        uint32_t handle_base = buffer + 0x0c + ((special & 1u) ? 8u : 0u);
        uint64_t descriptor_pid = (special & 1u)
            ? ((uint64_t)get32(buffer + 0x10) << 32) | get32(buffer + 0x0c) : 0;
        /* Descriptor region order and sizes as HipcMessageReader walks it:
         * X entries are two words, send/receive/exchange map data three
         * words each, and C entries follow the raw data. The ioctl calls put
         * their data buffers in the send and receive lists; hipc_read_buffer
         * and hipc_write_buffer also resolve the X/C forms the client uses
         * when it moves a buffer into its pointer buffer. */
        VPGuestHipcLayout layout;
        hipc_layout(buffer, header0, header1, &layout);
        uint32_t buffer_count = layout.x_count;
        uint32_t send_count = layout.a_count;
        uint32_t receive_count = layout.b_count;
        if (command == 0) {
            /* nn::nvdrv command 0 Open: the device path is passed in the
             * request's first read buffer. The reply carries the opened
             * descriptor and an NvResult. A device is opened only after its
             * node is registered here with an explicit model; an unreviewed
             * name stops the run instead of receiving a fabricated fd.
             * Ioctls on the returned fd are answered by their own models and
             * otherwise fail explicitly. */
            uint32_t size = 0, address = 0;
            hipc_read_buffer(&layout, 0, &size, &address);
            char device[VP_GUEST_NVDRV_NAME_MAX];
            memset(device, 0, sizeof(device));
            uint32_t result = 0;
            int32_t fd = 0;
            int supported = 1;
            /* The SDK passes the path length without the NUL terminator, so
             * the descriptor size itself bounds the name. */
            if ((!buffer_count && !send_count) || !size || size > 0x40
                || !vp_guest_svc_readable(svc, address, size)) {
                supported = 0;
            } else {
                memcpy(device, (const void *)(uintptr_t)address,
                       size < sizeof(device) ? size : sizeof(device) - 1);
                device[sizeof(device) - 1] = 0;
                for (size_t i = 0; i < strlen(device); ++i) {
                    unsigned char c = (unsigned char)device[i];
                    if (c < 0x20 || c > 0x7e) { supported = 0; break; }
                }
            }
            /* Reviewed nvdrv device nodes. The title's runtime constructs
             * these from the same driver; a name absent from this table has
             * no model and its open stops the run after the log line. */
            static const char *const device_nodes[] = {
                "/dev/nvhost-ctrl",
                "/dev/nvhost-ctrl-gpu",
                "/dev/nvhost-as-gpu",
                "/dev/nvhost-gpu",
                "/dev/nvhost-nvdec",
                "/dev/nvhost-vic",
                "/dev/nvmap",
            };
            int known_device = 0;
            for (unsigned i = 0; i < sizeof(device_nodes) / sizeof(device_nodes[0]); ++i)
                if (!strcmp(device, device_nodes[i])) known_device = 1;
            if (!strcmp(device, "/dev/nvhost-nvdec")
                || !strcmp(device, "/dev/nvhost-vic")) {
                /* This platform has no NVDEC or VIC engine. The transport
                 * models above can hold the channel/syncpoint bookkeeping but
                 * cannot produce a frame, so by default the devices report
                 * FileOperationFailed and the title's media stack takes its
                 * own fallback. media-decode.enable opts into the transport
                 * model for diagnostics; it still executes no decode. */
                FILE *media = fopen("ux0:data/mhgu-vita-port/media-decode.enable", "rb");
                if (!media)
                    known_device = 0;
                else
                    fclose(media);
            }
            if (supported && !known_device) {
                /* Not registered: an explicit unsupported path, never a
                 * silently fabricated descriptor. */
                supported = 0;
            }
            if (!supported) {
                ++svc->ipc_unsupported;
                /* The device node has no model on this platform (there is no
                 * NVDEC hardware). Do not fabricate an fd and do not stop
                 * the calling thread: answer with the nvdrv driver's own
                 * open-failure ABI -- fd = -1, NvResult::FileOperationFailed
                 * (0x30003, the same value the reference nvdrv service
                 * returns for an unknown device) -- so the original client
                 * code observes the failure and takes its own error or
                 * fallback path. The unsupported device stays recorded. */
                uint32_t response_meta = plain_response_meta(buffer);
                put32(buffer, 0);
                put32(buffer + 4, (response_meta - (buffer + 8u)) / 4u + 4u + 2u);
                put32(response_meta, 0x4F434653u);
                put32(response_meta + 4, 0);
                put32(response_meta + 8, 0);
                put32(response_meta + 0xc, 0);
                put32(response_meta + 0x10, 0xFFFFFFFFu); /* invalid fd */
                put32(response_meta + 0x14, 0x30003u);    /* FileOperationFailed */
                f->regs[0] = 0;
                slog(svc, "SVC_IPC_NVDRV_OPEN_UNSUPPORTED pointers=%u send=%u size=%u address=0x%08x name=\"%s\" nv_result=0x30003 fd=invalid model=device-absent result=to-guest\n",
                        buffer_count, send_count, size, address, device);
                sflush(svc);
                return 0;
            }
            if (state->nvdrv_devices >= VP_GUEST_NVDRV_DEVICE_LIMIT) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_NVDRV_DEVICE_LIMIT count=%u name=\"%s\"\n",
                        state->nvdrv_devices, device);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            fd = (int32_t)state->nvdrv_devices + 1;
            state->nvdrv_device_fd[state->nvdrv_devices] = fd;
            memcpy(state->nvdrv_device_name[state->nvdrv_devices], device,
                   strlen(device) + 1);
            ++state->nvdrv_devices;
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, (response_meta - (buffer + 8u)) / 4u + 4u + 2u);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            put32(response_meta + 0x10, (uint32_t)fd);
            put32(response_meta + 0x14, result); /* NvResult::Success */
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_NVDRV command=0 call=Open name=\"%s\" fd=%d devices=%u nv_result=%u meta=0x%08x\n",
                    device, fd, state->nvdrv_devices, result, response_meta);
            sflush(svc);
            return 0;
        }
        if (command == 2) {
            /* nn::nvdrv Close(fd): drop the reviewed device descriptor. The
             * loader owns no kernel device; the entry is retired (fd = -1) so
             * a later ioctl naming it finds no model instead of reaching a
             * stale one. An unknown fd answers NotImplemented, matching the
             * reference driver's lookup failure. */
            uint32_t fd = get32(request_meta + 0x10);
            uint32_t result = 1; /* NvResult::NotImplemented */
            const char *closed = "";
            for (uint32_t i = 0; i < state->nvdrv_devices; ++i) {
                if (state->nvdrv_device_fd[i] != (int32_t)fd) continue;
                closed = state->nvdrv_device_name[i];
                state->nvdrv_device_fd[i] = -1;
                vp_guest_gpu_channel_close(state, fd);
                result = 0;
                break;
            }
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, (response_meta - (buffer + 8u)) / 4u + 4u + 1u);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            put32(response_meta + 0x10, result);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_NVDRV command=2 call=Close fd=%u device=\"%s\" nv_result=%u meta=0x%08x\n",
                    fd, closed, result, response_meta);
            sflush(svc);
            return 0;
        }
        if (command == 1) {
            /* nn::nvdrv Ioctl1: raw parameters are the device descriptor and
             * the nvhost ioctl word; the input data is the send map data and
             * the output the receive map data. Only ioctls whose semantics
             * this loader models are answered; any other command stops the
             * run with the fd, device and ioctl word in the log. */
            uint32_t fd = get32(request_meta + 0x10);
            uint32_t ioctl = get32(request_meta + 0x14);
            uint32_t in_size, in_address, out_size, out_address;
            hipc_read_buffer(&layout, 0, &in_size, &in_address);
            hipc_write_buffer(&layout, 0, &out_size, &out_address);
            int32_t device_index = -1;
            for (uint32_t i = 0; i < state->nvdrv_devices; ++i)
                if (state->nvdrv_device_fd[i] == (int32_t)fd) device_index = (int32_t)i;
            const char *device_name = device_index >= 0
                ? state->nvdrv_device_name[device_index] : "";
            int ctrl_gpu = !strcmp(device_name, "/dev/nvhost-ctrl-gpu");
            int ctrl_dev = !strcmp(device_name, "/dev/nvhost-ctrl");
            int as_gpu = !strcmp(device_name, "/dev/nvhost-as-gpu");
            int gpu_dev = !strcmp(device_name, "/dev/nvhost-gpu");
            int nvdec_dev = !strcmp(device_name, "/dev/nvhost-nvdec");
            int vic_dev = !strcmp(device_name, "/dev/nvhost-vic");
            int nvmap_dev = !strcmp(device_name, "/dev/nvmap");
            VPGuestGpuChannel *channel = (gpu_dev || nvdec_dev || vic_dev)
                ? vp_guest_gpu_channel(state, fd) : NULL;
            if ((gpu_dev || nvdec_dev || vic_dev) && !channel) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_NVDRV_CHANNEL_LIMIT fd=%u count=%u\n",
                        fd, state->gpu_channel_count);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            if (channel && nvdec_dev) channel->nvdec_device = 1;
            if (channel && vic_dev) channel->vic_device = 1;
            if (ioctl == 0xc0284106u)
                slog(svc, "SVC_IPC_NVDRV_MAP_PRE fd=%u device=\"%s\" as=%d supported=%d in=%u@0x%08x out=%u@0x%08x\n",
                        fd, device_name, as_gpu, device_index >= 0,
                        in_size, in_address, out_size, out_address);
            /* VP01.5.3 evidence: record each (device fd, ioctl word) pair on
             * first use with the descriptor shape and leading input words, so
             * the live driver request set is on record even when later calls
             * repeat it every frame. */
            uint32_t trace_key = ((uint32_t)fd << 24) ^ ioctl;
            int trace_new = 0;
            if (state->nvdrv_ioctl_trace_count < VP_GUEST_NVDRV_IOCTL_TRACE) {
                uint32_t i = 0;
                for (; i < state->nvdrv_ioctl_trace_count; ++i)
                    if (state->nvdrv_ioctl_trace[i] == trace_key) break;
                if (i == state->nvdrv_ioctl_trace_count) {
                    state->nvdrv_ioctl_trace[state->nvdrv_ioctl_trace_count++] =
                        trace_key;
                    trace_new = 1;
                }
            }
            if (trace_new && device_index >= 0) {
                slog(svc, "SVC_IPC_NVDRV_IOCTL fd=%u device=\"%s\" ioctl=0x%08x in=%u@0x%08x out=%u@0x%08x data",
                        fd, device_name, ioctl, in_size, in_address,
                        out_size, out_address);
                for (unsigned i = 0; i < 8u; ++i) {
                    if (i * 4u >= in_size || !send_count
                        || !vp_guest_svc_readable(svc, in_address + i * 4u, 4u))
                        break;
                    slog(svc, " %08x", get32(in_address + i * 4u));
                }
                slog(svc, "\n");
            }
            int supported = device_index >= 0;
            uint32_t nv_result = 0;
            if (supported && as_gpu) {
                if (ioctl == 0x40044101u) {
                    /* NVGPU_AS_IOCTL_BIND_CHANNEL: {u32 fd}. The kernel
                     * attaches the GPU channel to this address space. No
                     * output buffer; the loader records the channel after
                     * checking it names an open /dev/nvhost-gpu. */
                    if (!send_count || in_size < 4u
                        || !vp_guest_svc_readable(svc, in_address, 4u)) {
                        supported = 0;
                    } else {
                        uint32_t channel = get32(in_address);
                        int channel_device = 0;
                        for (uint32_t i = 0; i < state->nvdrv_devices; ++i)
                            if (state->nvdrv_device_fd[i] == (int32_t)channel
                                && !strcmp(state->nvdrv_device_name[i], "/dev/nvhost-gpu"))
                                channel_device = 1;
                        if (!channel_device) {
                            nv_result = 4; /* BadParameter */
                        } else {
                            state->gpu_as.bound_channel = (int32_t)channel;
                            state->gpu_as.bound_channel_set = 1;
                            slog(svc, "SVC_IPC_NVDRV command=1 call=BindChannel fd=%u channel=%u result=0\n",
                                    fd, channel);
                        }
                    }
                } else if (ioctl == 0x40284109u) {
                    /* NVGPU_AS_IOCTL_ALLOC_AS: {flags, as_fd, big_page_size,
                     * reserved, va_range_start, va_range_end, va_range_split}.
                     * No output buffer: the kernel only programs the address
                     * space. The loader records the same virtual layout
                     * (defaults: big page 0x20000, start 0x8000000, split
                     * 1<<34, end 1<<37) so later allocations are consistent.
                     */
                    if (!send_count || in_size < 0x28u
                        || !vp_guest_svc_readable(svc, in_address, 0x28u)) {
                        supported = 0;
                    } else {
                        uint32_t flags = get32(in_address);
                        uint32_t big_page = get32(in_address + 8);
                        uint64_t va_start = (uint64_t)get32(in_address + 0x10)
                            | ((uint64_t)get32(in_address + 0x14) << 32);
                        uint64_t va_end = (uint64_t)get32(in_address + 0x18)
                            | ((uint64_t)get32(in_address + 0x1c) << 32);
                        uint64_t va_split = (uint64_t)get32(in_address + 0x20)
                            | ((uint64_t)get32(in_address + 0x24) << 32);
                        if (state->gpu_as.initialised) {
                            nv_result = 8; /* InvalidState */
                        } else if (big_page && (big_page & (big_page - 1u))) {
                            nv_result = 0xb; /* BadValue */
                        } else if (big_page && !(big_page & 0x30000u)) {
                            nv_result = 0xb; /* BadValue */
                        } else {
                            if (!big_page) big_page = 0x20000;
                            if (!va_start) {
                                va_start = 0x20000ull << 10;
                                va_split = 1ull << 34;
                                va_end = 1ull << 37;
                            }
                            if (va_start >= va_split || va_split >= va_end) {
                                nv_result = 0xb; /* BadValue */
                            } else {
                                state->gpu_as.initialised = 1;
                                state->gpu_as.big_page_size = big_page;
                                state->gpu_as.va_start = va_start;
                                state->gpu_as.va_split = va_split;
                                state->gpu_as.va_end = va_end;
                                state->gpu_as.small_next = va_start;
                                state->gpu_as.big_next = va_split;
                                slog(svc, "SVC_IPC_NVDRV command=1 call=AllocAs fd=%u flags=0x%x big_page=0x%x start=0x%llx split=0x%llx end=0x%llx result=0\n",
                                        fd, flags, big_page,
                                        (unsigned long long)va_start,
                                        (unsigned long long)va_split,
                                        (unsigned long long)va_end);
                            }
                        }
                    }
                } else if (ioctl == 0xc0404108u) {
                    /* NVGPU_AS_IOCTL_GET_VA_REGIONS: {u64 buf_addr, u32
                     * buf_size, u32 reserved, VaRegion[2]}. The kernel
                     * reports the small-page range below the split and the
                     * big-page range above it; the caller's buffer is 64
                     * bytes. */
                    if (!state->gpu_as.initialised) {
                        nv_result = 0xb; /* BadValue */
                    } else if (!receive_count || out_size < 0x40u
                               || !vp_guest_svc_writable(svc, out_address, 0x40u)) {
                        supported = 0;
                    } else {
                        uint32_t big_bits = 0;
                        for (uint32_t v = state->gpu_as.big_page_size; v > 1; v >>= 1) ++big_bits;
                        put32(out_address + 0x08, 0x30); /* 2 * sizeof(VaRegion) */
                        put32(out_address + 0x0c, 0);
                        put64(out_address + 0x10, state->gpu_as.va_start);
                        put32(out_address + 0x18, 0x1000);
                        put32(out_address + 0x1c, 0);
                        put64(out_address + 0x20,
                              (state->gpu_as.va_split - state->gpu_as.va_start) >> 12);
                        put64(out_address + 0x28, state->gpu_as.va_split);
                        put32(out_address + 0x30, state->gpu_as.big_page_size);
                        put32(out_address + 0x34, 0);
                        put64(out_address + 0x38,
                              (state->gpu_as.va_end - state->gpu_as.va_split) >> big_bits);
                        slog(svc, "SVC_IPC_NVDRV command=1 call=GetVARegions fd=%u small=0x%llx+%llu big=0x%llx page=0x%x pages=%llu result=0\n",
                                fd,
                                (unsigned long long)state->gpu_as.va_start,
                                (unsigned long long)((state->gpu_as.va_split - state->gpu_as.va_start) >> 12),
                                (unsigned long long)state->gpu_as.va_split,
                                state->gpu_as.big_page_size,
                                (unsigned long long)((state->gpu_as.va_end - state->gpu_as.va_split) >> big_bits));
                    }
                } else if (ioctl == 0xc0184102u) {
                    /* NVGPU_AS_IOCTL_ALLOC_SPACE: {u32 pages, u32 page_size,
                     * u32 flags, u32 pad, u64 offset}. Without the fixed-flag
                     * the loader returns the next free range of the matching
                     * allocator; with it the caller-chosen offset must fit
                     * the range and not overlap an earlier reservation. */
                    if (!state->gpu_as.initialised) {
                        nv_result = 0xb; /* BadValue */
                    } else if (!send_count || !receive_count || in_size < 0x18u
                               || out_size < 0x18u
                               || !vp_guest_svc_readable(svc, out_address, 0x18u)
                               || !vp_guest_svc_writable(svc, out_address, 0x18u)) {
                        supported = 0;
                    } else {
                        VPGuestGpuAs *as = &state->gpu_as;
                        uint32_t pages = get32(out_address);
                        uint32_t page_size = get32(out_address + 4);
                        uint32_t flags = get32(out_address + 8);
                        uint64_t requested = (uint64_t)get32(out_address + 0x10)
                            | ((uint64_t)get32(out_address + 0x14) << 32);
                        uint32_t big_bits = 0;
                        for (uint32_t v = as->big_page_size; v > 1; v >>= 1) ++big_bits;
                        uint32_t bits = page_size == 0x1000u ? 12u
                            : page_size == as->big_page_size ? big_bits : 0;
                        if (!bits) {
                            nv_result = 0xb; /* BadValue */
                        } else if (as->allocation_count >= VP_GUEST_GPU_AS_ALLOC_LIMIT) {
                            ++svc->ipc_unsupported;
                            slog(svc, "SVC_IPC_NVDRV_AS_ALLOC_LIMIT fd=%u count=%u pages=%u page_size=0x%x\n",
                                    fd, as->allocation_count, pages, page_size);
                            sflush(svc);
                            return VP_BRIDGE_UNSUPPORTED;
                        } else {
                            uint64_t base = page_size == 0x1000u
                                ? as->va_start : as->va_split;
                            uint64_t limit = page_size == 0x1000u
                                ? as->va_split : as->va_end;
                            uint64_t size = (uint64_t)pages << bits;
                            uint64_t offset;
                            if (flags & 1u) {
                                offset = requested & ~(((uint64_t)1 << bits) - 1);
                            } else {
                                offset = page_size == 0x1000u
                                    ? as->small_next : as->big_next;
                                offset = (offset + (((uint64_t)1 << bits) - 1))
                                    & ~(((uint64_t)1 << bits) - 1);
                            }
                            int fits = offset >= base && offset < limit
                                && size <= limit - offset;
                            for (uint32_t i = 0; fits && i < as->allocation_count; ++i) {
                                uint64_t other = as->allocations[i].offset;
                                uint64_t other_end = other + as->allocations[i].size;
                                if (offset < other_end && other < offset + size)
                                    fits = 0;
                            }
                            if (!fits) {
                                nv_result = 6; /* InsufficientMemory */
                                slog(svc, "SVC_IPC_NVDRV command=1 call=AllocateSpace fd=%u pages=%u page_size=0x%x flags=0x%x offset=0x%llx result=InsufficientMemory\n",
                                        fd, pages, page_size, flags,
                                        (unsigned long long)offset);
                            } else {
                                as->allocations[as->allocation_count].offset = offset;
                                as->allocations[as->allocation_count].size = size;
                                as->allocations[as->allocation_count].page_size = page_size;
                                ++as->allocation_count;
                                if (page_size == 0x1000u) as->small_next = offset + size;
                                else as->big_next = offset + size;
                                put64(out_address + 0x10, offset);
                                slog(svc, "SVC_IPC_NVDRV command=1 call=AllocateSpace fd=%u pages=%u page_size=0x%x flags=0x%x offset=0x%llx result=0\n",
                                        fd, pages, page_size, flags,
                                        (unsigned long long)offset);
                            }
                        }
                    }
                } else if (ioctl == 0xc0284106u) {
                    /* NVGPU_AS_IOCTL_MAP_BUFFER_EX: {u32 flags, u32 kind,
                     * u32 handle, u32 page_size, s64 buffer_offset, u64
                     * mapping_size, s64 offset}. The nvmap handle names
                     * client memory registered earlier; this loader records
                     * the GPU VA mapping (and returns its offset for
                     * non-fixed mappings) without programming a real GMMU. */
                    slog(svc, "SVC_IPC_NVDRV_MAP_ENTRY fd=%u initialised=%u in=%u@0x%08x out=%u@0x%08x struct=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                            fd, state->gpu_as.initialised, in_size, in_address,
                            out_size, out_address,
                            get32(out_address), get32(out_address + 4),
                            get32(out_address + 8), get32(out_address + 0xc),
                            get32(out_address + 0x10), get32(out_address + 0x14),
                            get32(out_address + 0x18), get32(out_address + 0x1c),
                            get32(out_address + 0x20), get32(out_address + 0x24));
                    if (!state->gpu_as.initialised) {
                        nv_result = 0xb; /* BadValue */
                    } else if (!send_count || !receive_count || in_size < 0x28u
                               || out_size < 0x28u
                               || !vp_guest_svc_readable(svc, out_address, 0x28u)
                               || !vp_guest_svc_writable(svc, out_address, 0x28u)) {
                        supported = 0;
                    } else {
                        VPGuestGpuAs *as = &state->gpu_as;
                        uint32_t flags = get32(out_address);
                        uint32_t kind = get32(out_address + 4);
                        uint32_t handle = get32(out_address + 8);
                        uint64_t buffer_offset = (uint64_t)get32(out_address + 0x10)
                            | ((uint64_t)get32(out_address + 0x14) << 32);
                        uint64_t mapping_size = (uint64_t)get32(out_address + 0x18)
                            | ((uint64_t)get32(out_address + 0x1c) << 32);
                        uint64_t offset = (uint64_t)get32(out_address + 0x20)
                            | ((uint64_t)get32(out_address + 0x24) << 32);
                        uint32_t remap = flags & 0x100u;
                        VPGuestNvMapHandle *h = nvmap_find(state, handle);
                        uint32_t big_bits = 0;
                        for (uint32_t v = as->big_page_size; v > 1; v >>= 1) ++big_bits;
                        uint64_t aligned_size = h
                            ? ((uint64_t)h->size + 0xfffu) & ~0xfffull : 0;
                        uint64_t size = mapping_size ? mapping_size
                            : (h ? h->size : 0);
                        if (remap) {
                            /* NVGPU_AS_MAP_BUFFER_FLAGS_REMAP: re-point a
                             * subrange of the mapping based at `offset` at
                             * `buffer_offset` inside the same nvmap
                             * allocation. The handle field is not used for
                             * this form; the base mapping supplies it. The
                             * loader validates the range against the base
                             * mapping's recorded extent and records the PTE
                             * rewrite; it programs no GMMU. */
                            uint32_t base = 0;
                            for (; base < as->mapping_count; ++base)
                                if (as->mappings[base].offset == offset) break;
                            if (base == as->mapping_count) {
                                nv_result = 0xb; /* BadValue */
                                slog(svc, "SVC_IPC_NVDRV_MAP_REMAP_REJECT fd=%u offset=0x%llx kind=%u buffer_offset=0x%llx mapping_size=0x%llx reason=no-base-mapping result=%u\n",
                                        fd, (unsigned long long)offset, kind,
                                        (unsigned long long)buffer_offset,
                                        (unsigned long long)mapping_size, nv_result);
                            } else if (as->mappings[base].size < mapping_size
                                       || buffer_offset
                                          > as->mappings[base].size - mapping_size) {
                                nv_result = 0xb; /* BadValue */
                                slog(svc, "SVC_IPC_NVDRV_MAP_REMAP_REJECT fd=%u offset=0x%llx kind=%u buffer_offset=0x%llx mapping_size=0x%llx base_size=0x%llx base_handle=%u reason=outside-base-mapping result=%u\n",
                                        fd, (unsigned long long)offset, kind,
                                        (unsigned long long)buffer_offset,
                                        (unsigned long long)mapping_size,
                                        (unsigned long long)as->mappings[base].size,
                                        as->mappings[base].handle, nv_result);
                            } else {
                                uint64_t gpu_va = offset + buffer_offset;
                                uint32_t device_address =
                                    as->mappings[base].device_address
                                    + (uint32_t)buffer_offset;
                                ++as->remap_count;
                                slog(svc, "SVC_IPC_NVDRV command=1 call=MapBufferExRemap fd=%u base=0x%llx base_handle=%u base_kind=%u kind=%u buffer_offset=0x%llx mapping_size=0x%llx gpu_va=0x%llx device=0x%08x remaps=%u model=pte-remap-within-existing-mapping result=0\n",
                                        fd, (unsigned long long)offset,
                                        as->mappings[base].handle,
                                        as->mappings[base].kind, kind,
                                        (unsigned long long)buffer_offset,
                                        (unsigned long long)mapping_size,
                                        (unsigned long long)gpu_va,
                                        device_address, as->remap_count);
                            }
                        } else if (!h || !h->allocated) {
                            nv_result = 0xb; /* BadValue */
                        } else if (buffer_offset > aligned_size
                                   || size > aligned_size - buffer_offset) {
                            nv_result = 0xb; /* BadValue */
                        } else if (as->mapping_count >= VP_GUEST_GPU_AS_MAP_LIMIT) {
                            ++svc->ipc_unsupported;
                            slog(svc, "SVC_IPC_NVDRV_AS_MAP_LIMIT fd=%u count=%u\n",
                                    fd, as->mapping_count);
                            sflush(svc);
                            return VP_BRIDGE_UNSUPPORTED;
                        } else {
                            int big_page = h->align
                                && !(h->align & (as->big_page_size - 1u));
                            uint32_t page_bytes = big_page ? as->big_page_size : 0x1000u;
                            uint32_t page_bits = big_page ? big_bits : 12u;
                            uint64_t map_size = (size + page_bytes - 1)
                                & ~((uint64_t)page_bytes - 1);
                            if (flags & 1u) {
                                /* Fixed: the GPU address must lie inside an
                                 * AllocateSpace reservation. */
                                int fits = 0;
                                for (uint32_t i = 0; i < as->allocation_count; ++i) {
                                    uint64_t a = as->allocations[i].offset;
                                    uint64_t a_end = a + as->allocations[i].size;
                                    if (offset >= a && offset + map_size <= a_end)
                                        fits = 1;
                                }
                                if (!fits) nv_result = 0xb; /* BadValue */
                            } else {
                                uint64_t limit = big_page ? as->va_end : as->va_split;
                                offset = big_page ? as->big_next : as->small_next;
                                offset = (offset + ((uint64_t)1 << page_bits) - 1)
                                    & ~(((uint64_t)1 << page_bits) - 1);
                                if (offset < (big_page ? as->va_split : as->va_start)
                                    || map_size > limit - offset) {
                                    nv_result = 6; /* InsufficientMemory */
                                    offset = 0;
                                } else {
                                    if (big_page) as->big_next = offset + map_size;
                                    else as->small_next = offset + map_size;
                                }
                            }
                            if (!nv_result) {
                                as->mappings[as->mapping_count].handle = handle;
                                as->mappings[as->mapping_count].kind = kind;
                                as->mappings[as->mapping_count].fixed =
                                    (flags & 1u) ? 1u : 0u;
                                as->mappings[as->mapping_count].big_page =
                                    big_page ? 1u : 0u;
                                as->mappings[as->mapping_count].device_address =
                                    h->address + (uint32_t)buffer_offset;
                                as->mappings[as->mapping_count].offset = offset;
                                as->mappings[as->mapping_count].size = map_size;
                                ++as->mapping_count;
                                if (!(flags & 1u)) put64(out_address + 0x20, offset);
                                slog(svc, "SVC_IPC_NVDRV command=1 call=MapBufferEx fd=%u handle=%u flags=0x%x kind=%u offset=0x%llx size=0x%llx device=0x%08x fixed=%u result=0\n",
                                        fd, handle, flags, kind,
                                        (unsigned long long)offset,
                                        (unsigned long long)map_size,
                                        h->address + (uint32_t)buffer_offset,
                                        (flags & 1u) ? 1u : 0u);
                            }
                            if (nv_result)
                                slog(svc, "SVC_IPC_NVDRV_MAP_REJECT fd=%u handle=%u flags=0x%x kind=%u buffer_offset=0x%llx mapping_size=0x%llx aligned=0x%llx found=%u allocated=%u offset=0x%llx struct=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x result=%u\n",
                                        fd, handle, flags, kind,
                                        (unsigned long long)buffer_offset,
                                        (unsigned long long)mapping_size,
                                        (unsigned long long)aligned_size,
                                        h ? 1u : 0u, h ? h->allocated : 0u,
                                        (unsigned long long)offset,
                                        get32(out_address), get32(out_address + 4),
                                        get32(out_address + 8), get32(out_address + 0xc),
                                        get32(out_address + 0x10), get32(out_address + 0x14),
                                        get32(out_address + 0x18), get32(out_address + 0x1c),
                                        get32(out_address + 0x20), get32(out_address + 0x24),
                                        nv_result);
                        }
                    }
                } else {
                    supported = 0;
                }
            } else if (supported && (nvdec_dev || vic_dev)) {
                /* /dev/nvhost-nvdec and /dev/nvhost-vic. The title's media
                 * stack opens these nodes for movie playback; both use the
                 * same nvhost channel ioctl set: group 0 cmd 1 Submit,
                 * 2 GetSyncpoint, 3 GetWaitbase, 7 SetSubmitTimeout,
                 * 9 MapBuffer, 0xa UnmapBuffer, and group 'H' cmd 1
                 * SetNVMAPfd (VIC omits SetSubmitTimeout). This loader
                 * implements the transport and synchronization contract
                 * (syncpoint ids, nvmap pinning, submission bookkeeping).
                 * The CDMA command buffers themselves are executed by the
                 * media hooks in media_decode.c/media_video.c when
                 * media-video.enable is present; without that marker the
                 * loads retire the fence with no video output, which the
                 * logged model then says explicitly. */
                const char *media = vic_dev ? "Vic" : "Nvdec";
                const int media_video = vp_media_video_enabled();
                const char *media_model = vic_dev
                    ? (media_video ? "composite-hook-fence-retired"
                                   : "composite-not-executed-fence-retired")
                    : (media_video ? "decode-hook-fence-retired"
                                   : "decode-not-executed-fence-retired");
                if (nvdec_dev) channel->nvdec_device = 1;
                else channel->vic_device = 1;
                if (ioctl == 0x40044801u) {
                    /* NVHOST_IOCTL_DEVICE_SET_NVMAP_FD: {s32 fd}. The device
                     * binds the nvmap descriptor it will pin handles through.
                     * The fd must name an open /dev/nvmap in this session. */
                    if (!send_count || in_size < 4u
                        || !vp_guest_svc_readable(svc, in_address, 4u)) {
                        supported = 0;
                    } else {
                        uint32_t nvmap_fd = get32(in_address);
                        int nvmap_device = 0;
                        for (uint32_t i = 0; i < state->nvdrv_devices; ++i)
                            if (state->nvdrv_device_fd[i] == (int32_t)nvmap_fd
                                && !strcmp(state->nvdrv_device_name[i], "/dev/nvmap"))
                                nvmap_device = 1;
                        if (!nvmap_device) {
                            nv_result = 4; /* BadParameter */
                        } else {
                            channel->nvdec_nvmap_fd = (int32_t)nvmap_fd;
                            channel->nvdec_nvmap_set = 1;
                            slog(svc, "SVC_IPC_NVDRV command=1 call=%sSetNvmapFd fd=%u nvmap_fd=%u result=0\n",
                                    media, fd, nvmap_fd);
                        }
                    }
                } else if (ioctl == 0xc0080002u) {
                    /* GetSyncpoint {u32 param, u32 value}. Every device open
                     * owns one syncpoint, allocated by the kernel's syncpoint
                     * manager at open. The id comes from the same monotonic
                     * pool as the GPU channels so waits and increments name
                     * one consistent counter. */
                    if (!receive_count || in_size < 4u || out_size < 8u
                        || !vp_guest_svc_readable(svc, in_address, 4u)
                        || !vp_guest_svc_writable(svc, out_address, 8u)) {
                        supported = 0;
                    } else {
                        if (!channel->nvdec_syncpoint_valid) {
                            channel->syncpoint = state->gpu_next_syncpoint++;
                            channel->fence_value = 0;
                            channel->nvdec_syncpoint_valid = 1;
                        }
                        put32(out_address, get32(in_address));
                        put32(out_address + 4, channel->syncpoint);
                        slog(svc, "SVC_IPC_NVDRV command=1 call=%sGetSyncpoint fd=%u param=%u syncpoint=%u model=allocated result=0\n",
                                media, fd, get32(in_address), channel->syncpoint);
                    }
                } else if (ioctl == 0xc0080003u) {
                    /* GetWaitbase {u32 unknown, u32 value}. The reference
                     * driver hard-codes the waitbase at 0. */
                    if (!receive_count || in_size < 4u || out_size < 8u
                        || !vp_guest_svc_readable(svc, in_address, 4u)
                        || !vp_guest_svc_writable(svc, out_address, 8u)) {
                        supported = 0;
                    } else {
                        put32(out_address, get32(in_address));
                        put32(out_address + 4, 0);
                        slog(svc, "SVC_IPC_NVDRV command=1 call=%sGetWaitbase fd=%u value=0 model=reference-hardcoded result=0\n",
                                media, fd);
                    }
                } else if (ioctl == 0x40040007u) {
                    /* SetSubmitTimeout(u32 timeout). Recorded only: there is
                     * no kernel job timeout behind this model. */
                    if (!send_count || in_size < 4u
                        || !vp_guest_svc_readable(svc, in_address, 4u)) {
                        supported = 0;
                    } else {
                        channel->nvdec_submit_timeout = get32(in_address);
                        slog(svc, "SVC_IPC_NVDRV command=1 call=%sSetSubmitTimeout fd=%u timeout=%u model=recorded result=0\n",
                                media, fd, channel->nvdec_submit_timeout);
                    }
                } else if ((ioctl & 0xc000ffffu) == 0xc0000009u
                           || (ioctl & 0xc000ffffu) == 0xc000000au) {
                    /* MapBuffer/UnmapBuffer: {u32 num_entries, u32
                     * data_address, u32 attach_host_ch_das} followed by
                     * num_entries {u32 map_handle, u32 map_address} entries.
                     * The _IOC size field is supplied by the caller at run
                     * time and counts the inline entry array: the media
                     * player sends 0xc01c0009 (12 + 2*8) where older callers
                     * send 0xc00c0009. Dispatch on direction/group/cmd only;
                     * the buffer lengths are validated below.
                     * The kernel pins the nvmap handle and returns the device
                     * address the command stream will name; this loader has no
                     * GPU page table for the device, so the pinned address is
                     * the allocation's registered guest address -- the same
                     * memory MapBufferEx points at. Unmap clears the entries
                     * exactly as the reference driver does. */
                    uint32_t num_entries = in_size >= 4u ? get32(in_address) : 0;
                    uint64_t need = 0x0cull + (uint64_t)num_entries * 8u;
                    if (!send_count || in_size < 0x0cu || out_size < need
                        || num_entries > 0x400u
                        || !vp_guest_svc_readable(svc, in_address, (uint32_t)need)
                        || !vp_guest_svc_writable(svc, out_address, (uint32_t)need)) {
                        supported = 0;
                    } else {
                        memmove((void *)(uintptr_t)out_address,
                                (const void *)(uintptr_t)in_address, (size_t)need);
                        if ((ioctl & 0xc000ffffu) == 0xc0000009u) {
                            for (uint32_t i = 0; i < num_entries; ++i) {
                                uint32_t entry = in_address + 0x0cu + i * 8u;
                                uint32_t map_handle = get32(entry);
                                VPGuestNvMapHandle *h = nvmap_find(state, map_handle);
                                if (!h || !h->allocated) {
                                    nv_result = 4; /* BadParameter */
                                    slog(svc, "SVC_IPC_NVDRV_MEDIA_MAP_REJECT fd=%u device=%s index=%u handle=%u allocated=%u\n",
                                            fd, media, i, map_handle, h ? h->allocated : 0u);
                                    break;
                                }
                                put32(out_address + 0x0cu + i * 8u + 4u, h->address);
                            }
                            slog(svc, "SVC_IPC_NVDRV command=1 call=%sMapBuffer fd=%u entries=%u model=pin-to-guest-address result=%u\n",
                                    media, fd, num_entries, nv_result);
                        } else {
                            for (uint32_t i = 0; i < num_entries; ++i) {
                                put32(out_address + 0x0cu + i * 8u, 0);
                                put32(out_address + 0x0cu + i * 8u + 4u, 0);
                            }
                            put32(out_address, 0);
                            put32(out_address + 4, 0);
                            put32(out_address + 8, 0);
                            slog(svc, "SVC_IPC_NVDRV command=1 call=%sUnmapBuffer fd=%u entries=%u result=0\n",
                                    media, fd, num_entries);
                        }
                    }
                } else if ((ioctl & 0xc000ffffu) == 0xc0000001u) {
                    /* Submit (group 0 cmd 1; the caller may encode the total
                     * variable length in the _IOC size field): {u32
                     * cmd_buffer_count, u32 relocation_count,
                     * u32 syncpoint_count, u32 fence_count} followed by the
                     * arrays {CommandBuffer[12]}{Reloc[16]}{u32 shift}
                     * {SyncptIncr[20]}{u32 fence_threshold}. The kernel reads
                     * each command buffer out of nvmap and queues it to the
                     * NVDEC engine, increments the declared syncpoints and
                     * writes the resulting maximums back into the fence
                     * thresholds. Here the buffers are read and recorded, and
                     * the syncpoint bookkeeping is performed exactly; the
                     * command list itself is not decoded. */
                    if (in_size < 0x10u) {
                        supported = 0;
                    } else {
                        uint32_t cb_count = get32(in_address);
                        uint32_t reloc_count = get32(in_address + 4);
                        uint32_t sync_count = get32(in_address + 8);
                        uint32_t fence_count = get32(in_address + 0x0c);
                        uint64_t total = 0x10ull
                            + (uint64_t)cb_count * 12u
                            + (uint64_t)reloc_count * 20u
                            + (uint64_t)sync_count * 20u
                            + (uint64_t)fence_count * 4u;
                        uint32_t cb_base = in_address + 0x10u;
                        uint32_t sync_base = cb_base + cb_count * 12u
                            + reloc_count * 20u;
                        uint32_t fence_base = sync_base + sync_count * 20u;
                        if (!send_count || !receive_count
                            || cb_count > 0x100u || reloc_count > 0x1000u
                            || sync_count > 0x40u || fence_count > 0x40u
                            || total > in_size || total > out_size
                            || !vp_guest_svc_readable(svc, in_address, (uint32_t)total)
                            || !vp_guest_svc_writable(svc, out_address, (uint32_t)total)) {
                            supported = 0;
                        } else {
                            uint32_t words = 0;
                            memmove((void *)(uintptr_t)out_address,
                                    (const void *)(uintptr_t)in_address, (size_t)total);
                            for (uint32_t i = 0; i < cb_count; ++i) {
                                uint32_t entry = cb_base + i * 12u;
                                uint32_t memory_id = get32(entry);
                                uint32_t offset = get32(entry + 4);
                                uint32_t count = get32(entry + 8);
                                VPGuestNvMapHandle *h = nvmap_find(state, memory_id);
                                if (!h || !h->allocated || count > 0x100000u
                                    || offset > h->size
                                    || count * 4u > h->size - offset
                                    || !vp_guest_svc_readable(svc, h->address + offset,
                                                              count * 4u)) {
                                    nv_result = 4; /* BadParameter */
                                    slog(svc, "SVC_IPC_NVDRV_MEDIA_CB_REJECT fd=%u device=%s index=%u memory=%u offset=0x%x words=%u allocated=%u size=0x%x\n",
                                            fd, media, i, memory_id, offset, count,
                                            h ? h->allocated : 0u, h ? h->size : 0u);
                                    break;
                                }
                                words += count;
                                if (channel->nvdec_submit_count < 4u) {
                                    uint32_t cpu = h->address + offset;
                                    slog(svc, "SVC_IPC_NVDRV_MEDIA_CB fd=%u device=%s submit=%u index=%u memory=%u offset=0x%x words=%u cpu=0x%08x first=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                                            fd, media, channel->nvdec_submit_count, i,
                                            memory_id, offset, count, cpu,
                                            count > 0u ? get32(cpu) : 0,
                                            count > 1u ? get32(cpu + 4u) : 0,
                                            count > 2u ? get32(cpu + 8u) : 0,
                                            count > 3u ? get32(cpu + 12u) : 0,
                                            count > 4u ? get32(cpu + 16u) : 0,
                                            count > 5u ? get32(cpu + 20u) : 0,
                                            count > 6u ? get32(cpu + 24u) : 0,
                                            count > 7u ? get32(cpu + 28u) : 0);
                                }
                                /* P0: decode the command headers for
                                 * evidence. The parser executes nothing and
                                 * is inert unless media-instrument.enable is
                                 * present; the submit result below is the
                                 * same with or without it. */
                                vp_media_parse_command_buffer(
                                    svc, state, channel,
                                    (const uint32_t *)(uintptr_t)(h->address + offset),
                                    count, nvdec_dev, vic_dev);
                            }
                            if (!nv_result) {
                                for (uint32_t i = 0; i < sync_count; ++i) {
                                    uint32_t sync_entry = sync_base + i * 20u;
                                    uint32_t id = get32(sync_entry);
                                    uint32_t increments = get32(sync_entry + 4);
                                    if (id >= VP_GUEST_NVCTRL_EVENT_LIMIT
                                        || increments > 0x10000u) {
                                        nv_result = 4; /* BadParameter */
                                        slog(svc, "SVC_IPC_NVDRV_MEDIA_SYNC_REJECT fd=%u device=%s index=%u id=%u increments=%u\n",
                                                fd, media, i, id, increments);
                                        break;
                                    }
                                    uint32_t value =
                                        state->nvctrl_syncpt_value[id] + increments;
                                    nvctrl_advance_syncpoint(svc, state, id, value);
                                    put32(out_address + (fence_base - in_address) + i * 4u,
                                          value);
                                    ++channel->nvdec_syncpoints;
                                }
                            }
                            channel->nvdec_cmd_buffers = cb_count;
                            channel->nvdec_cmd_words = words;
                            ++channel->nvdec_submit_count;
                            slog(svc, "SVC_IPC_NVDRV command=1 call=%sSubmit fd=%u cb=%u reloc=%u sync=%u fence=%u words=%u submit=%u syncpoint=%u model=%s result=%u\n",
                                    media, fd, cb_count, reloc_count, sync_count, fence_count,
                                    words, channel->nvdec_submit_count,
                                    channel->nvdec_syncpoint_valid
                                        ? channel->syncpoint : 0u, media_model, nv_result);
                        }
                    }
                } else {
                    supported = 0;
                }
            } else if (supported && ctrl_gpu && ioctl == 0xc0b04705u) {
                /* NVGPU_IOCTL_GET_CHARACTERISTICS on nvhost-ctrl-gpu. The
                 * ioctl buffer begins with {u64 size, u64 address} and the
                 * service returns the target's 0xa0-byte GPU characteristics
                 * in the same buffer at offset 0x10. The values describe the
                 * Tegra X1 GM20B the locked title was built for; no part of
                 * them comes from the Vita hardware. */
                if (!send_count || !receive_count || in_size < 0xb0u
                    || out_size < 0xb0u
                    || !vp_guest_svc_readable(svc, out_address, 0xb0u)
                    || !vp_guest_svc_writable(svc, out_address, 0xb0u)) {
                    supported = 0;
                } else {
                    uint32_t gc = out_address + 0x10;
                    uint64_t requested_size = (uint64_t)get32(out_address)
                        | ((uint64_t)get32(out_address + 4) << 32);
                    uint64_t requested_address = (uint64_t)get32(out_address + 8)
                        | ((uint64_t)get32(out_address + 0xc) << 32);
                    memset((void *)(uintptr_t)out_address, 0, 0xb0);
                    put64(out_address, 0xa0);
                    put64(out_address + 8, requested_address);
                    put32(gc + 0x00, 0x120);      /* arch: GM200 family */
                    put32(gc + 0x04, 0x0b);       /* impl: GM20B */
                    put32(gc + 0x08, 0xa1);       /* rev: A1 */
                    put32(gc + 0x0c, 1);          /* num_gpc */
                    put64(gc + 0x10, 0x40000);    /* l2_cache_size */
                    put64(gc + 0x18, 0);          /* on_board_video_memory_size */
                    put32(gc + 0x20, 2);          /* num_tpc_per_gpc */
                    put32(gc + 0x24, 0x20);       /* bus_type: AXI */
                    put32(gc + 0x28, 0x20000);    /* big_page_size */
                    put32(gc + 0x2c, 0x20000);    /* compression_page_size */
                    put32(gc + 0x30, 0x1b);       /* pde_coverage_bit_count */
                    put32(gc + 0x34, 0x30000);    /* available_big_page_sizes */
                    put32(gc + 0x38, 1);          /* gpc_mask */
                    put32(gc + 0x3c, 0x503);      /* sm_arch_sm_version */
                    put32(gc + 0x40, 0x503);      /* sm_arch_spa_version */
                    put32(gc + 0x44, 0x80);       /* sm_arch_warp_count */
                    put32(gc + 0x48, 0x28);       /* gpu_va_bit_count */
                    put32(gc + 0x4c, 0);          /* reserved */
                    put64(gc + 0x50, 0x55);       /* flags */
                    put32(gc + 0x58, 0x902d);     /* twod_class */
                    put32(gc + 0x5c, 0xb197);     /* threed_class */
                    put32(gc + 0x60, 0xb1c0);     /* compute_class */
                    put32(gc + 0x64, 0xb06f);     /* gpfifo_class */
                    put32(gc + 0x68, 0xa140);     /* inline_to_memory_class */
                    put32(gc + 0x6c, 0xb0b5);     /* dma_copy_class */
                    put32(gc + 0x70, 1);          /* max_fbps_count */
                    put32(gc + 0x74, 0);          /* fbp_en_mask */
                    put32(gc + 0x78, 2);          /* max_ltc_per_fbp */
                    put32(gc + 0x7c, 1);          /* max_lts_per_ltc */
                    put32(gc + 0x80, 0);          /* max_tex_per_tpc */
                    put32(gc + 0x84, 1);          /* max_gpc_count */
                    put32(gc + 0x88, 0x21d70);    /* rop_l2_en_mask_0 */
                    put32(gc + 0x8c, 0);          /* rop_l2_en_mask_1 */
                    put64(gc + 0x90, 0x6230326d67ULL); /* chipname "gm20b" */
                    put64(gc + 0x98, 0);          /* gr_compbit_store_base_hw */
                    slog(svc, "SVC_IPC_NVDRV command=1 call=GetCharacteristics fd=%u request_size=0x%llx request_address=0x%llx in=%u@0x%08x out=%u@0x%08x result=0\n",
                            fd, (unsigned long long)requested_size,
                            (unsigned long long)requested_address, in_size, in_address,
                            out_size, out_address);
                }
            } else if (supported && ctrl_gpu && ioctl == 0xc0184706u) {
                /* NVGPU_IOCTL_GET_TPC_MASKS: {u32 mask_buffer_size, u32 pad,
                 * u64 mask_buffer_address, u32 tpc_mask, u32 pad}. The
                 * target GM20B has two TPCs, so a non-zero mask buffer
                 * receives mask 3. The mask buffer itself is the caller's
                 * memory; the SDK reads the field back from this struct. */
                if (!send_count || !receive_count || in_size < 0x18u
                    || out_size < 0x18u
                    || !vp_guest_svc_readable(svc, out_address, 0x18u)
                    || !vp_guest_svc_writable(svc, out_address, 0x18u)) {
                    supported = 0;
                } else {
                    uint32_t mask_size = get32(out_address);
                    uint64_t mask_address = (uint64_t)get32(out_address + 8)
                        | ((uint64_t)get32(out_address + 0xc) << 32);
                    if (mask_size) put32(out_address + 0x10, 3);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=GetTPCMasks fd=%u mask_size=%u mask_address=0x%llx tpc_mask=%u result=0\n",
                            fd, mask_size, (unsigned long long)mask_address,
                            mask_size ? 3u : get32(out_address + 0x10));
                }
            } else if (supported && ctrl_gpu && ioctl == 0x80044701u) {
                /* NVGPU_IOCTL_ZCULL_GET_CTX_SIZE: one u32, the GM20B ZCull
                 * context size in pages. */
                if (!receive_count || out_size < 4u
                    || !vp_guest_svc_writable(svc, out_address, 4u)) {
                    supported = 0;
                } else {
                    put32(out_address, 1);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=ZCullGetCtxSize fd=%u size=1 result=0\n",
                            fd);
                }
            } else if (supported && ctrl_gpu && ioctl == 0x80284702u) {
                /* NVGPU_IOCTL_ZCULL_GET_INFO: the GM20B ZCull region
                 * geometry. The target has one full ZCull region. */
                if (!receive_count || out_size < 0x28u
                    || !vp_guest_svc_writable(svc, out_address, 0x28u)) {
                    supported = 0;
                } else {
                    static const uint32_t info[10] = {
                        0x20, 0x20, 0x400, 0x800, 0x20,
                        0x20, 0xc0, 0x20, 0x40, 0x10,
                    };
                    for (uint32_t i = 0; i < 10; ++i) put32(out_address + i * 4, info[i]);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=ZCullGetInfo fd=%u result=0\n", fd);
                }
            } else if (supported && ctrl_gpu && ioctl == 0x80084714u) {
                /* NVGPU_IOCTL_GET_ACTIVE_SLOT_MASK: slot 7, mask 1. */
                if (!receive_count || out_size < 8u
                    || !vp_guest_svc_writable(svc, out_address, 8u)) {
                    supported = 0;
                } else {
                    put32(out_address, 7);
                    put32(out_address + 4, 1);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=GetActiveSlotMask fd=%u slot=7 mask=1 result=0\n",
                            fd);
                }
            } else if (supported && ctrl_gpu && ioctl == 0x8010471cu) {
                /* NVGPU_IOCTL_GET_GPU_TIME: the GPU clock shares the
                 * loader's monotonic nanosecond timebase. */
                if (!receive_count || out_size < 0x10u
                    || !vp_guest_svc_writable(svc, out_address, 0x10u)) {
                    supported = 0;
                } else {
                    uint64_t ns = steady_ticks(svc) * 625u / 12u;
                    put64(out_address, ns);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=GetGpuTime fd=%u time_ns=%llu result=0\n",
                            fd, (unsigned long long)ns);
                }
            } else if (supported && ctrl_gpu && ioctl == 0x40084707u) {
                /* NVGPU_IOCTL_FLUSH_L2: the loader models no GPU cache, so
                 * there is no cache state to drain; the request flags are
                 * recorded for evidence. */
                if (!send_count || in_size < 8u
                    || !vp_guest_svc_readable(svc, in_address, 8u)) {
                    supported = 0;
                } else {
                    slog(svc, "SVC_IPC_NVDRV command=1 call=FlushL2 fd=%u flush=0x%08x result=0\n",
                            fd, get32(in_address));
                }
            } else if (supported && ctrl_gpu && ioctl == 0x402c4703u) {
                /* NVGPU_IOCTL_ZBC_SET_TABLE: 44-byte input of color_ds[4],
                 * color_l2[4], depth, format and type. The kernel programs
                 * the GPU's Z-buffer compression table; this loader has no
                 * GPU, so the exact request is recorded for a future device
                 * model and reported as unprogrammed, not silently applied. */
                if (!send_count || in_size < 0x2cu
                    || !vp_guest_svc_readable(svc, in_address, 0x2cu)) {
                    supported = 0;
                } else {
                    for (unsigned i = 0; i < 4; ++i) {
                        state->zbc_table.color_ds[i] = get32(in_address + i * 4);
                        state->zbc_table.color_l2[i] = get32(in_address + 16 + i * 4);
                    }
                    state->zbc_table.depth = get32(in_address + 32);
                    state->zbc_table.format = get32(in_address + 36);
                    state->zbc_table.type = get32(in_address + 40);
                    ++state->zbc_set_count;
                    slog(svc, "SVC_IPC_NVDRV command=1 call=ZBCSetTable fd=%u depth=0x%x format=0x%x type=0x%x color_ds=0x%08x,0x%08x,0x%08x,0x%08x color_l2=0x%08x,0x%08x,0x%08x,0x%08x model=recorded-no-device-programming result=0\n",
                            fd, state->zbc_table.depth, state->zbc_table.format,
                            state->zbc_table.type,
                            state->zbc_table.color_ds[0], state->zbc_table.color_ds[1],
                            state->zbc_table.color_ds[2], state->zbc_table.color_ds[3],
                            state->zbc_table.color_l2[0], state->zbc_table.color_l2[1],
                            state->zbc_table.color_l2[2], state->zbc_table.color_l2[3]);
                }
            } else if (supported && ctrl_dev && ioctl == 0xc004001fu) {
                /* NVHOST_CTRL_IOCTL_EVENT_REGISTER {u32 user_event_id}. The
                 * kernel creates a KEvent for the slot; the guest obtains a
                 * handle to it through nvdrv QueryEvent. No device produces
                 * syncpoint completions yet, so registration is recorded and
                 * the event stays unsignalled until a real fence retires. */
                if (!send_count || !receive_count || in_size < 4u || out_size < 4u
                    || !vp_guest_svc_readable(svc, in_address, 4u)
                    || !vp_guest_svc_writable(svc, out_address, 4u)) {
                    supported = 0;
                } else {
                    uint32_t event_id = get32(in_address);
                    if (event_id >= VP_GUEST_NVCTRL_EVENT_LIMIT) {
                        nv_result = 4; /* BadParameter */
                    } else {
                        state->nvctrl_events |= (uint64_t)1 << event_id;
                        put32(out_address, event_id);
                        slog(svc, "SVC_IPC_NVDRV command=1 call=CtrlEventRegister fd=%u event=%u model=kernel-event-slot result=0\n",
                                fd, event_id);
                    }
                }
            } else if (supported && ctrl_dev && ioctl == 0xc0040020u) {
                /* NVHOST_CTRL_IOCTL_EVENT_UNREGISTER {u32 user_event_id}. */
                if (!send_count || !receive_count || in_size < 4u || out_size < 4u
                    || !vp_guest_svc_readable(svc, in_address, 4u)
                    || !vp_guest_svc_writable(svc, out_address, 4u)) {
                    supported = 0;
                } else {
                    uint32_t event_id = get32(in_address) & 0xffu;
                    state->nvctrl_events &= ~((uint64_t)1 << event_id);
                    state->nvctrl_waits[event_id].armed = 0;
                    put32(out_address, event_id);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=CtrlEventUnregister fd=%u event=%u model=kernel-event-slot result=0\n",
                            fd, event_id);
                }
            } else if (supported && ctrl_dev && ioctl == 0xc0080021u) {
                /* NVHOST_CTRL_IOCTL_EVENT_UNREGISTER_BATCH {u64 mask}. */
                if (!send_count || !receive_count || in_size < 8u || out_size < 8u
                    || !vp_guest_svc_readable(svc, in_address, 8u)
                    || !vp_guest_svc_writable(svc, out_address, 8u)) {
                    supported = 0;
                } else {
                    uint64_t mask = (uint64_t)get32(in_address)
                        | ((uint64_t)get32(in_address + 4) << 32);
                    state->nvctrl_events &= ~mask;
                    for (uint32_t i = 0; i < VP_GUEST_NVCTRL_EVENT_LIMIT; ++i)
                        if (mask & ((uint64_t)1 << i))
                            state->nvctrl_waits[i].armed = 0;
                    put32(out_address, (uint32_t)mask);
                    put32(out_address + 4, (uint32_t)(mask >> 32));
                    slog(svc, "SVC_IPC_NVDRV command=1 call=CtrlEventUnregisterBatch fd=%u mask=0x%llx model=kernel-event-slot result=0\n",
                            fd, (unsigned long long)mask);
                }
            } else if (supported && ctrl_dev && ioctl == 0xc004001cu) {
                /* NVHOST_CTRL_IOCTL_CLEAR_EVENT_WAIT {u32 event_id}. */
                if (!send_count || !receive_count || in_size < 4u || out_size < 4u
                    || !vp_guest_svc_readable(svc, in_address, 4u)
                    || !vp_guest_svc_writable(svc, out_address, 4u)) {
                    supported = 0;
                } else {
                    uint32_t event_id = get32(in_address) & 0xffu;
                    state->nvctrl_waits[event_id].armed = 0;
                    put32(out_address, event_id);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=CtrlClearEventWait fd=%u event=%u model=kernel-wait-cleared result=0\n",
                            fd, event_id);
                }
            } else if (supported && ctrl_dev
                       && (ioctl == 0xc010001du || ioctl == 0xc010001eu)) {
                /* NVHOST_CTRL_IOCTL_EVENT_WAIT {u32 fence_id, u32 fence_value,
                 * u32 timeout, u32 value}. If the modeled syncpoint counter
                 * already reached the threshold the kernel answers Success
                 * immediately; otherwise the wait is armed on the registered
                 * event slot and the kernel's asynchronous answer is Timeout.
                 * The guest then blocks on the QueryEvent handle, which the
                 * submission retirement model signals. No fence value is
                 * invented: the counter only advances to values the guest
                 * itself declared on SubmitGPFIFO. */
                if (!send_count || !receive_count || in_size < 0x10u
                    || out_size < 0x10u
                    || !vp_guest_svc_readable(svc, in_address, 0x10u)
                    || !vp_guest_svc_writable(svc, out_address, 0x10u)) {
                    supported = 0;
                } else {
                    uint32_t fence_id = get32(in_address);
                    uint32_t fence_value = get32(in_address + 4);
                    uint32_t timeout = get32(in_address + 8);
                    uint32_t event_id = get32(in_address + 0x0c);
                    if (fence_id >= VP_GUEST_NVCTRL_EVENT_LIMIT) {
                        nv_result = 4; /* BadParameter */
                    } else {
                        uint32_t completed = state->nvctrl_syncpt_value[fence_id];
                        if (fence_value == 0 || completed >= fence_value) {
                            put32(out_address + 0x0c, completed);
                            slog(svc, "SVC_IPC_NVDRV command=1 call=CtrlEventWait fd=%u fence=%u:%u timeout=%u event=%u completed=%u model=syncpoint-already-retired result=0\n",
                                    fd, fence_id, fence_value, timeout, event_id,
                                    completed);
                        } else if (event_id >= VP_GUEST_NVCTRL_EVENT_LIMIT
                                   || !(state->nvctrl_events & ((uint64_t)1 << event_id))) {
                            nv_result = 4; /* BadParameter */
                        } else {
                            state->nvctrl_waits[event_id].armed = 1;
                            state->nvctrl_waits[event_id].fence_id = fence_id;
                            state->nvctrl_waits[event_id].fence_value = fence_value;
                            state->nvctrl_waits[event_id].event_id = event_id;
                            put32(out_address + 0x0c, event_id);
                            nv_result = 5; /* Timeout: armed, completion is async */
                            slog(svc, "SVC_IPC_NVDRV command=1 call=CtrlEventWait fd=%u fence=%u:%u timeout=%u event=%u completed=%u model=armed-pending-syncpoint result=Timeout\n",
                                    fd, fence_id, fence_value, timeout, event_id,
                                    completed);
                        }
                    }
                }
            } else if (supported && ctrl_dev) {
                supported = 0;
            } else if (supported && gpu_dev && ioctl == 0x40044801u) {
                /* NVGPU_IOCTL_CHANNEL_SET_NVMAP_FD: {u32 fd}. The channel is
                 * told which nvmap device its handles belong to; the loader
                 * records the descriptor after checking it is an open
                 * /dev/nvmap. No kernel object is programmed. */
                if (!send_count || in_size < 4u
                    || !vp_guest_svc_readable(svc, in_address, 4u)) {
                    supported = 0;
                } else {
                    uint32_t nvmap_fd = get32(in_address);
                    int nvmap_device = 0;
                    for (uint32_t i = 0; i < state->nvdrv_devices; ++i)
                        if (state->nvdrv_device_fd[i] == (int32_t)nvmap_fd
                            && !strcmp(state->nvdrv_device_name[i], "/dev/nvmap"))
                            nvmap_device = 1;
                    if (!nvmap_device) {
                        nv_result = 4; /* BadParameter */
                    } else {
                        channel->nvmap_fd = (int32_t)nvmap_fd;
                        channel->nvmap_set = 1;
                        slog(svc, "SVC_IPC_NVDRV command=1 call=SetNvmapFd fd=%u nvmap_fd=%u result=0\n",
                                fd, nvmap_fd);
                    }
                }
            } else if (supported && gpu_dev && ioctl == 0x40044803u) {
                /* NVGPU_IOCTL_CHANNEL_SET_TIMEOUT: {u32 timeout}. The kernel
                 * arms the channel job timeout; this loader has no kernel
                 * channel to program, so the requested value is recorded as
                 * an explicit unprogrammed setting. */
                if (!send_count || in_size < 4u
                    || !vp_guest_svc_readable(svc, in_address, 4u)) {
                    supported = 0;
                } else {
                    channel->timeout = get32(in_address);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=ChannelSetTimeout fd=%u timeout=0x%x model=recorded-no-device-programming result=0\n",
                            fd, channel->timeout);
                }
            } else if (supported && gpu_dev && ioctl == 0xc010480bu) {
                /* NVGPU_IOCTL_CHANNEL_ZCULL_BIND: {u64 gpu_va, u32 mode,
                 * pad}. The kernel binds the channel's ZCull context buffer;
                 * recorded only, because no ZCull engine exists here. The
                 * SDK declares this one _IOWR, like SET_ERROR_NOTIFIER. */
                if (!send_count || in_size < 0x10u
                    || !vp_guest_svc_readable(svc, in_address, 0x10u)) {
                    supported = 0;
                } else {
                    channel->zcull_gpu_va =
                        (uint64_t)get32(in_address)
                        | ((uint64_t)get32(in_address + 4) << 32);
                    channel->zcull_mode = get32(in_address + 8);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=ZCullBind fd=%u gpu_va=0x%llx mode=%u model=recorded-no-device-programming result=0\n",
                            fd, (unsigned long long)channel->zcull_gpu_va,
                            channel->zcull_mode);
                }
            } else if (supported && gpu_dev && ioctl == 0xc018480cu) {
                /* NVGPU_IOCTL_CHANNEL_SET_ERROR_NOTIFIER: {u64 offset,
                 * u64 size, u32 nvmap handle, pad}. The kernel installs the
                 * buffer a channel writes error records into. No channel
                 * executes and no error records can be produced, so the
                 * request is recorded as unprogrammed. */
                if (!send_count || in_size < 0x18u
                    || !vp_guest_svc_readable(svc, in_address, 0x18u)) {
                    supported = 0;
                } else {
                    channel->error_notifier_offset =
                        (uint64_t)get32(in_address)
                        | ((uint64_t)get32(in_address + 4) << 32);
                    channel->error_notifier_size =
                        (uint64_t)get32(in_address + 8)
                        | ((uint64_t)get32(in_address + 0xc) << 32);
                    channel->error_notifier_mem = get32(in_address + 0x10);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=SetErrorNotifier fd=%u offset=0x%llx size=0x%llx mem=%u model=recorded-no-device-programming result=0\n",
                            fd,
                            (unsigned long long)channel->error_notifier_offset,
                            (unsigned long long)channel->error_notifier_size,
                            channel->error_notifier_mem);
                }
            } else if (supported && gpu_dev && ioctl == 0x4004480du) {
                /* NVGPU_IOCTL_CHANNEL_SET_PRIORITY: {u32 priority}. */
                if (!send_count || in_size < 4u
                    || !vp_guest_svc_readable(svc, in_address, 4u)) {
                    supported = 0;
                } else {
                    channel->priority = get32(in_address);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=SetChannelPriority fd=%u priority=0x%x model=recorded-no-device-programming result=0\n",
                            fd, channel->priority);
                }
            } else if (supported && gpu_dev && ioctl == 0x4004481du) {
                /* NVGPU_IOCTL_CHANNEL_SET_TIMESLICE: {u32 timeslice}. */
                if (!send_count || in_size < 4u
                    || !vp_guest_svc_readable(svc, in_address, 4u)) {
                    supported = 0;
                } else {
                    channel->timeslice = get32(in_address);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=ChannelSetTimeslice fd=%u timeslice=0x%x model=recorded-no-device-programming result=0\n",
                            fd, channel->timeslice);
                }
            } else if (supported && gpu_dev && ioctl == 0x40084714u) {
                /* NVGPU_IOCTL_CHANNEL_SET_CLIENT_DATA: {u64 user_data}. The
                 * driver stores opaque data on its channel and can read it
                 * back; no device is programmed, so this is pure bookkeeping
                 * and GetClientData returns exactly this value. */
                if (!send_count || in_size < 8u
                    || !vp_guest_svc_readable(svc, in_address, 8u)) {
                    supported = 0;
                } else {
                    channel->client_data =
                        (uint64_t)get32(in_address)
                        | ((uint64_t)get32(in_address + 4) << 32);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=SetClientData fd=%u data=0x%llx model=recorded result=0\n",
                            fd, (unsigned long long)channel->client_data);
                }
            } else if (supported && gpu_dev && ioctl == 0xc020481au) {
                /* NVGPU_IOCTL_CHANNEL_ALLOC_GPFIFO_EX2:
                 * {u32 entries, u32 flags, u32 unk0, NvFence fence_out,
                 *  u32 unk1, u32 unk2, u32 unk3}. The kernel allocates the
                 * channel's command FIFO and returns the synchronous fence
                 * the guest uses for submissions. No device executes the
                 * FIFO here; the loader records its size and hands out a
                 * syncpoint allocated for this channel with fence value 0.
                 * Each channel gets a distinct id, as the kernel's syncpoint
                 * manager does; later submissions advance their channel's
                 * fence only when their commands are really consumed. */
                if (!send_count || !receive_count || in_size < 0x20u
                    || out_size < 0x20u
                    || !vp_guest_svc_readable(svc, in_address, 0x20u)
                    || !vp_guest_svc_writable(svc, out_address, 0x20u)) {
                    supported = 0;
                } else if (channel->gpfifo_allocated) {
                    nv_result = 0xd; /* AlreadyAllocated */
                } else {
                    channel->gpfifo_entries = get32(in_address);
                    channel->gpfifo_flags = get32(in_address + 4);
                    channel->gpfifo_allocated = 1;
                    channel->syncpoint = state->gpu_next_syncpoint++;
                    channel->fence_value = 0;
                    put32(out_address + 0x0c, channel->syncpoint);
                    put32(out_address + 0x10, channel->fence_value);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=AllocGPFIFOEx2 fd=%u entries=%u flags=0x%x unk0=0x%x syncpoint=%u fence=%u model=fifo-not-executed-syncpoint-allocated result=0\n",
                            fd, channel->gpfifo_entries,
                            channel->gpfifo_flags,
                            get32(in_address + 8), channel->syncpoint,
                            channel->fence_value);
                }
            } else if (supported && gpu_dev && ioctl == 0xc0104809u) {
                /* NVGPU_IOCTL_CHANNEL_ALLOC_OBJ_CTX: {u32 class, u32 flags,
                 * u64 obj_id}. The kernel returns an id for a GPU object of
                 * that class. No device object exists in this loader, so it
                 * records the class and returns a virtual id; the id only
                 * appears in command streams, which are not executed here. */
                if (!send_count || !receive_count || in_size < 0x10u
                    || out_size < 0x10u
                    || !vp_guest_svc_readable(svc, in_address, 0x10u)
                    || !vp_guest_svc_writable(svc, out_address, 0x10u)) {
                    supported = 0;
                } else if (channel->object_count
                           >= sizeof(channel->objects)
                              / sizeof(channel->objects[0])) {
                    ++svc->ipc_unsupported;
                    slog(svc, "SVC_IPC_NVDRV_OBJECT_LIMIT fd=%u count=%u\n",
                            fd, channel->object_count);
                    sflush(svc);
                    return VP_BRIDGE_UNSUPPORTED;
                } else {
                    uint32_t class_num = get32(in_address);
                    uint32_t flags = get32(in_address + 4);
                    uint32_t id = channel->next_object_id + 1u;
                    channel->next_object_id = id;
                    unsigned i = channel->object_count++;
                    channel->objects[i].class_num = class_num;
                    channel->objects[i].flags = flags;
                    channel->objects[i].id = id;
                    put32(out_address + 8, id);
                    put32(out_address + 0x0c, 0);
                    slog(svc, "SVC_IPC_NVDRV command=1 call=AllocateObjectContext fd=%u class=0x%x flags=0x%x id=%u model=virtual-no-device-object result=0\n",
                            fd, class_num, flags, id);
                }
            } else if (supported && nvmap_dev) {
                /* nvmap command group 1: Create(1), Alloc(4), Param(9). The
                 * client registers memory it already owns; the loader keeps
                 * the id/size/address table used by MapBufferEx later. */
                uint32_t group = (ioctl >> 8) & 0xffu;
                uint32_t cmd = ioctl & 0xffu;
                uint32_t encoded_size = (ioctl >> 16) & 0x3fffu;
                if (group != 1u) {
                    supported = 0;
                } else if (cmd == 1u && encoded_size == 8u) {
                    if (!send_count || !receive_count || in_size < 8u || out_size < 8u
                        || !vp_guest_svc_readable(svc, out_address, 8u)
                        || !vp_guest_svc_writable(svc, out_address, 8u)) {
                        supported = 0;
                    } else {
                        uint32_t requested = get32(out_address);
                        uint32_t id = 0;
                        VPNvMapResult created = vp_nvmap_table_create(
                            state->nvmap_handles, VP_GUEST_NVMAP_HANDLE_LIMIT,
                            &state->nvmap_handle_count, &state->nvmap_next_id,
                            requested, &id);
                        if (created == VP_NVMAP_FULL) {
                            ++svc->ipc_unsupported;
                            slog(svc, "SVC_IPC_NVDRV_NVMAP_LIMIT fd=%u count=%u\n",
                                    fd, state->nvmap_handle_count);
                            sflush(svc);
                            return VP_BRIDGE_UNSUPPORTED;
                        } else if (created == VP_NVMAP_ID_EXHAUSTED) {
                            ++svc->ipc_unsupported;
                            slog(svc, "SVC_IPC_NVDRV_NVMAP_ID_EXHAUSTED fd=%u next_id=%u\n",
                                    fd, state->nvmap_next_id);
                            sflush(svc);
                            return VP_BRIDGE_UNSUPPORTED;
                        } else if (created != VP_NVMAP_OK) {
                            ++svc->ipc_unsupported;
                            slog(svc, "SVC_IPC_NVDRV_NVMAP_CREATE_INVALID fd=%u result=%u\n",
                                    fd, (unsigned)created);
                            sflush(svc);
                            return VP_BRIDGE_UNSUPPORTED;
                        } else {
                            put32(out_address + 4, id);
                            slog(svc, "SVC_IPC_NVDRV command=1 call=nvmapCreate fd=%u size=0x%x handle=%u result=0\n",
                                    fd, requested, id);
                        }
                    }
                } else if (cmd == 4u && encoded_size == 0x20u) {
                    if (!send_count || !receive_count || in_size < 0x20u
                        || out_size < 0x20u
                        || !vp_guest_svc_readable(svc, out_address, 0x20u)
                        || !vp_guest_svc_writable(svc, out_address, 0x20u)) {
                        supported = 0;
                    } else {
                        uint32_t handle = get32(out_address);
                        uint32_t heap_mask = get32(out_address + 4);
                        uint32_t flags = get32(out_address + 8);
                        uint32_t align = get32(out_address + 0xc);
                        uint32_t kind = get32(out_address + 0x10) & 0xffu;
                        uint32_t address = get32(out_address + 0x18);
                        uint32_t address_high = get32(out_address + 0x1c);
                        VPGuestNvMapHandle *h = nvmap_find(state, handle);
                        if (!h) {
                            nv_result = 0xb; /* BadValue */
                        } else if (h->allocated) {
                            nv_result = 6; /* InsufficientMemory */
                        } else if ((align && (align & (align - 1u)))
                                   || !address || address_high
                                   || (h->size && !vp_guest_svc_readable(
                                           svc, address, (h->size + 0xfffu) & ~0xfffu))) {
                            nv_result = 0xb; /* BadValue */
                        } else {
                            h->flags = flags;
                            h->align = align < 0x1000u ? 0x1000u : align;
                            h->kind = kind;
                            h->address = address;
                            h->allocated = 1;
                            put32(out_address + 0xc, h->align);
                            slog(svc, "SVC_IPC_NVDRV command=1 call=nvmapAlloc fd=%u handle=%u heap=0x%x flags=0x%x align=0x%x kind=%u address=0x%08x size=0x%x result=0\n",
                                    fd, handle, heap_mask, flags, align, kind, address, h->size);
                        }
                    }
                } else if (cmd == 9u && encoded_size == 0x0cu) {
                    if (!send_count || !receive_count || in_size < 0x0cu
                        || out_size < 0x0cu
                        || !vp_guest_svc_readable(svc, out_address, 0x0cu)
                        || !vp_guest_svc_writable(svc, out_address, 0x0cu)) {
                        supported = 0;
                    } else {
                        uint32_t handle = get32(out_address);
                        uint32_t param = get32(out_address + 4);
                        VPGuestNvMapHandle *h = nvmap_find(state, handle);
                        uint32_t value = 0;
                        if (!h) nv_result = 0xb; /* BadValue */
                        else if (param == 1u) value = h->size;
                        else if (param == 2u) value = h->align;
                        else if (param == 3u) value = (uint32_t)-22; /* EINVAL */
                        else if (param == 4u) value = h->allocated ? 0x40000000u : 0;
                        else if (param == 5u) value = h->kind;
                        else if (param == 6u) value = 0;
                        else nv_result = 0xb; /* BadValue */
                        if (!nv_result) {
                            put32(out_address + 8, value);
                            slog(svc, "SVC_IPC_NVDRV command=1 call=nvmapParam fd=%u handle=%u param=%u value=0x%x result=0\n",
                                    fd, handle, param, value);
                        }
                    }
                } else if (cmd == 5u && encoded_size == 0x18u) {
                    /* nvmap Free(handle): retire the live record. The loader
                     * did not allocate the pages (the guest registered its own
                     * range in Alloc), so only the bookkeeping entry goes
                     * away and the reply reports the address/size/uncached
                     * flag the kernel would return. The freed slot is reused
                     * by a later Create, which always gets a new id; the old
                     * handle is no longer findable. */
                    if (!send_count || !receive_count || in_size < 0x18u
                        || out_size < 0x18u
                        || !vp_guest_svc_readable(svc, out_address, 0x18u)
                        || !vp_guest_svc_writable(svc, out_address, 0x18u)) {
                        supported = 0;
                    } else {
                        uint32_t handle = get32(out_address);
                        VPGuestNvMapHandle retired;
                        VPNvMapResult result = vp_nvmap_table_retire(
                            state->nvmap_handles, &state->nvmap_handle_count,
                            handle, &retired);
                        if (result != VP_NVMAP_OK) {
                            /* Unknown or already freed: reject, never a
                             * silent success. */
                            nv_result = 0xb; /* BadValue */
                        } else {
                            uint32_t address = retired.address;
                            uint32_t size = (retired.size + 0xfffu) & ~0xfffu;
                            uint32_t flags = retired.flags & 1u; /* map_uncached */
                            put32(out_address, 0);
                            put32(out_address + 8, address);
                            put32(out_address + 0xc, 0);
                            put32(out_address + 0x10, size);
                            put32(out_address + 0x14, flags);
                            slog(svc, "SVC_IPC_NVDRV command=1 call=nvmapFree fd=%u handle=%u address=0x%08x size=0x%x flags=0x%x allocated=%u result=0\n",
                                    fd, handle, address, size, flags,
                                    retired.allocated);
                        }
                    }
                } else if (cmd == 0xeu && encoded_size == 8u) {
                    /* nvmap GetId(handle): in this single-process model the
                     * handle and its global id are the same value. Validate
                     * the handle and report its id so a future consumer can
                     * name the buffer without reaching kernel state. */
                    if (!send_count || !receive_count || in_size < 8u
                        || out_size < 8u
                        || !vp_guest_svc_readable(svc, out_address, 8u)
                        || !vp_guest_svc_writable(svc, out_address, 8u)) {
                        supported = 0;
                    } else {
                        uint32_t handle = get32(out_address + 4);
                        VPGuestNvMapHandle *h = nvmap_find(state, handle);
                        if (!handle) {
                            nv_result = 0xb; /* BadValue */
                        } else if (!h) {
                            nv_result = 0x30010; /* AccessDenied */
                        } else {
                            put32(out_address, h->id);
                            slog(svc, "SVC_IPC_NVDRV command=1 call=nvmapGetId fd=%u handle=%u id=%u result=0\n",
                                    fd, handle, h->id);
                        }
                    }
                } else {
                    supported = 0;
                }
            } else {
                supported = 0;
            }
            if (!supported) {
                char dump[0x60];
                dump[0] = 0;
                if (send_count && in_address && in_size
                    && vp_guest_svc_readable(svc, in_address,
                                             in_size < 0x20u ? in_size : 0x20u)) {
                    uint32_t words = in_size < 0x20u ? in_size / 4u : 8u;
                    for (uint32_t i = 0; i < words; ++i)
                        snprintf(dump + strlen(dump), sizeof(dump) - strlen(dump),
                                 "%08x ", get32(in_address + i * 4u));
                }
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_NVDRV_IOCTL_UNSUPPORTED fd=%u device=\"%s\" ioctl=0x%08x in=%u@0x%08x out=%u@0x%08x initialized=%u args=%s\n",
                        fd, device_index >= 0
                            ? state->nvdrv_device_name[device_index] : "",
                        ioctl, in_size, in_address, out_size, out_address,
                        state->nvdrv_initialized, dump);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, (response_meta - (buffer + 8u)) / 4u + 4u + 1u);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            put32(response_meta + 0x10, nv_result);
            slog(svc, "SVC_IPC_NVDRV_REPLY hdr=0x%08x,0x%08x meta=0x%08x nv_result=%u\n",
                    get32(buffer), get32(buffer + 4), response_meta, nv_result);
            if (nv_result)
                slog(svc, "SVC_IPC_NVDRV_RESULT fd=%u device=\"%s\" ioctl=0x%08x nv_result=%u in=%u@0x%08x out=%u@0x%08x\n",
                        fd, device_name, ioctl, nv_result, in_size, in_address,
                        out_size, out_address);
            f->regs[0] = 0;
            sflush(svc);
            return 0;
        }
        if (command == 11) {
            /* nn::nvdrv Ioctl2: the raw parameters are the same (fd, ioctl) as
             * Ioctl, but the send list carries two buffers. The reviewed
             * caller is NvRmGpuChannelKickoffPbWithAttr with ioctl
             * 0xc018481b (24-byte in/out): send buffer 0 is the
             * IoctlSubmitGpfifo struct, send buffer 1 the inlined GPFIFO
             * entries, receive buffer 0 the kernel's updated struct. No GPU
             * executes the entries here. The loader performs the kernel's
             * submitted-fence bookkeeping (fence id = channel syncpoint;
             * max += 2 when fence_increment is set, plus the guest's
             * increment_value when requested) and feeds the resulting value to
             * the explicit submission-retirement model: the command stream is
             * not interpreted, but the fence the guest declared is exposed as
             * completed and wakes nvhost-ctrl waits. Rendering is NOT
             * performed; every retirement is logged with that model tag. */
            uint32_t fd = get32(request_meta + 0x10);
            uint32_t ioctl = get32(request_meta + 0x14);
            uint32_t in_size, in_address, inline_size, inline_address;
            uint32_t out_size, out_address;
            hipc_read_buffer(&layout, 0, &in_size, &in_address);
            hipc_read_buffer(&layout, 1, &inline_size, &inline_address);
            hipc_write_buffer(&layout, 0, &out_size, &out_address);
            int32_t device_index = -1;
            for (uint32_t i = 0; i < state->nvdrv_devices; ++i)
                if (state->nvdrv_device_fd[i] == (int32_t)fd) device_index = (int32_t)i;
            const char *device_name = device_index >= 0
                ? state->nvdrv_device_name[device_index] : "";
            int gpu_dev = device_index >= 0
                && !strcmp(state->nvdrv_device_name[device_index], "/dev/nvhost-gpu");
            if (ioctl != 0xc018481bu || !gpu_dev || !state->nvdrv_initialized
                || !inline_size || in_size < 0x18u
                || out_size < 0x18u || !vp_guest_svc_readable(svc, in_address, 0x18u)
                || !vp_guest_svc_writable(svc, out_address, 0x18u)) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_NVDRV_IOCTL2_UNSUPPORTED fd=%u device=\"%s\" ioctl=0x%08x send=%u recv=%u in=%u@0x%08x inline=%u@0x%08x out=%u@0x%08x initialized=%u\n",
                        fd, device_name, ioctl, send_count, receive_count,
                        in_size, in_address, inline_size, inline_address,
                        out_size, out_address, state->nvdrv_initialized);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            uint64_t address = (uint64_t)get32(in_address)
                | ((uint64_t)get32(in_address + 4) << 32);
            uint32_t entries = get32(in_address + 8);
            uint32_t flags = get32(in_address + 0x0c);
            uint32_t fence_id = get32(in_address + 0x10);
            uint32_t fence_value = get32(in_address + 0x14);
            uint32_t gpfifo_bytes = entries * 8u;
            if (!entries || entries > 0x10000u || gpfifo_bytes > inline_size
                || !vp_guest_svc_readable(svc, inline_address, gpfifo_bytes)) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_NVDRV_GPFIFO_INVALID fd=%u entries=%u inline=%u@0x%08x out=%u@0x%08x\n",
                        fd, entries, inline_size, inline_address, out_size, out_address);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            VPGuestGpuChannel *channel = vp_guest_gpu_channel(state, fd);
            if (!channel) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_NVDRV_CHANNEL_LIMIT fd=%u count=%u\n",
                        fd, state->gpu_channel_count);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            uint32_t increment = ((flags & 2u) ? 2u : 0u)
                + ((flags & 0x100u) ? fence_value : 0u);
            uint32_t new_fence = channel->fence_value + increment;
            /* Submission bookkeeping is visible to the decode (submission
             * number, entry layout) but the public fence is not retired until
             * the decoded GPU work is flushed below. */
            ++channel->submit_count;
            channel->submit_address = address;
            channel->submit_entries = entries;
            channel->submit_flags = flags;
            channel->submit_fence_id = fence_id;
            channel->submit_fence_value = fence_value;
            channel->submit_inline_size = inline_size;
            channel->submit_inline_address = inline_address;
            uint64_t first = (uint64_t)get32(inline_address)
                | ((uint64_t)get32(inline_address + 4) << 32);
            uint32_t first_va = (uint32_t)(first & 0xFFFFFFFFFFull);
            uint32_t first_words = (uint32_t)((first >> 42) & 0x1FFFFFull);
            uint32_t first_nonmain = (uint32_t)((first >> 41) & 1u);
            slog(svc, "SVC_IPC_NVDRV command=11 call=SubmitGPFIFOInlined fd=%u entries=%u flags=0x%x req_fence=%u:%u entries_at=0x%08x first=0x%llx gpu_va=0x%x words=0x%x nonmain=%u syncpoint=%u new_fence=%u submission=%u model=flush-before-fence-retirement result=0\n",
                    fd, entries, flags, fence_id, fence_value,
                    inline_address, (unsigned long long)first, first_va,
                    first_words, first_nonmain, channel->syncpoint,
                    new_fence, channel->submit_count);
            /* Record the entry layout of every submission (metadata only, no
             * command words) so the frame profile is visible without a dump
             * window: the sizes field is the per-entry word count. */
            {
                char sizes[64];
                size_t at = 0;
                sizes[0] = 0;
                for (uint32_t i = 0; i < entries && i < 8u
                    && at + 8u < sizeof(sizes); ++i) {
                    uint64_t raw = (uint64_t)get32(inline_address + i * 8u)
                        | ((uint64_t)get32(inline_address + i * 8u + 4u) << 32);
                    uint32_t w = (uint32_t)((raw >> 42) & 0x1FFFFFull);
                    int n = snprintf(sizes + at, sizeof(sizes) - at, "%s%u",
                            i ? "," : "", w);
                    if (n < 0) break;
                    at += (size_t)n;
                }
                slog(svc, "SVC_IPC_NVDRV_SUB fd=%u submission=%u entries=%u sizes=%s\n",
                        fd, channel->submit_count, entries, sizes);
            }
            /* Temporary diagnosis: translate each inlined GPFIFO entry back
             * through the recorded MapBufferEx mappings and dump the command
             * words, so the semaphore/report writes the retired stream would
             * have performed can be read without executing any command. */
            /* Sample the first submissions of a channel and every 128th after
             * that, so a late frame can be compared against the opening ones
             * without dumping every frame. */
            const int dump_this_submission = channel->submit_count <= 3u
                || (channel->submit_count % 128u) == 0u;
            for (uint32_t entry_index = 0;
                entry_index < entries; ++entry_index) {
                uint64_t raw = (uint64_t)get32(inline_address + entry_index * 8u)
                    | ((uint64_t)get32(inline_address + entry_index * 8u + 4u) << 32);
                uint64_t entry_va = raw & 0xFFFFFFFFFFull;
                uint32_t entry_words = (uint32_t)((raw >> 42) & 0x1FFFFFull);
                uint32_t entry_nonmain = (uint32_t)((raw >> 41) & 1u);
                uint32_t cpu = 0;
                const int readable = entry_words != 0u
                    && gpu_va_to_cpu(state,entry_va,(uint64_t)entry_words*4u,&cpu)
                    && vp_guest_svc_readable(svc, cpu, entry_words * 4u);
                /* Observation only: decode the entry into draw snapshots on
                 * every submission. The sampled block below still dumps the
                 * raw words for the first submissions and every 128th. */
                if (readable) {
                    if (vp_guest_gpu_decode_entry(svc, state, channel, fd,
                            channel->submit_count, cpu, entry_words,
                            dump_this_submission) < 0) {
                        sflush(svc);
                        return VP_BRIDGE_UNSUPPORTED;
                    }
                } else {
                    ++svc->ipc_unsupported;
                    slog(svc,"VP_GPU_ENTRY_REJECT fd=%u submission=%u entry=%u va=0x%llx words=%u reason=unmapped-or-empty\n",
                         fd,channel->submit_count,entry_index,(unsigned long long)entry_va,entry_words);
                }
                if (!dump_this_submission) continue;
                slog(svc, "SVC_IPC_NVDRV_CMD entry=%u va=0x%llx words=0x%x nonmain=%u cpu=0x%08x readable=%u\n",
                        entry_index, (unsigned long long)entry_va, entry_words,
                        entry_nonmain, cpu, readable ? 1u : 0u);
                if (!readable)
                    continue;
                /* Locate references to the 4 KiB page the driver mapped at GPU
                 * VA 0x500000000; a semaphore release must name an address in
                 * that page. Print the reference and its neighbouring words. */
                for (uint32_t w = 0; w + 1u < entry_words; ++w) {
                    uint64_t v = (uint64_t)get32(cpu + w * 4u)
                        | ((uint64_t)get32(cpu + w * 4u + 4u) << 32);
                    if (v < 0x500000000ull || v >= 0x500001000ull) continue;
                    uint32_t from = w > 8u ? w - 8u : 0u;
                    uint32_t to = w + 9u < entry_words ? w + 9u : entry_words;
                    slog(svc, "SVC_IPC_NVDRV_SEMREF entry=%u word=%u value=0x%llx\n",
                            entry_index, w, (unsigned long long)v);
                    for (uint32_t q = from; q < to; q += 8u) {
                        slog(svc, "SVC_IPC_NVDRV_SEMW %u:", q);
                        for (uint32_t j = 0; j < 8u && q + j < to; ++j)
                            slog(svc, " %08x", get32(cpu + (q + j) * 4u));
                        slog(svc, "\n");
                    }
                }
                /* The channel-init entry (0x88A words) carries the shader
                 * program upload, so it is dumped in full as well: the frame
                 * streams bind programs by address and the program images only
                 * exist here. The head below is a fallback for anything the
                 * full dump would be too large for. */
                if (entry_words > 0x2000u) {
                    for (uint32_t w = 0; w < 64u && w < entry_words; w += 8u) {
                        slog(svc, "SVC_IPC_NVDRV_HEAD %u:", w);
                        for (uint32_t j = 0; j < 8u && w + j < entry_words; ++j)
                            slog(svc, " %08x", get32(cpu + (w + j) * 4u));
                        slog(svc, "\n");
                    }
                    continue;
                }
                for (uint32_t w = 0; w < entry_words; w += 8u) {
                    slog(svc, "SVC_IPC_NVDRV_CMD_W %u:", w);
                    for (uint32_t j = 0; j < 8u && w + j < entry_words; ++j)
                        slog(svc, " %08x", get32(cpu + (w + j) * 4u));
                    slog(svc, "\n");
                }
            }
            /* Replay only the register writes the sampled window contains for
             * vertex_streams (0x700..0x740, four words per stream: format,
             * address_high, address_low, frequency) and dump the memory those
             * streams point at. Vertex streams are the one resource a draw
             * cannot be translated without; everything else the draw reads
             * (shaders, const buffers) is identified from the same window by
             * the offline tools. This runs even when the window itself was not
             * dumped, so the address format is visible for every size profile. */
            /* Shader bindings are tracked on EVERY submission, not only the
             * sampled ones: a vertex program is typically bound once early and
             * never rebound, so sampling would never reveal it. */
            gpu_track_shader_binds(svc, state, fd, channel->submit_count,
                    inline_address, entries);
            if (dump_this_submission) {
                gpu_dump_stream_state(svc, state, fd, channel->submit_count,
                        inline_address, entries);
                gpu_dump_shader_programs(svc, state, fd, channel->submit_count,
                        inline_address, entries);
            }
            /* All draws decoded for this submission are encoded but not
             * necessarily GPU-complete. Flush the bounded scene before the
             * fence is retired, so guest-visible completion always implies the
             * GPU work and its resource reads finished. */
            {
                int flush_rc = guest_gxm_request(svc, 5, NULL, 0);
                if (flush_rc < 0) {
                    slog(svc, "VP_GXM_BATCH_SUBMIT_FLUSH fd=%u submission=%u "
                         "rc=%d action=fence-not-retired\n",
                         fd, channel->submit_count, flush_rc);
                    sflush(svc);
                    return VP_BRIDGE_UNSUPPORTED;
                }
                slog(svc, "VP_GXM_BATCH_SUBMIT_FLUSH fd=%u submission=%u "
                     "rc=0 action=fence-eligible\n",
                     fd, channel->submit_count);
            }
            channel->fence_value = new_fence;
            /* The submission is accepted into the modeled command processor
             * only after all decoded GPU work completed. Its declared fence
             * value becomes the channel syncpoint's completed value and
             * completes any armed nvhost-ctrl wait at or below it. */
            nvctrl_advance_syncpoint(svc, state, channel->syncpoint, new_fence);
            /* The SDK's NvRm sync layer allocates a 4 KiB nvmap buffer and maps
             * it into the GPU at virtual address 0x500000000 during channel
             * setup. Its fence wait polls a 32-bit value at
             * page + syncpoint*16. Publish the retired value only after the
             * batch flush above, and never beyond the retired fence. */
            for (uint32_t m = 0; m < state->gpu_as.mapping_count; ++m) {
                const uint64_t page_va = 0x500000000ull;
                if (state->gpu_as.mappings[m].offset != page_va
                    || state->gpu_as.mappings[m].size < 0x1000u
                    || state->gpu_as.mappings[m].kind != 0)
                    continue;
                uint32_t page = state->gpu_as.mappings[m].device_address;
                uint32_t at = page + channel->syncpoint * 16u;
                if (!vp_guest_svc_writable(svc, at, 4)) {
                    slog(svc, "SVC_IPC_NVDRV_SYNCPAGE_UNWRITABLE fd=%u at=0x%08x syncpoint=%u value=%u\n",
                            fd, at, channel->syncpoint, new_fence);
                    break;
                }
                put32(at, new_fence);
                slog(svc, "SVC_IPC_NVDRV_SYNCPAGE fd=%u page=0x%08x syncpoint=%u value=%u model=post-flush-kernel-syncpoint-retirement\n",
                        fd, page, channel->syncpoint, new_fence);
                break;
            }
            /* Return the struct the kernel would return: the submitted
             * command-list address, one entry count, cleared flags, the
             * channel syncpoint id and the newly submitted fence value. */
            put32(out_address, get32(in_address));
            put32(out_address + 4, get32(in_address + 4));
            put32(out_address + 8, entries);
            put32(out_address + 0x0c, 0);
            put32(out_address + 0x10, channel->syncpoint);
            put32(out_address + 0x14, new_fence);
            f->regs[0] = 0;
            sflush(svc);
            return 0;
        }
        if (command == 4) {
            /* nn::nvdrv QueryEvent(fd, event_id): the reply carries one copy
             * handle for the device event and an NvResult. The loader gives
             * the guest a real unsignaled kernel event object; nothing in
             * this platform signals nvdrv events yet, so a wait stays
             * pending instead of returning a fabricated signal. */
            uint32_t fd = get32(request_meta + 0x10);
            uint32_t event_id = get32(request_meta + 0x14);
            int32_t device_index = -1;
            for (uint32_t i = 0; i < state->nvdrv_devices; ++i)
                if (state->nvdrv_device_fd[i] == (int32_t)fd) device_index = (int32_t)i;
            uint32_t event = 0;
            if (device_index >= 0)
                event = create_handle(svc, 3, "nvdrv", event_id);
            if (!event) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_NVDRV_QUERY_EVENT_FAILED fd=%u event_id=%u device_index=%d\n",
                        fd, event_id, device_index);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            /* Out-native-handle reply with no domain header: HIPC special
             * word (Pid + one copy handle) at buffer+8, pid words at
             * buffer+0xc/0x10, the handle at buffer+0x14, and the CMIF
             * response at align16(buffer + 6*4 + 0xf). */
            uint32_t response_meta = (buffer + 6u * 4u + 0x0fu) & ~0x0fu;
            put32(buffer, 0);
            put32(buffer + 4, 0x80000006u); /* handle, CMIF header, NvResult */
            put32(buffer + 8, 3);           /* Pid present, one copy handle */
            put32(buffer + 0xc, 0);
            put32(buffer + 0x10, 0);
            put32(buffer + 0x14, event);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            put32(response_meta + 0x10, 0); /* NvResult::Success */
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_NVDRV command=4 call=QueryEvent fd=%u event_id=%u event=0x%08x meta=0x%08x result=0\n",
                    fd, event_id, event, response_meta);
            sflush(svc);
            return 0;
        }
        if (command == 3) {
            uint32_t process = copies >= 1 ? get32(handle_base) : 0;
            uint32_t transfer = copies >= 2 ? get32(handle_base + 4) : 0;
            uint32_t size = get32(request_meta + 0x10);
            VPGuestHandle *memory = find_handle(svc, transfer);
            uint32_t result = 0;
            if (!copies || (process != 0xffff8001u && process != 0xffff8000u))
                result = SVC_RESULT_INVALID_HANDLE;
            else if (!memory || memory->kind != VP_GUEST_HANDLE_KIND_TRANSFER
                     || size != memory->transfer_size)
                result = SVC_RESULT_INVALID_COMBINATION;
            if (!result) {
                state->nvdrv_initialized = 1;
                state->nvdrv_process_handle = process;
                state->nvdrv_transfer_handle = transfer;
                state->nvdrv_transfer_size = size;
            }
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, (response_meta - (buffer + 8u)) / 4u + 4u + 1u);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, result);
            put32(response_meta + 0xc, 0);
            put32(response_meta + 0x10, result);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_NVDRV command=3 call=Initialize process=0x%08x transfer=0x%08x size=0x%08x result=0x%08x meta=0x%08x\n",
                    process, transfer, size, result, response_meta);
            sflush(svc);
            return 0;
        }
        if (command == 8) {
            /* SetAruid: the PID descriptor (special-header PID, zero means
             * omitted) and the 64-bit AppletResourceUserId payload. The
             * driver only records the id for scheduling; the loader keeps it
             * on the session so later ioctls can be attributed. */
            uint64_t aruid = (uint64_t)get32(request_meta + 0x10)
                | ((uint64_t)get32(request_meta + 0x14) << 32);
            state->nvdrv_pid = descriptor_pid;
            state->nvdrv_aruid = aruid;
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, (response_meta - (buffer + 8u)) / 4u + 4u + 1u);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            put32(response_meta + 0x10, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_NVDRV command=8 call=SetAruid pid=0x%llx aruid=0x%llx initialized=%u result=0 meta=0x%08x\n",
                    (unsigned long long)descriptor_pid, (unsigned long long)aruid,
                    state->nvdrv_initialized, response_meta);
            sflush(svc);
            return 0;
        }
        if (command == 13) {
            /* SetGraphicsFirmwareMemoryMarginEnabled(u64 enabled): the driver
             * only remembers the flag. No output payload. */
            state->nvdrv_firmware_margin = get32(request_meta + 0x10);
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, (response_meta - (buffer + 8u)) / 4u + 4u);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_NVDRV command=13 call=SetGraphicsFirmwareMemoryMarginEnabled enabled=%u result=0 meta=0x%08x\n",
                    state->nvdrv_firmware_margin, response_meta);
            sflush(svc);
            return 0;
        }
        ++svc->ipc_unsupported;
        slog(svc, "SVC_IPC_NVDRV_UNSUPPORTED command=%u initialized=%u\n",
                command, state->nvdrv_initialized);
        sflush(svc);
        return VP_BRIDGE_UNSUPPORTED;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "set")) {
        /* nn::settings::ISettingsServer is used through the stateless CMIF
         * proxy, not a domain proxy. The reviewed generated wrappers at
         * sdk+0x18e4b8..0x18e558 give the commands: 0 GetLanguageCode,
         * 1 GetAvailableLanguageCodes, 3 GetAvailableLanguageCodeCount,
         * 4 GetRegionCode, 5 GetAvailableLanguageCodes2, 6
         * GetAvailableLanguageCodeCount2. This platform models one language,
         * en-US, for the US title and region America. GetLanguageCode returns
         * the eight-byte LanguageCode; the array forms write it into the
         * guest OutArray buffer described by HIPC word 2/3. */
        static const uint32_t language_words[2] = {
            0x552d6e65u /* "en-U" */, 0x00000053u /* "S\0\0\0" */,
        };
        uint32_t out_words[2] = {0, 0};
        uint32_t out_count = 0;
        uint32_t result = 0;
        uint32_t descriptor = (header0 & 0x01000000u) != 0;
        uint32_t descriptor_size = get32(buffer + 8);
        uint32_t descriptor_address = get32(buffer + 0xc);
        if (command == 0) {
            out_words[0] = language_words[0];
            out_words[1] = language_words[1];
            out_count = 2;
        } else if (command == 1 || command == 5) {
            if (!descriptor || descriptor_size < 8
                || !vp_guest_svc_writable(svc, descriptor_address, 8)) {
                result = SVC_RESULT_INVALID_COMBINATION;
            } else {
                put32(descriptor_address, language_words[0]);
                put32(descriptor_address + 4, language_words[1]);
                out_words[0] = 1;
                out_count = 1;
            }
        } else if (command == 3 || command == 6) {
            out_words[0] = 1; /* one modeled language */
            out_count = 1;
        } else if (command == 4) {
            out_words[0] = 1; /* America */
            out_count = 1;
        } else {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_SETTINGS_UNSUPPORTED command=%u\n", command);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        /* Mirror the request's HIPC shape so the stateless reader derives the
         * same CMIF offset: header word 1 counts every word from buffer+8 to
         * the end of the CMIF payload. */
        put32(buffer, descriptor ? 0x01000004u : 0u);
        put32(buffer + 4,
             (request_meta - (buffer + 8u)) / 4u + 4u + out_count);
        put32(request_meta, 0x4F434653u);
        put32(request_meta + 4, 0);
        put32(request_meta + 8, result);
        put32(request_meta + 0xc, 0);
        for (uint32_t i = 0; i < out_count; ++i)
            put32(request_meta + 0x10 + i * 4, out_words[i]);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_SETTINGS command=%u call=%s count=%u result=0x%08x meta=0x%08x\n",
             command,
             command == 0 ? "GetLanguageCode"
                 : command == 1 ? "GetAvailableLanguageCodes"
                 : command == 3 ? "GetAvailableLanguageCodeCount"
                 : command == 4 ? "GetRegionCode"
                 : command == 5 ? "GetAvailableLanguageCodes2"
                 : "GetAvailableLanguageCodeCount2",
             out_count ? out_words[0] : 0, result, request_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "audren:u") && command == 1u) {
        /* nn::audio::detail::IAudioRendererManager command 1 is
         * GetWorkBufferSize(AudioRendererParameter) -> Out<u64>. The value is
         * a pure function of the locked SDK's server-side BehaviorInfo and
         * the parameter counts, so execute the original
         * AudioRenderSystem::GetWorkBufferSize on a spare executor instead of
         * duplicating its constants. The request carries the parameter inline
         * after the CMIF header; no descriptor or handle is involved. */
        uint32_t size = 0;
        int calc = svc->guest_call && svc->audio_work_size_fn
            ? svc->guest_call(svc->guest_call_user, 3, svc->audio_work_size_fn,
                              request_meta + 0x10, &size)
            : VP_BRIDGE_UNSUPPORTED;
        if (calc) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_AUDIO_WORK_SIZE_UNSUPPORTED address=0x%08x status=%d\n",
                    svc->audio_work_size_fn, calc);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        uint32_t response_meta = plain_response_meta(buffer);
        put32(buffer, 0);
        put32(buffer + 4, (response_meta - (buffer + 8u)) / 4u + 4u + 2u);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        put32(response_meta + 0x10, size);
        put32(response_meta + 0x14, 0);
        state->audio_work_size = size;
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_AUDIO command=1 call=GetWorkBufferSize size=0x%x model=original-sdk-calculation meta=0x%08x result=0\n",
                size, response_meta);
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "audren:u") && command == 0) {
        /* nn::audio::detail::IAudioRendererManager command 0 OpenAudioRenderer(
         * const AudioRendererParameterInternal&, NativeHandle tmem,
         * NativeHandle process, u64 tmem_size, AppletResourceUserId). The
         * locked client declares a process id and two copy handles, so the
         * special-data words are the pid u64 followed by the transfer-memory
         * handle and the current-process pseudo handle; those words sit at
         * fixed offsets from the buffer base (word 2 special, words 3-4 pid,
         * words 5-6 handles) while the CMIF header is 16-byte aligned after
         * them, so its distance from the handles depends on the TLS buffer's
         * 8/0 mod 16 alignment. The inline arguments follow the CMIF header
         * at their natural alignment. The out
         * SharedPointer<IAudioRenderer> is an OutInterface, which a
         * non-domain reply transports as one move handle. The reply uses the
         * reviewed hid CreateAppletResource shape: pid bit plus one move
         * handle, handle word at +0x14 and CMIF header at +0x18. */
        uint32_t special = get32(buffer + 8);
        if (!(header1 & 0x80000000u) || special != 5u
            || request_meta < buffer + 0x20u) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_AUDIO_OPEN_UNSUPPORTED header1=0x%08x special=0x%x meta=0x%08x\n",
                    header1, special, request_meta);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        uint32_t tmem = get32(buffer + 0x14u);
        uint32_t process = get32(buffer + 0x18u);
        slog(svc, "SVC_IPC_AUDIO_OPEN_ENTER buffer=0x%08x meta=0x%08x special=0x%x tmem=0x%08x process=0x%08x\n",
                buffer, request_meta, special, tmem, process);
        sflush(svc);
        VPGuestHandle *tmem_handle = find_handle(svc, tmem);
        if (!tmem_handle || tmem_handle->kind != VP_GUEST_HANDLE_KIND_TRANSFER
            || process != HORIZON_CURRENT_PROCESS) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_AUDIO_OPEN_UNSUPPORTED tmem=0x%08x kind=%u process=0x%08x\n",
                    tmem, tmem_handle ? tmem_handle->kind : 0u, process);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        uint32_t param = request_meta + 0x10u;
        uint32_t sample_rate = get32(param + 0x00u);
        uint32_t sample_count = get32(param + 0x04u);
        uint32_t mixes = get32(param + 0x08u);
        uint32_t sub_mixes = get32(param + 0x0cu);
        uint32_t voices = get32(param + 0x10u);
        uint32_t sinks = get32(param + 0x14u);
        uint32_t effects = get32(param + 0x18u);
        uint32_t perf_frames = get32(param + 0x1cu);
        uint32_t splitter_infos = get32(param + 0x24u);
        uint32_t splitter_destinations = get32(param + 0x28u);
        uint32_t external_context_size = get32(param + 0x2cu);
        uint32_t execution_mode = get32(param + 0x20u) >> 24;
        uint32_t revision = get32(param + 0x30u);
        uint32_t tmem_size = get32(request_meta + 0x48u);
        if (!sample_rate || !sample_count || !mixes
            || execution_mode > 1u
            || (state->audio_work_size && tmem_size < state->audio_work_size)) {
            ++svc->ipc_unsupported;
            slog(svc, "SVC_IPC_AUDIO_OPEN_INVALID rate=%u count=%u mixes=%u mode=%u tmem_size=0x%x work=0x%x\n",
                    sample_rate, sample_count, mixes, execution_mode, tmem_size,
                    state->audio_work_size);
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        uint32_t renderer = create_handle(svc, 2, "renderer", 0);
        if (!renderer) {
            uint32_t response_meta = plain_out_handle_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, 0x8000000Au);
            put32(buffer + 8, 0x21u);
            put32(buffer + 0xc, 0);
            put32(buffer + 0x10, 0);
            put32(buffer + 0x14, 0);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 8, SVC_RESULT_OUT_OF_MEMORY);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_AUDIO_OPEN_LIMIT tmem=0x%08x result=0x%08x\n",
                    tmem, SVC_RESULT_OUT_OF_MEMORY);
            sflush(svc);
            return 0;
        }
        VPGuestSession *renderer_state = handle_session(svc, find_handle(svc, renderer));
        if (!renderer_state) {
            ++svc->ipc_unsupported;
            sflush(svc);
            return VP_BRIDGE_UNSUPPORTED;
        }
        renderer_state->audio_sample_rate = sample_rate;
        renderer_state->audio_sample_count = sample_count;
        renderer_state->audio_mix_buffer_count = mixes;
        renderer_state->audio_voice_count = voices;
        renderer_state->audio_sink_count = sinks;
        renderer_state->audio_effect_count = effects;
        renderer_state->audio_revision = revision;
        renderer_state->audio_execution_mode = execution_mode;
        renderer_state->audio_active = 0;
        renderer_state->audio_rendering_time_limit = 100;
        renderer_state->audio_system_event = 0;
        renderer_state->audio_tmem_handle = tmem;
        renderer_state->audio_tmem_size = tmem_size;
        uint32_t handle_offset = (3u + 2u) * 4u;
        uint32_t response_meta = (buffer + handle_offset + 4u + 0xfu) & ~0xfu;
        put32(buffer, 0);
        put32(buffer + 4, 0x8000000Au); /* special header, raw words */
        put32(buffer + 8, 0x21u);       /* pid word, one move handle */
        put32(buffer + 0xc, 0);         /* pid low: not needed by the loader */
        put32(buffer + 0x10, 0);        /* pid high */
        put32(buffer + handle_offset, renderer);
        put32(buffer + 0x18, 0);
        put32(buffer + 0x1c, 0);
        put32(buffer + 0x20, 0);
        put32(buffer + 0x24, 0);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, 0);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        slog(svc, "SVC_IPC_AUDIO_OPEN_REPLY renderer=0x%08x meta=0x%08x\n",
                renderer, response_meta);
        sflush(svc);
        slog(svc, "SVC_IPC_AUDIO command=0 call=OpenAudioRenderer renderer=0x%08x tmem=0x%08x tmem_size=0x%x rate=%u count=%u mixes=%u sub_mixes=%u voices=%u sinks=%u effects=%u perf_frames=%u splitter_infos=%u splitter_dests=%u external_ctx=0x%x mode=%u mode_name=%s revision=0x%08x model=silent-renderer meta=0x%08x result=0\n",
                renderer, tmem, tmem_size, sample_rate, sample_count, mixes,
                sub_mixes, voices, sinks, effects, perf_frames, splitter_infos,
                splitter_destinations, external_context_size, execution_mode,
                execution_mode == 0u ? "auto" : "manual",
                revision, response_meta);
        /* M5 assertion harness: the title's successful OpenAudioRenderer is
         * the point where the locked SDK's audio library is initialized.
         * Ask the loader to create the probe thread once; the loader checks
         * its own marker file and never touches game logic. */
        if (svc->audio_probe_request && !svc->audio_probe_requested) {
            svc->audio_probe_requested = 1;
            svc->audio_probe_request(svc->audio_probe_request_user);
        }
        sflush(svc);
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "audren:u") && command == 4) {
        /* IAudioRendererManager command 4
         * GetAudioDeviceServiceWithRevisionInfo(Out<SharedPointer<IAudioDevice>>,
         * u32 revision, AppletResourceUserId). The device object is another
         * OutInterface, transported as one move handle exactly like
         * OpenAudioRenderer. */
        uint32_t revision = get32(request_meta + 0x10u);
        uint64_t aruid = (uint64_t)get32(request_meta + 0x18u)
            | ((uint64_t)get32(request_meta + 0x1cu) << 32);
        uint32_t device = create_handle(svc, 2, "aud-dev", 0);
        uint32_t response_meta = plain_out_handle_meta(buffer);
        put32(buffer, 0);
        put32(buffer + 4, 0x8000000Au);
        put32(buffer + 8, 0x21u);
        put32(buffer + 0xc, 0);
        put32(buffer + 0x10, 0);
        put32(buffer + 0x14, device);
        put32(buffer + 0x18, 0);
        put32(buffer + 0x1c, 0);
        put32(buffer + 0x20, 0);
        put32(buffer + 0x24, 0);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, device ? 0u : SVC_RESULT_OUT_OF_MEMORY);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        if (device) {
            VPGuestSession *device_state = handle_session(svc, find_handle(svc, device));
            if (device_state) {
                device_state->audio_device_revision = revision;
                device_state->audio_device_aruid = aruid;
                device_state->audio_device_volume_bits = 0x3F800000u;
            }
        }
        /* This manager call is issued once per audio update in the real
         * game (run10). Keep the exact failure line; bound only the
         * successful per-request line and carry the per-command counters. */
        int emit_device_line = 1;
        if (state && device)
            emit_device_line = vp_audio_log_cadence_should_emit(
                &state->audio_manager_get_device_log, 4u, 64u);
        if (emit_device_line) {
            uint32_t c0 = state ? state->audio_manager_cmd_counts[0] : 0u;
            uint32_t c1 = state ? state->audio_manager_cmd_counts[1] : 0u;
            uint32_t c2 = state ? state->audio_manager_cmd_counts[2] : 0u;
            uint32_t c3 = state ? state->audio_manager_cmd_counts[3] : 0u;
            uint32_t c4 = state ? state->audio_manager_cmd_counts[4] : 0u;
            uint32_t c5 = state ? state->audio_manager_cmd_counts[5] : 0u;
            uint32_t c6 = state ? state->audio_manager_cmd_counts[6] : 0u;
            uint32_t c7 = state ? state->audio_manager_cmd_counts[7] : 0u;
            slog(svc, "SVC_IPC_AUDIO command=4 call=GetAudioDeviceServiceWithRevisionInfo device=0x%08x revision=0x%08x aruid=0x%llx model=no-audio-sink meta=0x%08x result=%u cmd_counts=%u,%u,%u,%u,%u,%u,%u,%u\n",
                    device, revision, (unsigned long long)aruid, response_meta,
                    device ? 0u : SVC_RESULT_OUT_OF_MEMORY,
                    c0, c1, c2, c3, c4, c5, c6, c7);
            sflush(svc);
        }
        return 0;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "renderer")) {
        /* nn::audio::detail::IAudioRenderer command set. The renderer owns no
         * DSP and no audio device here, so it is a silent consumer: updates
         * zero the client's output buffer (the reference clears the output
         * before decoding the command list) and complete immediately, which
         * is signalled through the QuerySystemEvent event. All other
         * commands answer from the parameters recorded at open time. An
         * unknown command or a malformed buffer stops the run. */
        if (command == 0u || command == 1u || command == 2u || command == 3u
            || command == 9u || command == 13u) {
            uint32_t value = 0;
            const char *call = command == 0u ? "GetSampleRate"
                : command == 1u ? "GetSampleCount"
                : command == 2u ? "GetMixBufferCount"
                : command == 3u ? "GetState"
                : command == 9u ? "GetRenderingTimeLimit" : "GetVoiceDropParameter";
            if (command == 0u) value = state->audio_sample_rate;
            else if (command == 1u) value = state->audio_sample_count;
            else if (command == 2u) value = state->audio_mix_buffer_count;
            else if (command == 3u) value = state->audio_active ? 0u : 2u;
            else if (command == 9u) value = state->audio_rendering_time_limit;
            else value = state->audio_voice_drop_bits;
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, 5); /* CMIF header and one out word */
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            put32(response_meta + 0x10, value);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_RENDERER command=%u call=%s value=0x%08x model=silent-renderer meta=0x%08x result=0\n",
                    command, call, value, response_meta);
            sflush(svc);
            return 0;
        }
        if (command == 4u) {
            /* RequestUpdate(out_buffer, out_performance_buffer, input). The
             * descriptors travel as map-alias entries; the two output
             * buffers are zeroed to silence. The platform has no audio sink,
             * so the update still consumes one buffer per nominal audio
             * period (sample_count / sample_rate): the render event is armed
             * with that deadline and reported signaled once it passes. An
             * instantaneous signal would let the SDK's render thread spin at
             * CPU speed instead of the device's clock. */
            VPGuestHipcLayout layout;
            hipc_layout(buffer, header0, header1, &layout);
            /* Bring-up diagnostic: the A descriptor is the InBuffer carrying
             * the SDK-generated command list. Dump the first words of the
             * first few updates (and periodically afterwards) so the
             * command encoding can be reviewed from the locked client's own
             * output before any interpretation is implemented. Gated by a
             * pref marker and bounded. */
            uint32_t cmd_size = 0, cmd_address = 0;
            hipc_read_buffer(&layout, 0, &cmd_size, &cmd_address);
            /* Like the other diagnostic gates, this is a launch-time option.
             * Reopening a missing marker on every audio period produces two
             * Vita3K ENOENT errors per update and unnecessary filesystem I/O. */
            static int cmd_dump_state = -1;
            if (cmd_dump_state < 0) {
                FILE *cmd_marker = fopen("ux0:data/mhgu-vita-port/audio-command-dump.enable", "rb");
                cmd_dump_state = cmd_marker != NULL;
                if (cmd_marker) fclose(cmd_marker);
                slog(svc, "VP_AUDIO_COMMAND_DUMP enabled=%d scope=launch\n",cmd_dump_state);
            }
            int cmd_dump = cmd_dump_state;
            static unsigned cmd_dump_count;
            if (cmd_dump && (cmd_dump_count < 8u || (cmd_dump_count & 0xffu) == 0u)) {
                int readable = cmd_size
                    && vp_guest_svc_readable(svc, cmd_address, cmd_size);
                slog(svc, "SVC_IPC_RENDERER_INPUT update=%u size=0x%x address=0x%08x readable=%d\n",
                        cmd_dump_count, cmd_size, cmd_address, readable);
                if (readable) {
                    if (cmd_size >= 0x40u) {
                        /* UpdateDataHeader: voices section starts after the
                         * 0x40-byte header, behaviour and memory pools. Each
                         * VoiceInfo::InParameter is 0x170 bytes; in_use is at
                         * +0x09 and play_state at +0x0a. Report only changes,
                         * and keep the first active update for inspection. */
                        uint32_t voices_size = get32(cmd_address + 0x0cu);
                        uint32_t voice_count = voices_size / 0x170u;
                        uint32_t voice_base = cmd_address + 0x40u
                            + get32(cmd_address + 0x04u) + get32(cmd_address + 0x08u)
                            + get32(cmd_address + 0x10u); /* channel resources precede voices */
                        uint32_t voice_bytes = voice_count * 0x170u;
                        if (voice_bytes && voice_base >= cmd_address
                            && voice_bytes <= cmd_size
                            && voice_base - cmd_address <= cmd_size - voice_bytes
                            && vp_guest_svc_readable(svc, voice_base, voice_bytes)) {
                            unsigned active = 0, playing = 0;
                            for (uint32_t i = 0; i < voice_count; ++i) {
                                uint8_t in_use = *(const uint8_t *)(uintptr_t)(voice_base + i * 0x170u + 0x09u);
                                if (in_use) {
                                    ++active;
                                    if (*(const uint8_t *)(uintptr_t)(voice_base + i * 0x170u + 0x0au) == 0u)
                                        ++playing;
                                }
                            }
                            static unsigned last_active_voices = ~0u;
                            static int active_dumped;
                            if (active != last_active_voices) {
                                last_active_voices = active;
                                slog(svc, "SVC_IPC_RENDERER_VOICES update=%u count=%u active=%u playing=%u\n",
                                        cmd_dump_count, voice_count, active, playing);
                            }
                            if (active && !active_dumped && cmd_size <= 0x100000u) {
                                FILE *dump = fopen("ux0:data/mhgu-vita-port/audio-active.bin", "wb");
                                if (dump) {
                                    size_t wrote = fwrite((const void *)(uintptr_t)cmd_address,
                                            1, cmd_size, dump);
                                    fclose(dump);
                                    active_dumped = 1;
                                    slog(svc, "SVC_IPC_RENDERER_ACTIVE_FILE bytes=0x%x active=%u\n",
                                            (unsigned)wrote, active);
                                }
                            }
                        }
                    }
                    if (cmd_dump_count < 2u && cmd_size <= 0x100000u) {
                        char path[96];
                        snprintf(path, sizeof(path),
                                "ux0:data/mhgu-vita-port/audio-input-%u.bin",
                                cmd_dump_count);
                        FILE *dump = fopen(path, "wb");
                        if (dump) {
                            size_t wrote = fwrite((const void *)(uintptr_t)cmd_address,
                                    1, cmd_size, dump);
                            fclose(dump);
                            slog(svc, "SVC_IPC_RENDERER_INPUT_FILE path=%s bytes=0x%x\n",
                                    path, (unsigned)wrote);
                        }
                    }
                    uint32_t words = cmd_size < 0x100u ? cmd_size / 4u : 0x40u;
                    for (uint32_t base = 0; base < words; base += 8u) {
                        uint32_t end = base + 8u < words ? base + 8u : words;
                        slog(svc, "SVC_IPC_RENDERER_INPUT_WORDS +0x%02x:"
                                " %08x %08x %08x %08x %08x %08x %08x %08x\n",
                                base * 4u,
                                base + 0 < end ? get32(cmd_address + (base + 0) * 4u) : 0u,
                                base + 1 < end ? get32(cmd_address + (base + 1) * 4u) : 0u,
                                base + 2 < end ? get32(cmd_address + (base + 2) * 4u) : 0u,
                                base + 3 < end ? get32(cmd_address + (base + 3) * 4u) : 0u,
                                base + 4 < end ? get32(cmd_address + (base + 4) * 4u) : 0u,
                                base + 5 < end ? get32(cmd_address + (base + 5) * 4u) : 0u,
                                base + 6 < end ? get32(cmd_address + (base + 6) * 4u) : 0u,
                                base + 7 < end ? get32(cmd_address + (base + 7) * 4u) : 0u);
                    }
                }
            }
            ++cmd_dump_count;
            /* M5: a started renderer is rendered into the process-wide
             * SceAudioOut sink. The mixer parses the client-generated
             * UpdateDataHeader; errors are returned to the client instead of
             * being reported as a successful silent update. */
            int probe_update = svc->audio_probe_thread_handle
                && svc->current_thread_handle == svc->audio_probe_thread_handle;
            int timed_update = audio_request_us != 0;
            if (timed_update) {
                /* Actual new owner: this thread issued the real renderer
                 * RequestUpdate. Cleared on Stop and when this session is
                 * freed. */
                svc->audio_update_thread_handle = svc->current_thread_handle;
                svc->audio_update_session =
                    (uint32_t)(state - svc->sessions) + 1u;
            }
            uint64_t wait_us = 0, render_us = 0;
            uint32_t accepted_frames = 0;
            uint32_t render_result = 0;
            if (state->audio_active && state->audio_mixer && cmd_size) {
                VPAudioRenderStats rs;
                /* Backpressure at the device ring. The client can issue
                 * updates faster than the 48 kHz port drains them; the
                 * original server paces against its own execution clock and
                 * never drops its input. Before the drain starts, pace at the
                 * full ring so the client can pre-roll it (the first updates
                 * are slow; the device must not start half-full). Once the
                 * drain has emitted its first block, pace at
                 * VP_AUDIO_SINK_PACE_FRAMES so a fast client produces exactly
                 * the drain rate with a fixed ~64 ms playback buffer instead
                 * of overwriting queued audio or forcing the sink to pad
                 * silence. */
                uint64_t wait_start = timed_update
                    ? sceKernelGetProcessTimeWide() : 0;
                if (state->audio_sink_owned && state->audio_sample_count
                    && state->audio_sample_count <= VP_AUDIO_SINK_PACE_FRAMES
                    && vp_audio_sink_running()) {
                    uint32_t want = state->audio_sample_count;
                    uint32_t pace = vp_audio_sink_started()
                        ? VP_AUDIO_SINK_PACE_FRAMES
                        : VP_AUDIO_SINK_RING_FRAMES;
                    uint32_t waited_us = 0;
                    while (vp_audio_sink_running()
                           && vp_audio_sink_pending() + want > pace
                           && waited_us < 200000u) {
                        sceKernelDelayThread(500);
                        waited_us += 500;
                    }
                }
                if (timed_update) {
                    uint64_t now = sceKernelGetProcessTimeWide();
                    wait_us = now >= wait_start ? now - wait_start : 0;
                    state->audio_probe_wait_us += wait_us;
                }
                uint64_t render_start = timed_update ? sceKernelGetProcessTimeWide() : 0;
                uint32_t periods_done = 0;
                uint32_t source_frames = 0;
                uint32_t starved_periods = 0;
                uint32_t cap_hits = 0;
                uint32_t debt_frames = 0;
                uint32_t dropped_total = 0;
                uint32_t frames = 0;
                /* Apply the client's configuration once, then render the
                 * first fresh period from the renderer-owned snapshot. The
                 * snapshot is what later periods use; the guest command
                 * buffer may be overwritten as soon as this handler returns. */
                int rr = vp_audio_renderer_apply_config(state->audio_mixer, svc,
                                                        cmd_address, cmd_size,
                                                        NULL);
                if (!rr)
                    rr = vp_audio_renderer_render_period(state->audio_mixer, svc, &rs);
                if (rr) {
                    ++state->audio_render_errors;
                    render_result = 0xE0020001u;
                    slog(svc, "SVC_IPC_RENDERER_RENDER_ERROR update=%u size=0x%x rc=%d\n",
                         state->audio_update_count, cmd_size, rr);
                } else {
                    const int16_t *pcm = vp_audio_renderer_output(
                        state->audio_mixer, &frames);
                    uint32_t dropped = 0;
                    uint32_t accepted = vp_audio_sink_write(pcm, frames, &dropped);
                    uint32_t period_source = rs.source_frames;
                    uint32_t period_peak = rs.peak_left > rs.peak_right
                        ? rs.peak_left : rs.peak_right;
                    uint32_t period_rms = rs.rms_left > rs.rms_right
                        ? rs.rms_left : rs.rms_right;
                    ++state->audio_sink_submits;
                    ++periods_done;
                    accepted_frames = accepted;
                    dropped_total += dropped;
                    source_frames += period_source;
                    if (rs.voice_starved) ++starved_periods;
                    state->audio_render_frames += accepted;
                    state->audio_last_peak = period_peak;
                    state->audio_last_rms = period_rms;
                    state->audio_zero_left += rs.zero_left;
                    state->audio_zero_right += rs.zero_right;
                    state->audio_last_voices = rs.voices_rendered;
                    state->audio_last_unsupported =
                        rs.voices_unsupported + rs.sink_unsupported;
                    if (accepted < frames) {
                        /* The ring was already at capacity; report the lost
                         * frames instead of pretending the period is queued. */
                        debt_frames += frames - accepted;
                    } else if (state->audio_execution_mode == 0u
                               && state->audio_sink_owned
                               && vp_audio_sink_running()
                               && vp_audio_sink_started()
                               && frames
                               && frames <= VP_AUDIO_SINK_PACE_FRAMES) {
                        /* Automatic execution mode: the reviewed reference
                         * runs the renderer system on its own audio clock
                         * between RequestUpdate calls. This shim has no
                         * independent DSP thread, so RequestUpdate refills
                         * the bounded sink queue from the same snapshot:
                         * render fresh periods until the actual sink pending
                         * reaches VP_AUDIO_SINK_PACE_FRAMES or the fixed
                         * period cap is reached. Every period processes real
                         * voice state and advances its cursor; no output is
                         * duplicated or time-stretched. */
                        uint32_t budget = VP_AR_AUTO_MAX_PERIODS - 1u;
                        for (;;) {
                            uint32_t pending = vp_audio_sink_pending();
                            if (!vp_audio_catchup_periods(
                                    pending, frames,
                                    VP_AUDIO_SINK_PACE_FRAMES, 1u))
                                break;
                            if (!budget) {
                                ++cap_hits;
                                debt_frames +=
                                    VP_AUDIO_SINK_PACE_FRAMES - pending;
                                break;
                            }
                            VPAudioRenderStats extra;
                            int erc = vp_audio_renderer_render_period(
                                state->audio_mixer, svc, &extra);
                            if (erc) {
                                ++state->audio_render_errors;
                                render_result = 0xE0020001u;
                                slog(svc, "SVC_IPC_RENDERER_RENDER_ERROR update=%u"
                                     " period=%u rc=%d\n",
                                     state->audio_update_count,
                                     periods_done + 1u, erc);
                                break;
                            }
                            uint32_t extra_frames = 0;
                            const int16_t *extra_pcm =
                                vp_audio_renderer_output(state->audio_mixer,
                                                         &extra_frames);
                            uint32_t extra_dropped = 0;
                            uint32_t extra_accepted = vp_audio_sink_write(
                                extra_pcm, extra_frames, &extra_dropped);
                            ++periods_done;
                            --budget;
                            accepted_frames += extra_accepted;
                            dropped_total += extra_dropped;
                            source_frames += extra.source_frames;
                            if (extra.voice_starved) ++starved_periods;
                            state->audio_render_frames += extra_accepted;
                            state->audio_zero_left += extra.zero_left;
                            state->audio_zero_right += extra.zero_right;
                            uint32_t peak = extra.peak_left > extra.peak_right
                                ? extra.peak_left : extra.peak_right;
                            uint32_t rms = extra.rms_left > extra.rms_right
                                ? extra.rms_left : extra.rms_right;
                            if (peak > state->audio_last_peak)
                                state->audio_last_peak = peak;
                            if (rms > state->audio_last_rms)
                                state->audio_last_rms = rms;
                            if (extra.voices_rendered > state->audio_last_voices)
                                state->audio_last_voices = extra.voices_rendered;
                            state->audio_last_unsupported +=
                                extra.voices_unsupported + extra.sink_unsupported;
                            if (extra_accepted != extra_frames) {
                                debt_frames += extra_frames - extra_accepted;
                                break;
                            }
                        }
                    }
                    state->audio_sink_dropped += dropped_total;
                    if (svc->audio_probe_pending_field)
                        *(volatile uint32_t *)(uintptr_t)svc->audio_probe_pending_field =
                            vp_audio_sink_pending();
                    if (state->audio_sink_submits <= 4u
                        || (state->audio_sink_submits % 64u) == 0u) {
                        VPAudioSinkStats ss;
                        vp_audio_sink_stats(&ss);
                        slog(svc, "SVC_IPC_RENDERER_RENDER update=%u active=%u rendered=%u unsupported=%u mixes=%u sink_inputs=%u period=%u source=%u accepted=%u peak=%u/%u rms=%u/%u zc=%llu/%llu sink_written=%llu sink_out=%llu underruns=%llu silence=%llu overruns=%llu pending=%u rest=%u..%u\n",
                             state->audio_update_count, rs.voices_active, rs.voices_rendered,
                             rs.voices_unsupported, rs.mix_count, rs.sink_inputs,
                             periods_done, source_frames, accepted_frames,
                             state->audio_last_peak, rs.peak_right, rs.rms_left, rs.rms_right,
                             (unsigned long long)rs.zero_left, (unsigned long long)rs.zero_right,
                             (unsigned long long)ss.written_frames,
                             (unsigned long long)ss.output_frames,
                             (unsigned long long)ss.underruns,
                             (unsigned long long)ss.silence_frames,
                             (unsigned long long)ss.overruns,
                             ss.pending_frames, ss.rest_min, ss.rest_max);
                    }
                }
                if (timed_update) {
                    uint64_t now = sceKernelGetProcessTimeWide();
                    render_us = now >= render_start ? now - render_start : 0;
                    uint64_t service_us = now >= audio_request_us
                        ? now - audio_request_us : 0;
                    state->audio_probe_render_us += render_us;
                    vp_audio_timing_note_periods(&state->audio_timing,
                                                 audio_request_us, render_us,
                                                 wait_us, service_us,
                                                 accepted_frames, periods_done,
                                                 source_frames, debt_frames,
                                                 starved_periods, cap_hits);
                }
            }
            for (uint32_t index = 0; index < 2u; ++index) {
                uint32_t out_size = 0, out_address = 0;
                hipc_write_buffer(&layout, index, &out_size, &out_address);
                if (out_size && (!vp_guest_svc_writable(svc, out_address, out_size)
                    || out_size > 0x100000u)) {
                    ++svc->ipc_unsupported;
                    slog(svc, "SVC_IPC_RENDERER_UPDATE_INVALID index=%u size=0x%x address=0x%08x\n",
                            index, out_size, out_address);
                    sflush(svc);
                    return VP_BRIDGE_UNSUPPORTED;
                }
                if (out_size) memset((void *)(uintptr_t)out_address, 0, out_size);
            }
            /* Out buffer 0 is the client's AudioRendererConfig status buffer.
             * The locked client's RequestUpdateAudioRenderer validates its
             * UpdateDataHeader against values its own managers compute from
             * the parameter counts and returns 0x5299 on any mismatch; a
             * zeroed header is therefore not a successful reply. Write the
             * header the original server would produce: per-section byte
             * sizes from the managers' element sizes (voice 0x10, effect
             * 0x10, sink 0x20, performance 0x10, behaviour 0xb0). The
             * declared total is the client-provided buffer size, which is
             * what the client compares GetTotalSize() with. */
            {
                uint32_t status_size = 0, status_address = 0;
                hipc_write_buffer(&layout, 0, &status_size, &status_address);
                if (status_size >= 0x40u
                    && vp_guest_svc_writable(svc, status_address, 0x40u)) {
                    uint32_t voice_out = state->audio_voice_count * 0x10u;
                    uint32_t effect_out = state->audio_effect_count * 0x10u;
                    uint32_t sink_out = state->audio_sink_count * 0x20u;
                    /* Memory pools are sized by the client's own input header:
                     * each pool consumes 0x20 input bytes and produces 0x10
                     * output bytes. The original server rejects an update when
                     * input+0x08 does not equal pool_count<<5, so the same
                     * count is the one the client's MemoryPoolManager expects
                     * back in output+0x08. A zero here makes the locked
                     * RequestUpdateAudioRenderer fail its first manager check
                     * with 0x5299. */
                    uint32_t mempool_in = 0u;
                    if (cmd_size >= 0x40u
                        && get32(cmd_address + 0x00u) == state->audio_revision)
                        mempool_in = get32(cmd_address + 0x08u);
                    uint32_t mempool_out = (mempool_in >> 5) * 0x10u;
                    uint32_t status_out = 0x40u + mempool_out + voice_out
                        + effect_out + sink_out + 0x10u + 0xB0u;
                    put32(status_address + 0x00, state->audio_revision);
                    put32(status_address + 0x04, 0xB0u);
                    put32(status_address + 0x08, mempool_out);
                    put32(status_address + 0x0c, voice_out);
                    put32(status_address + 0x10, 0u);
                    put32(status_address + 0x14, effect_out);
                    put32(status_address + 0x18, 0u);
                    put32(status_address + 0x1c, sink_out);
                    put32(status_address + 0x20, 0x10u);
                    put32(status_address + 0x24, 0u);
                    put32(status_address + 0x3c, status_out);
                    uint32_t voices_at = status_address + 0x40u + mempool_out;
                    if (status_out <= status_size
                        && vp_guest_svc_writable(svc, status_address, status_out)) {
                        for (uint32_t i = 0; i < state->audio_voice_count; ++i) {
                            uint64_t played = 0;
                            uint32_t consumed = 0;
                            if (vp_audio_renderer_voice_status(state->audio_mixer, i,
                                                              &played, &consumed) == 0) {
                                put32(voices_at + i * 0x10u, (uint32_t)played);
                                put32(voices_at + i * 0x10u + 4u, (uint32_t)(played >> 32));
                                put32(voices_at + i * 0x10u + 8u, consumed);
                            }
                        }
                    }
                    if (svc->audio_probe_thread_handle
                        && svc->current_thread_handle == svc->audio_probe_thread_handle
                        && state->audio_update_count == 0u) {
                        /* One-shot evidence only on the first update. Emitting
                         * it on updates 1-2 as well put three close-per-line
                         * I/O bursts into the startup window; those updates
                         * then took 32-52 ms against a 10 ms audio period and
                         * drained the sink pre-roll. The dump happens here,
                         * before any device block is played. */
                        durable_event("RENDERER_UPDATE_STATUS size=0x%x addr=0x%08x"
                            " revision=0x%08x mempool=0x%x(0x%x) voice=0x%x"
                            " effect=0x%x sink=0x%x total=0x%x\n",
                            status_size, status_address, state->audio_revision,
                            mempool_out, mempool_in, voice_out, effect_out,
                            sink_out, status_out);
                        if (cmd_size >= 0x40u) {
                            durable_event("RENDERER_UPDATE_IN size=0x%x addr=0x%08x"
                                " hdr=%08x %08x %08x %08x %08x %08x %08x %08x"
                                " %08x %08x %08x %08x %08x %08x %08x %08x\n",
                                cmd_size, cmd_address,
                                get32(cmd_address + 0x00), get32(cmd_address + 0x04),
                                get32(cmd_address + 0x08), get32(cmd_address + 0x0c),
                                get32(cmd_address + 0x10), get32(cmd_address + 0x14),
                                get32(cmd_address + 0x18), get32(cmd_address + 0x1c),
                                get32(cmd_address + 0x20), get32(cmd_address + 0x24),
                                get32(cmd_address + 0x28), get32(cmd_address + 0x2c),
                                get32(cmd_address + 0x30), get32(cmd_address + 0x34),
                                get32(cmd_address + 0x38), get32(cmd_address + 0x3c));
                            /* The mixer reports the probe's voice as not in
                             * use; record the first VoiceInfo::InParameter
                             * exactly as the client serialized it. */
                            if (state->audio_update_count == 0u) {
                                FILE *dump = fopen(
                                    "ux0:data/mhgu-vita-port/audio-probe-input.bin", "wb");
                                if (dump) {
                                    fwrite((const void *)(uintptr_t)cmd_address, 1,
                                           cmd_size, dump);
                                    fclose(dump);
                                    durable_event("RENDERER_UPDATE_FILE bytes=0x%x\n",
                                                  cmd_size);
                                }
                                uint32_t vb = cmd_address + 0x40u
                                    + get32(cmd_address + 0x04u)
                                    + get32(cmd_address + 0x08u)
                                    + get32(cmd_address + 0x10u);
                                if (vb >= cmd_address && vb - cmd_address <= cmd_size
                                    && cmd_size - (vb - cmd_address) >= 0x60u
                                    && vp_guest_svc_readable(svc, vb, 0x60u))
                                    durable_event("RENDERER_VOICE0 base=0x%08x"
                                        " w=%08x %08x %08x %08x %08x %08x %08x %08x"
                                        " %08x %08x %08x %08x %08x %08x %08x %08x"
                                        " %08x %08x %08x %08x %08x %08x %08x %08x\n",
                                        vb,
                                        get32(vb + 0x00), get32(vb + 0x04),
                                        get32(vb + 0x08), get32(vb + 0x0c),
                                        get32(vb + 0x10), get32(vb + 0x14),
                                        get32(vb + 0x18), get32(vb + 0x1c),
                                        get32(vb + 0x20), get32(vb + 0x24),
                                        get32(vb + 0x28), get32(vb + 0x2c),
                                        get32(vb + 0x30), get32(vb + 0x34),
                                        get32(vb + 0x38), get32(vb + 0x3c),
                                        get32(vb + 0x40), get32(vb + 0x44),
                                        get32(vb + 0x48), get32(vb + 0x4c),
                                        get32(vb + 0x50), get32(vb + 0x54),
                                        get32(vb + 0x58), get32(vb + 0x5c));
                            }
                        }
                    }
                }
            }
            ++state->audio_update_count;
            uint64_t period_us = state->audio_sample_rate
                ? (uint64_t)state->audio_sample_count * 1000000ull
                    / state->audio_sample_rate : 0ull;
            if (!period_us) period_us = 1000ull;
            if (state->audio_system_event) {
                VPGuestHandle *event = find_handle(svc, state->audio_system_event);
                if (event && event->kind == 3) {
                    if (state->audio_active && state->audio_sink_owned) {
                        /* The sink drain thread already runs on the device
                         * clock; an extra paced signal would double-pace the
                         * client's update loop. */
                        event->signaled = 1;
                        event->signal_at_us = 0;
                    } else {
                        event->signaled = 0;
                        event->signal_at_us = sceKernelGetProcessTimeWide() + period_us;
                    }
                }
            }
            if (probe_update) {
                uint64_t probe_interval = 0;
                if (state->audio_probe_prev_us && audio_request_us >= state->audio_probe_prev_us) {
                    probe_interval = audio_request_us - state->audio_probe_prev_us;
                    state->audio_probe_block_us += probe_interval;
                    ++state->audio_probe_block_n;
                }
                /* The first four updates run before the ring is full, so
                 * their intervals are the raw client round trip; later
                 * samples show the device-paced cadence. avg_us is the mean
                 * over the updates since the previous sample and is what the
                 * ring sees. */
                if (state->audio_probe_prev_us
                    && ((state->audio_update_count & 63u) == 0u
                        || state->audio_update_count <= 4u)) {
                    durable_event("RENDERER_TIMING update=%u interval_us=%llu"
                        " avg_us=%llu render_us=%llu wait_us=%llu pending=%u\n",
                        state->audio_update_count,
                        (unsigned long long)probe_interval,
                        (unsigned long long)(state->audio_probe_block_n
                            ? state->audio_probe_block_us / state->audio_probe_block_n : 0),
                        (unsigned long long)state->audio_probe_render_us,
                        (unsigned long long)state->audio_probe_wait_us,
                        vp_audio_sink_pending());
                    state->audio_probe_block_us = 0;
                    state->audio_probe_block_n = 0;
                }
                state->audio_probe_prev_us = audio_request_us;
                state->audio_probe_render_us = 0;
                state->audio_probe_wait_us = 0;
            }
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, 4);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, render_result);
            put32(response_meta + 0xc, 0);
            f->regs[0] = 0;
            /* RequestUpdate runs at the audio clock (200 Hz). Logging every
             * call and flushing the emulated file each time cost about 12 ms
             * per update on the probe's thread, which starved the sink and
             * showed up as underruns rather than as a logging defect. Keep
             * the first calls and a sparse heartbeat; the durable per-update
             * evidence lives in the sink/renderer counters. */
            if (vp_audio_log_cadence_should_emit(&state->audio_request_log, 4u, 64u)) {
                slog(svc, "SVC_IPC_RENDERER command=4 call=RequestUpdate active=%u updates=%u model=%s period_us=%llu signal=0x%08x meta=0x%08x result=0x%08x\n",
                        state->audio_active, state->audio_update_count,
                        state->audio_active && state->audio_sink_owned ? "sink-renderer" : "silent-renderer",
                        (unsigned long long)period_us,
                        state->audio_system_event, response_meta, render_result);
                if (timed_update) {
                    /* Per-renderer timing window over the updates since the
                     * previous sparse emission. Request interval is measured
                     * at IPC entry, so it is the client cadence and not the
                     * service time. Sink deltas are read here only, at the
                     * window boundary. */
                    VPAudioTimingSummary ts;
                    VPAudioSinkStats ss;
                    uint64_t under_delta = 0, silence_delta = 0, overrun_delta = 0;
                    vp_audio_timing_summary(&state->audio_timing, &ts);
                    vp_audio_sink_stats(&ss);
                    if (state->audio_timing_sink_base_valid) {
                        if (ss.underruns >= state->audio_timing_underrun_base)
                            under_delta = ss.underruns - state->audio_timing_underrun_base;
                        if (ss.silence_frames >= state->audio_timing_silence_base)
                            silence_delta = ss.silence_frames - state->audio_timing_silence_base;
                        if (ss.overruns >= state->audio_timing_overrun_base)
                            overrun_delta = ss.overruns - state->audio_timing_overrun_base;
                    } else {
                        state->audio_timing_sink_base_valid = 1;
                    }
                    state->audio_timing_underrun_base = ss.underruns;
                    state->audio_timing_silence_base = ss.silence_frames;
                    state->audio_timing_overrun_base = ss.overruns;
                    slog(svc, "SVC_IPC_RENDERER_TIMING_WINDOW update=%u window_updates=%u interval_samples=%u interval_us_mean=%llu interval_us_max=%llu render_us_mean=%llu render_us_max=%llu wait_us_mean=%llu wait_us_max=%llu service_us_mean=%llu service_us_max=%llu mode=%u periods_total=%u periods_max=%u source_frames=%u accepted_frames=%llu catchup_debt=%u starvation_periods=%u cap_hits=%u underrun_delta=%llu silence_delta=%llu overrun_delta=%llu pending=%u owner_thread=0x%08x unowned_reset=%u unowned_sm_getservice=%u ipc=mgr_open:%u,mgr_work:%u,mgr_get_device:%u,mgr_other:%u,r_get:%u,r_update:%u,r_start:%u,r_stop:%u,r_other:%u,device:%u,control:%u,close:%u\n",
                        state->audio_update_count, ts.updates, ts.interval_samples,
                        (unsigned long long)ts.interval_mean_us,
                        (unsigned long long)ts.interval_max_us,
                        (unsigned long long)ts.render_mean_us,
                        (unsigned long long)ts.render_max_us,
                        (unsigned long long)ts.wait_mean_us,
                        (unsigned long long)ts.wait_max_us,
                        (unsigned long long)ts.service_mean_us,
                        (unsigned long long)ts.service_max_us,
                        state->audio_execution_mode,
                        ts.periods, ts.periods_max, ts.source_frames,
                        (unsigned long long)ts.accepted_frames,
                        ts.debt_frames, ts.starvation_periods, ts.cap_hits,
                        (unsigned long long)under_delta,
                        (unsigned long long)silence_delta,
                        (unsigned long long)overrun_delta,
                        vp_audio_sink_pending(),
                        svc->audio_update_thread_handle,
                        svc->audio_log_unowned_reset,
                        svc->audio_log_unowned_sm_getservice,
                        state->audio_ipc_class_counts[VP_AUDIO_IPC_MANAGER_OPEN],
                        state->audio_ipc_class_counts[VP_AUDIO_IPC_MANAGER_GET_WORK_SIZE],
                        state->audio_ipc_class_counts[VP_AUDIO_IPC_MANAGER_GET_DEVICE],
                        state->audio_ipc_class_counts[VP_AUDIO_IPC_MANAGER_OTHER],
                        state->audio_ipc_class_counts[VP_AUDIO_IPC_RENDERER_GET],
                        state->audio_ipc_class_counts[VP_AUDIO_IPC_RENDERER_UPDATE],
                        state->audio_ipc_class_counts[VP_AUDIO_IPC_RENDERER_START],
                        state->audio_ipc_class_counts[VP_AUDIO_IPC_RENDERER_STOP],
                        state->audio_ipc_class_counts[VP_AUDIO_IPC_RENDERER_OTHER],
                        state->audio_ipc_class_counts[VP_AUDIO_IPC_DEVICE],
                        state->audio_ipc_class_counts[VP_AUDIO_IPC_CONTROL],
                        state->audio_ipc_class_counts[VP_AUDIO_IPC_SESSION_CLOSE]);
                    vp_audio_timing_reset(&state->audio_timing);
                }
                sflush(svc);
            }
            return 0;
        }
        if (command == 5u || command == 6u) {
            /* Start/Stop. Start owns the process-wide SceAudioOut sink; the
             * mixer is created from the parameters recorded at open time. A
             * failure is reported to the client, never hidden behind a
             * successful reply. */
            uint32_t result = 0;
            int probe_thread = svc->audio_probe_thread_handle
                && svc->current_thread_handle == svc->audio_probe_thread_handle;
            if (probe_thread)
                durable_event("RENDERER_START_ENTER command=%u active=%u rate=%u count=%u sink_owner=%p\n",
                    command, state->audio_active, state->audio_sample_rate,
                    state->audio_sample_count, svc->audio_sink_owner);
            if (command == 5u) {
                if (state->audio_active) {
                    result = 0; /* idempotent */
                } else if (svc->audio_sink_owner && svc->audio_sink_owner != (void *)state) {
                    result = 0xE0010002u; /* another renderer owns the sink */
                    ++state->audio_sink_refused;
                } else {
                    if (!state->audio_mixer)
                        state->audio_mixer = vp_audio_renderer_create(
                            state->audio_sample_rate, state->audio_sample_count);
                    if (probe_thread)
                        durable_event("RENDERER_START_MIXER mixer=%p\n", state->audio_mixer);
                    if (!state->audio_mixer) {
                        result = 0xE0010003u;
                    } else if (!state->audio_sink_owned) {
                        if (probe_thread) durable_event("RENDERER_START_SINK_BEGIN\n");
                        int rc = vp_audio_sink_start(state->audio_sample_rate, svc->log);
                        if (probe_thread)
                            durable_event("RENDERER_START_SINK_END rc=%d\n", rc);
                        if (rc < 0) {
                            result = rc == -2 ? 0xE0010004u : 0xE0010001u;
                        } else {
                            state->audio_sink_owned = 1;
                            svc->audio_sink_owner = state;
                        }
                    }
                    if (!result) state->audio_active = 1;
                }
            } else {
                state->audio_active = 0;
                if (svc->audio_update_session
                    == (uint32_t)(state - svc->sessions) + 1u) {
                    svc->audio_update_thread_handle = 0;
                    svc->audio_update_session = 0;
                }
                if (state->audio_sink_owned) {
                    vp_audio_sink_stop();
                    svc->audio_sink_owner = NULL;
                    state->audio_sink_owned = 0;
                }
            }
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, 4);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, result);
            put32(response_meta + 0xc, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_RENDERER command=%u call=%s active=%u model=%s rate=%u count=%u meta=0x%08x result=0x%08x\n",
                    command, command == 5u ? "Start" : "Stop", state->audio_active,
                    state->audio_active ? "sce-audio-out" : "sink-closed",
                    state->audio_sample_rate, state->audio_sample_count,
                    response_meta, result);
            sflush(svc);
            return 0;
        }
        if (command == 7u) {
            /* QuerySystemEvent: one KReadableEvent owned by the renderer,
             * returned as a copy handle and signalled by RequestUpdate. */
            if (!state->audio_system_event) {
                state->audio_system_event = create_handle(svc, 3, "audio-ev", 0);
                if (state->audio_system_event) {
                    VPGuestHandle *event = find_handle(svc, state->audio_system_event);
                    if (event)
                        event->audio_owner_session =
                            (uint32_t)(state - svc->sessions) + 1u;
                }
                if (!state->audio_system_event) {
                    uint32_t response_meta = plain_out_handle_meta(buffer);
                    put32(buffer, 0);
                    put32(buffer + 4, 0x8000000Au);
                    put32(buffer + 8, 0x21u);
                    put32(buffer + 0xc, 0);
                    put32(buffer + 0x10, 0);
                    put32(buffer + 0x14, 0);
                    put32(response_meta, 0x4F434653u);
                    put32(response_meta + 8, SVC_RESULT_OUT_OF_MEMORY);
                    f->regs[0] = 0;
                    slog(svc, "SVC_IPC_RENDERER_EVENT_LIMIT result=0x%08x\n",
                            SVC_RESULT_OUT_OF_MEMORY);
                    sflush(svc);
                    return 0;
                }
            }
            uint32_t handle_offset = (3u + 2u) * 4u;
            uint32_t response_meta = (buffer + handle_offset + 4u + 0xfu) & ~0xfu;
            put32(buffer, 0);
            put32(buffer + 4, 0x8000000Au);
            put32(buffer + 8, 0x03u); /* pid word, one copy handle */
            put32(buffer + 0xc, 0);
            put32(buffer + 0x10, 0);
            put32(buffer + handle_offset, state->audio_system_event);
            put32(buffer + 0x18, 0);
            put32(buffer + 0x1c, 0);
            put32(buffer + 0x20, 0);
            put32(buffer + 0x24, 0);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_RENDERER command=7 call=QuerySystemEvent event=0x%08x model=silent-renderer meta=0x%08x result=0\n",
                    state->audio_system_event, response_meta);
            sflush(svc);
            return 0;
        }
        if (command == 8u) {
            state->audio_rendering_time_limit = get32(request_meta + 0x10u);
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, 4);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_RENDERER command=8 call=SetRenderingTimeLimit limit=%u model=silent-renderer meta=0x%08x result=0\n",
                    state->audio_rendering_time_limit, response_meta);
            sflush(svc);
            return 0;
        }
        if (command == 12u) {
            state->audio_voice_drop_bits = get32(request_meta + 0x10u);
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, 4);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_RENDERER command=12 call=SetVoiceDropParameter value=0x%08x model=silent-renderer meta=0x%08x result=0\n",
                    state->audio_voice_drop_bits, response_meta);
            sflush(svc);
            return 0;
        }
        ++svc->ipc_unsupported;
        slog(svc, "SVC_IPC_RENDERER_UNSUPPORTED command=%u\n", command);
        sflush(svc);
        return VP_BRIDGE_UNSUPPORTED;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "aud-dev")) {
        /* nn::audio::detail::IAudioDevice. The device set is the console's
         * output layout for the revision the client asked for; no sink is
         * wired to the platform yet, so names and the AudioTvOutput volume
         * are the observable state. */
        static const char *device_names_rev4[] = {
            "AudioStereoJackOutput", "AudioBuiltInSpeakerOutput",
            "AudioTvOutput", "AudioUsbDeviceOutput",
        };
        static const char *device_names_old[] = {
            "AudioStereoJackOutput", "AudioBuiltInSpeakerOutput",
            "AudioTvOutput",
        };
        static const char *output_names[] = {
            "AudioBuiltInSpeakerOutput", "AudioTvOutput", "AudioExternalOutput",
        };
        uint32_t device_revision = ((state->audio_device_revision >> 24) & 0xffu);
        device_revision = device_revision >= '0' ? device_revision - '0' : 0;
        const char **names = device_revision >= 4u ? device_names_rev4 : device_names_old;
        uint32_t name_count = device_revision >= 4u ? 4u : 3u;
        if (command == 0u || command == 6u) {
            VPGuestHipcLayout layout;
            hipc_layout(buffer, header0, header1, &layout);
            uint32_t out_size = 0, out_address = 0;
            hipc_write_buffer(&layout, 0, &out_size, &out_address);
            if (out_size && !vp_guest_svc_writable(svc, out_address, out_size)) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_AUDIODEV_NAME_INVALID size=0x%x address=0x%08x\n",
                        out_size, out_address);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            uint32_t capacity = out_size / 0x100u;
            uint32_t count = capacity < name_count ? capacity : name_count;
            for (uint32_t i = 0; i < count; ++i) {
                char *slot = (char *)(uintptr_t)(out_address + i * 0x100u);
                memset(slot, 0, 0x100u);
                memcpy(slot, names[i], strlen(names[i]));
            }
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, 5);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            put32(response_meta + 0x10, count);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_AUDIODEV command=%u call=ListAudioDeviceName revision=0x%08x count=%u capacity=%u model=no-audio-sink meta=0x%08x result=0\n",
                    command, state->audio_device_revision, count, capacity, response_meta);
            sflush(svc);
            return 0;
        }
        if (command == 14u || command == 3u || command == 10u || command == 13u) {
            VPGuestHipcLayout layout;
            hipc_layout(buffer, header0, header1, &layout);
            uint32_t out_size = 0, out_address = 0;
            hipc_write_buffer(&layout, 0, &out_size, &out_address);
            const char *name = command == 14u ? output_names[0] : "AudioTvOutput";
            uint32_t write_count = command == 14u ? 3u : 1u;
            if (out_size < write_count * 0x100u
                || !vp_guest_svc_writable(svc, out_address, out_size)) {
                ++svc->ipc_unsupported;
                slog(svc, "SVC_IPC_AUDIODEV_NAME_INVALID command=%u size=0x%x address=0x%08x\n",
                        command, out_size, out_address);
                sflush(svc);
                return VP_BRIDGE_UNSUPPORTED;
            }
            for (uint32_t i = 0; i < write_count; ++i) {
                char *slot = (char *)(uintptr_t)(out_address + i * 0x100u);
                const char *entry = command == 14u ? output_names[i] : name;
                memset(slot, 0, 0x100u);
                memcpy(slot, entry, strlen(entry));
            }
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, 4);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            f->regs[0] = 0;
            /* run10 shows GetActiveAudioDeviceName (commands 3/10/13) at the
             * update cadence. Bound only its successful line; the list calls
             * (0/6/14) and every malformed path keep their exact lines. */
            int emit_name_line = 1;
            if (state && (command == 3u || command == 10u || command == 13u))
                emit_name_line = vp_audio_log_cadence_should_emit(
                    &state->audio_device_name_log, 4u, 64u);
            if (emit_name_line) {
                uint32_t c0 = state ? state->audio_device_cmd_counts[0] : 0u;
                uint32_t c3 = state ? state->audio_device_cmd_counts[3] : 0u;
                uint32_t c6 = state ? state->audio_device_cmd_counts[6] : 0u;
                uint32_t c10 = state ? state->audio_device_cmd_counts[10] : 0u;
                uint32_t c13 = state ? state->audio_device_cmd_counts[13] : 0u;
                uint32_t c14 = state ? state->audio_device_cmd_counts[14] : 0u;
                slog(svc, "SVC_IPC_AUDIODEV command=%u call=%s name=\"%s\" count=%u model=AudioTvOutput meta=0x%08x result=0 cmd_counts=%u,%u,%u,%u,%u,%u\n",
                        command, command == 14u ? "ListAudioOutputDeviceName"
                        : "GetActiveAudioDeviceName", name, write_count, response_meta,
                        c0, c3, c6, c10, c13, c14);
                sflush(svc);
            }
            return 0;
        }
        if (command == 1u || command == 7u || command == 2u || command == 8u) {
            VPGuestHipcLayout layout;
            hipc_layout(buffer, header0, header1, &layout);
            uint32_t in_size = 0, in_address = 0;
            hipc_read_buffer(&layout, 0, &in_size, &in_address);
            int is_tv = 0;
            if (in_size >= 0x100u && vp_guest_svc_readable(svc, in_address, 0x100u)) {
                const char *name = (const char *)(uintptr_t)in_address;
                is_tv = strncmp(name, "AudioTvOutput", 0x100u) == 0;
            }
            if (command == 1u || command == 7u) {
                uint32_t volume_bits = get32(request_meta + 0x10u);
                if (is_tv) state->audio_device_volume_bits = volume_bits;
            }
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, command == 2u || command == 8u ? 5u : 4u);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            if (command == 2u || command == 8u)
                put32(response_meta + 0x10, state->audio_device_volume_bits);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_AUDIODEV command=%u call=%s tv=%d volume=0x%08x model=AudioTvOutput meta=0x%08x result=0\n",
                    command, command == 1u ? "SetAudioDeviceOutputVolume"
                    : command == 2u ? "GetAudioDeviceOutputVolume"
                    : command == 7u ? "SetAudioDeviceOutputVolumeAuto"
                    : "GetAudioDeviceOutputVolumeAuto",
                    is_tv, state->audio_device_volume_bits, response_meta);
            sflush(svc);
            return 0;
        }
        if (command == 4u || command == 11u || command == 12u) {
            if (!state->audio_device_event)
                state->audio_device_event = create_handle(svc, 3, "audio-dev-ev", 0);
            if (state->audio_device_event) {
                VPGuestHandle *event = find_handle(svc, state->audio_device_event);
                if (event)
                    event->audio_owner_session =
                        (uint32_t)(state - svc->sessions) + 1u;
            }
            if (!state->audio_device_event) {
                uint32_t response_meta = plain_out_handle_meta(buffer);
                put32(buffer, 0);
                put32(buffer + 4, 0x8000000Au);
                put32(buffer + 8, 0x03u);
                put32(buffer + 0xc, 0);
                put32(buffer + 0x10, 0);
                put32(buffer + 0x14, 0);
                put32(response_meta, 0x4F434653u);
                put32(response_meta + 8, SVC_RESULT_OUT_OF_MEMORY);
                f->regs[0] = 0;
                slog(svc, "SVC_IPC_AUDIODEV_EVENT_LIMIT result=0x%08x\n", SVC_RESULT_OUT_OF_MEMORY);
                sflush(svc);
                return 0;
            }
            if (command == 4u) {
                VPGuestHandle *event = find_handle(svc, state->audio_device_event);
                if (event) event->signaled = 1;
            }
            uint32_t response_meta = plain_out_handle_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, 0x8000000Au);
            put32(buffer + 8, 0x03u);
            put32(buffer + 0xc, 0);
            put32(buffer + 0x10, 0);
            put32(buffer + 0x14, state->audio_device_event);
            put32(buffer + 0x18, 0);
            put32(buffer + 0x1c, 0);
            put32(buffer + 0x20, 0);
            put32(buffer + 0x24, 0);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_AUDIODEV command=%u call=QueryAudioDeviceEvent event=0x%08x model=no-audio-sink meta=0x%08x result=0\n",
                    command, state->audio_device_event, response_meta);
            sflush(svc);
            return 0;
        }
        if (command == 5u) {
            uint32_t response_meta = plain_response_meta(buffer);
            put32(buffer, 0);
            put32(buffer + 4, 5);
            put32(response_meta, 0x4F434653u);
            put32(response_meta + 4, 0);
            put32(response_meta + 8, 0);
            put32(response_meta + 0xc, 0);
            put32(response_meta + 0x10, 2);
            f->regs[0] = 0;
            slog(svc, "SVC_IPC_AUDIODEV command=5 call=GetActiveChannelCount channels=2 model=stereo-output meta=0x%08x result=0\n",
                    response_meta);
            sflush(svc);
            return 0;
        }
        ++svc->ipc_unsupported;
        slog(svc, "SVC_IPC_AUDIODEV_UNSUPPORTED command=%u revision=0x%08x\n",
                command, state->audio_device_revision);
        sflush(svc);
        return VP_BRIDGE_UNSUPPORTED;
    }
    if (session->kind == 2 && request_meta && !(request_meta & 0xfu)
        && !strcmp(session->name, "mm:u") && command <= 7u) {
        /* nn::mmnv::IRequest: Module, Priority, EventClearMode -> RequestId;
         * SetAndWait(RequestId, Min, Max); Get(RequestId) -> current. The old
         * commands 0..3 use Module where the new commands 4..7 use the id.
         * The service manages the multimedia engines' clock settings; this
         * platform has no media clock, so SetAndWait records the requested
         * minimum as the current setting and completes, like the apm
         * configuration model. No frequency or decode behavior is implied. */
        unsigned op = command & 3u;
        int old_form = command < 4u;
        uint32_t first = get32(request_meta + 0x10u);
        uint32_t second = get32(request_meta + 0x14u);
        uint32_t third = get32(request_meta + 0x18u);
        /* Bring-up diagnostic: the request word layout is the ABI under test,
         * so record the raw prefix instead of only the decoded fields. */
        uint32_t raw[8];
        for (unsigned i = 0; i < 8u; ++i) raw[i] = get32(request_meta + 0x10u + i * 4u);
        slog(svc, "SVC_IPC_MM_RAW command=%u words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x lr=0x%08x pc=0x%08x\n",
             command, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
             f->lr, f->pc);
        if (command == 4u) {
            /* One-shot bring-up stack: the media player issues mm:u from a
             * deep SDK call chain, so the main-module return address that
             * identifies the caller is only visible on the guest stack. */
            for (unsigned i = 0; i < 32u; ++i) {
                uint32_t p = f->sp + i * 4u;
                if (!vp_guest_svc_readable(svc, p, 4u)) break;
                slog(svc, "SVC_IPC_MM_STACK +0x%02x 0x%08x 0x%08x\n",
                     i * 4u, p, get32(p));
            }
        }
        int by_module = old_form || op == 0u;
        VPGuestMediaClock *entry = NULL;
        for (unsigned i = 0; i < VP_GUEST_MEDIA_CLOCK_LIMIT; ++i)
            if (svc->media_clock[i].active
                && (by_module ? svc->media_clock[i].module == first
                              : svc->media_clock[i].request_id == first)) {
                entry = &svc->media_clock[i];
                break;
            }
        uint32_t response_meta = plain_response_meta(buffer);
        uint32_t response_words = 4;
        uint32_t result = 0;
        const char *call = op == 0u ? "Initialize"
                         : op == 1u ? "Finalize"
                         : op == 2u ? "SetAndWait" : "Get";
        if (op == 0u) {
            if (!entry || !old_form) {
                VPGuestMediaClock *slot = NULL;
                for (unsigned i = 0; i < VP_GUEST_MEDIA_CLOCK_LIMIT; ++i)
                    if (!svc->media_clock[i].active) { slot = &svc->media_clock[i]; break; }
                if (!slot) {
                    result = SVC_RESULT_OUT_OF_MEMORY;
                    entry = NULL;
                } else {
                    uint32_t id = ++svc->media_clock_next;
                    if (!id) id = ++svc->media_clock_next;
                    memset(slot, 0, sizeof(*slot));
                    slot->active = 1;
                    slot->request_id = id;
                    slot->module = first;
                    slot->priority = second;
                    slot->clear_mode = third;
                    slot->current = 0;
                    entry = slot;
                }
            }
            if (entry) {
                response_words = old_form ? 4u : 5u;
                if (!old_form) put32(response_meta + 0x10u, entry->request_id);
            }
        } else if (op == 1u) {
            if (!entry) result = SVC_RESULT_INVALID_HANDLE;
            else entry->active = 0;
        } else if (op == 2u) {
            if (!entry) result = SVC_RESULT_INVALID_HANDLE;
            else {
                /* Reference semantics (yuzu/mhgu-runtime mm:u stub): the
                 * requested minimum becomes the current setting, and Get
                 * returns that same value. The media player requests
                 * 12,000,000 once, then negative and positive adjustments
                 * while it paces the NVDEC clock; echoing the requested
                 * absolute value is what the reference stack observes. No
                 * clock exists here and the value has no hardware effect. */
                entry->min_setting = second;
                entry->max_setting = third;
                entry->current = second;
            }
        } else {
            if (!entry) result = SVC_RESULT_INVALID_HANDLE;
            else {
                response_words = 5;
                put32(response_meta + 0x10u, entry->current);
            }
        }
        put32(buffer, 0);
        put32(buffer + 4, response_words);
        put32(response_meta, 0x4F434653u);
        put32(response_meta + 4, 0);
        put32(response_meta + 8, result);
        put32(response_meta + 0xc, 0);
        f->regs[0] = 0;
        if (entry)
            slog(svc, "SVC_IPC_MM command=%u call=%s form=%s module=%u id=%u priority=%u clear_mode=%u min=%u max=%u current=%u model=no-media-clock result=0x%08x meta=0x%08x t_us=%llu thread=0x%x\n",
                    command, call, old_form ? "module" : "request-id",
                    entry->module, entry->request_id, entry->priority, entry->clear_mode,
                    entry->min_setting, entry->max_setting, entry->current,
                    result, response_meta,
                    (unsigned long long)sceKernelGetProcessTimeWide(),
                    svc->current_thread_handle);
        else
            slog(svc, "SVC_IPC_MM command=%u call=%s form=%s key=%u model=no-media-clock result=0x%08x meta=0x%08x t_us=%llu thread=0x%x\n",
                    command, call, old_form ? "module" : "request-id", first,
                    result, response_meta,
                    (unsigned long long)sceKernelGetProcessTimeWide(),
                    svc->current_thread_handle);
        sflush(svc);
        return 0;
    }
    ++svc->ipc_unsupported;
    slog(svc, "SVC_IPC_UNSUPPORTED service=%s command=%u\n", session->name, command);
    sflush(svc);
    return VP_BRIDGE_UNSUPPORTED;
}

static const VPGuestRegion *find_region(const VPGuestSvc *svc, uint32_t address) {
    for (unsigned i = 0; i < svc->region_count; ++i) {
        const VPGuestRegion *r = &svc->regions[i];
        if (address >= r->base && address - r->base < r->size) return r;
    }
    return NULL;
}

static uint32_t memory_attribute_at(const VPGuestSvc *svc, uint32_t address) {
    return vp_memory_attributes_at(&svc->attributes, address);
}

/* svcSetMemoryAttribute(address, size, mask, attr). The attribute bits are
 * Locked 1, IpcLocked 2, DeviceShared 4, Uncached 8. Ordinary guest loads and
 * stores run natively on the Vita CPU, so recording the effective per-page
 * state is the whole observable effect here; any device that later accesses
 * the range (the GPU boundary) has to consume this record instead of
 * assuming CPU coherence. */
static int set_memory_attribute(VPGuestSvc *svc, VPBridgeFrame *f) {
    uint32_t address = f->regs[0], size = f->regs[1];
    uint32_t mask = f->regs[2], attr = f->regs[3];
    if (!size || (address & 0xFFFu) || (size & 0xFFFu) || (attr & ~mask)) {
        f->regs[0] = SVC_RESULT_INVALID_ADDRESS;
        slog(svc, "SVC_SET_MEMORY_ATTRIBUTE address=0x%08x size=0x%08x mask=0x%x attr=0x%x result=0x%08x\n",
                address, size, mask, attr, SVC_RESULT_INVALID_ADDRESS);
        sflush(svc);
        return 0;
    }
    if (mask & ~0xFu) {
        f->regs[0] = SVC_RESULT_INVALID_ENUM;
        slog(svc, "SVC_SET_MEMORY_ATTRIBUTE_INVALID address=0x%08x size=0x%08x mask=0x%x attr=0x%x\n",
                address, size, mask, attr);
        sflush(svc);
        return 0;
    }
    /* The range end is 64-bit; address + size == 0x100000000 is valid, while
     * anything above it is a wrap the kernel would reject. */
    if ((uint64_t)address + size > VP_MEMORY_ATTRIBUTE_END
        || !vp_guest_svc_writable(svc, address, size)) {
        f->regs[0] = SVC_RESULT_INVALID_ADDRESS;
        slog(svc, "SVC_SET_MEMORY_ATTRIBUTE_UNMAPPED address=0x%08x size=0x%08x mask=0x%x attr=0x%x\n",
                address, size, mask, attr);
        sflush(svc);
        return 0;
    }
    int state = vp_memory_attributes_set(&svc->attributes, address, size,
                                         mask, attr);
    if (state == VP_MEMORY_ATTRIBUTE_INVALID) {
        f->regs[0] = SVC_RESULT_INVALID_ADDRESS;
        slog(svc, "SVC_SET_MEMORY_ATTRIBUTE_REJECT address=0x%08x size=0x%08x mask=0x%x attr=0x%x model=effective-intervals result=0x%08x\n",
                address, size, mask, attr, SVC_RESULT_INVALID_ADDRESS);
        sflush(svc);
        return 0;
    }
    if (state == VP_MEMORY_ATTRIBUTE_FULL) {
        ++svc->unsupported;
        slog(svc, "SVC_SET_MEMORY_ATTRIBUTE_LIMIT count=%u address=0x%08x size=0x%08x mask=0x%x attr=0x%x model=effective-intervals result=unsupported\n",
                svc->attributes.count, address, size, mask, attr);
        sflush(svc);
        return VP_BRIDGE_UNSUPPORTED;
    }
    f->regs[0] = 0;
    slog(svc, "SVC_SET_MEMORY_ATTRIBUTE address=0x%08x size=0x%08x mask=0x%x attr=0x%x ranges=%u result=0\n",
            address, size, mask, attr, svc->attributes.count);
    sflush(svc);
    return 0;
}

/* svcInvalidateProcessDataCache(0x5d), svcStoreProcessDataCache(0x5e) and
 * svcFlushProcessDataCache(0x5f). The AArch32 veneer passes the process
 * handle in r0, the size low/high in r1/r4 and the address low/high in
 * r2/r3. The title uses these to publish its nvmap buffers to the GPU. This
 * loader has no device reading guest memory, and all guest accesses are
 * native CPU accesses whose cache behavior is coherent, so nothing is lost;
 * every range is logged because the future GPU boundary must perform its own
 * coherence instead of assuming these calls did anything. */
static int data_cache_maintenance(VPGuestSvc *svc, VPBridgeFrame *f, const char *call) {
    uint32_t handle = f->regs[0];
    uint64_t size = (uint64_t)f->regs[1] | ((uint64_t)f->regs[4] << 32);
    uint64_t address = (uint64_t)f->regs[2] | ((uint64_t)f->regs[3] << 32);
    if (handle != 0xffff8001u && handle != 0xffff8000u) {
        f->regs[0] = SVC_RESULT_INVALID_HANDLE;
        slog(svc, "SVC_%s handle=0x%08x address=0x%llx size=0x%llx result=0x%08x\n",
                call, handle, (unsigned long long)address,
                (unsigned long long)size, SVC_RESULT_INVALID_HANDLE);
        sflush(svc);
        return 0;
    }
    if (address > 0xffffffffull || size > 0xffffffffull
        || (uint32_t)size != size
        || !vp_guest_svc_readable(svc, (uint32_t)address, (uint32_t)size)) {
        f->regs[0] = SVC_RESULT_INVALID_ADDRESS;
        slog(svc, "SVC_%s address=0x%llx size=0x%llx result=0x%08x\n",
                call, (unsigned long long)address,
                (unsigned long long)size, SVC_RESULT_INVALID_ADDRESS);
        sflush(svc);
        return 0;
    }
    f->regs[0] = 0;
    /* Success is not logged per call: the title flushes cache lines every
     * frame, and the failures above are the events that matter. */
    return 0;
}

/* Mapped shared-memory pages belong to the object, not to the free backing
 * region they were copied into: the kernel reports them as Shared so an
 * address-space allocator cannot hand the same virtual range out twice. */
static const VPGuestHandle *find_shared_mapping(const VPGuestSvc *svc, uint32_t address) {
    for (unsigned i = 0; i < VP_GUEST_HANDLE_LIMIT; ++i) {
        const VPGuestHandle *h = &svc->handles[i];
        if (h->kind == VP_GUEST_HANDLE_KIND_SHARED && h->shared_mapped
            && address >= h->shared_address && address - h->shared_address < h->shared_size)
            return h;
    }
    return NULL;
}

/* svcMapMemory destinations are owned by the alias mapping for as long as the
 * mapping exists. Reporting them as Free lets the SDK address-space allocator
 * place another object on top (observed: two alias thread stacks overlapped,
 * the second copy zeroed the first thread's entry slot). */
static int find_map_copy(const VPGuestSvc *svc, uint32_t address,
                         uint64_t *base, uint64_t *size) {
    for (uint32_t i = 0; i < svc->map_copy_count; ++i) {
        uint32_t b = svc->map_copy_ranges[i].dst;
        uint32_t s = svc->map_copy_ranges[i].size;
        if (address >= b && address - b < s) {
            *base = b;
            *size = s;
            return 1;
        }
    }
    return 0;
}

static int map_copy_overlaps(const VPGuestSvc *svc, uint32_t dst, uint32_t size) {
    uint32_t end = dst + size;
    for (uint32_t i = 0; i < svc->map_copy_count; ++i) {
        uint32_t b = svc->map_copy_ranges[i].dst;
        uint32_t e = b + svc->map_copy_ranges[i].size;
        if (dst < e && b < end) return 1;
    }
    return 0;
}

static int query_memory(VPGuestSvc *svc, VPBridgeFrame *f) {
    uint32_t info = f->regs[0], page_out = f->regs[1], address = f->regs[2];
    if (!vp_guest_svc_writable(svc, info, 40)) return -141;
    if (page_out && !vp_guest_svc_writable(svc, page_out, 4)) return -141;
    const VPGuestHandle *shared = find_shared_mapping(svc, address);
    const VPGuestRegion *r = find_region(svc, address);
    uint64_t base = 0, limit = 0;
    uint32_t state = 0, permission = 0;
    if (shared) {
        base = shared->shared_address;
        limit = base + shared->shared_size;
        state = 6;          /* Kernel::Svc::MemoryState::Shared */
        permission = 1;     /* hid client maps its object read-only */
    } else if (find_map_copy(svc, address, &base, &limit)) {
        /* A live svcMapMemory destination: the alias mapping owns the range
         * until svcUnmapMemory. Alias stacks are read/write. */
        limit = base + limit;
        state = 6;          /* Kernel::Svc::MemoryState::Shared */
        permission = 3;
    } else if (r) {
        base = address - ((address - r->base) & ~0xFFFu); /* page-aligned view */
        if (base < r->base) base = r->base;
        limit = (uint64_t)r->base + r->size;
        state = r->state;
        permission = r->permission;
    } else {
        base = address & ~0xFFFu;
        limit = base + 0x1000;
    }
    /* svcSetMemoryAttribute is per-page state: a returned block must not mix
     * two attributes. Trim the mapping-derived block to the effective unit
     * that contains the queried address. State and permission still describe
     * the surrounding mapping; only its reported extent shrinks. */
    const uint32_t attribute = memory_attribute_at(svc, address);
    const uint64_t attribute_start =
        vp_memory_attributes_start(&svc->attributes, address);
    const uint64_t attribute_end =
        vp_memory_attributes_end(&svc->attributes, address);
    if (base < attribute_start) base = attribute_start;
    if (limit > attribute_end) limit = attribute_end;
    const uint64_t size = limit > base ? limit - base : 0;
    uint8_t out[40] = {0};
    memcpy(out, &base, 8);
    memcpy(out + 8, &size, 8);
    memcpy(out + 16, &state, 4);
    memcpy(out + 20, &attribute, 4);
    memcpy(out + 24, &permission, 4);
    memcpy((void *)(uintptr_t)info, out, sizeof(out));
    f->regs[0] = 0;
    f->regs[1] = 0; /* PageInfo: no device/uncached flags for loader-owned memory */
    return 0;
}

static int get_info(VPGuestSvc *svc, VPBridgeFrame *f) {
    uint64_t subtype = (uint64_t)f->regs[0] | ((uint64_t)f->regs[3] << 32);
    uint32_t type = f->regs[1], handle = f->regs[2];
    uint64_t value = 0;
    uint32_t result = 0;
    switch (type) {
    case 0: value = 1; break;                 /* CoreMask: one Vita core */
    case 1: value = 0xFFFFFFFFu; break;       /* PriorityMask */
    case 2: value = svc->alias_base; break;   /* AliasRegionAddress */
    case 3: value = svc->alias_size; break;   /* AliasRegionSize */
    case 4: value = svc->heap_base; break;    /* HeapRegionAddress */
    case 5: value = svc->heap_size; break;    /* HeapRegionSize */
    case 6: value = svc->total_memory; break; /* TotalMemorySize */
    case 7: value = svc->used_memory; break;  /* UsedMemorySize */
    case 10:                                  /* IdleTickCount */
        if (handle != 0 && handle != SVC_INVALID_HANDLE) { result = SVC_RESULT_INVALID_HANDLE; break; }
        value = 0;
        break;
    case 11: {                                /* RandomEntropy */
        if (handle != 0 || subtype >= 4) { result = SVC_RESULT_INVALID_COMBINATION; break; }
        uint64_t entropy = 0;
        if (sceKernelGetRandomNumber(&entropy, sizeof(entropy)) < 0) entropy = 0;
        value = entropy ^ ((uint64_t)svc->process_id << 32) ^ subtype;
        break;
    }
    case 12: value = svc->aslr_base; break;   /* AslrRegionAddress */
    case 13: value = svc->aslr_size; break;   /* AslrRegionSize */
    case 14: value = svc->stack_base; break;  /* StackRegionAddress */
    case 15: value = svc->stack_size; break;  /* StackRegionSize */
    case 18: value = ((uint64_t)PROGRAM_ID_HIGH << 32) | PROGRAM_ID_LOW; break;
    case 20: value = svc->plr_base; break;    /* UserExceptionContextAddress */
    case 23: value = 1; break;                /* IsApplication */
    case 24: value = svc->free_threads; break;
    case 25: value = 0; break;                /* ThreadTickCount */
    default:
        slog(svc, "SVC_GETINFO_UNSUPPORTED type=0x%x handle=0x%08x subtype=0x%llx\n",
                type, handle, (unsigned long long)subtype);
        sflush(svc);
        return VP_BRIDGE_UNSUPPORTED;
    }
    if (result) {
        slog(svc, "SVC_GETINFO_REJECT type=0x%x handle=0x%08x subtype=0x%llx result=0x%08x\n",
                type, handle, (unsigned long long)subtype, result);
        sflush(svc);
    }
    f->regs[0] = result;
    f->regs[1] = (uint32_t)value;
    f->regs[2] = (uint32_t)(value >> 32);
    return 0;
}

static int map_memory(VPGuestSvc *svc, VPBridgeFrame *f) {
    /* svcMapMemory(dst, src, size): Horizon aliases the pages backing `src`
     * into the caller's address space at `dst`. A user process cannot create
     * a second mapping of its own pages on Vita, so the loader models the
     * call by copying the source range into the destination. The only live
     * user of this path is ThreadManager::CreateAliasStackUnsafe, which maps
     * a fresh thread stack: the initial CPU context lives in ThreadType and
     * the guest never reads the source buffer after the map. Writes performed
     * through the two views after this call are not coherent; every call is
     * logged so a guest path that starts relying on aliasing is visible. */
    uint32_t dst = f->regs[0], src = f->regs[1], size = f->regs[2];
    if (!size || (dst & 0xFFFu) || (src & 0xFFFu) || (size & 0xFFFu)
        || size > 0x10000000u || !vp_guest_svc_writable(svc, dst, size)
        || !vp_guest_svc_readable(svc, src, size)) {
        f->regs[0] = SVC_RESULT_INVALID_ADDRESS;
        slog(svc, "SVC_MAPMEMORY dst=0x%08x src=0x%08x size=0x%08x model=copy result=0x%08x\n",
                dst, src, size, SVC_RESULT_INVALID_ADDRESS);
        sflush(svc);
        return 0;
    }
    if (map_copy_overlaps(svc, dst, size)) {
        f->regs[0] = SVC_RESULT_INVALID_ADDRESS;
        slog(svc, "SVC_MAPMEMORY_OVERLAP dst=0x%08x src=0x%08x size=0x%08x live=%u\n",
                dst, src, size, svc->map_copy_count);
        sflush(svc);
        return 0;
    }
    if (svc->map_copy_count >= VP_GUEST_MAP_COPY_LIMIT) {
        f->regs[0] = SVC_RESULT_INVALID_ADDRESS;
        slog(svc, "SVC_MAPMEMORY_TABLE_FULL dst=0x%08x src=0x%08x size=0x%08x live=%u\n",
                dst, src, size, svc->map_copy_count);
        sflush(svc);
        return 0;
    }
    memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, size);
    svc->map_copy_ranges[svc->map_copy_count].dst = dst;
    svc->map_copy_ranges[svc->map_copy_count].src = src;
    svc->map_copy_ranges[svc->map_copy_count].size = size;
    svc->map_copy_ranges[svc->map_copy_count].owner = 0;
    ++svc->map_copy_count;
    ++svc->map_copies;
    f->regs[0] = 0;
    slog(svc, "SVC_MAPMEMORY dst=0x%08x src=0x%08x size=0x%08x model=copy result=0\n",
            dst, src, size);
    sflush(svc);
    return 0;
}

/* svcUnmapMemory(dst, src, size). The alias created by MapMemory above is a
 * copy, so there is no kernel page group to detach. The kernel also removes
 * the mapping's memory attributes when a range is unmapped; the effective
 * attribute table drops the range before any map-copy bookkeeping changes.
 * A split that would exceed the fixed interval bound rejects the call with
 * the original table and mapping state intact. */
static void forget_map_copy_range(VPGuestSvc *svc, uint32_t base, uint32_t size) {
    uint32_t end = base + size;
    unsigned out = 0;
    int found = 0;
    for (unsigned i = 0; i < svc->map_copy_count; ++i) {
        uint32_t b = svc->map_copy_ranges[i].dst;
        uint32_t e = b + svc->map_copy_ranges[i].size;
        uint32_t src = svc->map_copy_ranges[i].src;
        uint32_t owner = svc->map_copy_ranges[i].owner;
        if (e <= base || b >= end) {
            svc->map_copy_ranges[out] = svc->map_copy_ranges[i];
            ++out;
            continue;
        }
        found = 1;
        if (b < base && out < VP_GUEST_MAP_COPY_LIMIT) {
            svc->map_copy_ranges[out].dst = b;
            svc->map_copy_ranges[out].src = src;
            svc->map_copy_ranges[out].size = base - b;
            svc->map_copy_ranges[out].owner = owner;
            ++out;
        }
        if (e > end && out < VP_GUEST_MAP_COPY_LIMIT) {
            svc->map_copy_ranges[out].dst = end;
            svc->map_copy_ranges[out].src = src + (end - b);
            svc->map_copy_ranges[out].size = e - end;
            svc->map_copy_ranges[out].owner = owner;
            ++out;
        }
    }
    svc->map_copy_count = out;
    if (!found) {
        ++svc->map_copy_untracked;
        if (svc->map_copy_untracked <= 16u)
            slog(svc, "SVC_UNMAPMEMORY_UNTRACKED dst=0x%08x size=0x%08x live=%u\n",
                    base, size, svc->map_copy_count);
    }
}

static int unmap_memory(VPGuestSvc *svc, VPBridgeFrame *f) {
    uint32_t dst = f->regs[0], src = f->regs[1], size = f->regs[2];
    if (!size || (dst & 0xFFFu) || (size & 0xFFFu) || size > 0x10000000u
        || (uint64_t)dst + size > VP_MEMORY_ATTRIBUTE_END
        || !vp_guest_svc_writable(svc, dst, size)) {
        f->regs[0] = SVC_RESULT_INVALID_ADDRESS;
        slog(svc, "SVC_UNMAPMEMORY dst=0x%08x src=0x%08x size=0x%08x result=0x%08x\n",
                dst, src, size, SVC_RESULT_INVALID_ADDRESS);
        sflush(svc);
        return 0;
    }
    int attribute_state = vp_memory_attributes_forget(&svc->attributes, dst, size);
    if (attribute_state == VP_MEMORY_ATTRIBUTE_FULL) {
        ++svc->unsupported;
        slog(svc, "SVC_UNMAPMEMORY_ATTRIBUTE_LIMIT dst=0x%08x size=0x%08x count=%u model=effective-intervals result=unsupported\n",
                dst, size, svc->attributes.count);
        sflush(svc);
        return VP_BRIDGE_UNSUPPORTED;
    }
    if (attribute_state == VP_MEMORY_ATTRIBUTE_INVALID) {
        f->regs[0] = SVC_RESULT_INVALID_ADDRESS;
        slog(svc, "SVC_UNMAPMEMORY_ATTRIBUTE_REJECT dst=0x%08x size=0x%08x result=0x%08x\n",
                dst, size, SVC_RESULT_INVALID_ADDRESS);
        sflush(svc);
        return 0;
    }
    forget_map_copy_range(svc, dst, size);
    if (svc->map_copies) --svc->map_copies;
    f->regs[0] = 0;
    slog(svc, "SVC_UNMAPMEMORY dst=0x%08x src=0x%08x size=0x%08x model=copy-alias result=0\n",
            dst, src, size);
    sflush(svc);
    return 0;
}

/* svc 0x15 CreateTransferMemory. The AArch32 veneer keeps the kernel ABI
 * (r1=address, r2=size, r3=permission) and passes its own out-pointer in r0;
 * the kernel returns Result in r0 and the new handle in r1, which the veneer
 * body stores through that out-pointer. The source range stays mapped in the
 * process; the handle records what a later MapTransferMemory would expose. */
/* svc 0x13 MapSharedMemory(handle, address, size, permission). Horizon
 * aliases the shared memory object's pages into the caller at an address the
 * caller chose; the kernel requires the requested size to equal the object
 * size and the permission to equal the object's client permission. A user
 * process cannot create a second mapping of a kernel-owned page group on
 * Vita, so the loader models the alias by writing the object's current bytes
 * over the destination range. The only object it creates is the hid
 * applet-resource block, which is zero at creation and never written by any
 * server here (this platform has no input devices), so the copied image is
 * exact; the call and its range are logged so a future input model that
 * starts writing the object is visible. */
static int map_shared_memory(VPGuestSvc *svc, VPBridgeFrame *f) {
    uint32_t handle = f->regs[0], address = f->regs[1], size = f->regs[2], permission = f->regs[3];
    VPGuestHandle *slot = find_handle(svc, handle);
    uint32_t result = 0;
    const char *reason = "";
    if (!slot || slot->kind != VP_GUEST_HANDLE_KIND_SHARED) {
        result = SVC_RESULT_INVALID_HANDLE;
        reason = "handle";
    } else if (size != slot->shared_size) {
        result = SVC_RESULT_INVALID_ADDRESS;
        reason = "size";
    } else if (permission != 1u) {
        /* Kernel checks map_perm == client permission; the hid object's
         * client permission is Read. */
        result = SVC_RESULT_INVALID_COMBINATION;
        reason = "permission";
    } else if ((address & 0xFFFu) || slot->shared_mapped
               || !vp_guest_svc_writable(svc, address, size)) {
        result = SVC_RESULT_INVALID_ADDRESS;
        reason = slot->shared_mapped ? "already-mapped" : "range";
    }
    if (result) {
        f->regs[0] = result;
        slog(svc, "SVC_MAP_SHARED handle=0x%08x address=0x%08x size=0x%x permission=%u model=rejected reason=%s result=0x%08x\n",
                handle, address, size, permission, reason, result);
        sflush(svc);
        return 0;
    }
    memset((void *)(uintptr_t)address, 0, size);
    slot->shared_mapped = 1;
    slot->shared_address = address;
    {
        unsigned si;
        for (si = 0; si < VP_GUEST_SESSION_LIMIT; si++) {
            if (svc->sessions[si].hid_shared_handle == handle)
                svc->hid_shared_addr = address;
        }
    }
    slot->shared_permission = permission;
    ++svc->shared_maps;
    f->regs[0] = 0;
    slog(svc, "SVC_MAP_SHARED handle=0x%08x address=0x%08x size=0x%x permission=%u model=zeroed-object-copy result=0\n",
            handle, address, size, permission);
    sflush(svc);
    return 0;
}

/* svc 0x14 UnmapSharedMemory(address, size). The kernel drops the page-group
 * mapping and requires size == object size. The range was the guest's own
 * memory in this model, so the call only retires the bookkeeping; it does not
 * alter any bytes. */
static int unmap_shared_memory(VPGuestSvc *svc, VPBridgeFrame *f) {
    uint32_t address = f->regs[0], size = f->regs[1];
    VPGuestHandle *slot = NULL;
    for (unsigned i = 0; i < VP_GUEST_HANDLE_LIMIT && !slot; ++i) {
        VPGuestHandle *candidate = &svc->handles[i];
        if (candidate->kind == VP_GUEST_HANDLE_KIND_SHARED && candidate->shared_mapped
            && candidate->shared_address == address) slot = candidate;
    }
    uint32_t result = 0;
    const char *reason = "";
    if (!slot) {
        result = SVC_RESULT_INVALID_ADDRESS;
        reason = "not-mapped";
    } else if (size != slot->shared_size) {
        result = SVC_RESULT_INVALID_ADDRESS;
        reason = "size";
    } else {
        slot->shared_mapped = 0;
        slot->shared_address = 0;
        slot->shared_permission = 0;
    }
    f->regs[0] = result;
    slog(svc, "SVC_UNMAP_SHARED address=0x%08x size=0x%x model=bookkeeping reason=%s result=0x%08x\n",
            address, size, reason, result);
    sflush(svc);
    return 0;
}

static int create_transfer_memory(VPGuestSvc *svc, VPBridgeFrame *f) {
    uint32_t out = f->regs[0], address = f->regs[1], size = f->regs[2], permission = f->regs[3];
    if (!vp_guest_svc_writable(svc, out, 4)) return -141;
    if (!size || (address & 0xFFFu) || (size & 0xFFFu) || permission > 3
        || size > 0x10000000u || !vp_guest_svc_writable(svc, address, size)) {
        uint32_t result = permission > 3 ? SVC_RESULT_INVALID_ENUM : SVC_RESULT_INVALID_ADDRESS;
        f->regs[0] = result;
        slog(svc, "SVC_CREATE_TRANSFER address=0x%08x size=0x%08x permission=%u model=rejected result=0x%08x\n",
                address, size, permission, result);
        sflush(svc);
        return 0;
    }
    uint32_t handle = create_handle(svc, VP_GUEST_HANDLE_KIND_TRANSFER, "transfer", 0);
    if (!handle) {
        f->regs[0] = SVC_RESULT_OUT_OF_MEMORY;
        slog(svc, "SVC_CREATE_TRANSFER address=0x%08x size=0x%08x permission=%u model=rejected result=0x%08x\n",
                address, size, permission, SVC_RESULT_OUT_OF_MEMORY);
        sflush(svc);
        return 0;
    }
    VPGuestHandle *slot = find_handle(svc, handle);
    slot->transfer_address = address;
    slot->transfer_size = size;
    slot->transfer_permission = permission;
    f->regs[0] = 0;
    f->regs[1] = handle;
    slog(svc, "SVC_CREATE_TRANSFER address=0x%08x size=0x%08x permission=%u handle=0x%08x model=owner-keeps-mapping\n",
            address, size, permission, handle);
    sflush(svc);
    return 0;
}

static int set_heap_size(VPGuestSvc *svc, VPBridgeFrame *f) {
    uint32_t out = f->regs[0], requested = f->regs[1];
    if (!vp_guest_svc_writable(svc, out, 4)) return -141;
    if (requested > svc->heap_size) {
        f->regs[0] = SVC_RESULT_OUT_OF_MEMORY;
        slog(svc, "SVC_SETHEAP requested=%u capacity=%u result=0x%08x\n",
                requested, svc->heap_size, SVC_RESULT_OUT_OF_MEMORY);
        sflush(svc);
        return 0;
    }
    /* Horizon returns the fixed process heap base in r1; the 32-bit veneer
     * continuation stores that register through the caller's out pointer. */
    f->regs[0] = 0;
    f->regs[1] = svc->heap_base;
    if (requested > svc->used_memory) svc->used_memory = requested;
    slog(svc, "SVC_SETHEAP requested=%u base=0x%08x capacity=%u\n", requested, svc->heap_base, svc->heap_size);
    sflush(svc);
    return 0;
}

static int signal_process_wide_key(VPGuestSvc *svc, VPBridgeFrame *f) {
    /* 32-bit wrapper is void: the address is an opaque futex key and the
     * kernel has no Result register to report. Signals are transient, so a
     * wake with no waiter is a real no-op rather than a fabricated success. */
    ++svc->key_signals;
    slog(svc, "SVC_SIGNAL_KEY address=0x%08x count=%d waiters=0\n",
            f->regs[0], (int)f->regs[1]);
    return 0;
}

static int is_current_thread(const VPGuestSvc *svc, uint32_t handle) {
    return handle == svc->current_thread_handle || handle == 0xFFFF8000u;
}

/* Guest-created worker threads are ordinary kernel handles. The SDK sets a
 * worker's core mask right after svcCreateThread and before svcStartThread,
 * so the call arrives while the caller is still a different thread. */
static VPGuestHandle *thread_handle(VPGuestSvc *svc, uint32_t handle) {
    if (handle < VP_GUEST_HANDLE_FIRST || handle >= VP_GUEST_HANDLE_FIRST + VP_GUEST_HANDLE_LIMIT)
        return NULL;
    VPGuestHandle *slot = &svc->handles[handle - VP_GUEST_HANDLE_FIRST];
    return slot->kind == VP_GUEST_HANDLE_KIND_THREAD ? slot : NULL;
}

static int get_thread_priority(VPGuestSvc *svc, VPBridgeFrame *f) {
    /* 32-bit kernel ABI: r1 = handle; result r0, priority r1. The veneer
     * stores the returned priority through the caller's out pointer. */
    uint32_t handle = f->regs[1], result = 0, priority = 0;
    VPGuestHandle *slot = thread_handle(svc, handle);
    if (is_current_thread(svc, handle)) priority = svc->thread_priority;
    else if (slot) priority = slot->thread_priority;
    else result = SVC_RESULT_INVALID_HANDLE;
    f->regs[0] = result;
    f->regs[1] = priority;
    slog(svc, "SVC_GETPRIORITY handle=0x%08x priority=%u result=0x%08x\n",
            handle, priority, result);
    return 0;
}

static int set_thread_priority(VPGuestSvc *svc, VPBridgeFrame *f) {
    uint32_t handle = f->regs[0];
    int32_t priority = (int32_t)f->regs[1];
    uint32_t result = 0;
    VPGuestHandle *slot = thread_handle(svc, handle);
    if (!is_current_thread(svc, handle) && !slot) result = SVC_RESULT_INVALID_HANDLE;
    else if (priority < 0 || priority > (int32_t)HORIZON_PRIORITY_MAX) result = SVC_RESULT_INVALID_ENUM;
    else if (is_current_thread(svc, handle)) svc->thread_priority = (uint32_t)priority;
    else slot->thread_priority = (uint32_t)priority;
    f->regs[0] = result;
    slog(svc, "SVC_SETPRIORITY handle=0x%08x priority=%d result=0x%08x\n",
            handle, (int)priority, result);
    return 0;
}

static int get_thread_core_mask(VPGuestSvc *svc, VPBridgeFrame *f) {
    /* 32-bit kernel ABI: r2 = handle; result r0, core r1, mask r2:r3. */
    uint32_t handle = f->regs[2], result = 0, core = 0, low = 0, high = 0;
    VPGuestHandle *slot = thread_handle(svc, handle);
    uint64_t mask = 0;
    if (is_current_thread(svc, handle)) {
        core = svc->thread_core;
        mask = svc->thread_affinity;
    } else if (slot) {
        /* -1 is a recorded ideal core of "no preference"; -2 is the port's
         * internal "never set" sentinel and keeps answering 0. */
        core = slot->thread_core < -1 ? 0u : (uint32_t)slot->thread_core;
        mask = slot->thread_affinity;
    } else result = SVC_RESULT_INVALID_HANDLE;
    low = (uint32_t)mask;
    high = (uint32_t)(mask >> 32);
    f->regs[0] = result;
    f->regs[1] = core;
    f->regs[2] = low;
    f->regs[3] = high;
    slog(svc, "SVC_GETCOREMASK handle=0x%08x core=%u mask=0x%llx result=0x%08x\n",
            handle, core, (unsigned long long)mask, result);
    return 0;
}

static int set_thread_core_mask(VPGuestSvc *svc, VPBridgeFrame *f) {
    uint32_t handle = f->regs[0], result = 0;
    int32_t core = (int32_t)f->regs[1];
    uint64_t mask = (uint64_t)f->regs[2] | ((uint64_t)f->regs[3] << 32);
    VPGuestHandle *slot = thread_handle(svc, handle);
    /* svcSetThreadCoreMask's r1 is the ideal core; -1 means "no preference"
     * (switchbrew/Atmosph猫re ABI). The movie player's NuPlayer threads call
     * SetThreadCoreMask(thread, -1, mask) when they start, so rejecting -1
     * makes nn::os abort the thread with 0xE0010004 before it opens the
     * movie file. Only a concrete ideal core has to be inside the mask. */
    if (!is_current_thread(svc, handle) && !slot) result = SVC_RESULT_INVALID_HANDLE;
    else if (core < -1 || core > 3 || mask == 0
             || (core >= 0 && (mask & (1ull << core)) == 0))
        result = SVC_RESULT_INVALID_COMBINATION;
    else if (is_current_thread(svc, handle)) {
        svc->thread_core = (uint32_t)core; svc->thread_affinity = (uint32_t)mask;
    } else {
        slot->thread_core = core; slot->thread_affinity = mask;
    }
    f->regs[0] = result;
    slog(svc, "SVC_SETCOREMASK handle=0x%08x core=%d mask=0x%llx result=0x%08x\n",
            handle, (int)core, (unsigned long long)mask, result);
    return 0;
}

static int get_thread_id(VPGuestSvc *svc, VPBridgeFrame *f) {
    /* 32-bit kernel ABI: r1 = handle; result r0, thread id r1:r2. */
    uint32_t handle = f->regs[1], result = 0;
    uint64_t id = 0;
    if (is_current_thread(svc, handle))
        id = ((uint64_t)svc->process_id << 32) | svc->current_thread_handle;
    else if (thread_handle(svc, handle))
        id = ((uint64_t)svc->process_id << 32) | handle;
    else result = SVC_RESULT_INVALID_HANDLE;
    f->regs[0] = result;
    f->regs[1] = (uint32_t)id;
    f->regs[2] = (uint32_t)(id >> 32);
    slog(svc, "SVC_GETTHREADID handle=0x%08x id=0x%llx result=0x%08x\n",
            handle, (unsigned long long)id, result);
    return 0;
}

static int get_process_id(VPGuestSvc *svc, VPBridgeFrame *f) {
    /* 32-bit kernel ABI: r1 = handle; result r0, process id r1:r2. */
    uint32_t handle = f->regs[1], result = 0;
    uint64_t id = 0;
    if (handle == HORIZON_CURRENT_PROCESS || handle == svc->current_thread_handle)
        id = svc->process_id;
    else result = SVC_RESULT_INVALID_HANDLE;
    f->regs[0] = result;
    f->regs[1] = (uint32_t)id;
    f->regs[2] = (uint32_t)(id >> 32);
    slog(svc, "SVC_GETPROCESSID handle=0x%08x id=0x%llx result=0x%08x\n",
            handle, (unsigned long long)id, result);
    return 0;
}

/* The SDK's only MRRC p15,c14 use is the 64-bit AArch32 generic timer. The
 * locked nn::os::GetSystemTickFrequency returns 19200000 Hz, and Vita process
 * time is monotonic microseconds, so 192 ticks per 10 us preserves the SDK's
 * tick scale exactly. Only the requested Rt:Rt2 pair changes; MRRC has no
 * memory side effect. */
static int read_generic_timer(VPGuestSvc *svc, VPBridgeFrame *f) {
    unsigned rt = f->reserved;
    if ((rt & 1) || rt > 10) return VP_BRIDGE_ARGUMENT;
    uint64_t ticks = steady_ticks(svc);
    f->regs[rt] = (uint32_t)ticks;
    f->regs[rt + 1] = (uint32_t)(ticks >> 32);
    ++svc->timer_reads;
    if (svc->timer_reads <= 8) {
        slog(svc, "TIMER_READ rt=%u ticks=%llu reads=%u status=0\n",
                rt, (unsigned long long)ticks, svc->timer_reads);
        sflush(svc);
    }
    return 0;
}

/* AArch32 svcWaitSynchronization ABI observed at the locked SDK veneer
 * (0xc0798): r0:r3 hold the 64-bit timeout with the handles pointer in r1 and
 * the handle count in r2. Horizon events created through svcCreateEvent are
 * manual-clear: a successful wait leaves the signal set until the guest calls
 * svcResetSignal, and the SDK relies on that for events it waits on repeatedly
 * (audio, applet). A zero timeout polls and reports the reviewed Horizon
 * timeout result. An unsignaled infinite/positive wait is not emulated with a
 * busy spin or a fabricated signal: it is reported as unsupported. */
static int wait_synchronization(VPGuestSvc *svc, VPBridgeFrame *f) {
    uint32_t handles = f->regs[1], count = f->regs[2];
    uint64_t timeout = (uint64_t)f->regs[0] | ((uint64_t)f->regs[3] << 32);
    if (count != 1 || !vp_guest_svc_readable(svc, handles, 4)) {
        ++svc->unsupported;
        slog(svc, "SVC_WAIT_UNSUPPORTED handles=0x%08x count=%u timeout=0x%llx\n",
                handles, count, (unsigned long long)timeout);
        sflush(svc);
        return VP_BRIDGE_UNSUPPORTED;
    }
    uint32_t handle = get32(handles);
    if (!handle) {
        /* Temporary diagnosis: a zero handle means the caller built its wait
         * array from an unset object field; dump the frame to identify it. */
        slog(svc, "SVC_WAIT_ZERO sp=0x%08x handles=0x%08x", f->sp, handles);
        for (unsigned i = 4; i < 13; ++i) slog(svc, " r%u=0x%08x", i, f->regs[i]);
        slog(svc, "\n");
        for (unsigned i = 0; i < 96; ++i) {
            uint32_t a = f->sp + i * 4;
            if (!vp_guest_svc_readable(svc, a, 4)) break;
            slog(svc, "SVC_WAIT_STACK +0x%02x=0x%08x\n", i * 4, get32(a));
        }
        /* The caller's MultiWait list lives inside the thread object at r9;
         * dump it and the node/object/vtable words that produced handle 0. */
        uint32_t list = f->regs[9];
        for (unsigned i = 0; i < 48; ++i) {
            uint32_t a = list + i * 4;
            if (!vp_guest_svc_readable(svc, a, 4)) break;
            slog(svc, "SVC_WAIT_LIST +0x%02x=0x%08x\n", i * 4, get32(a));
        }
        /* The list sits at ThreadType+0x12c; dump the whole thread object so
         * the entry pointer and the unset handle field are both visible. */
        /* Walk the frame-pointer chain the caller established; the SDK stack
         * frames link through fp with the return address at fp+4. */
        uint32_t fp = f->regs[11];
        for (unsigned i = 0; i < 12 && fp; ++i) {
            if (!vp_guest_svc_readable(svc, fp, 8)) break;
            uint32_t next = get32(fp), lr = get32(fp + 4);
            slog(svc, "SVC_WAIT_FRAME %u fp=0x%08x next=0x%08x lr=0x%08x\n", i, fp, next, lr);
            if (next <= fp) break;
            fp = next;
        }
        uint32_t thread_object = list - 0x12c;
        if (vp_guest_svc_readable(svc, thread_object, 0x160))
            for (unsigned i = 0; i < 0x58; ++i)
                slog(svc, "SVC_WAIT_THREAD +0x%02x=0x%08x\n", i * 4, get32(thread_object + i * 4));
        /* The trampoline computes the user entry from the thread's guest
         * stack top; the mapped region is a real loader copy, so read it. */
        {
            uint32_t stack_top = get32(thread_object + 0x2c) + get32(thread_object + 0x30);
            uint32_t entry = (stack_top > 0x10) ? get32(stack_top - 0x10) : 0;
            slog(svc, "SVC_WAIT_ENTRY stack_top=0x%08x entry=0x%08x\n", stack_top, entry);
        }
        uint32_t node = vp_guest_svc_readable(svc, f->sp + 0x3c, 4) ? get32(f->sp + 0x3c) : 0;
        if (node && vp_guest_svc_readable(svc, node, 20)) {
            slog(svc, "SVC_WAIT_NODE node=0x%08x", node);
            for (unsigned i = 0; i < 5; ++i) slog(svc, " [%d]=0x%08x", i, get32(node + i * 4));
            slog(svc, "\n");
            uint32_t object = get32(node);
            if (object && vp_guest_svc_readable(svc, object, 20)) {
                slog(svc, "SVC_WAIT_OBJECT object=0x%08x", object);
                for (unsigned i = 0; i < 5; ++i) slog(svc, " +%x=0x%08x", i * 4, get32(object + i * 4));
                slog(svc, "\n");
            }
            /* The holder virtual at 0xA0CF8 reads the wait handle from
             * [[node+0x18]+0xc]; dump that object as well. */
            if (vp_guest_svc_readable(svc, node + 0x18, 4)) {
                uint32_t holder = get32(node + 0x18);
                slog(svc, "SVC_WAIT_HOLDER holder=0x%08x", holder);
                for (unsigned i = 0; holder && i < 24 && vp_guest_svc_readable(svc, holder, 96); ++i)
                    slog(svc, " +%02x=0x%08x", i * 4, get32(holder + i * 4));
                slog(svc, "\n");
            }
        }
    }
    VPGuestHandle *slot = find_handle(svc, handle);
    if (handle == 0x118u && !svc->wait_trace_118) {
        svc->wait_trace_118 = 1;
        slog(svc, "SVC_WAIT_TRACE118 sp=0x%08x lr=0x%08x pc=0x%08x thread=0x%08x\n",
                f->sp, f->lr, f->pc, svc->current_thread_handle);
        for (unsigned i = 0; i < 0x100; ++i) {
            uint32_t a = f->sp + i * 4;
            if (!vp_guest_svc_readable(svc, a, 4)) break;
            slog(svc, "SVC_WAIT_TRACE118_STACK +0x%03x=0x%08x\n", i * 4, get32(a));
        }
        sflush(svc);
    }
    if (!slot || slot->kind != 3) {
        f->regs[0] = SVC_RESULT_INVALID_HANDLE;
        f->regs[1] = 0;
        slog(svc, "SVC_WAIT handle=0x%08x timeout=0x%llx result=0x%08x\n",
                handle, (unsigned long long)timeout, f->regs[0]);
        sflush(svc);
        return 0;
    }
    if (!slot->signaled && slot->signal_at_us
        && sceKernelGetProcessTimeWide() >= slot->signal_at_us) {
        slot->signaled = 1;
        slot->signal_at_us = 0;
    }
    if (slot->signaled) {
        /* Manual-clear: the signal stays set until svcResetSignal (0x17). */
        f->regs[0] = 0;
        f->regs[1] = 0; /* signaled handle index */
        slog(svc, "SVC_WAIT handle=0x%08x signaled=1 timeout=0x%llx result=0 index=0\n",
                handle, (unsigned long long)timeout);
        sflush(svc);
        return 0;
    }
    if (timeout == 0) {
        f->regs[0] = SVC_RESULT_TIMEOUT;
        f->regs[1] = 0;
        slog(svc, "SVC_WAIT handle=0x%08x signaled=0 timeout=0 result=0x%08x\n",
                handle, f->regs[0]);
        sflush(svc);
        return 0;
    }
    ++svc->unsupported;
    slog(svc, "SVC_WAIT_UNSUPPORTED handle=0x%08x signaled=0 timeout=0x%llx no host blocking\n",
            handle, (unsigned long long)timeout);
    sflush(svc);
    return VP_BRIDGE_UNSUPPORTED;
}

/* svcSignalEvent/svcClearEvent (SVC 0x11/0x12) operate on an event handle:
 * r0 is the handle, the result returns in r0. The loader's event model is
 * auto-clear: a successful wait already consumed the signal, and the SDK's
 * ClearSystemEvent path observes the same zeroed state. Only handles the
 * loader created as events are accepted; any other handle is reported with
 * the reviewed invalid-uid result instead of silently succeeding. */
static int event_signal(VPGuestSvc *svc, VPBridgeFrame *f) {
    uint32_t handle = f->regs[0];
    VPGuestHandle *slot = find_handle(svc, handle);
    if (!slot || slot->kind != 3) {
        f->regs[0] = SVC_RESULT_INVALID_HANDLE;
        slog(svc, "SVC_EVENT_INVALID call=SignalEvent handle=0x%08x result=0x%08x\n",
                handle, f->regs[0]);
        sflush(svc);
        return 0;
    }
    slot->signaled = 1;
    slot->signal_at_us = 0;
    f->regs[0] = 0;
    slog(svc, "SVC_SIGNAL_EVENT handle=0x%08x result=0\n", handle);
    sflush(svc);
    return 0;
}

static int event_clear(VPGuestSvc *svc, VPBridgeFrame *f) {    uint32_t handle = f->regs[0];
    VPGuestHandle *slot = find_handle(svc, handle);
    if (!slot || slot->kind != 3) {
        f->regs[0] = SVC_RESULT_INVALID_HANDLE;
        slog(svc, "SVC_EVENT_INVALID call=ClearEvent handle=0x%08x result=0x%08x\n",
                handle, f->regs[0]);
        sflush(svc);
        return 0;
    }
    slot->signaled = 0;
    slot->signal_at_us = 0;
    f->regs[0] = 0;
    slog(svc, "SVC_CLEAR_EVENT handle=0x%08x result=0\n", handle);
    sflush(svc);
    return 0;
}

static int event_reset(VPGuestSvc *svc, VPBridgeFrame *f) {
    /* svcResetSignal (SVC 0x17): r0 is the signal handle. Horizon accepts it
     * for KEvent and clears the signal if it was set. The loader's event
     * model is the same boolean the SDK's EventType resets. */
    uint32_t handle = f->regs[0];
    VPGuestHandle *slot = find_handle(svc, handle);
    if (!slot || slot->kind != 3) {
        f->regs[0] = SVC_RESULT_INVALID_HANDLE;
        slog(svc, "SVC_EVENT_INVALID call=ResetSignal handle=0x%08x result=0x%08x\n",
                handle, f->regs[0]);
        sflush(svc);
        return 0;
    }
    slot->signaled = 0;
    slot->signal_at_us = 0;
    f->regs[0] = 0;
    uint32_t owner_index = slot->audio_owner_session;
    VPGuestSession *owner = NULL;
    if (owner_index && owner_index <= VP_GUEST_SESSION_LIMIT
        && svc->sessions[owner_index - 1].refs)
        owner = &svc->sessions[owner_index - 1];
    int owned_thread = vp_audio_ipc_thread_owned(
        svc->current_thread_handle, svc->audio_update_thread_handle);
    if (owner && owned_thread) {
        ++owner->audio_reset_count;
        if (vp_audio_log_cadence_should_emit(&owner->audio_reset_log, 4u, 64u)) {
            slog(svc, "SVC_RESET_SIGNAL handle=0x%08x result=0 owner=renderer-thread count=%u log=%u/%u\n",
                    handle, owner->audio_reset_count,
                    owner->audio_reset_log.emitted,
                    owner->audio_reset_log.seen);
            sflush(svc);
        }
    } else if (slot->audio_owner_session) {
        ++svc->audio_log_unowned_reset;
        slog(svc, "SVC_RESET_SIGNAL handle=0x%08x result=0 owner=unowned-thread\n",
                handle);
        sflush(svc);
    } else {
        slog(svc, "SVC_RESET_SIGNAL handle=0x%08x result=0\n", handle);
        sflush(svc);
    }
    return 0;
}

int vp_guest_svc_event_state(VPGuestSvc *svc, uint32_t handle, int *signaled) {
    if (!svc || !signaled) return 0;
    VPGuestHandle *slot = find_handle(svc, handle);
    if (!slot || slot->kind != 3) return 0;
    if (!slot->signaled && slot->signal_at_us
        && sceKernelGetProcessTimeWide() >= slot->signal_at_us) {
        slot->signaled = 1;
        slot->signal_at_us = 0;
    }
    *signaled = slot->signaled ? 1 : 0;
    return 1;
}

int vp_guest_svc_event_consume(VPGuestSvc *svc, uint32_t handle) {
    VPGuestHandle *slot = find_handle(svc, handle);
    if (!slot || slot->kind != 3 || !slot->signaled) return 0;
    slot->signaled = 0;
    slot->signal_at_us = 0;
    return 1;
}

/* svcExitProcess does not return on Horizon: the kernel tears the process
 * down. The loader has no process-termination model yet, so it records the
 * guest's request together with the frame chain that reached it and stops,
 * instead of resuming the wrapper as if the process had continued. */
static int exit_process(VPGuestSvc *svc, VPBridgeFrame *f) {
    slog(svc, "SVC_EXIT_PROCESS pc=0x%08x sp=0x%08x lr=0x%08x r0=0x%08x r1=0x%08x\n",
            f->pc, f->sp, f->lr, f->regs[0], f->regs[1]);
    uint32_t fp = f->sp;
    for (unsigned depth = 0; depth < 10 && fp; ++depth) {
        if (!vp_guest_svc_readable(svc, fp, 8)) break;
        slog(svc, "SVC_EXIT_FRAME depth=%u fp=0x%08x saved_lr=0x%08x\n",
                depth, fp, get32(fp + 4));
        fp = get32(fp);
    }
    for (unsigned row = 0; row < 5; ++row) {
        uint32_t base = f->sp + row * 32;
        if (!vp_guest_svc_readable(svc, base, 32)) break;
        slog(svc, "SVC_EXIT_STACK +0x%02x", row * 32);
        for (unsigned column = 0; column < 8; ++column)
            slog(svc, " %08x", get32(base + column * 4));
        slog(svc, "\n");
    }
    sflush(svc);
    ++svc->unsupported;
    return VP_BRIDGE_UNSUPPORTED;
}

int vp_guest_svc_dispatch(VPGuestSvc *svc, VPBridgeFrame *f) {
    if (!svc || !f) return VP_BRIDGE_ARGUMENT;
    if (f->service == VP_GUEST_TIMER_SERVICE) return read_generic_timer(svc, f);
    uint32_t id = f->service - VP_GUEST_SVC_BASE;
    ++svc->calls;
    switch (id) {
    case 0x01: return set_heap_size(svc, f);
    case 0x03: return set_memory_attribute(svc, f);
    case 0x04: return map_memory(svc, f);
    case 0x05: return unmap_memory(svc, f);
    case 0x06: return query_memory(svc, f);
    case 0x07: return exit_process(svc, f);
    case 0x0c: return get_thread_priority(svc, f);
    case 0x0d: return set_thread_priority(svc, f);
    case 0x0e: return get_thread_core_mask(svc, f);
    case 0x0f: return set_thread_core_mask(svc, f);
    case 0x11: return event_signal(svc, f);
    case 0x12: return event_clear(svc, f);
    case 0x13: return map_shared_memory(svc, f);
    case 0x14: return unmap_shared_memory(svc, f);
    case 0x15: return create_transfer_memory(svc, f);
    case 0x16: return close_handle(svc, f);
    case 0x17: return event_reset(svc, f);
    case 0x1d: return signal_process_wide_key(svc, f);
    case 0x1f: return connect_named_port(svc, f);
    case 0x21: return send_sync_request(svc, f, 0);
    case 0x22: return send_sync_request(svc, f, 1);
    case 0x24: return get_process_id(svc, f);
    case 0x25: return get_thread_id(svc, f);
    case 0x18: return wait_synchronization(svc, f);
    case 0x26: {
        /* Break while diagnosing the platform boundary: record the guest
         * frame chain before returning the explicit unsupported result. */
        slog(svc, "SVC_BREAK_REGS r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x r8=0x%08x r9=0x%08x r10=0x%08x r11=0x%08x r12=0x%08x lr=0x%08x\n",
             f->regs[4], f->regs[5], f->regs[6], f->regs[7], f->regs[8],
             f->regs[9], f->regs[10], f->regs[11], f->regs[12], f->lr);
        uint32_t stack_base = f->sp;
        for (unsigned row = 0; row < 16; ++row) {
            if (!vp_guest_svc_readable(svc, stack_base + row * 16, 16)) break;
            slog(svc, "SVC_BREAK_STACK +0x%02x", row * 16);
            for (unsigned column = 0; column < 16; ++column)
                slog(svc, " %02x", *(const uint8_t *)(uintptr_t)(stack_base + row * 16 + column));
            slog(svc, "\n");
        }
        uint32_t fp = f->sp;
        for (unsigned depth = 0; depth < 8 && fp; ++depth) {
            if (!vp_guest_svc_readable(svc, fp, 8)) break;
            slog(svc, "SVC_BREAK_FRAME depth=%u fp=0x%08x saved_lr=0x%08x\n",
                    depth, fp, get32(fp + 4));
            fp = get32(fp);
        }
        /* The worker aborts inside the SDK CMIF client after it builds a
         * request. Dump the IPC TLS buffer so the last message (request or
         * response) can be classified. */
        if (vp_guest_svc_readable(svc, svc->tls_buffer, 0x100u)) {
            slog(svc, "SVC_BREAK_TLS buffer=0x%08x\n", svc->tls_buffer);
            for (unsigned row = 0; row < 16; ++row) {
                slog(svc, "SVC_BREAK_TLS_ROW +0x%02x", row * 16);
                for (unsigned column = 0; column < 4; ++column)
                    slog(svc, " %08x", get32(svc->tls_buffer + row * 16 + column * 4));
                slog(svc, "\n");
            }
        }
        /* Temporary diagnostic: nn::diag::AbortImpl receives file/function/
         * message pointers; they stay in the abort frames' stack slots.
         * Print any stack word that names a printable guest string. */
        for (unsigned row = 0; row < 64; ++row) {
            uint32_t slot = stack_base + row * 4u;
            if (!vp_guest_svc_readable(svc, slot, 4)) break;
            uint32_t address = get32(slot);
            if (address < 0x1000u || !vp_guest_svc_readable(svc, address, 1)) continue;
            unsigned char first = *(const unsigned char *)(uintptr_t)address;
            if (first < 0x20 || first > 0x7e) continue;
            char text[65];
            unsigned i = 0;
            for (; i < 64u; ++i) {
                if (!vp_guest_svc_readable(svc, address + i, 1)) break;
                unsigned char c = *(const unsigned char *)(uintptr_t)(address + i);
                if (c < 0x20 || c > 0x7e) break;
                text[i] = (char)c;
            }
            text[i] = '\0';
            if (i >= 4u)
                slog(svc, "SVC_BREAK_STRING +0x%02x address=0x%08x \"%s\"\n",
                        row * 4u, address, text);
        }
        /* Temporary diagnostic: the reserve routine at sdk+0x30d60c asserts
         * when the command buffer's memory callback [buffer+4] is NULL. The
         * buffer's owner is still on the abort stack; dump each heap pointer
         * whose word 4 is NULL but whose command-memory fields are not, so
         * the exact object can be identified offline. */
        for (unsigned row = 0; row < 64; ++row) {
            uint32_t slot = stack_base + row * 4u;
            if (!vp_guest_svc_readable(svc, slot, 4)) break;
            uint32_t candidate = get32(slot);
            if (candidate < 0x90000000u || candidate >= 0xa0000000u) continue;
            if (!vp_guest_svc_readable(svc, candidate, 0x50u)) continue;
            if (get32(candidate + 4) != 0) continue;
            if (!(get32(candidate + 0x0c) | get32(candidate + 0x10)
                  | get32(candidate + 0x18) | get32(candidate + 0x1c)
                  | get32(candidate + 0x40)))
                continue;
            slog(svc, "SVC_BREAK_CMDBUF slot=+0x%02x object=0x%08x words",
                    row * 4u, candidate);
            for (unsigned i = 0; i < 0x14u; ++i)
                slog(svc, " %08x", get32(candidate + i * 4u));
            slog(svc, "\n");
        }
        /* Temporary diagnostic for the account save mount: the sdk module's
         * nn::fs::detail mount table is the BSS object at module offset
         * 0x862ccc in the locked 1.4.0 sdk build. Walking it shows whether
         * the accessor was registered and under which name. The fourth
         * module-text region belongs to the sdk module, in load order. */
        {
            uint32_t sdk_base = 0;
            unsigned texts = 0;
            for (unsigned i = 0; i < svc->region_count; ++i) {
                if (!svc->regions[i].name || strcmp(svc->regions[i].name, "module-text")) continue;
                if (++texts == 4) { sdk_base = svc->regions[i].base; break; }
            }
            if (sdk_base) {
                uint32_t table = sdk_base + 0x862cccu;
                if (vp_guest_svc_readable(svc, table, 0x10u)) {
                    uint32_t next = get32(table + 4);
                    slog(svc, "SVC_BREAK_MOUNTTABLE table=0x%08x head_next=0x%08x head_prev=0x%08x\n",
                            table, next, get32(table));
                    for (unsigned n = 0; n < 8 && next != table; ++n) {
                        if (!vp_guest_svc_readable(svc, next, 0x18u)) break;
                        char name[17];
                        for (unsigned i = 0; i < 16; ++i)
                            name[i] = (char)*(const uint8_t *)(uintptr_t)(next + 8u + i);
                        name[16] = '\0';
                        slog(svc, "SVC_BREAK_MOUNT node=0x%08x name=\"%s\" next=0x%08x prev=0x%08x\n",
                                next, name, get32(next + 4), get32(next));
                        if (!strcmp(name, "savedata")
                            && vp_guest_svc_readable(svc, next - 4u, 0x4cu)) {
                            uint32_t accessor = next - 4u;
                            slog(svc, "SVC_BREAK_SAVE_ACCESSOR accessor=0x%08x words", accessor);
                            for (unsigned i = 0; i < 0x13u; ++i)
                                slog(svc, " %08x", get32(accessor + i * 4u));
                            slog(svc, "\n");
                            uint32_t adapter = get32(accessor + 0x1cu);
                            slog(svc, "SVC_BREAK_SAVE_ADAPTER adapter=0x%08x fs=0x%08x\n",
                                    adapter, vp_guest_svc_readable(svc, adapter + 4u, 4u) ? get32(adapter + 4u) : 0);
                            if (adapter && vp_guest_svc_readable(svc, adapter, 8u)) {
                                uint32_t fs = get32(adapter + 4u);
                                if (fs && vp_guest_svc_readable(svc, fs, 0x20u)) {
                                    slog(svc, "SVC_BREAK_SAVE_FS fs=0x%08x words", fs);
                                    for (unsigned i = 0; i < 8u; ++i)
                                        slog(svc, " %08x", get32(fs + i * 4u));
                                    slog(svc, "\n");
                                    uint32_t vt = get32(fs);
                                    if (vt && vp_guest_svc_readable(svc, vt, 0x40u)) {
                                        slog(svc, "SVC_BREAK_SAVE_VTABLE vt=0x%08x words", vt);
                                        for (unsigned i = 0; i < 16u; ++i)
                                            slog(svc, " %08x", get32(vt + i * 4u));
                                        slog(svc, "\n");
                                    }
                                }
                            }
                        }
                        next = get32(next + 4);
                    }
                }
            }
        }
        /* Temporary: the fsp-srv CMIF client session object referenced by the
         * IFileSystem proxy at +8. The 16-bit field at +4 feeds the HIPC
         * message writer's capacity check; dumping it ties the client-side
         * 0x11A0B rejection to an observable session state. */
        {
            uint32_t sdk_base = 0;
            unsigned texts = 0;
            for (unsigned i = 0; i < svc->region_count; ++i) {
                if (!svc->regions[i].name || strcmp(svc->regions[i].name, "module-text")) continue;
                if (++texts == 4) { sdk_base = svc->regions[i].base; break; }
            }
            if (sdk_base) {
                uint32_t session = sdk_base + 0x862ce8u;
                if (vp_guest_svc_readable(svc, session, 0x20u)) {
                    slog(svc, "SVC_BREAK_CMIF_SESSION obj=0x%08x u16_4=%u words",
                            session, get32(session + 4u) & 0xffffu);
                    for (unsigned i = 0; i < 8u; ++i)
                        slog(svc, " %08x", get32(session + i * 4u));
                    slog(svc, "\n");
                }
            }
        }
        sflush(svc);
        ++svc->breaks;
        return VP_BRIDGE_UNSUPPORTED;
    }
    case 0x29: return get_info(svc, f);
    case 0x5d: return data_cache_maintenance(svc, f, "INVALIDATE_DATA_CACHE");
    case 0x5e: return data_cache_maintenance(svc, f, "STORE_DATA_CACHE");
    case 0x5f: return data_cache_maintenance(svc, f, "FLUSH_DATA_CACHE");
    default:
        ++svc->unsupported;
        slog(svc, "SVC_UNSUPPORTED id=0x%x pc=0x%08x\n", id, f->pc);
        sflush(svc);
        return VP_BRIDGE_UNSUPPORTED;
    }
}

#include "gxm_runtime.h"
#include "texture_pipeline.h"
#include "texture_regions.h"
#include "batch_policy.h"
#include "clear_fusion.h"
#include "engine_full.h"
#include "gxm_sampler_plan.h"
#include "texture_snapshot.h"
#include "gpu_texture.h"
#include "gxm_depth.h"
#include "gxm_depth_clear_native.h"
#include "gxm_depth_view.h"
#include "gxm_depth_view_native.h"
#include "gxm_resource_layout.h"
#include "gxm_shadow_sampler.h"
#include "log_sink.h"
#include "native_movie.h"
#include "nso_loader.h"
#include "pair_cache_policy.h"
#include "render_resolution.h"
#include "static_shader_index.h"
#include "vertex_format.h"
#include "vertex_window.h"
#include "gpu_indices.h"
#include "gpu_wait_profile.h"
#include "frame_cost_profile.h"

#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The pure sampler planner uses portable enum values; these assertions keep
 * them identical to the VitaSDK sampler enums actually passed to GXM. */
_Static_assert(SCE_GXM_TEXTURE_FILTER_POINT == 0, "sampler point value");
_Static_assert(SCE_GXM_TEXTURE_FILTER_LINEAR == 1, "sampler linear value");
_Static_assert(SCE_GXM_TEXTURE_MIP_FILTER_DISABLED == 0, "mip disabled value");
_Static_assert(SCE_GXM_TEXTURE_MIP_FILTER_ENABLED == 0x200, "mip enabled value");
_Static_assert((unsigned)SCE_GXM_TEXTURE_ADDR_REPEAT ==
               (unsigned)VP_GXM_SAMPLER_ADDR_REPEAT, "addr repeat value");
_Static_assert((unsigned)SCE_GXM_TEXTURE_ADDR_MIRROR ==
               (unsigned)VP_GXM_SAMPLER_ADDR_MIRROR, "addr mirror value");
_Static_assert((unsigned)SCE_GXM_TEXTURE_ADDR_CLAMP ==
               (unsigned)VP_GXM_SAMPLER_ADDR_CLAMP, "addr clamp value");
_Static_assert((unsigned)SCE_GXM_TEXTURE_ADDR_MIRROR_CLAMP ==
               (unsigned)VP_GXM_SAMPLER_ADDR_MIRROR_CLAMP,
               "addr mirror clamp value");
_Static_assert((unsigned)SCE_GXM_TEXTURE_ADDR_CLAMP_FULL_BORDER ==
               (unsigned)VP_GXM_SAMPLER_ADDR_CLAMP_FULL_BORDER,
               "addr border value");

#define VP_GXM_WIDTH 960
#define VP_GXM_HEIGHT 544
#define VP_GXM_PITCH 960
#define VP_GXM_BYTES (VP_GXM_WIDTH * VP_GXM_HEIGHT * 4u)
#define VP_GXM_BLOCK_LIMIT 96u
#define VP_GXM_PATCHER_BUFFER (1024u * 1024u)
#define VP_GXM_PATCHER_USSE (256u * 1024u)
#define VP_GXM_PROGRAM_STORE 128u
#define VP_GXM_SURFACE_LIMIT 32u
/* The village binds more distinct render-target sizes than the original 8-slot
 * pool (UI layers at 48x22/80x45/etc.); a full pool refused those draws. */
#define VP_GXM_TARGET_LIMIT 16u
#define VP_GXM_LOG_PATH "ux0:data/mhgu-vita-port/gxm-runtime.log"
#define VP_GXM_BATCH_REASON_LIMIT 16u
#define VP_GXM_BATCH_ARENAS 3u
#define VP_GXM_BATCH_STAGING_PAGES 2u
#define VP_GXM_BATCH_PAGE_BYTES (VP_BATCH_STAGING_BUDGET / VP_GXM_BATCH_STAGING_PAGES)
/* Default uniform buffers are carved from the SDK fragment ring. Reserve only a
 * small fraction of that ring for one open scene, then flush before the next
 * draw could exhaust it. The per-program sizes are queried from the SDK. */
#define VP_GXM_BATCH_UNIFORM_BUDGET \
    ((size_t)(SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE / 4))

typedef struct {
    SceUID id;
    void *ptr;
    int kind;
    unsigned bytes;
    int cdram;
} VPGxmBlock;

static VPGxmBlock blocks[VP_GXM_BLOCK_LIMIT];
static unsigned block_count;
typedef struct {
    unsigned stage;
    unsigned viewport_transform;
    unsigned long hash;
    SceGxmShaderPatcherId id;
    SceGxmProgram *program;
} VPShaderEntry;

static SceGxmShaderPatcher *patcher;
static VPShaderEntry shaders[VP_GXM_PROGRAM_STORE];
static unsigned shader_count;
static SceGxmVertexProgram *vertex_program;
static SceGxmFragmentProgram *fragment_program;
static const SceGxmProgram *gxm_vs_program;
static const SceGxmProgram *gxm_fs_program;
/* VP_HOST_TEST_EXTRACT:pair_base_begin */
typedef struct {
    unsigned hash;
    unsigned viewport_transform;
    unsigned stride;
    uint32_t attributes[16];
    SceGxmShaderPatcherId vertex_id;
    SceGxmShaderPatcherId fragment_id;
    SceGxmVertexProgram *vs;
    SceGxmFragmentProgram *fs;
    const SceGxmProgram *vs_program;
    const SceGxmProgram *fs_program;
} VPGxmProgramPair;
/* VP_HOST_TEST_EXTRACT:pair_base_end */
static VPGxmProgramPair gxm_pairs[VP_GXM_PROGRAM_STORE];
static unsigned gxm_pair_count;
/* Static shader index v1. The index is built offline; this module only
 * validates it, registers the GXPs the observed working set needs, and keeps a
 * bounded cache of resident programs and created GXM objects. All GXM calls
 * here happen on the single worker thread. */
#define VP_GXM_RESIDENT_LIMIT 128u
#define VP_GXM_RESIDENT_BYTES (8u * 1024u * 1024u)
#define VP_GXM_STATIC_PAIR_LIMIT 96u
#define VP_GXM_STATIC_PATH 256u
#ifndef VP_GXM_STATIC_DIR
#define VP_GXM_STATIC_DIR "ux0:data/mhgu-vita-port/static-shaders-current"
#endif
#define VP_GXM_LEGACY_DIR "ux0:data/mhgu-vita-port/shaders"
typedef struct {
    const SceGxmProgramParameter *cbuf[18];
    unsigned cbuf_float_counts[18];
    unsigned default_uniform_bytes;
    const SceGxmProgramParameter *tex[VP_GXM_FRAGMENT_SAMPLER_LIMIT];
    const SceGxmProgramParameter *tex_inv_size[VP_GXM_FRAGMENT_SAMPLER_LIMIT];
    /* Vertex-only RGBA32F layout selector: 0 ordinary, 1 packed/repeat U,
     * 2 packed/clamp U. Logical inverse extents remain unchanged. */
    const SceGxmProgramParameter *tex_rgba32f_packed[VP_GXM_FRAGMENT_SAMPLER_LIMIT];
    const SceGxmProgramParameter *fragcoord_scale;
    /* Software-shadow helper ABI: three float4 uniforms per declared shadow
     * slot.  A partial trio is never filled with zero; it is refused. */
    const SceGxmProgramParameter *shadow_extent[VP_GXM_FRAGMENT_SAMPLER_LIMIT];
    const SceGxmProgramParameter *shadow_state[VP_GXM_FRAGMENT_SAMPLER_LIMIT];
    const SceGxmProgramParameter *shadow_border[VP_GXM_FRAGMENT_SAMPLER_LIMIT];
    unsigned shadow_complete_mask;
    unsigned shadow_incomplete_mask;
    const SceGxmProgramParameter *render_area;
    const SceGxmProgramParameter *varying_one;
    const SceGxmProgramParameter *alpha_test;
    const SceGxmProgramParameter *face_state;
    const SceGxmProgramParameter *f_mask;
    const SceGxmProgramParameter *attr[16];
    const SceGxmProgramParameter *attr_uint[16];
    unsigned sampler_count;
    /* Set when a declared sampler cannot be represented by the bounded
     * tex0..tex3 fragment ABI (unknown name, duplicate slot or overflow), or
     * when a vertex program declares anything other than tex0. */
    unsigned sampler_unsupported;
} VPStaticReflection;
typedef struct {
    int used;
    uint32_t stage_index;
    uint16_t variant_index;
    unsigned refs;
    uint32_t last_use;
    SceGxmProgram *program;
    unsigned bytes;
    SceGxmShaderPatcherId id;
    VPStaticReflection refl;
    /* Cached f_mask presence: the depth-only path used to search the program
     * parameters on every depth draw. */
    unsigned has_f_mask;
} VPStaticResident;
/* VP_HOST_TEST_EXTRACT:pair_struct_begin */
typedef struct VPStaticPair {
    int used;
    uint32_t vs_stage, fs_stage;
    uint16_t vs_variant, fs_variant;
    uint16_t viewport;
    uint16_t index_source;
    uint32_t stride;
    uint32_t color_mask;
    uint32_t attributes[16];
    uint32_t packed_mask;
    uint16_t packed_stream_stride;
    uint32_t blend[6];
    /* Immutable result of build_blend_info at pair creation. Shared mode reuses
     * it on later hits instead of converting the same six guest enums again. */
    SceGxmBlendInfo blend_info;
    /* Per-stage default-uniform sizes and the face-state condition used by the
     * batch uniform budget. Pair-held residents cannot be evicted, so these
     * values stay valid for the pair lifetime. */
    uint32_t vs_default_uniform_bytes;
    uint32_t fs_default_uniform_bytes;
    uint32_t fs_face_state;
    SceGxmVertexProgram *vs;
    SceGxmFragmentProgram *fs;
    const SceGxmProgram *vs_program;
    const SceGxmProgram *fs_program;
    unsigned vs_resident, fs_resident;
    uint32_t last_use;
    VPGxmProgramPair base;
} VPStaticPair;
/* VP_HOST_TEST_EXTRACT:pair_struct_end */
typedef struct {
    unsigned index_hits, index_misses, gxp_loads, registrations, unregistrations;
    unsigned pair_hits, pair_misses, pair_evictions, resident_evictions;
    unsigned pair_hint_hits;
    unsigned reflections, draw_ok, draw_fail, query_unobserved;
    unsigned batch_encoded, batch_completed, batch_failed, batch_flushes;
    unsigned batch_sync_fallbacks, batch_max, batch_bytes_peak;
    unsigned batch_uniform_peak;
    /* Whole-entry-array bytes that the scene reset no longer writes; the
     * per-admission memset is still performed. */
    uint64_t batch_reset_bytes_avoided;
    /* Per-draw reflection work the static path no longer performs: parameter
     * names that would have been formatted, and SDK parameter lookups replaced
     * by the reflection cache built once per resident. */
    uint64_t reflect_name_formats_avoided;
    uint64_t reflect_lookups_avoided;
    /* Index 0 is unused; index N counts successful flushes that published N
     * draws. Reason buckets use the GXM_BATCH_REASON_* order. */
    unsigned batch_size_hist[VP_BATCH_MAX_DRAWS + 1u];
    unsigned batch_reason_hist[VP_GXM_BATCH_REASON_LIMIT];
    unsigned resident_bytes, resident_peak_bytes;
} VPStaticStats;
static int static_mode;
static VPStaticShaderIndex static_index;
static char static_index_dir[VP_GXM_STATIC_PATH];
/* Per-stage resource layout derived from the locked catalogue.  The sidecar is
 * bound to the exact shader-index bytes (SHA-256 + CRC) and every stage SHA is
 * checked by the parser.  A missing or mismatched sidecar refuses static mode;
 * there is no fixed-offset fallback. */
static VPResourceLayout static_resources;
static int static_resources_ready;
static unsigned char static_index_sha256[32];
static uint32_t static_index_crc32;
static unsigned char static_resources_sha256[32];
static VPStaticResident static_residents[VP_GXM_RESIDENT_LIMIT];
/* Variant identity is fixed by the loaded package; zero means not resident. */
static uint16_t static_resident_slots[VP_STATIC_INDEX_MAX_VARIANTS];
static unsigned static_resident_used;
static unsigned static_resident_bytes;
static unsigned static_clock;
static VPStaticPair static_pairs[VP_GXM_STATIC_PAIR_LIMIT];
/* VP_HOST_TEST_EXTRACT:pair_hint_begin */
/* Recent successful pairs. Only keys back to slots; every probe still runs the
 * full input comparison. Several pairs normally alternate (UI, terrain,
 * characters), so a one-entry hint misses about half of all lookups and every
 * miss scans the whole pair table. */
#define VP_STATIC_PAIR_HINTS 8u
typedef struct {
    VPStaticPair *pair;
    uint32_t vs_token;
    uint32_t fs_token;
    unsigned viewport;
    uint64_t age;
} VPStaticPairHint;
static VPStaticPairHint static_pair_hints[VP_STATIC_PAIR_HINTS];
static uint64_t static_pair_hint_clock;

static void static_pair_hint_reset(void) {
    for (unsigned i = 0; i < VP_STATIC_PAIR_HINTS; ++i)
        memset(&static_pair_hints[i], 0, sizeof(static_pair_hints[i]));
    static_pair_hint_clock = 0u;
}

/* Called before a slot is released or reused; only its identity matters. */
static void static_pair_hint_forget(const VPStaticPair *pair) {
    for (unsigned i = 0; i < VP_STATIC_PAIR_HINTS; ++i)
        if (static_pair_hints[i].pair == pair)
            memset(&static_pair_hints[i], 0, sizeof(static_pair_hints[i]));
}

static void static_pair_hint_set(VPStaticPair *pair, uint32_t vs_token,
                                 uint32_t fs_token, unsigned viewport) {
    VPStaticPairHint *slot = NULL;
    for (unsigned i = 0; i < VP_STATIC_PAIR_HINTS; ++i) {
        VPStaticPairHint *hint = &static_pair_hints[i];
        if (hint->pair == pair) { slot = hint; break; }
        if (hint->pair == NULL) {
            if (slot == NULL || slot->pair != NULL) slot = hint;
        } else if (slot == NULL ||
                   (slot->pair != NULL && hint->age < slot->age)) {
            slot = hint;
        }
    }
    if (slot == NULL) return;
    slot->pair = pair;
    slot->vs_token = vs_token;
    slot->fs_token = fs_token;
    slot->viewport = viewport;
    slot->age = ++static_pair_hint_clock;
}
/* VP_HOST_TEST_EXTRACT:pair_hint_end */
static VPStaticPair *static_current_pair;
static VPStaticStats static_stats;
/* Resident/index identity changes bump this generation; every shader-plan
 * cache entry from an older generation is rejected and rebuilt. */
static uint32_t static_plan_generation;
static VPGxmShaderPlanCacheEntry static_plan_cache[VP_GXM_SHADER_PLAN_CACHE_ENTRIES];
static VPGxmShaderPlanCounts static_plan_counts;
static unsigned static_plan_stage_calls[2];
static unsigned static_plan_slot_calls[2][VP_GXM_FRAGMENT_SAMPLER_LIMIT];
static VPGxmSamplerPlanCacheEntry
    gxm_sampler_plan_cache[VP_GXM_SAMPLER_PLAN_CACHE_ENTRIES];
static unsigned gxm_sampler_plan_hits, gxm_sampler_plan_misses;
/* Counters for the mode-3-only substitutions; they remain zero in modes 0-2. */
static unsigned static_shared_pair_hits, static_shared_uniform_reuses;
static unsigned static_shared_blend_reuses;
static int gxm_shared_prepare_active(void) {
    return __atomic_load_n(&vp_engine_shared_prepare_enabled,
                           __ATOMIC_ACQUIRE) != 0u;
}
static void static_plan_invalidate(void) {
    static_plan_generation++;
}
static int static_index_load(const char *dir);
static int static_preload_observed(void);
static int static_register_bringup(unsigned token);
static int static_query_sampler(unsigned token, unsigned stage,
                                unsigned viewport, unsigned sampler);
static int static_pair_get(const VPGxmDrawRequest *req, VPGxmProgramPair **pair,
                           VPStaticPair **owner);
static void static_log_stats(void);
static SceGxmContext *gxm_context;
static SceGxmRenderTarget *gxm_target;
static SceGxmColorSurface gxm_color;
static SceGxmDepthStencilSurface gxm_depth;
static SceGxmTexture gxm_texture;
static SceGxmTexture gxm_mask_texture;
static void *gxm_mask_mem;
static void *gxm_rt;
static void *gxm_present;
static void *gxm_game_vertices;
static void *gxm_game_vertices_packed;
static void *gxm_game_indices;
static void *gxm_game_texture_mem;
static unsigned gxm_game_texture_format;
static size_t gxm_game_texture_capacity;
static void *gxm_extra_mem[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
static SceGxmTexture gxm_extra_texture[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
static unsigned gxm_extra_width[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u],
                gxm_extra_height[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
static unsigned gxm_extra_format[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
static unsigned gxm_extra_cube[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u],
                gxm_extra_mips[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
static size_t gxm_extra_bytes[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u],
              gxm_extra_capacity[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
static void *gxm_vertex_texture_mem;
static SceGxmTexture gxm_vertex_texture;
static unsigned gxm_vertex_texture_width,gxm_vertex_texture_height;
static unsigned gxm_vertex_texture_format,gxm_vertex_texture_cube,gxm_vertex_texture_mips;
static size_t gxm_vertex_texture_bytes,gxm_vertex_texture_capacity;
static SceGxmTexture gxm_game_texture;
static unsigned gxm_game_texture_width;
static unsigned gxm_game_texture_height;
static int gxm_ready;
/* Set when a scene-ending synchronization fails on the worker's main GXM
 * context. Until a real teardown/reinitialization exists, draw/clear/present
 * entries must reject instead of reusing staging buffers with an unknown GPU
 * owner. Probe contexts never set this flag. */
static int gxm_scene_poisoned;
static SceUID gxm_main_thread;
/* Offscreen bring-up checks still initialize the context in play mode. Only
 * explicit evidence runs display the synthetic transfer-fill test frame. */
static int gxm_show_bringup_frame = 1;

static void display_callback(const void *data) { (void)data; }

static VPLogSink gxm_log_sink;
/* 0 = uninitialized, 1 = initializing, 2 = ready, 3 = open failed. The state
 * is what makes the first glog from any runtime entry deterministic and safe
 * even when a direct diagnostic call and the GXM worker race on first use. */
static volatile int gxm_log_state;
static volatile int gxm_log_failure_reported;

/* Bounded, non-recursive failure channel: one numeric line for the first open
 * or write failure of this process. It deliberately does not call glog. */
static void gxm_log_report_failure(int status, unsigned detail) {
    if (__sync_lock_test_and_set(&gxm_log_failure_reported, 1)) return;
    sceClibPrintf("VP_GXM_LOG_FAIL status=%d detail=%u\n", status, detail);
}

/* Returns 0 when the sink is ready, -1 when it is initialized but failed, and
 * -2 when another thread is still initializing after the bounded wait. */
static int gxm_log_ensure(void) {
    int state = __atomic_load_n(&gxm_log_state, __ATOMIC_ACQUIRE);
    if (state == 2) return 0;
    if (state == 3) return -1;
    if (__sync_bool_compare_and_swap(&gxm_log_state, 0, 1)) {
        int rc = vp_log_sink_init(&gxm_log_sink, &vp_log_sink_stdio_ops,
                                  VP_GXM_LOG_PATH);
        __atomic_store_n(&gxm_log_state, rc == 0 ? 2 : 3, __ATOMIC_RELEASE);
        if (rc != 0) gxm_log_report_failure(rc, 1u);
        return rc == 0 ? 0 : -1;
    }
    for (unsigned spins = 0; spins < 5000u; ++spins) {
        state = __atomic_load_n(&gxm_log_state, __ATOMIC_ACQUIRE);
        if (state != 1) return state == 2 ? 0 : -1;
        sceKernelDelayThread(1000);
    }
    return -2;
}

/* All glog call sites are serialized by the GXM worker in normal play, but the
 * public runtime entries (vp_gxm_runtime_test, vp_gxm_game_draw and the shader
 * registration helpers) can log before vp_gxm_worker_start from a diagnostic
 * caller. First use therefore initializes the one process-lifetime sink. The
 * sink has one append handle; a failed open is reported once above and all
 * later lines are counted as suppressed by the helper. Write/flush failures
 * fail closed and are also reported once. */
static void glog(const char *format, ...) {
    uint64_t fc = vp_gxm_frame_cost_log_begin();
    va_list args;
    int state = gxm_log_ensure();
    int rc;
    if (state == -2) {
        vp_gxm_frame_cost_category_end(VP_FRAME_COST_LOG, fc);
        return; /* half-initialized; do not read its fields */
    }
    va_start(args, format);
    rc = vp_log_sink_vprintf(&gxm_log_sink, format, args);
    va_end(args);
    if (rc < 0 && rc != VP_LOG_SINK_NOT_READY)
        gxm_log_report_failure(rc, 1u);
    vp_gxm_frame_cost_category_end(VP_FRAME_COST_LOG, fc);
}

int vp_gxm_log_get_stats(VPLogSinkStats *out) {
    if (out == NULL) return -1;
    if (gxm_log_ensure() == -2) return -1;
    return vp_log_sink_get_stats(&gxm_log_sink, out);
}

/* B88 exploration: place render-target/depth/scanout buffers in CDRAM while
 * dynamic uploads, staging and caches stay in main memory. Vita3K maps guest
 * memory uniformly, so only correctness (and whether the emulator accepts the
 * CDRAM memblock type) can be observed here; the bandwidth benefit needs a
 * real Vita. Default off; falls back to main memory beyond a fixed cap. */
#define VP_CDRAM_BUDGET_BYTES (48u * 1024u * 1024u)
#define VP_CDRAM_ALIGN_BYTES  (256u * 1024u)
static int gxm_cdram_state = -1;
static uint64_t gxm_cdram_bytes, gxm_cdram_allocs, gxm_cdram_fallbacks;

static int gxm_cdram_enabled(void) {
    if (gxm_cdram_state < 0) {
        FILE *marker = fopen("ux0:data/mhgu-vita-port/cdram-attachments.enable",
                             "rb");
        gxm_cdram_state = marker != NULL;
        if (marker != NULL) fclose(marker);
        glog("VP_GXM_CDRAM enabled=%d budget=%u\n", gxm_cdram_state,
             VP_CDRAM_BUDGET_BYTES);
    }
    return gxm_cdram_state;
}

static void *gpu_alloc_purpose(unsigned bytes, int kind, unsigned *offset,
                               int attachment) {
    unsigned size = (bytes + 0xfffu) & ~0xfffu;
    unsigned type = SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
    int cdram = 0;
    SceUID id;
    void *ptr = NULL;
    int rc;

    if (block_count >= VP_GXM_BLOCK_LIMIT) return NULL;
    if (attachment && kind == 0 && gxm_cdram_enabled()) {
        unsigned cdram_size = (unsigned)((size + VP_CDRAM_ALIGN_BYTES - 1u) &
                                         ~(VP_CDRAM_ALIGN_BYTES - 1u));
        if (cdram_size >= size &&
            gxm_cdram_bytes + cdram_size <= VP_CDRAM_BUDGET_BYTES) {
            size = cdram_size;
            type = SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
            cdram = 1;
        } else {
            gxm_cdram_fallbacks++;
            if (gxm_cdram_fallbacks <= 8u)
                glog("VP_GXM_CDRAM fallback bytes=%u total=%llu reason=budget\n",
                     size, (unsigned long long)gxm_cdram_bytes);
        }
    }
    id = sceKernelAllocMemBlock(cdram ? "vp-gxm-cdram" : "vp-gxm-rt", type,
                                size, NULL);
    if (id < 0 && cdram) {
        gxm_cdram_fallbacks++;
        glog("VP_GXM_CDRAM fallback bytes=%u rc=%d reason=alloc action=main\n",
             size, id);
        cdram = 0;
        type = SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
        id = sceKernelAllocMemBlock("vp-gxm-rt", type, size, NULL);
    }
    if (id < 0 || sceKernelGetMemBlockBase(id, &ptr) < 0) return NULL;
    rc = kind == 1 ? sceGxmMapVertexUsseMemory(ptr, size, offset)
         : kind == 2 ? sceGxmMapFragmentUsseMemory(ptr, size, offset)
                     : sceGxmMapMemory(ptr, size, SCE_GXM_MEMORY_ATTRIB_READ |
                                                   SCE_GXM_MEMORY_ATTRIB_WRITE);
    if (rc < 0) {
        sceKernelFreeMemBlock(id);
        return NULL;
    }
    memset(ptr, 0, size);
    blocks[block_count].id = id;
    blocks[block_count].ptr = ptr;
    blocks[block_count].kind = kind;
    blocks[block_count].bytes = size;
    blocks[block_count].cdram = cdram;
    block_count++;
    if (cdram) {
        gxm_cdram_bytes += size;
        gxm_cdram_allocs++;
        if (gxm_cdram_allocs <= 12u)
            glog("VP_GXM_CDRAM alloc bytes=%u total=%llu allocs=%llu\n",
                 size, (unsigned long long)gxm_cdram_bytes,
                 (unsigned long long)gxm_cdram_allocs);
    }
    return ptr;
}

static void *gpu_alloc(unsigned bytes, int kind, unsigned *offset) {
    return gpu_alloc_purpose(bytes, kind, offset, 0);
}

static void gpu_free(void *ptr) {
    unsigned i;
    if (ptr == NULL) return;
    for (i = 0; i < block_count; i++) {
        if (blocks[i].ptr != ptr) continue;
        if (blocks[i].kind == 1)
            sceGxmUnmapVertexUsseMemory(ptr);
        else if (blocks[i].kind == 2)
            sceGxmUnmapFragmentUsseMemory(ptr);
        else
            sceGxmUnmapMemory(ptr);
        if (blocks[i].cdram && gxm_cdram_bytes >= blocks[i].bytes)
            gxm_cdram_bytes -= blocks[i].bytes;
        sceKernelFreeMemBlock(blocks[i].id);
        for (unsigned j = i + 1u; j < block_count; j++) blocks[j - 1u] = blocks[j];
        block_count--;
        return;
    }
}

/* G2: surface identity and content. A surface is keyed by the guest GPU VA the
 * game binds as a render target or samples as tex0. The colour buffer is also
 * exposed as a linear texture so a later pass can consume an earlier pass's
 * output without copying or decoding guest block-linear memory. */
/* VP_HOST_TEST_EXTRACT:clear_surface_begin */
typedef struct {
    int used;
    uint64_t va;
    unsigned width;
    unsigned height;
    uint32_t format;
    unsigned render_width, render_height;
    void *color_mem;
    SceGxmColorSurface color;
    SceGxmTexture texture;
    int has_color;
    unsigned cleared_count;
    unsigned draw_count;
    unsigned sampled_count;
    uint64_t last_use;
    /* Display bridge crop. Every draw that targets a VI slot contributes its
     * viewport (intersected with the scissor, when enabled) to this rectangle.
     * It is consumed by the next present and then reset. */
    unsigned disp_x0, disp_y0, disp_x1, disp_y1;
    unsigned disp_valid;
    unsigned present_draws;
    /* Actual colour contents: a draw with a nonzero write mask rasterized, or
     * every colour channel was explicitly cleared at least once. A depth-only
     * clear does not initialize colour and must not be treated as a producer. */
    unsigned color_drawn;
    uint32_t color_clear_mask;
    /* Full-surface colour clear deferred until the next draw that writes this
     * target. Marker-gated; materialized before sampling/present/readback. */
    unsigned clear_pending;
    uint32_t clear_pending_color;
    /* Mode-6 fast path: the pending clear was recorded after the worker
     * submitted the open scene through a non-synchronous boundary. A later
     * CPU/GPU transfer materialization must first prove every pending scene
     * complete and keep this tag on failure. In-scene GPU clears consume it
     * under the same-context submission order. The tag survives a mode
     * 6 -> 5 switch so an already deferred clear keeps its drain. */
    unsigned clear_pending_async;
} VPGxmSurface;
/* VP_HOST_TEST_EXTRACT:clear_surface_end */

typedef struct {
    unsigned width;
    unsigned height;
    SceGxmRenderTarget *target;
} VPGxmTargetSlot;

static VPGxmSurface gxm_surfaces[VP_GXM_SURFACE_LIMIT];
static VPGxmTargetSlot gxm_targets[VP_GXM_TARGET_LIMIT];
static uint64_t gxm_surface_clock;

/* Keep guest image identities and shader coordinates in logical pixels.
 * Screen-sized 16:9 attachments use the selected 360p/540p native storage;
 * square shadow maps and small lookup/effect images retain their original
 * sampling grid. The selection is read once before GXM resources exist. */
#define VP_GXM_RENDER_RESOLUTION_PATH \
    "ux0:data/mhgu-vita-port/render-resolution.txt"
/* 0 = not loaded, 1 = ready, 2 = invalid. Only the startup paths below call
 * the loader; every later render_extent uses the cached height. */
static unsigned gxm_render_resolution_height = VP_RENDER_RESOLUTION_MAX_HEIGHT;
static int gxm_render_resolution_state;

static int gxm_render_resolution_ensure(void) {
    char text[64];
    size_t length;
    unsigned chosen = 0u;
    FILE *file;
    int extra, read_failed;

    if (gxm_render_resolution_state != 0)
        return gxm_render_resolution_state == 1 ? 0 : -1;
    file = fopen(VP_GXM_RENDER_RESOLUTION_PATH, "rb");
    if (file == NULL) {
        /* Missing file is the documented 540p default. An invalid existing
         * file is never treated as missing. */
        gxm_render_resolution_height = VP_RENDER_RESOLUTION_MAX_HEIGHT;
        gxm_render_resolution_state = 1;
    } else {
        length = fread(text, 1u, sizeof(text) - 1u, file);
        extra = fgetc(file);
        read_failed = ferror(file);
        fclose(file);
        if (read_failed || extra != EOF) {
            glog("VP_GXM_RENDER_RESOLUTION_INVALID path=%s reason=%s "
                 "expected=360p|540p\n", VP_GXM_RENDER_RESOLUTION_PATH,
                 read_failed ? "read" : "too-long");
            gxm_render_resolution_state = 2;
            return -1;
        }
        if (vp_render_resolution_parse(text, length, &chosen) != 0) {
            glog("VP_GXM_RENDER_RESOLUTION_INVALID path=%s reason=unsupported "
                 "expected=360p|540p\n", VP_GXM_RENDER_RESOLUTION_PATH);
            gxm_render_resolution_state = 2;
            return -1;
        }
        gxm_render_resolution_height = chosen;
        gxm_render_resolution_state = 1;
    }
    {
        VPRenderExtent native = vp_render_resolution_extent(
            1280u, 720u, gxm_render_resolution_height);
        glog("VP_GXM_RENDER_RESOLUTION mode=%s logical=1280x720 native=%ux%u\n",
             gxm_render_resolution_height == 360u ? "360p" : "540p",
             native.width, native.height);
    }
    return 0;
}

static unsigned render_extent(unsigned width, unsigned height, unsigned extent) {
    VPRenderExtent native = vp_render_resolution_extent(
        width, height, gxm_render_resolution_height);
    if (extent == width) return native.width;
    if (extent == height) return native.height;
    return extent;
}

static VPGxmSurface *surface_find(uint64_t va) {
    for (unsigned i = 0; i < VP_GXM_SURFACE_LIMIT; i++)
        if (gxm_surfaces[i].used && gxm_surfaces[i].va == va) {
            gxm_surfaces[i].last_use = ++gxm_surface_clock;
            return &gxm_surfaces[i];
        }
    return NULL;
}

/* Defined with the batch pipeline; every resource release and CPU readback
 * waits any deferred scene first. */
static int gxm_pending_scene_complete(const char *why);
static int surface_recycle_slot(void);

static void surface_release(VPGxmSurface *s) {
    if (s == NULL) return;
    /* The GPU may still read this allocation from a deferred scene. */
    (void)gxm_pending_scene_complete("surface-release");
    gpu_free(s->color_mem);
    memset(s, 0, sizeof(*s));
}

static SceGxmRenderTarget *target_for(unsigned width, unsigned height) {
    SceGxmRenderTargetParams params;
    SceGxmRenderTarget *target = NULL;
    int rc;

    for (unsigned i = 0; i < VP_GXM_TARGET_LIMIT; i++)
        if (gxm_targets[i].target != NULL && gxm_targets[i].width == width &&
            gxm_targets[i].height == height)
            return gxm_targets[i].target;
    if (width < 16u || height < 16u ||
        width > VP_GXM_RENDER_TARGET_MAX_DIM ||
        height > VP_GXM_RENDER_TARGET_MAX_DIM) return NULL;
    memset(&params, 0, sizeof(params));
    params.width = width;
    params.height = height;
    /* Every draw opens and closes its own scene; a frame contains about 20. */
    params.scenesPerFrame = 64u;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;
    rc = sceGxmCreateRenderTarget(&params, &target);
    if (rc < 0) {
        glog("VP_GXM_TARGET create %ux%u rc=%d\n", width, height, rc);
        return NULL;
    }
    for (unsigned i = 0; i < VP_GXM_TARGET_LIMIT; i++) {
        if (gxm_targets[i].target != NULL) continue;
        gxm_targets[i].width = width;
        gxm_targets[i].height = height;
        gxm_targets[i].target = target;
        glog("VP_GXM_TARGET created %ux%u slot=%u\n", width, height, i);
        return target;
    }
    glog("VP_GXM_TARGET slot limit reached %ux%u\n", width, height);
    sceGxmDestroyRenderTarget(target);
    return NULL;
}

static VPGxmSurface *surface_get(uint64_t va, unsigned width, unsigned height,
                                 uint32_t format, const char *why) {
    VPGxmSurface *s;
    unsigned slot = VP_GXM_SURFACE_LIMIT;
    int rc;

    if (va == 0u || width < 16u || height < 16u ||
        width > VP_GXM_RENDER_TARGET_MAX_DIM ||
        height > VP_GXM_RENDER_TARGET_MAX_DIM)
        return NULL;
    s = surface_find(va);
    if (s != NULL) {
        if (s->width == width && s->height == height) return s;
        glog("VP_GXM_SURFACE resize va=0x%llx %ux%u -> %ux%u why=%s\n",
             (unsigned long long)va, s->width, s->height, width, height, why);
        surface_release(s);
    }
    for (unsigned i = 0; i < VP_GXM_SURFACE_LIMIT; i++) {
        if (!gxm_surfaces[i].used) { slot = i; break; }
    }
    if (slot == VP_GXM_SURFACE_LIMIT) {
        int recycled = surface_recycle_slot();
        if (recycled < 0) return NULL;
        slot = (unsigned)recycled;
    }
    s = &gxm_surfaces[slot];
    memset(s, 0, sizeof(*s));
    s->va = va;
    s->width = width;
    s->height = height;
    s->format = format;
    s->render_width = render_extent(width, height, width);
    s->render_height = render_extent(width, height, height);
    s->color_mem = gpu_alloc_purpose((size_t)s->render_width * s->render_height * 4u,
                                     0, NULL, 1);
    if (s->color_mem == NULL) {
        glog("VP_GXM_SURFACE alloc failed va=0x%llx %ux%u why=%s\n",
             (unsigned long long)va, width, height, why);
        surface_release(s);
        return NULL;
    }
    rc = sceGxmColorSurfaceInit(&s->color, SCE_GXM_COLOR_FORMAT_A8B8G8R8,
            SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
            SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, s->render_width, s->render_height,
            s->render_width, s->color_mem);
    if (rc < 0) { surface_release(s); return NULL; }
    rc = sceGxmTextureInitLinear(&s->texture, s->color_mem,
            SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, s->render_width, s->render_height, 1);
    if (rc < 0) { surface_release(s); return NULL; }
    s->used = 1;
    s->has_color = 1;
    s->last_use = ++gxm_surface_clock;
    glog("VP_GXM_RESCALE va=0x%llx logical=%ux%u native=%ux%u\n",
         (unsigned long long)va, width, height, s->render_width, s->render_height);
    glog("VP_GXM_SURFACE create va=0x%llx %ux%u fmt=%u slot=%u why=%s\n",
         (unsigned long long)va, width, height, format, slot, why);
    return s;
}

static unsigned surface_nonzero_rgb(const VPGxmSurface *s) {
    const uint32_t *pixels;
    unsigned count, nonzero = 0;
    if (s == NULL || s->color_mem == NULL) return 0;
    pixels = (const uint32_t *)s->color_mem;
    count = s->render_width * s->render_height;
    for (unsigned i = 0; i < count; i++)
        if ((pixels[i] & 0xffffffu) != 0u) nonzero++;
    return nonzero;
}

static unsigned surface_nonzero_alpha(const VPGxmSurface *s) {
    const uint32_t *pixels;
    unsigned count, nonzero = 0;
    if (s == NULL || s->color_mem == NULL) return 0;
    pixels = (const uint32_t *)s->color_mem;
    count = s->render_width * s->render_height;
    for (unsigned i = 0; i < count; i++)
        if ((pixels[i] >> 24) != 0u) nonzero++;
    return nonzero;
}

/* Guest zeta identity is independent of the colour surface. Every draw and
 * every depth/stencil clear resolves the same VPGxmGuestDepth through the
 * pure planner in gxm_depth.c. A live identity is never evicted: returning to
 * the same zeta VA must preserve its depth content. Shape/format changes on
 * the same VA explicitly recreate the storage and declare the old content
 * undefined. All GXM calls and pool mutations happen on the worker between
 * synchronous submissions (frame_end_scene/Finish or transfer Finish). */
typedef struct {
    int used;
    VPGxmDepthDesc desc;
    VPGxmDepthDesc native_desc;
    void *depth_mem;
    void *stencil_mem;
    SceGxmDepthStencilSurface surface;
    unsigned depth_valid;
    /* Guest stencil may be live even though this backend has no stencil test
     * operations (native stencil state is fixed to ALWAYS/KEEP/KEEP/KEEP).
     * Set when an S8D24 identity sees a draw with Maxwell stencil_enable so a
     * later depth-only clear cannot silently discard expected stencil content.
     * Reset by an explicit depth+stencil clear, create and recreate. */
    unsigned stencil_live;
    float background_depth;
    unsigned background_stencil;
    unsigned clear_count;
    unsigned draw_count;
    /* 1 = a background clear was recorded but no completed scene has
     * materialized it yet; such content is refused as a sample source. */
    unsigned clear_defined;
    /* Monotonic non-zero epoch of the last completed scene on this identity.
     * Reset by create/recreate so a view can never describe an older
     * allocation.  This is a synchronous-draw epoch only: no view survives a
     * draw, and future batching must add its own lifetime protocol rather than
     * inherit this counter.  Exhaustion permanently invalidates the entry until
     * it is recreated; values are never wrapped or aliased. */
    uint32_t content_generation;
    unsigned generation_exhausted;
    /* Monotonic epoch of this native allocation.  Recreate assigns a new value
     * from a process-wide counter; it is never reset by a clear or draw so a
     * future persistent/batched view cannot confuse old storage with new. */
    uint32_t storage_generation;
} VPGxmDepthEntry;

typedef struct {
    int enabled;
    int slot;
    VPGxmDepthEntry *entry;
    VPGxmDepthDesc desc;
} VPGxmDepthBinding;

typedef struct {
    unsigned resolve, reuse, create, recreate, disabled, ignored_state;
    unsigned masked_refuse, masked_noop;
    unsigned reject_meta, reject_target, reject_budget, reject_pool;
    unsigned reject_alloc, clear_depth, clear_stencil;
    unsigned clear_refuse_stencil, clear_coupled_stencil;
    unsigned clear_materialize, clear_materialize_fail;
    uint64_t clear_materialize_us;
    unsigned stencil_unsupported;
} VPGxmDepthStats;

static VPGxmDepthEntry gxm_depths[VP_GXM_DEPTH_POOL_SLOTS];
static VPGxmDepthPool gxm_depth_pool;
static int gxm_depth_pool_ready;
static SceGxmDepthStencilSurface gxm_depth_disabled;
static int gxm_depth_disabled_ready;
static VPGxmDepthStats gxm_depth_stats;
static unsigned gxm_depth_bind_logs;
static unsigned gxm_depth_reject_logs;
static unsigned gxm_depth_disabled_logs;
static unsigned gxm_stencil_unsupported_logs;
static unsigned gxm_depth_write_normalized_logs;
static unsigned gxm_depth_materialize_logs;
/* Process-wide storage epoch.  Exhaustion is permanent for this process: a
 * wrapped epoch could alias an allocation an older view already observed. */
static uint32_t gxm_depth_storage_epoch;
static unsigned gxm_depth_storage_exhausted;

static int frame_poison_context(int rc);
static int frame_context_reject(const char *entry);

/* Monotonic GXM notification value shared by synchronous scenes and the
 * deferred scene pipeline. Waiting a value implies completion of every earlier
 * scene because the GPU consumes scenes in submission order. */
static uint32_t gxm_notify_seq;
/* VP_HOST_TEST_EXTRACT:notify_begin */
/* Values are monotonic and skip zero when the 32-bit sequence wraps: zero is
 * the region's initial/unset value, never a submitted notification. */
static uint32_t gxm_notify_next(uint32_t value) {
    value++;
    if (value == 0u) value++;
    return value;
}

/* Serial-number comparison for values separated by less than 2^31. At most one
 * scene per arena is outstanding, so the pending set stays far inside that
 * window; this must not be used as a general timestamp comparison. */
static int gxm_notify_newer(uint32_t value, uint32_t reference) {
    uint32_t distance = value - reference;
    return distance != 0u && distance < UINT32_C(0x80000000);
}

/* True when waiting for completed proves pending has also completed. This is
 * the exact complement of gxm_notify_newer, so newest selection and retirement
 * always agree. */
static int gxm_notify_reached(uint32_t completed, uint32_t pending) {
    return !gxm_notify_newer(pending, completed);
}

/* Acquire observation of the notification word. Vita GCC uses the atomic
 * builtin; the MSVC host test relies on x86 load ordering, which is acquire
 * for ordinary cached loads. */
static uint32_t gxm_notify_observe(volatile unsigned *address) {
#if defined(_MSC_VER)
    return *(volatile uint32_t *)address;
#else
    return __atomic_load_n(address, __ATOMIC_ACQUIRE);
#endif
}
/* VP_HOST_TEST_EXTRACT:notify_end */
static int gxm_scene_pipeline_enabled(void);
static void gxm_pending_scene_retire_upto(uint32_t value);

/* Per-draw timestamps are diagnostics. On the A9 every
 * sceKernelGetProcessTimeWide() is a kernel call, and the draw path samples a
 * dozen of them. They stay available when the play-perf harness asks for them
 * and are skipped in production; all counters fed by them then read zero. */
static int gxm_profile_state = -1;
static int gxm_profile_enabled(void) {
    if (gxm_profile_state < 0) {
        FILE *marker = fopen("ux0:data/mhgu-vita-port/play-perf.enable", "rb");
        gxm_profile_state = marker != NULL;
        if (marker != NULL) fclose(marker);
        glog("VP_GXM_PROFILE enabled=%d\n", gxm_profile_state);
    }
    return gxm_profile_state;
}
static uint64_t gxm_prof_now(void) {
    return gxm_profile_enabled() ? sceKernelGetProcessTimeWide() : 0u;
}

/* Full-surface colour clears may be folded into the next draw's scene. B111
 * enables that by default only for normal bounded-batch gameplay. Direct
 * diagnostics, the batch self test and legacy (non-static) mode keep the
 * previous standalone behaviour unless clear-fusion.enable is present. The
 * disable marker always wins over both the enable marker and the default. */
static int gxm_batch_enabled(void);
static int gxm_batch_aux_disabled(void);
static int gxm_batch_test_active(void);
static int gxm_clear_fusion_markers = -1;
static int gxm_clear_fusion_last_static = -1;
static int gxm_clear_fusion_last_test = -1;
static VPClearFusionSetting gxm_clear_fusion_setting;
static int gxm_clear_fusion_reported = -1;
static int gxm_clear_fusion_source_reported = -1;
static uint64_t gxm_clear_fusion_deferred, gxm_clear_fusion_in_scene;
static uint64_t gxm_clear_fusion_blue;
static uint64_t gxm_clear_fusion_materialized;
static uint64_t gxm_clear_fusion_combined;
/* A/B mode 6 clear-defer experiment. Cumulative worker-only counters; the
 * debug log prints them at the existing static-draw cadence and never per
 * clear. The pending tag itself lives on each VPGxmSurface. */
static uint64_t gxm_clear_defer_tries;
static uint64_t gxm_clear_defer_accepts;
static uint64_t gxm_clear_defer_fallbacks;
static uint64_t gxm_clear_defer_scene_submits;
static uint64_t gxm_clear_defer_submit_failures;
static uint64_t gxm_clear_defer_coalesced;
static uint64_t gxm_clear_defer_materialize_calls;
static uint64_t gxm_clear_defer_materialize_failures;
static uint64_t gxm_clear_defer_in_scene;
static int gxm_clear_defer_seen;
static int gxm_clear_defer_reported = -1;

/* Published before the root entry, read here on the worker. A mode switch only
 * changes admission for new clear requests; existing per-surface async tags
 * stay protective regardless of this value. */
static int gxm_clear_defer_enabled(void) {
    int enabled = __atomic_load_n(&vp_engine_clear_defer_enabled,
                                  __ATOMIC_ACQUIRE) != 0u;
    if (enabled) gxm_clear_defer_seen = 1;
    if (enabled != gxm_clear_defer_reported) {
        /* Default runs stay silent; only an actual transition (including the
         * 6 -> 5 switch after a deferred clear existed) is reported. */
        if (enabled || gxm_clear_defer_seen)
            glog("VP_GXM_CLEAR_DEFER enabled=%d\n", enabled);
        gxm_clear_defer_reported = enabled;
    }
    return enabled;
}

/* Marker state is read once. The default components are re-resolved only when
 * static mode or the self-test override changes, so the per-draw path stays a
 * few integer comparisons. No helper called here reaches clear fusion again. */
static VPClearFusionSetting gxm_clear_fusion_setting_get(void) {
    int test_active = gxm_batch_test_active();
    if (gxm_clear_fusion_markers < 0) {
        FILE *enable = fopen("ux0:data/mhgu-vita-port/clear-fusion.enable", "rb");
        FILE *disable = fopen("ux0:data/mhgu-vita-port/clear-fusion.disable", "rb");
        gxm_clear_fusion_markers =
            (enable != NULL ? 1 : 0) | (disable != NULL ? 2 : 0);
        if (enable != NULL) fclose(enable);
        if (disable != NULL) fclose(disable);
    }
    if (gxm_clear_fusion_last_static < 0 ||
        gxm_clear_fusion_last_static != static_mode ||
        gxm_clear_fusion_last_test != test_active) {
        gxm_clear_fusion_last_static = static_mode;
        gxm_clear_fusion_last_test = test_active;
        gxm_clear_fusion_setting = vp_clear_fusion_resolve(
            (gxm_clear_fusion_markers & 2) != 0,
            (gxm_clear_fusion_markers & 1) != 0,
            static_mode, gxm_batch_enabled(), gxm_batch_aux_disabled(),
            test_active ? 0 : -1);
    }
    return gxm_clear_fusion_setting;
}
static int gxm_clear_fusion_enabled(void) {
    static const char *const sources[] = {
        "disable-marker", "enable-marker", "legacy", "default-bounded-batch"
    };
    VPClearFusionSetting setting = gxm_clear_fusion_setting_get();
    if (setting.enabled != gxm_clear_fusion_reported ||
        (int)setting.source != gxm_clear_fusion_source_reported) {
        glog("VP_GXM_CLEAR_FUSION enabled=%d source=%s\n", setting.enabled,
             sources[(unsigned)setting.source < 4u
                         ? (unsigned)setting.source : 2u]);
        gxm_clear_fusion_reported = setting.enabled;
        gxm_clear_fusion_source_reported = (int)setting.source;
    }
    return setting.enabled;
}

/* Actual runtime completion blocking, sampled immediately before and after the
 * SDK calls. Notification and Finish are sequential and therefore disjoint;
 * EndScene is CPU/API wall time and is counted separately. All counters stay
 * zero unless the play-perf marker gates the timestamps on. */
static uint64_t gxm_endscene_calls, gxm_endscene_us;
static uint64_t gxm_notify_wait_calls, gxm_notify_wait_us;
static VPGpuWaitProfile gxm_wait_profile;
static unsigned gxm_wait_reclaim_active;
static unsigned gxm_wait_snapshot;
static uint64_t gxm_notify_poll_hits, gxm_notify_real_waits;
static uint64_t gxm_finish_calls, gxm_finish_us;
static uint64_t gxm_pending_complete_calls, gxm_pending_wait_us;
/* A combined drain submits the current scene first and proves all older
 * pending scenes complete with one wait for the newest value. */
static uint64_t gxm_combined_drains;
static uint64_t gxm_deferred_scenes;
/* Direct decoder-owned texture counters (deep mode only). */
static uint64_t gxm_direct_binds, gxm_direct_bytes_avoided;
static uint64_t gxm_direct_retains;
static uint64_t gxm_direct_bind_failures;
static int gxm_direct_seen;
static VPTextureRegions gxm_texture_region_arena;
/* Tentative declarations used by the early statistics logger; the
 * authoritative definitions live in the extracted direct/pending sections. */
static uint64_t gxm_direct_releases;
static uint64_t gxm_direct_quarantined;
static unsigned gxm_pending_scene_count;
/* Age since EndScene, not blocking; the worker may have encoded a whole scene
 * while the deferred scene ran. Kept only for overlap diagnostics. */
static uint64_t gxm_pipeline_waits, gxm_pipeline_age_us, gxm_pipeline_age_max;

static void gxm_wait_profile_log(uint64_t now, uint64_t frames) {
    if (gxm_profile_state != 1) return;
    unsigned snapshot = ++gxm_wait_snapshot;
    for (unsigned i = 0u; i < VP_GPU_WAIT_CAUSES; ++i) {
        const VPGpuWaitCause *c = &gxm_wait_profile.causes[i];
        glog("VP_GPU_WAIT snap=%u t_us=%llu frames=%llu row=%u rows=%u "
             "cause=%s calls=%llu real=%llu polls=%llu failures=%llu "
             "block_us=%llu pending_sum=%llu draws=%llu sync=%llu "
             "b0=%llu b1=%llu b2=%llu b3=%llu b4=%llu\n",
             snapshot, (unsigned long long)now, (unsigned long long)frames,
             i, (unsigned)VP_GPU_WAIT_CAUSES, vp_gpu_wait_names[i],
             (unsigned long long)c->calls, (unsigned long long)c->real_waits,
             (unsigned long long)c->polls, (unsigned long long)c->failures,
             (unsigned long long)c->block_us, (unsigned long long)c->pending_sum,
             (unsigned long long)c->draws, (unsigned long long)c->synchronous,
             (unsigned long long)c->buckets[0], (unsigned long long)c->buckets[1],
             (unsigned long long)c->buckets[2], (unsigned long long)c->buckets[3],
             (unsigned long long)c->buckets[4]);
    }
    glog("VP_GPU_WAIT_TOTAL snap=%u t_us=%llu frames=%llu real=%llu "
         "block_us=%llu\n", snapshot, (unsigned long long)now,
         (unsigned long long)frames, (unsigned long long)gxm_notify_wait_calls,
         (unsigned long long)gxm_notify_wait_us);
}

/* Bounded draw timing. Fields are split exactly at the synchronous boundaries
 * this backend owns: CPU preparation, BeginScene, state/encode, sceGxmDraw,
 * EndScene, notification wait and Finish. Every sample is aggregated into a
 * fixed histogram; nothing is written per draw, no readback is added and the
 * submission order is unchanged. */
#define VP_GXM_PERF_BUCKETS 16u
#define VP_GXM_PERF_WINDOW_US 2000000ull
#define VP_GXM_PERF_WINDOW_DRAWS 256u
static const uint64_t gxm_perf_bounds[VP_GXM_PERF_BUCKETS - 1u] = {
    50ull, 100ull, 250ull, 500ull, 1000ull, 2500ull, 5000ull, 10000ull,
    25000ull, 50000ull, 100000ull, 250000ull, 500000ull, 1000000ull, 5000000ull
};
typedef struct {
    uint64_t count, sum, max;
    uint64_t bucket[VP_GXM_PERF_BUCKETS];
} VPGxmPerfMetric;
typedef struct {
    unsigned draws;
    unsigned failures;
    unsigned skips;
    uint64_t window_start;
    VPGxmPerfMetric prep, begin, encode, draw, end_scene, notify, finish, total;
    VPGxmPerfMetric worker; /* worker thread draw processing, not API roundtrip */
    VPGxmPerfMetric fail;   /* rejected/failed draw processing time */
    VPGxmPerfMetric staged; /* batch-encoded worker time (no EndScene here) */
} VPGxmPerfKind;
static VPGxmPerfKind gxm_perf_kinds[2]; /* 0 = colour, 1 = depth-only */
static unsigned gxm_perf_bounds_logged;
static uint64_t gxm_perf_present_frames, gxm_perf_present_sum, gxm_perf_present_max;
static uint64_t gxm_perf_present_last_at, gxm_perf_present_interval_sum;
static uint64_t gxm_perf_present_interval_max, gxm_perf_present_interval_count;
static uint64_t gxm_perf_present_buckets[VP_GXM_PERF_BUCKETS];
/* Cumulative whole-frame intervals, distinct from the Present call duration.
 * Difference two reports from the same scene to exclude boot/loading. */
static uint32_t gxm_perf_frame_intervals[512];
static uint64_t gxm_perf_global_ok, gxm_perf_global_fail, gxm_perf_global_skip;
static unsigned gxm_last_draw_class = 2u; /* 0 colour, 1 depth-only, 2 noop */

/* ---- Bounded frame-cost decomposition -----------------------------------
 *
 * Only the single GXM worker records spans. The play-perf marker is not used:
 * frame-cost.enable is an independent gate, and frame-cost-mode.txt is polled
 * at decoder-entry boundaries at most every 500 ms. Mode 0 keeps the exact
 * same boundary clock and file polling cadence but performs no span clocks, so
 * the two modes can be compared descriptively. A missing/illegal control file
 * is a recorded control error, never a silent fallback. No span is opened on a
 * non-worker thread or while the profile is disabled. */
#define VP_FRAME_COST_MARKER_PATH "ux0:data/mhgu-vita-port/frame-cost.enable"
#define VP_FRAME_COST_MODE_PATH "ux0:data/mhgu-vita-port/frame-cost-mode.txt"
#define VP_FRAME_COST_POLL_US 500000ull

static VPFrameCostProfile gxm_frame_cost_profile;
static int gxm_frame_cost_marker = -1;   /* -1 unknown, 0 absent, 1 present */
static int gxm_frame_cost_mode;          /* applied control mode */
static int gxm_frame_cost_enabled;       /* spans active (mode 1) */
static int gxm_frame_cost_failed;        /* sticky control error */
static unsigned gxm_frame_cost_epoch;
static unsigned gxm_frame_cost_seq;
static int gxm_frame_cost_reported_mode = -1;
static int gxm_frame_cost_reported_failed = -1;
static uint64_t gxm_frame_cost_last_poll_us;
static uint64_t gxm_frame_cost_poll_clocks;
static uint64_t gxm_frame_cost_boundaries;
static int gxm_frame_resource_audit;
static uint64_t gxm_frame_resource_start_frame;
static void gxm_frame_cost_details(uint64_t now, uint64_t frames, unsigned seq);
static void gxm_texture_memory_log(uint64_t now, uint64_t frames);

int vp_gxm_frame_cost_active(void) {
    return gxm_frame_cost_enabled;
}

int vp_gxm_frame_cost_resource_audit(uint64_t *frame) {
    if (frame) *frame = gxm_perf_present_frames;
    return gxm_frame_cost_enabled && gxm_frame_resource_audit &&
        gxm_perf_present_frames - gxm_frame_resource_start_frame < 16u;
}

uint64_t vp_gxm_frame_cost_log_begin(void) {
    /* Reporting an empty stack must not create a new root which reports
     * itself. Ordinary logging inside work remains a nested measured cost. */
    if (!gxm_frame_cost_enabled || !gxm_frame_cost_profile.depth) return 0u;
    return vp_gxm_frame_cost_category_begin(VP_FRAME_COST_LOG);
}

uint64_t vp_gxm_frame_cost_now(void) {
    if (!gxm_frame_cost_enabled) return 0u;
    gxm_frame_cost_profile.clock_calls++;
    return sceKernelGetProcessTimeWide();
}

uint64_t vp_gxm_frame_cost_category_begin(unsigned category) {
    if (!gxm_frame_cost_enabled || !vp_gxm_worker_is_current()) return 0u;
    uint64_t now = vp_gxm_frame_cost_now();
    if (now != 0u &&
        !vp_frame_cost_span_begin(&gxm_frame_cost_profile, category, now))
        return 0u;
    return now;
}

void vp_gxm_frame_cost_category_end(unsigned category, uint64_t started_us) {
    if (started_us == 0u) return;
    uint64_t now = vp_gxm_frame_cost_now();
    int root = gxm_frame_cost_profile.depth == 1u;
    int rc = vp_frame_cost_span_end(&gxm_frame_cost_profile, category, now);
    if (!rc && root)
        glog("VP_FRAME_COST_ROOT schema=%u epoch=%u cat=%s entry_us=%llu "
             "t_us=%llu frames=%llu\n", VP_FRAME_COST_SCHEMA,
             gxm_frame_cost_epoch, vp_frame_cost_category_names[category],
             (unsigned long long)started_us, (unsigned long long)now,
             (unsigned long long)gxm_perf_present_frames);
}

static uint64_t gxm_frame_cost_errors(void) {
    return vp_frame_cost_error_count(&gxm_frame_cost_profile) +
           (gxm_frame_cost_failed ? 1u : 0u);
}

static void gxm_frame_cost_log_point(uint64_t now, uint64_t frames,
                                     const char *reason) {
    glog("VP_FRAME_COST_POINT schema=%u epoch=%u mode=%d t_us=%llu "
         "frames=%llu clocks=%llu poll_clocks=%llu boundaries=%llu "
         "control_error=%d reason=%s\n",
         VP_FRAME_COST_SCHEMA, gxm_frame_cost_epoch, gxm_frame_cost_mode,
         (unsigned long long)now, (unsigned long long)frames,
         (unsigned long long)gxm_frame_cost_profile.clock_calls,
         (unsigned long long)gxm_frame_cost_poll_clocks,
         (unsigned long long)gxm_frame_cost_boundaries,
         gxm_frame_cost_failed ? 1 : 0, reason);
}

/* Emit one complete snapshot: BEGIN, every fixed category row, END. All lines
 * carry the same schema/seq/epoch/mode/t_us/frames identity. A snapshot with an
 * open span is refused and recorded as incomplete instead of being presented
 * as a closed interval. */
static void gxm_frame_cost_log_snapshot(uint64_t now, uint64_t frames,
                                        const char *reason) {
    VPFrameCostProfile copy;
    unsigned seq, row;
    uint64_t errors;

    if (!gxm_frame_cost_enabled) return;
    if (gxm_frame_cost_profile.depth != 0u) {
        gxm_frame_cost_profile.incomplete_snapshot++;
        glog("VP_FRAME_COST_SNAPSHOT_SKIP schema=%u epoch=%u t_us=%llu "
             "frames=%llu depth=%u reason=span-active\n",
             VP_FRAME_COST_SCHEMA, gxm_frame_cost_epoch,
             (unsigned long long)now, (unsigned long long)frames,
             gxm_frame_cost_profile.depth);
        return;
    }
    copy = gxm_frame_cost_profile;
    seq = ++gxm_frame_cost_seq;
    errors = gxm_frame_cost_errors();
    glog("VP_FRAME_COST_SNAPSHOT_BEGIN schema=%u seq=%u epoch=%u mode=1 "
         "t_us=%llu frames=%llu rows=%u depth=%u errors=%llu mismatch=%llu "
         "overflow=%llu underflow=%llu clock_regress=%llu missing_time=%llu "
         "incomplete=%llu control_error=%d clocks=%llu poll_clocks=%llu "
         "boundaries=%llu reason=%s\n",
         VP_FRAME_COST_SCHEMA, seq, gxm_frame_cost_epoch,
         (unsigned long long)now, (unsigned long long)frames,
         VP_FRAME_COST_CATEGORY_COUNT, copy.depth,
         (unsigned long long)errors, (unsigned long long)copy.mismatch,
         (unsigned long long)copy.overflow, (unsigned long long)copy.underflow,
         (unsigned long long)copy.clock_regression,
         (unsigned long long)copy.missing_time,
         (unsigned long long)copy.incomplete_snapshot,
         gxm_frame_cost_failed ? 1 : 0,
         (unsigned long long)copy.clock_calls,
         (unsigned long long)gxm_frame_cost_poll_clocks,
         (unsigned long long)gxm_frame_cost_boundaries, reason);
    for (row = 0u; row < VP_FRAME_COST_CATEGORY_COUNT; ++row) {
        const VPFrameCostCategory *c = &copy.categories[row];
        glog("VP_FRAME_COST_SNAPSHOT_ROW schema=%u seq=%u epoch=%u mode=1 "
             "t_us=%llu frames=%llu row=%u cat=%s calls=%llu incl_us=%llu "
             "self_us=%llu max_us=%llu root_calls=%llu root_incl_us=%llu\n",
             VP_FRAME_COST_SCHEMA, seq, gxm_frame_cost_epoch,
             (unsigned long long)now, (unsigned long long)frames, row,
             vp_frame_cost_category_names[row],
             (unsigned long long)c->calls,
             (unsigned long long)c->inclusive_us,
             (unsigned long long)c->self_us,
             (unsigned long long)c->max_us,
             (unsigned long long)c->root_calls,
             (unsigned long long)c->root_inclusive_us);
    }
    gxm_frame_cost_details(now, frames, seq);
    glog("VP_FRAME_COST_SNAPSHOT_END schema=%u seq=%u epoch=%u mode=1 "
         "t_us=%llu frames=%llu rows=%u errors=%llu\n",
         VP_FRAME_COST_SCHEMA, seq, gxm_frame_cost_epoch,
         (unsigned long long)now, (unsigned long long)frames,
         VP_FRAME_COST_CATEGORY_COUNT, (unsigned long long)errors);
}

static void gxm_frame_cost_start_epoch(uint64_t now, uint64_t frames,
                                       const char *source) {
    gxm_frame_cost_epoch++;
    gxm_frame_cost_seq = 0u;
    gxm_frame_cost_poll_clocks = 0u;
    gxm_frame_cost_boundaries = 0u;
    vp_frame_cost_profile_reset(&gxm_frame_cost_profile);
    gxm_frame_cost_enabled = 1;
    gxm_frame_resource_start_frame = frames;
    glog("VP_FRAME_COST_EPOCH action=start schema=%u epoch=%u mode=1 "
         "t_us=%llu frames=%llu source=%s\n",
         VP_FRAME_COST_SCHEMA, gxm_frame_cost_epoch,
         (unsigned long long)now, (unsigned long long)frames, source);
}

static void gxm_frame_cost_stop_epoch(uint64_t now, uint64_t frames,
                                      const char *reason) {
    if (!gxm_frame_cost_enabled) return;
    /* Record the final closed interval before disabling span clocks. */
    gxm_frame_cost_log_snapshot(now, frames, reason);
    gxm_frame_cost_enabled = 0;
    glog("VP_FRAME_COST_EPOCH action=stop schema=%u epoch=%u mode=0 "
         "t_us=%llu frames=%llu reason=%s\n",
         VP_FRAME_COST_SCHEMA, gxm_frame_cost_epoch,
         (unsigned long long)now, (unsigned long long)frames, reason);
}

/* -1 = missing/unreadable, -2 = parse error, 0 = valid mode. */
static int gxm_frame_cost_read_mode(unsigned *mode) {
    char text[64];
    size_t count;
    int extra;
    FILE *file = fopen(VP_FRAME_COST_MODE_PATH, "rb");
    if (file == NULL) return -1;
    count = fread(text, 1u, sizeof(text), file);
    extra = fgetc(file);
    fclose(file);
    if (extra != EOF) return -2;
    return vp_frame_cost_parse_mode(text, count, mode) == 0 ? 0 : -2;
}

static void gxm_frame_cost_apply(unsigned requested_mode, int control_error,
                                 uint64_t now, uint64_t frames,
                                 const char *source) {
    if (control_error) {
        gxm_frame_cost_mode = 0;
        if (!gxm_frame_cost_failed) {
            gxm_frame_cost_failed = 1;
            glog("VP_FRAME_COST_CONTROL source=%s error=%s action=disable-data "
                 "epoch=%u t_us=%llu frames=%llu\n", source,
                 control_error == -1 ? "mode-file-missing" : "mode-invalid",
                 gxm_frame_cost_epoch, (unsigned long long)now,
                 (unsigned long long)frames);
            gxm_frame_cost_stop_epoch(now, frames, "control-error");
            gxm_frame_cost_log_point(now, frames, "control-error");
        }
        return;
    }
    /* A control error invalidates this process's closure data. Keep polling
     * (so the condition stays observable) but never resume spans. */
    if (gxm_frame_cost_failed) {
        if (gxm_frame_cost_reported_failed != 1) {
            gxm_frame_cost_reported_failed = 1;
            gxm_frame_cost_log_point(now, frames, "control-error-sticky");
        }
        return;
    }
    if (requested_mode == (unsigned)gxm_frame_cost_mode) {
        if (gxm_frame_cost_reported_mode != (int)requested_mode) {
            gxm_frame_cost_reported_mode = (int)requested_mode;
            gxm_frame_cost_log_point(now, frames,
                                     requested_mode == 1 ? "mode-1" : "mode-0");
        }
        return;
    }
    if (gxm_frame_cost_mode == 1)
        gxm_frame_cost_stop_epoch(now, frames, "mode-change");
    gxm_frame_cost_mode = (int)requested_mode;
    if (requested_mode == 1) {
        gxm_frame_cost_start_epoch(now, frames, source);
    } else {
        gxm_frame_cost_log_point(now, frames, "mode-0");
    }
    gxm_frame_cost_reported_mode = (int)requested_mode;
}

/* Called once at the start of every complete decoder-entry boundary, while the
 * span stack is empty. This is the only place that reads frame-cost control
 * files or takes the boundary clock. */
void vp_gxm_frame_cost_worker_boundary(void) {
    FILE *marker;
    uint64_t now;
    unsigned requested = 0u;
    int read_rc;

    if (gxm_frame_cost_marker == 0) return;
    if (gxm_frame_cost_profile.depth != 0u) {
        /* Mode switches only happen with a closed span stack. This cannot be
         * a normal decoder-entry boundary; refuse instead of splitting a
         * cumulative epoch in the middle of a span. */
        glog("VP_FRAME_COST_CONTROL action=defer-boundary depth=%u\n",
             gxm_frame_cost_profile.depth);
        return;
    }
    if (gxm_frame_cost_marker < 0) {
        marker = fopen(VP_FRAME_COST_MARKER_PATH, "rb");
        gxm_frame_cost_marker = marker != NULL;
        if (marker != NULL) fclose(marker);
        glog("VP_FRAME_COST_MARKER enabled=%d path=%s\n",
             gxm_frame_cost_marker, VP_FRAME_COST_MARKER_PATH);
        if (!gxm_frame_cost_marker) return; /* no polling, no clock */
        FILE *resource = fopen("ux0:data/mhgu-vita-port/frame-resource-audit.enable", "rb");
        gxm_frame_resource_audit = resource != NULL;
        if (resource) fclose(resource);
    }

    /* The marker is present. Take the boundary clock even in mode 0 so both
     * modes pay the same polling structure; file reads stay bounded to the
     * 500 ms boundary cadence. */
    now = sceKernelGetProcessTimeWide();
    gxm_frame_cost_poll_clocks++;
    gxm_frame_cost_boundaries++;
    if (gxm_frame_cost_last_poll_us != 0u &&
        now - gxm_frame_cost_last_poll_us < VP_FRAME_COST_POLL_US)
        return;
    gxm_frame_cost_last_poll_us = now;
    marker = fopen(VP_FRAME_COST_MARKER_PATH, "rb");
    if (marker == NULL) {
        gxm_frame_cost_marker = 0;
        gxm_frame_cost_stop_epoch(now, gxm_perf_present_frames, "marker-removed");
        gxm_frame_cost_mode = 0;
        gxm_frame_cost_log_point(now, gxm_perf_present_frames, "marker-removed");
        return;
    }
    fclose(marker);
    read_rc = gxm_frame_cost_read_mode(&requested);
    gxm_frame_cost_apply(read_rc == 0 ? requested : 0u, read_rc,
                         now, gxm_perf_present_frames,
                         VP_FRAME_COST_MODE_PATH);
}


/* Draw refusal census. Every frame_draw_internal_body failure records the last
 * stage marker it passed, so a run identifies which section refused the draw
 * without a per-draw hash chain. Bounded logging only; counters are exact. */
#define VP_GXM_DRAW_STAGES 16u
static unsigned gxm_draw_stage;
static uint64_t gxm_draw_refuse_stage[VP_GXM_DRAW_STAGES];
static uint64_t gxm_draw_refuse_total;
static unsigned gxm_draw_refuse_logs;

/* Refusals inside stage 9 (texture preparation) that previously returned
 * without their own marker. Counted exactly; the first hit logs once. */
enum {
    VP_GXM_S9_PRIMARY_DEPTH = 0,
    VP_GXM_S9_PRIMARY_FORMAT,
    VP_GXM_S9_PRIMARY_BYTES,
    VP_GXM_S9_PRIMARY_UNIFORMS,
    VP_GXM_S9_VERTEX_TEX,
    VP_GXM_S9_TEX_MISSING,
    VP_GXM_S9_REASON_COUNT
};
static uint64_t gxm_stage9_reason[VP_GXM_S9_REASON_COUNT];
static const char *const gxm_stage9_names[VP_GXM_S9_REASON_COUNT] = {
    "primary-depth-view", "primary-format", "primary-bytes",
    "primary-uniforms", "vertex-tex", "tex-missing-resource"};
static void gxm_stage9_note(unsigned reason, unsigned draw_index) {
    if (reason >= VP_GXM_S9_REASON_COUNT) return;
    if (gxm_stage9_reason[reason]++ == 0u)
        glog("VP_GXM_STAGE9_REASON reason=%s first_idx=%u\n",
             gxm_stage9_names[reason], draw_index);
}

/* Per-hash refusal census: keeps the exact count for the 64 hashes that first
 * fail, and logs the first two refusals of each so village-only classes are
 * identifiable even when the global bounded log has moved past them. */
#define VP_GXM_REFUSE_HASH_LIMIT 64u
static struct {
    unsigned hash, stage;
    uint64_t count;
    unsigned logs;
} gxm_refuse_hash[VP_GXM_REFUSE_HASH_LIMIT];
static unsigned gxm_refuse_hash_used;
static unsigned gxm_refuse_hash_order[12];

/* Extra-texture and shadow rejection counters are printed by static_log_stats;
 * the definitions live here so the summary can see them. */
static unsigned gxm_extra_gpu_binds, gxm_extra_cpu_binds, gxm_extra_rejects;
static unsigned gxm_extra_reject_feedback, gxm_extra_reject_missing;
static unsigned gxm_extra_reject_uninitialized, gxm_extra_reject_view;
static unsigned gxm_extra_reject_format, gxm_extra_reject_sampler;
static unsigned gxm_extra_reject_layout;
static unsigned gxm_extra_reject_logs, gxm_extra_summary_logs;
static unsigned gxm_shadow_rejects;
static void gxm_refuse_hash_note(unsigned hash, unsigned stage,
                                 unsigned draw_index, int rc) {
    unsigned i;
    for (i = 0; i < gxm_refuse_hash_used; ++i)
        if (gxm_refuse_hash[i].hash == hash) break;
    if (i == gxm_refuse_hash_used) {
        if (gxm_refuse_hash_used >= VP_GXM_REFUSE_HASH_LIMIT) return;
        gxm_refuse_hash[i].hash = hash;
        gxm_refuse_hash[i].stage = stage;
        gxm_refuse_hash_used++;
    }
    gxm_refuse_hash[i].count++;
    if (gxm_refuse_hash[i].logs < 2u) {
        gxm_refuse_hash[i].logs++;
        glog("VP_GXM_DRAW_REFUSE_HASH idx=%u hash=0x%x stage=%u rc=%d "
             "count=%llu\n", draw_index, hash, stage, rc,
             (unsigned long long)gxm_refuse_hash[i].count);
    }
}

/* Diagnostic-only per-draw trace. Gated by draw-trace.enable. It exists to
 * attribute residual pure-black pixel regions to concrete draws: every request
 * logs its program hash, size, target, blend/alpha/depth state and sampler
 * identities, and (inside the last part of a run) the first bytes of the
 * fragment tex0..tex3 payloads are written next to the log once per VA.
 * Bounded so a long run stays readable; never changes draw behaviour. */
#define VP_GXM_DRAW_TRACE_LIMIT 220000u
#define VP_GXM_DRAW_TRACE_TEX_LIMIT 160u
/* Texture dumps start at 700 s of process time: the village close-up frames
 * (740/800/860 s captures) finish boot/lobby long before this point, so the
 * dumped payloads belong to the frames of interest. */
#define VP_GXM_DRAW_TRACE_TEX_MS_MIN 700000ull
#define VP_GXM_DRAW_TRACE_TEX_BYTES (6u*1024u*1024u)
static int gxm_draw_trace_state = -1;
static unsigned gxm_draw_trace_lines, gxm_draw_trace_tex_dumps;
static uint64_t gxm_draw_trace_tex_va[VP_GXM_DRAW_TRACE_TEX_LIMIT];

static int gxm_draw_trace_enabled(void) {
    if (gxm_draw_trace_state < 0) {
        FILE *marker = fopen("ux0:data/mhgu-vita-port/draw-trace.enable", "rb");
        gxm_draw_trace_state = marker != NULL;
        if (marker != NULL) fclose(marker);
        glog("VP_GXM_DRAW_TRACE enabled=%d limit=%u tex_limit=%u\n",
             gxm_draw_trace_state, VP_GXM_DRAW_TRACE_LIMIT,
             VP_GXM_DRAW_TRACE_TEX_LIMIT);
    }
    return gxm_draw_trace_state;
}

static int gxm_draw_trace_u32(const VPGxmUniform *b, unsigned index,
                              uint32_t *out) {
    if (b == NULL || b->data == NULL ||
        b->size < (size_t)(index + 1u) * 4u)
        return 0;
    memcpy(out, (const uint8_t *)b->data + (size_t)index * 4u, 4u);
    return 1;
}

static void gxm_draw_trace_dump_tex(const char *slot, const void *texels,
                                    uint64_t va, unsigned width,
                                    unsigned height, unsigned format,
                                    unsigned cube) {
    size_t bytes;
    FILE *file;
    char path[128];
    unsigned i;
    if (texels == NULL || va == 0u || width == 0u || height == 0u ||
        width > 1024u || height > 1024u)
        return;
    if (gxm_draw_trace_tex_dumps >= VP_GXM_DRAW_TRACE_TEX_LIMIT) return;
    for (i = 0; i < gxm_draw_trace_tex_dumps; i++)
        if (gxm_draw_trace_tex_va[i] == va) return;
    bytes = cube ? vp_texture_cube_upload_bytes(width, height)
                 : vp_texture_upload_bytes(width, height, format);
    if (bytes == 0u || bytes > VP_GXM_DRAW_TRACE_TEX_BYTES) return;
    snprintf(path, sizeof(path),
             "ux0:data/mhgu-vita-port/probe-tex%u-0x%llx-%ux%u-f%u.rgba",
             gxm_draw_trace_tex_dumps, (unsigned long long)va, width, height,
             format);
    file = fopen(path, "wb");
    if (file == NULL) return;
    fwrite(texels, 1u, bytes, file);
    fclose(file);
    gxm_draw_trace_tex_va[gxm_draw_trace_tex_dumps] = va;
    gxm_draw_trace_tex_dumps++;
    glog("VP_GXM_DRAW_TRACE_TEX slot=%s va=0x%llx %ux%u fmt=%u cube=%u "
         "bytes=%llu path=%s\n", slot, (unsigned long long)va, width, height,
         format, cube, (unsigned long long)bytes, path);
}

/* One-shot vertex dump for terrain attribution. Diagnostic only, gated by the
 * vtx-dump.enable marker; it reads guest vertex/index data plus the VS/FS
 * constant banks needed to reconstruct position, UV and colour offline, and
 * never changes draw behaviour or ordering. */
#define VP_GXM_VTX_DUMP_LIMIT 2000u
#define VP_GXM_VTX_DUMP_MS_MIN 735000ull
#define VP_GXM_VTX_DUMP_MS_MAX 750000ull
#define VP_GXM_VTX_DUMP_BYTES (4u * 1024u * 1024u)
#define VP_GXM_VTX_DUMP_MAGIC 0x30585456u /* "VTX0" */
static int gxm_vtx_dump_state = -1;
static unsigned gxm_vtx_dump_count;

static int gxm_vtx_dump_enabled(void) {
    if (gxm_vtx_dump_state < 0) {
        FILE *marker = fopen("ux0:data/mhgu-vita-port/vtx-dump.enable", "rb");
        gxm_vtx_dump_state = marker != NULL;
        if (marker != NULL) fclose(marker);
        glog("VP_GXM_VTX_DUMP enabled=%d limit=%u ms=[%llu,%llu]\n",
             gxm_vtx_dump_state, VP_GXM_VTX_DUMP_LIMIT,
             (unsigned long long)VP_GXM_VTX_DUMP_MS_MIN,
             (unsigned long long)VP_GXM_VTX_DUMP_MS_MAX);
    }
    return gxm_vtx_dump_state;
}

static void gxm_vtx_dump_u32(FILE *f, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
    fwrite(b, 1u, 4u, f);
}

static void gxm_vtx_dump(const VPGxmDrawRequest *req) {
    static const unsigned bank_list[6][2] = {
        {0u, 3u}, {0u, 4u}, {0u, 5u}, {1u, 3u}, {1u, 5u}, {1u, 7u}
    };
    char path[128], attrs[129];
    size_t vbytes, ibytes = 0u;
    FILE *f;
    uint64_t now;
    unsigned i;

    if (req == NULL || !gxm_vtx_dump_enabled()) return;
    if (gxm_vtx_dump_count >= VP_GXM_VTX_DUMP_LIMIT) return;
    if (!req->frame_draw || req->vertices == NULL || req->stride == 0u ||
        req->count < 8u)
        return;
    if (req->target_width != 1280u || req->target_height != 720u) return;
    now = sceKernelGetProcessTimeWide() / 1000u;
    if (now < VP_GXM_VTX_DUMP_MS_MIN || now > VP_GXM_VTX_DUMP_MS_MAX) return;
    vbytes = (size_t)req->count * req->stride;
    if (vbytes > VP_GXM_VTX_DUMP_BYTES) return;
    if (req->indexed && req->indices != NULL) {
        size_t isz = req->index_format == SCE_GXM_INDEX_FORMAT_U32 ? 4u : 2u;
        ibytes = (size_t)req->index_count * isz;
        if (ibytes > VP_GXM_VTX_DUMP_BYTES) return;
    }
    for (i = 0; i < 16u; i++)
        snprintf(attrs + i * 8u, 9u, "%08x", req->attributes[i]);
    snprintf(path, sizeof(path),
             "ux0:data/mhgu-vita-port/probe-vtx%u-idx%u-h%x.bin",
             gxm_vtx_dump_count, req->draw_index, req->hash);
    f = fopen(path, "wb");
    if (f == NULL) return;
    gxm_vtx_dump_u32(f, VP_GXM_VTX_DUMP_MAGIC);
    gxm_vtx_dump_u32(f, req->draw_index);
    gxm_vtx_dump_u32(f, req->hash);
    gxm_vtx_dump_u32(f, req->vs_token);
    gxm_vtx_dump_u32(f, req->fs_token);
    gxm_vtx_dump_u32(f, req->count);
    gxm_vtx_dump_u32(f, req->stride);
    gxm_vtx_dump_u32(f, (uint32_t)req->indexed);
    gxm_vtx_dump_u32(f, req->index_count);
    gxm_vtx_dump_u32(f, req->index_format);
    gxm_vtx_dump_u32(f, (uint32_t)req->base_vertex);
    gxm_vtx_dump_u32(f, req->index_window_first);
    gxm_vtx_dump_u32(f, (uint32_t)now);
    gxm_vtx_dump_u32(f, (uint32_t)gxm_perf_present_frames);
    gxm_vtx_dump_u32(f, req->viewport_x);
    gxm_vtx_dump_u32(f, req->viewport_y);
    gxm_vtx_dump_u32(f, req->viewport_width);
    gxm_vtx_dump_u32(f, req->viewport_height);
    gxm_vtx_dump_u32(f, req->target_width);
    gxm_vtx_dump_u32(f, req->target_height);
    gxm_vtx_dump_u32(f, (uint32_t)(req->target_va & 0xffffffffu));
    gxm_vtx_dump_u32(f, (uint32_t)(req->target_va >> 32));
    for (i = 0; i < 16u; i++) gxm_vtx_dump_u32(f, req->attributes[i]);
    for (i = 0; i < 6u; i++) {
        const VPGxmUniform *u = &req->stage_cbuf[bank_list[i][0]][bank_list[i][1]];
        gxm_vtx_dump_u32(f, bank_list[i][0]);
        gxm_vtx_dump_u32(f, bank_list[i][1]);
        gxm_vtx_dump_u32(f, u->data != NULL ? u->size : 0u);
        if (u->data != NULL && u->size != 0u) fwrite(u->data, 1u, u->size, f);
    }
    fwrite(req->vertices, 1u, vbytes, f);
    if (ibytes != 0u) fwrite(req->indices, 1u, ibytes, f);
    fclose(f);
    ++gxm_vtx_dump_count;
    glog("VP_GXM_VTX_DUMP slot=%u idx=%u hash=0x%x vs=0x%x fs=0x%x ms=%llu "
         "present=%llu count=%u stride=%u indexed=%u icount=%u ifmt=%u "
         "base=%d iwf=%u target=0x%llx %ux%u vp=%u,%u,%u,%u attrs=%s "
         "vbytes=%llu ibytes=%llu path=%s\n",
         gxm_vtx_dump_count, req->draw_index, req->hash, req->vs_token,
         req->fs_token, (unsigned long long)now,
         (unsigned long long)gxm_perf_present_frames, req->count, req->stride,
         req->indexed, req->index_count, req->index_format, req->base_vertex,
         req->index_window_first, (unsigned long long)req->target_va,
         req->target_width, req->target_height, req->viewport_x,
         req->viewport_y, req->viewport_width, req->viewport_height, attrs,
         (unsigned long long)vbytes, (unsigned long long)ibytes, path);
}

static void gxm_draw_trace(const VPGxmDrawRequest *req) {
    uint32_t u3x = 0u, u5_1 = 0u, u8_10 = 0u;
    int h3x, h5_1, h8_10;
    unsigned i;
    if (req == NULL || !gxm_draw_trace_enabled()) return;
    if (gxm_draw_trace_lines >= VP_GXM_DRAW_TRACE_LIMIT) return;
    gxm_draw_trace_lines++;
    h3x = gxm_draw_trace_u32(&req->stage_cbuf[1][3], 0u, &u3x);
    h5_1 = gxm_draw_trace_u32(&req->stage_cbuf[1][5], 4u, &u5_1);
    h8_10 = gxm_draw_trace_u32(&req->stage_cbuf[1][8], 40u, &u8_10);
    glog("VP_GXM_DRAW_TRACE present=%llu ms=%llu idx=%u hash=0x%x fs=0x%x "
         "count=%u icount=%u indexed=%u target=0x%llx %ux%u "
         "vp=%u,%u,%u,%u blend=%u/%u/%u,%u/%u/%u mask=0x%x alpha=%u/%u/%g "
         "ztest=%u/%u/%u cull=%u/%u/%u "
         "u3x=%08x u5_1=%08x u8_10=%08x "
         "t0=0x%llx,%u,%u,%u,%u t1=0x%llx,%u,%u,%u,%u "
         "t2=0x%llx,%u,%u,%u,%u t3=0x%llx,%u,%u,%u,%u\n",
         (unsigned long long)gxm_perf_present_frames,
         (unsigned long long)(sceKernelGetProcessTimeWide() / 1000u),
         req->draw_index, req->hash, req->fs_token, req->count,
         req->indexed ? req->index_count : 0u, req->indexed,
         (unsigned long long)req->target_va, req->target_width,
         req->target_height, req->viewport_x, req->viewport_y,
         req->viewport_width, req->viewport_height,
         req->blend_color_op, req->blend_color_src, req->blend_color_dst,
         req->blend_alpha_op, req->blend_alpha_src, req->blend_alpha_dst,
         req->color_mask, req->alpha_test_enable, req->alpha_test_func,
         (double)req->alpha_test_ref, req->depth_test, req->depth_write,
         req->depth_func, req->cull_enable, req->cull_face, req->front_face,
         h3x ? u3x : 0u, h5_1 ? u5_1 : 0u, h8_10 ? u8_10 : 0u,
         (unsigned long long)req->texture_va, req->tex_width, req->tex_height,
         req->tex_tic_format, req->texture_source,
         (unsigned long long)req->extra_textures[0].va,
         req->extra_textures[0].width, req->extra_textures[0].height,
         req->extra_textures[0].tic_format, req->extra_textures[0].source,
         (unsigned long long)req->extra_textures[1].va,
         req->extra_textures[1].width, req->extra_textures[1].height,
         req->extra_textures[1].tic_format, req->extra_textures[1].source,
         (unsigned long long)req->extra_textures[2].va,
         req->extra_textures[2].width, req->extra_textures[2].height,
         req->extra_textures[2].tic_format, req->extra_textures[2].source);
    if ((sceKernelGetProcessTimeWide() / 1000u) >=
        VP_GXM_DRAW_TRACE_TEX_MS_MIN) {
        gxm_draw_trace_dump_tex("t0", req->texels, req->texture_va,
                                req->tex_width, req->tex_height,
                                req->tex_format, 0u);
        for (i = 0; i < VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u; i++) {
            const VPGxmExtraTexture *extra = &req->extra_textures[i];
            char name[8];
            snprintf(name, sizeof(name), "t%u", i + 1u);
            gxm_draw_trace_dump_tex(name, extra->texels, extra->va,
                                    extra->width, extra->height,
                                    extra->tex_format, extra->cube);
        }
    }
}

/* Aggregate GPU completion cost of every successfully flushed batch scene.
 * The per-flush log is sampled; these totals cover the whole run. */
static struct {
    uint64_t flushes, end_sum, end_max, notify_sum, notify_max;
    uint64_t finish_sum, finish_max, total_sum, total_max;
} gxm_batch_perf;
static struct {
    uint64_t draws, prep_us, begin_us, encode_us, draw_us;
    uint64_t setup_us, vertex_us, index_us, texture_us, nested_flush_us;
    /* Both are subsets of this draw's preparation wall time and are mutually
     * disjoint: nested_flush_us is the batch completion path (EndScene API,
     * notification and Finish) of flushes performed inside the draw, while
     * nested_pending_us is blocking on a previously submitted scene before a
     * flush. nested_notify_us/nested_finish_us are the actual SDK-call blocks
     * contained in those two fields, never additional time. */
    uint64_t nested_pending_us, nested_notify_us, nested_finish_us;
    /* B90 split of setup_us: validation/traces, pair+eligibility, feedback,
     * batch admission (reserve/uniform/snapshot), target/depth resolution. */
    uint64_t validate_us, pair_us, feedback_us, admit_us, resolve_us;
    uint64_t pair_flush_us, admit_flush_us;
    uint64_t prj_us, reserve_us, uniform_us, policy_us;
} gxm_batch_encode_perf[2];

/* Capture these existing successful-batch counters at exactly the same
 * endpoints as the nested profile. They are a second, nested view, never
 * additional frame costs. Failed/legacy draws remain in gxm-draw instead. */
static void gxm_frame_cost_details(uint64_t now, uint64_t frames, unsigned seq) {
    for (unsigned kind = 0; kind < 2u; ++kind) {
#define FC_VALUE(field) (unsigned long long)gxm_batch_encode_perf[kind].field
        glog("VP_FRAME_COST_GXM schema=%u seq=%u epoch=%u mode=1 t_us=%llu frames=%llu "
             "kind=%u draws=%llu prep_us=%llu begin_us=%llu encode_us=%llu draw_us=%llu "
             "setup_us=%llu vertex_us=%llu index_us=%llu texture_us=%llu "
             "nested_flush_us=%llu nested_pending_us=%llu nested_notify_us=%llu "
             "nested_finish_us=%llu validate_us=%llu pair_us=%llu feedback_us=%llu "
             "admit_us=%llu resolve_us=%llu pair_flush_us=%llu admit_flush_us=%llu "
             "prj_us=%llu reserve_us=%llu uniform_us=%llu policy_us=%llu\n",
             VP_FRAME_COST_SCHEMA, seq, gxm_frame_cost_epoch, (unsigned long long)now, (unsigned long long)frames,
             kind, FC_VALUE(draws), FC_VALUE(prep_us), FC_VALUE(begin_us), FC_VALUE(encode_us),
             FC_VALUE(draw_us), FC_VALUE(setup_us), FC_VALUE(vertex_us), FC_VALUE(index_us),
             FC_VALUE(texture_us), FC_VALUE(nested_flush_us), FC_VALUE(nested_pending_us),
             FC_VALUE(nested_notify_us), FC_VALUE(nested_finish_us), FC_VALUE(validate_us),
             FC_VALUE(pair_us), FC_VALUE(feedback_us), FC_VALUE(admit_us), FC_VALUE(resolve_us),
             FC_VALUE(pair_flush_us), FC_VALUE(admit_flush_us), FC_VALUE(prj_us),
             FC_VALUE(reserve_us), FC_VALUE(uniform_us), FC_VALUE(policy_us));
#undef FC_VALUE
    }
    VPGuestFramePerf guest;
    vp_guest_frame_perf(&guest);
    glog("VP_FRAME_COST_TEXTURE schema=%u seq=%u epoch=%u mode=1 t_us=%llu frames=%llu "
         "requests=%llu hits=%llu misses=%llu rgba8_us=%llu fp16_us=%llu blocks_us=%llu "
         "cube_us=%llu other_us=%llu rgba8_bytes=%llu fp16_bytes=%llu blocks_bytes=%llu "
         "cube_bytes=%llu other_bytes=%llu miss_changed=%llu miss_absent=%llu "
         "miss_gpu_only=%llu compare_bytes=%llu compare_calls=%llu\n", VP_FRAME_COST_SCHEMA, seq, gxm_frame_cost_epoch,
         (unsigned long long)now, (unsigned long long)frames,
         (unsigned long long)guest.tex_requests, (unsigned long long)guest.tex_hits,
         (unsigned long long)guest.tex_misses,
         (unsigned long long)guest.tex_family_us[0], (unsigned long long)guest.tex_family_us[1],
         (unsigned long long)guest.tex_family_us[2], (unsigned long long)guest.tex_family_us[3],
         (unsigned long long)guest.tex_family_us[4], (unsigned long long)guest.tex_family_bytes[0],
         (unsigned long long)guest.tex_family_bytes[1], (unsigned long long)guest.tex_family_bytes[2],
         (unsigned long long)guest.tex_family_bytes[3], (unsigned long long)guest.tex_family_bytes[4],
         (unsigned long long)guest.tex_miss_changed, (unsigned long long)guest.tex_miss_absent,
         (unsigned long long)guest.tex_miss_gpu_only, (unsigned long long)guest.tex_compare_bytes,
         (unsigned long long)guest.tex_compare_calls);
}

/* Scene-class completion census (B86 exploration). A scene is one EndScene;
 * the class decides which attachment family paid for its GPU completion. The
 * synchronous path measures start/notification/Finish exactly; the deferred
 * pipeline adds its wait at the retire point. Diagnostic accounting only. */
enum {
    VP_SCENE_CLASS_SHADOW = 0,
    VP_SCENE_CLASS_MAIN,
    VP_SCENE_CLASS_POST,
    VP_SCENE_CLASS_OTHER,
    VP_SCENE_CLASS_COUNT
};
static const char *const gxm_scene_class_names[VP_SCENE_CLASS_COUNT] = {
    "shadow-depth", "main-color", "post-color", "other"
};
static struct {
    uint64_t flushes, draws, notify_us, finish_us, total_us, total_max;
} gxm_scene_classes[VP_SCENE_CLASS_COUNT];
static uint64_t gxm_draws_blended, gxm_draws_opaque;

static unsigned gxm_scene_class_of(unsigned width, unsigned height,
                                   int depth_only) {
    if (depth_only)
        return height >= 1024u ? VP_SCENE_CLASS_SHADOW : VP_SCENE_CLASS_OTHER;
    if (width >= 640u) return VP_SCENE_CLASS_MAIN;
    if (width != 0u && width <= 320u) return VP_SCENE_CLASS_POST;
    return VP_SCENE_CLASS_OTHER;
}

/* Draws refused batch membership before the shared scene opens: face state,
 * alpha test, or an unsupported sampler shape. */
static unsigned gxm_batch_pre_reject[4];
static unsigned gxm_batch_pre_reject_why;

/* Why a draw could not join the open scene. One primary cause per refused
 * draw, evaluated in the same order as the eligibility gate, counted only
 * when a scene is actually open (so the sum tracks the ineligible flushes). */
enum {
    VP_GXM_BATCH_INELIG_DISABLED = 0, /* batch off, dynamic pair, aux marker */
    VP_GXM_BATCH_INELIG_QUIET,        /* guest request not quiet / trace */
    VP_GXM_BATCH_INELIG_DEPTH_VIEW,   /* depth sampler needs materialization */
    VP_GXM_BATCH_INELIG_HAZARD,       /* samples a producer in this scene */
    VP_GXM_BATCH_INELIG_FEEDBACK,     /* primary sampler aliases the target */
    VP_GXM_BATCH_INELIG_COUNT
};
static uint64_t gxm_batch_inelig[VP_GXM_BATCH_INELIG_COUNT];
/* Budget flushes split by the binding constraint: uniform-ring share, one draw
 * larger than the staging arena, or the staging bytes already used. */
static uint64_t gxm_batch_budget[3];

/* Guest submit roundtrip: signal + wait on the worker, excluding the worker's
 * own draw processing. This is the per-draw queue cost the guest pays. */
static struct {
    uint64_t submits, notify_sum, notify_max, wait_sum, wait_max;
    uint64_t direct_draws, stream_entries;
} gxm_api_perf;

/* Scene users pin immutable uploads until GPU completion. The 32 MiB capacity
 * is an opt-in candidate selected at first use; production keeps 16 MiB. */
#define VP_UPLOAD_CACHE_BYTES (16u * 1024u * 1024u)
#define VP_UPLOAD_CACHE_MAX_BYTES (32u * 1024u * 1024u)
#define VP_UPLOAD_PAGE_BYTES (64u * 1024u)
#define VP_UPLOAD_CACHE_SLOTS 80u
typedef struct {
    VPTextureSnapshot snapshot;
    unsigned first_page, pages, stamp, pinned;
    /* Number of scenes whose GPU work may still read this page. Only the
     * pipeline defers completion; every decrement is paired with a recorded
     * scene pin list. */
    unsigned pending;
} VPGxmUpload;
static VPGxmUpload gxm_uploads[VP_UPLOAD_CACHE_SLOTS];
static unsigned char
    gxm_upload_pages[VP_UPLOAD_CACHE_MAX_BYTES / VP_UPLOAD_PAGE_BYTES];
static unsigned char *gxm_upload_memory;
static unsigned gxm_upload_capacity = VP_UPLOAD_CACHE_BYTES;
static int gxm_upload_capacity_state = -1;
static unsigned gxm_upload_stamp;
static uint64_t gxm_upload_hits, gxm_upload_copies, gxm_upload_saved_bytes;
static uint64_t gxm_upload_fill_bytes;
/* Owned-source association probes that resolved without a byte comparison.
 * Each lookup site is counted; these are not unique payloads and must not be
 * compared with the exact-compare hit counters. */
static uint64_t gxm_upload_owned_hits, gxm_upload_owned_bytes;
static uint64_t gxm_snapshot_owned_hits, gxm_snapshot_owned_bytes;
static uint64_t gxm_owned_retirements, gxm_owned_retired_assocs;
/* B115 experiment: key+size hits that skipped the exact byte comparison. */
static uint64_t gxm_upload_skip_hits, gxm_snapshot_skip_hits;
static uint64_t gxm_strip_draws, gxm_strip_segments;

/* Result of the admission-time snapshot probe, reused by the same draw's copy
 * so an equal-content snapshot or upload-cache hit is not searched twice.
 * Only a source explicitly marked as a VPTextureCache-owned decode may use this
 * memo; its payload cannot change between admission and encode and the decoder
 * retires matching memo entries before any rewrite/free. The upload field names
 * the entry gxm_upload_prepare already pinned and stamped at admission, so the
 * copy reuses it and preserves the original LRU update. The memo is also
 * cleared at every reserve and scene reset. */
#define VP_SNAPSHOT_MEMO_MAX 8u
typedef struct {
    VPTextureSnapshotKey key;
    const void *source;
    size_t bytes;
    const void *owned;
    VPGxmUpload *upload;
} VPGxmSnapshotMemo;
static VPGxmSnapshotMemo gxm_snapshot_memo[VP_SNAPSHOT_MEMO_MAX];
static unsigned gxm_snapshot_memo_count;
/* Copy sites whose admission upload entry was reused without a second
 * linear-slot search. Diagnostic only. */
static uint64_t gxm_upload_memo_hits;

/* Guest->owned staging copies: vertex/index bytes and CPU texture bytes. */
static uint64_t gxm_xfer_bytes, gxm_xfer_us;
static uint64_t gxm_tex_bytes, gxm_tex_us;
static uint64_t gxm_snapshot_hits, gxm_snapshot_saved_bytes;
/* B106 index preparation and CPU descriptor-cache evidence. */
static uint64_t gxm_index_norestart, gxm_index_remap_copy, gxm_index_ramp_copy;
static uint64_t gxm_index_ramp_bind;
static uint64_t gxm_texture_desc_hits, gxm_texture_desc_misses;

static unsigned gxm_perf_bucket(uint64_t us) {
    for (unsigned i = 0; i < VP_GXM_PERF_BUCKETS - 1u; i++)
        if (us < gxm_perf_bounds[i]) return i;
    return VP_GXM_PERF_BUCKETS - 1u;
}

static void gxm_perf_add(VPGxmPerfMetric *m, uint64_t us) {
    m->count++;
    m->sum += us;
    if (us > m->max) m->max = us;
    m->bucket[gxm_perf_bucket(us)]++;
}

static unsigned gxm_perf_quantile_bucket(const VPGxmPerfMetric *m,
                                         unsigned percent) {
    uint64_t need, seen = 0;
    if (m->count == 0u) return 0u;
    need = (m->count * percent + 99u) / 100u;
    for (unsigned i = 0; i < VP_GXM_PERF_BUCKETS; i++) {
        seen += m->bucket[i];
        if (seen >= need) return i;
    }
    return VP_GXM_PERF_BUCKETS - 1u;
}

static void gxm_perf_log_bounds(void) {
    if (gxm_perf_bounds_logged) return;
    gxm_perf_bounds_logged = 1u;
    glog("VP_GXM_PERF_BOUNDS buckets=%u b0_lt_us=50 b1_lt_us=100 "
         "b2_lt_us=250 b3_lt_us=500 b4_lt_us=1000 b5_lt_us=2500 "
         "b6_lt_us=5000 b7_lt_us=10000 b8_lt_us=25000 b9_lt_us=50000 "
         "b10_lt_us=100000 b11_lt_us=250000 b12_lt_us=500000 "
         "b13_lt_us=1000000 b14_lt_us=5000000 b15_ge_us=5000000\n",
         VP_GXM_PERF_BUCKETS);
}

static void gxm_perf_window_flush(unsigned kind, uint64_t now, int force) {
    VPGxmPerfKind *k = &gxm_perf_kinds[kind & 1u];
    unsigned samples = k->draws + k->failures + k->skips +
                       (unsigned)k->worker.count + (unsigned)k->staged.count;
    if (samples == 0u) return;
    if (!force && samples < VP_GXM_PERF_WINDOW_DRAWS &&
        now - k->window_start < VP_GXM_PERF_WINDOW_US)
        return;
    gxm_perf_log_bounds();
    glog("VP_GXM_PERF_WINDOW kind=%s draws=%u failures=%u skips=%u "
         "t_us=%llu window_us=%llu presents=%llu "
         "g_ok=%llu g_fail=%llu g_skip=%llu "
         "prep_sum=%llu prep_max=%llu begin_sum=%llu begin_max=%llu "
         "encode_sum=%llu encode_max=%llu draw_sum=%llu draw_max=%llu "
         "end_sum=%llu end_max=%llu notify_sum=%llu notify_max=%llu "
         "finish_sum=%llu finish_max=%llu total_sum=%llu total_max=%llu "
         "worker_sum=%llu worker_max=%llu fail_sum=%llu fail_max=%llu "
         "staged_n=%llu staged_sum=%llu staged_max=%llu "
         "p50_bucket=%u p90_bucket=%u p99_bucket=%u "
         "total_b=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
         "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu "
         "fail_b=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
         "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
         kind ? "depth-only" : "colour", k->draws, k->failures, k->skips,
         (unsigned long long)now,
         (unsigned long long)(now - k->window_start),
         (unsigned long long)gxm_perf_present_frames,
         (unsigned long long)gxm_perf_global_ok,
         (unsigned long long)gxm_perf_global_fail,
         (unsigned long long)gxm_perf_global_skip,
         (unsigned long long)k->prep.sum, (unsigned long long)k->prep.max,
         (unsigned long long)k->begin.sum, (unsigned long long)k->begin.max,
         (unsigned long long)k->encode.sum, (unsigned long long)k->encode.max,
         (unsigned long long)k->draw.sum, (unsigned long long)k->draw.max,
         (unsigned long long)k->end_scene.sum,
         (unsigned long long)k->end_scene.max,
         (unsigned long long)k->notify.sum, (unsigned long long)k->notify.max,
         (unsigned long long)k->finish.sum, (unsigned long long)k->finish.max,
         (unsigned long long)k->total.sum, (unsigned long long)k->total.max,
         (unsigned long long)k->worker.sum, (unsigned long long)k->worker.max,
         (unsigned long long)k->fail.sum, (unsigned long long)k->fail.max,
         (unsigned long long)k->staged.count,
         (unsigned long long)k->staged.sum,
         (unsigned long long)k->staged.max,
         gxm_perf_quantile_bucket(&k->total, 50u),
         gxm_perf_quantile_bucket(&k->total, 90u),
         gxm_perf_quantile_bucket(&k->total, 99u),
         (unsigned long long)k->total.bucket[0],
         (unsigned long long)k->total.bucket[1],
         (unsigned long long)k->total.bucket[2],
         (unsigned long long)k->total.bucket[3],
         (unsigned long long)k->total.bucket[4],
         (unsigned long long)k->total.bucket[5],
         (unsigned long long)k->total.bucket[6],
         (unsigned long long)k->total.bucket[7],
         (unsigned long long)k->total.bucket[8],
         (unsigned long long)k->total.bucket[9],
         (unsigned long long)k->total.bucket[10],
         (unsigned long long)k->total.bucket[11],
         (unsigned long long)k->total.bucket[12],
         (unsigned long long)k->total.bucket[13],
         (unsigned long long)k->total.bucket[14],
         (unsigned long long)k->total.bucket[15],
         (unsigned long long)k->fail.bucket[0],
         (unsigned long long)k->fail.bucket[1],
         (unsigned long long)k->fail.bucket[2],
         (unsigned long long)k->fail.bucket[3],
         (unsigned long long)k->fail.bucket[4],
         (unsigned long long)k->fail.bucket[5],
         (unsigned long long)k->fail.bucket[6],
         (unsigned long long)k->fail.bucket[7],
         (unsigned long long)k->fail.bucket[8],
         (unsigned long long)k->fail.bucket[9],
         (unsigned long long)k->fail.bucket[10],
         (unsigned long long)k->fail.bucket[11],
         (unsigned long long)k->fail.bucket[12],
         (unsigned long long)k->fail.bucket[13],
         (unsigned long long)k->fail.bucket[14],
         (unsigned long long)k->fail.bucket[15]);
    memset(&k->prep, 0, sizeof(k->prep));
    memset(&k->begin, 0, sizeof(k->begin));
    memset(&k->encode, 0, sizeof(k->encode));
    memset(&k->draw, 0, sizeof(k->draw));
    memset(&k->end_scene, 0, sizeof(k->end_scene));
    memset(&k->notify, 0, sizeof(k->notify));
    memset(&k->finish, 0, sizeof(k->finish));
    memset(&k->total, 0, sizeof(k->total));
    memset(&k->worker, 0, sizeof(k->worker));
    memset(&k->fail, 0, sizeof(k->fail));
    memset(&k->staged, 0, sizeof(k->staged));
    k->draws = 0u;
    k->failures = 0u;
    k->skips = 0u;
    k->window_start = now;
}

static void gxm_perf_present(uint64_t fn_us, uint64_t now) {
    uint64_t interval = 0u;
    gxm_perf_present_frames++;
    gxm_perf_present_sum += fn_us;
    if (fn_us > gxm_perf_present_max) gxm_perf_present_max = fn_us;
    gxm_perf_present_buckets[gxm_perf_bucket(fn_us)]++;
    if (gxm_perf_present_last_at != 0u) {
        interval = now - gxm_perf_present_last_at;
        gxm_perf_present_interval_sum += interval;
        gxm_perf_present_interval_count++;
        unsigned bucket=interval/1000u<511u ? (unsigned)(interval/1000u):511u;
        gxm_perf_frame_intervals[bucket]++;
        if (interval > gxm_perf_present_interval_max)
            gxm_perf_present_interval_max = interval;
    }
    gxm_perf_present_last_at = now;
    if (gxm_perf_present_frames <= 4u ||
        (gxm_perf_present_frames % 32u) == 0u)
        glog("VP_GXM_PERF_PRESENT frames=%llu t_us=%llu present_fn_us=%llu "
             "present_fn_avg_us=%llu present_fn_max_us=%llu interval_us=%llu "
             "interval_avg_us=%llu interval_max_us=%llu "
             "g_ok=%llu g_fail=%llu g_skip=%llu\n",
             (unsigned long long)gxm_perf_present_frames,
             (unsigned long long)now,
             (unsigned long long)fn_us,
             (unsigned long long)(gxm_perf_present_sum /
                                  gxm_perf_present_frames),
             (unsigned long long)gxm_perf_present_max,
             (unsigned long long)interval,
             (unsigned long long)(gxm_perf_present_interval_count == 0u ? 0u :
                 gxm_perf_present_interval_sum /
                 gxm_perf_present_interval_count),
             (unsigned long long)gxm_perf_present_interval_max,
             (unsigned long long)gxm_perf_global_ok,
             (unsigned long long)gxm_perf_global_fail,
             (unsigned long long)gxm_perf_global_skip);
    if ((gxm_perf_present_frames % 32u) == 0u) {
        gxm_wait_profile_log(now, gxm_perf_present_frames);
        /* Frame-cost snapshots use this exact existing reporting point. The
         * profile must be closed here; otherwise the snapshot is refused. */
        gxm_frame_cost_log_snapshot(now, gxm_perf_present_frames, "periodic");
        gxm_texture_memory_log(now, gxm_perf_present_frames);
        glog("VP_GXM_FRAME_INTERVALS t_us=%llu count=%llu sum_us=%llu width_us=1000\n",
            (unsigned long long)now,(unsigned long long)gxm_perf_present_interval_count,
            (unsigned long long)gxm_perf_present_interval_sum);
        /* Keep every line below the bounded log sink's formatting limit. */
        for (unsigned base=0u;base<512u;base+=64u) {
            char histogram[768];
            unsigned used=0u;
            for (unsigned i=0;i<64u;i++)
                used+=(unsigned)snprintf(histogram+used,sizeof(histogram)-used,"%s%u",
                    i?",":"",gxm_perf_frame_intervals[base+i]);
            glog("VP_GXM_FRAME_BUCKETS t_us=%llu offset=%u hist=%s\n",
                (unsigned long long)now,base,histogram);
        }
    }
    if ((gxm_perf_present_frames % 16u) == 0u) {
        /* Whole-frame budget from existing cumulative counters.
         *
         * Nesting (larger contains smaller, never add them):
         *   wall = decode_incl + present + residual (residual is unassigned
         *          wall time, not an independently measured guest execution)
         *   decode_incl contains draw_prepare_incl, tex_decode/lookup and the
         *          B112 entry phases entry_setup + command_copy +
         *          command_collect + command_dispatch + submission_finish
         *   command_dispatch_incl contains draw/GXM work (and therefore
         *          draw_prepare_incl)
         *   draw_prepare_incl contains gxm_call_incl (inline worker request)
         *   gxm_call_incl contains gxm_prep_net + nested batch completion
         *   since B104 the inline path keeps the session lock, so only
         *          gxm_core advances; device_release/device_reacquire stay zero
         *          there and remain external-handoff counters
         *   segment_acquire/segment_release are the single worker-entry/exit
         *          session lock handoffs, outside decode_incl, so they are
         *          part of residual rather than of decode_incl
         *   B112 outer wall: gate_wait (real mutex block), worker_queue
         *          (enqueue to callback entry), worker_postwake (callback end
         *          to submit return) and caller_reacquire (post-worker Session
         *          lock) are outside decode_incl. They are disjoint segments of
         *          one worker handoff but are nested observations, not a
         *          mutually exclusive closure of residual. external_* cover
         *          the !on_worker present/flush path; external_core overlaps
         *          the worker-side present_us for a present request, so never
         *          add those two.
         *   tex_decode_us excludes the miss-only swizzle + raw copy; the
         *          separately timed tex_update_tail_us is added only as its
         *          own diagnostic field. Both sit inside draw_prepare_incl.
         *   nested_flush_us is a subset of batch_complete_us; adding them
         *          would count each synchronous flush twice
         *   notify_block and finish_block are disjoint SDK waits and are
         *          subsets of the same calls, never extra GPU time
         * With the scene pipeline a deferred flush contributes only its
         * EndScene API time to batch_complete_us; its later wait appears in
         * notify_block. All timestamps are play-perf gated; when disabled the
         * cumulative counters stay zero and every delta below is zero. */
        static struct {
            uint64_t at, decode, prepare, tex_decode, tex_lookup, gxm_call;
            uint64_t release, core, reacquire, seg_acquire, seg_release;
            uint64_t gate_wait, worker_queue, worker_postwake, caller_reacquire;
            uint64_t external_release, external_core, external_reacquire;
            uint64_t entry_setup, command_copy, command_collect;
            uint64_t command_dispatch, submission_finish;
            uint64_t command_words, decoded_writes, tex_tail;
            uint64_t tex_requests, tex_hits, tex_misses;
            uint64_t gxm_prep, nested_flush, nested_pending, complete, endscene;
            uint64_t notify_block, finish_block, notify_calls, finish_calls;
            uint64_t present, frames;
        } budget;
        VPGuestFramePerf gp;
        uint64_t enc_cum = 0u, enc = 0u, comp = 0u, wall = 0u;
        uint64_t decode = 0u, prepare = 0u, texd = 0u, texl = 0u;
        uint64_t call = 0u, pres = 0u, nested_cum = 0u, nested = 0u;
        uint64_t release = 0u, core = 0u, reacquire = 0u;
        uint64_t seg_acq = 0u, seg_rel = 0u;
        uint64_t pending_cum = 0u, pending = 0u;
        uint64_t endapi = 0u, notify_block = 0u, finish_block = 0u;
        uint64_t gate_wait = 0u, wqueue = 0u, wpost = 0u, creacq = 0u;
        uint64_t ext_rel = 0u, ext_core = 0u, ext_reacq = 0u;
        uint64_t setup = 0u, copy = 0u, collect = 0u, dispatch = 0u, finish = 0u;
        uint64_t cmd_words = 0u, dec_writes = 0u, tex_tail = 0u;
        unsigned kind;
        vp_guest_frame_perf(&gp);
        for (kind = 0u; kind < 2u; ++kind) {
            enc_cum += gxm_batch_encode_perf[kind].prep_us -
                       gxm_batch_encode_perf[kind].nested_flush_us -
                       gxm_batch_encode_perf[kind].nested_pending_us +
                       gxm_batch_encode_perf[kind].encode_us +
                       gxm_batch_encode_perf[kind].draw_us;
            nested_cum += gxm_batch_encode_perf[kind].nested_flush_us;
            pending_cum += gxm_batch_encode_perf[kind].nested_pending_us;
        }
        if (budget.at != 0u) {
            uint64_t residual;
            wall = now - budget.at;
            decode = gp.decode_us - budget.decode;
            prepare = gp.draw_prepare_us - budget.prepare;
            texd = gp.tex_decode_us - budget.tex_decode;
            texl = gp.tex_lookup_us - budget.tex_lookup;
            call = gp.gxm_call_us - budget.gxm_call;
            release = gp.device_release_us - budget.release;
            core = gp.gxm_request_core_us - budget.core;
            reacquire = gp.device_reacquire_us - budget.reacquire;
            seg_acq = gp.segment_acquire_us - budget.seg_acquire;
            seg_rel = gp.segment_release_us - budget.seg_release;
            gate_wait = gp.gate_wait_us - budget.gate_wait;
            wqueue = gp.worker_queue_us - budget.worker_queue;
            wpost = gp.worker_postwake_us - budget.worker_postwake;
            creacq = gp.caller_reacquire_us - budget.caller_reacquire;
            ext_rel = gp.external_release_us - budget.external_release;
            ext_core = gp.external_core_us - budget.external_core;
            ext_reacq = gp.external_reacquire_us - budget.external_reacquire;
            setup = gp.entry_setup_us - budget.entry_setup;
            copy = gp.command_copy_us - budget.command_copy;
            collect = gp.command_collect_us - budget.command_collect;
            dispatch = gp.command_dispatch_us - budget.command_dispatch;
            finish = gp.submission_finish_us - budget.submission_finish;
            cmd_words = gp.command_words - budget.command_words;
            dec_writes = gp.decoded_writes - budget.decoded_writes;
            tex_tail = gp.tex_update_tail_us - budget.tex_tail;
            enc = enc_cum - budget.gxm_prep;
            nested = nested_cum - budget.nested_flush;
            pending = pending_cum - budget.nested_pending;
            comp = gxm_batch_perf.total_sum - budget.complete;
            endapi = gxm_endscene_us - budget.endscene;
            notify_block = gxm_notify_wait_us - budget.notify_block;
            finish_block = gxm_finish_us - budget.finish_block;
            pres = gxm_perf_present_sum - budget.present;
            residual = wall > decode + pres ? wall - decode - pres : 0u;
            glog("VP_FRAME_BUDGET t_us=%llu frames=%llu wall_us=%llu "
                 "residual_us=%llu "
                 "decode_incl_us=%llu draw_prepare_incl_us=%llu "
                 "tex_decode_us=%llu tex_lookup_us=%llu tex_update_tail_us=%llu "
                 "gxm_call_incl_us=%llu gxm_prep_net_us=%llu "
                 "device_release_us=%llu gxm_request_core_us=%llu "
                 "device_reacquire_us=%llu "
                 "segment_acquire_us=%llu segment_release_us=%llu "
                 "entry_setup_us=%llu command_copy_us=%llu "
                 "command_collect_us=%llu command_dispatch_us=%llu "
                 "submission_finish_us=%llu command_words=%llu "
                 "decoded_writes=%llu gate_wait_us=%llu worker_queue_us=%llu "
                 "worker_postwake_us=%llu caller_reacquire_us=%llu "
                 "external_release_us=%llu external_core_us=%llu "
                 "external_reacquire_us=%llu "
                 "nested_flush_us=%llu nested_pending_complete_us=%llu "
                 "batch_complete_us=%llu endscene_api_us=%llu "
                 "notify_block_us=%llu finish_block_us=%llu present_us=%llu "
                 "tex_requests=%llu tex_hits=%llu tex_misses=%llu "
                 "notify_calls=%llu finish_calls=%llu\n",
                 (unsigned long long)now,
                 (unsigned long long)(gxm_perf_present_frames - budget.frames),
                 (unsigned long long)wall, (unsigned long long)residual,
                 (unsigned long long)decode, (unsigned long long)prepare,
                 (unsigned long long)texd, (unsigned long long)texl,
                 (unsigned long long)tex_tail,
                 (unsigned long long)call, (unsigned long long)enc,
                 (unsigned long long)release, (unsigned long long)core,
                 (unsigned long long)reacquire,
                 (unsigned long long)seg_acq, (unsigned long long)seg_rel,
                 (unsigned long long)setup, (unsigned long long)copy,
                 (unsigned long long)collect, (unsigned long long)dispatch,
                 (unsigned long long)finish, (unsigned long long)cmd_words,
                 (unsigned long long)dec_writes,
                 (unsigned long long)gate_wait, (unsigned long long)wqueue,
                 (unsigned long long)wpost, (unsigned long long)creacq,
                 (unsigned long long)ext_rel, (unsigned long long)ext_core,
                 (unsigned long long)ext_reacq,
                 (unsigned long long)nested, (unsigned long long)pending,
                 (unsigned long long)comp, (unsigned long long)endapi,
                 (unsigned long long)notify_block,
                 (unsigned long long)finish_block,
                 (unsigned long long)pres,
                 (unsigned long long)(gp.tex_requests - budget.tex_requests),
                 (unsigned long long)(gp.tex_hits - budget.tex_hits),
                 (unsigned long long)(gp.tex_misses - budget.tex_misses),
                 (unsigned long long)(gxm_notify_wait_calls - budget.notify_calls),
                 (unsigned long long)(gxm_finish_calls - budget.finish_calls));
        }
        budget.at = now;
        budget.decode = gp.decode_us;
        budget.prepare = gp.draw_prepare_us;
        budget.tex_decode = gp.tex_decode_us;
        budget.tex_lookup = gp.tex_lookup_us;
        budget.gxm_call = gp.gxm_call_us;
        budget.release = gp.device_release_us;
        budget.core = gp.gxm_request_core_us;
        budget.reacquire = gp.device_reacquire_us;
        budget.seg_acquire = gp.segment_acquire_us;
        budget.seg_release = gp.segment_release_us;
        budget.gate_wait = gp.gate_wait_us;
        budget.worker_queue = gp.worker_queue_us;
        budget.worker_postwake = gp.worker_postwake_us;
        budget.caller_reacquire = gp.caller_reacquire_us;
        budget.external_release = gp.external_release_us;
        budget.external_core = gp.external_core_us;
        budget.external_reacquire = gp.external_reacquire_us;
        budget.entry_setup = gp.entry_setup_us;
        budget.command_copy = gp.command_copy_us;
        budget.command_collect = gp.command_collect_us;
        budget.command_dispatch = gp.command_dispatch_us;
        budget.submission_finish = gp.submission_finish_us;
        budget.command_words = gp.command_words;
        budget.decoded_writes = gp.decoded_writes;
        budget.tex_tail = gp.tex_update_tail_us;
        budget.tex_requests = gp.tex_requests;
        budget.tex_hits = gp.tex_hits;
        budget.tex_misses = gp.tex_misses;
        budget.gxm_prep = enc_cum;
        budget.nested_flush = nested_cum;
        budget.nested_pending = pending_cum;
        budget.complete = gxm_batch_perf.total_sum;
        budget.endscene = gxm_endscene_us;
        budget.notify_block = gxm_notify_wait_us;
        budget.finish_block = gxm_finish_us;
        budget.notify_calls = gxm_notify_wait_calls;
        budget.finish_calls = gxm_finish_calls;
        budget.present = gxm_perf_present_sum;
        budget.frames = gxm_perf_present_frames;
    }
}


static void depth_log_stats(void) {
    unsigned used = 0u, valid = 0u;
    if (!gxm_depth_pool_ready) return;
    for (unsigned i = 0; i < VP_GXM_DEPTH_POOL_SLOTS; i++) {
        if (!gxm_depth_pool.entries[i].used) continue;
        used++;
        if (gxm_depths[i].depth_valid) valid++;
    }
    glog("VP_GXM_DEPTH_SUMMARY resolve=%u reuse=%u create=%u recreate=%u "
         "disabled=%u ignored_state=%u masked_refuse=%u masked_noop=%u "
         "reject_meta=%u reject_target=%u reject_budget=%u reject_pool=%u "
         "reject_alloc=%u clear_depth=%u clear_stencil=%u "
         "clear_refuse_stencil=%u clear_coupled_stencil=%u "
         "clear_materialize=%u clear_materialize_fail=%u "
         "clear_materialize_us=%llu "
         "stencil_unsupported=%u slots=%u valid=%u "
         "bytes=%llu cap=%llu\n",
         gxm_depth_stats.resolve, gxm_depth_stats.reuse,
         gxm_depth_stats.create, gxm_depth_stats.recreate,
         gxm_depth_stats.disabled, gxm_depth_stats.ignored_state,
         gxm_depth_stats.masked_refuse, gxm_depth_stats.masked_noop,
         gxm_depth_stats.reject_meta, gxm_depth_stats.reject_target,
         gxm_depth_stats.reject_budget, gxm_depth_stats.reject_pool,
         gxm_depth_stats.reject_alloc, gxm_depth_stats.clear_depth,
         gxm_depth_stats.clear_stencil,
         gxm_depth_stats.clear_refuse_stencil,
         gxm_depth_stats.clear_coupled_stencil,
         gxm_depth_stats.clear_materialize,
         gxm_depth_stats.clear_materialize_fail,
         (unsigned long long)gxm_depth_stats.clear_materialize_us,
         gxm_depth_stats.stencil_unsupported, used, valid,
         (unsigned long long)gxm_depth_pool.bytes,
         (unsigned long long)gxm_depth_pool.byte_cap);
}

static int depth_pool_ensure(void) {
    int rc;
    if (gxm_depth_pool_ready) return 0;
    rc = vp_gxm_depth_pool_init(&gxm_depth_pool, VP_GXM_DEPTH_POOL_SLOTS,
                                VP_GXM_DEPTH_BYTE_CAP);
    if (rc != VP_GXM_DEPTH_OK) return -1;
    memset(gxm_depths, 0, sizeof(gxm_depths));
    if (!gxm_depth_disabled_ready) {
        rc = sceGxmDepthStencilSurfaceInitDisabled(&gxm_depth_disabled);
        if (rc < 0) {
            glog("VP_GXM_DEPTH_DISABLED_INIT rc=%d\n", rc);
            return -1;
        }
        gxm_depth_disabled_ready = 1;
    }
    gxm_depth_pool_ready = 1;
    return 0;
}

static void depth_entry_destroy(unsigned slot) {
    VPGxmDepthEntry *e;
    if (slot >= VP_GXM_DEPTH_POOL_SLOTS) return;
    e = &gxm_depths[slot];
    (void)gxm_pending_scene_complete("depth-destroy");
    gpu_free(e->depth_mem);
    gpu_free(e->stencil_mem);
    memset(e, 0, sizeof(*e));
}

static int depth_entry_create(unsigned slot, const VPGxmDepthDesc *desc) {    VPGxmDepthEntry *e;
    SceGxmDepthStencilFormat format;
    int rc;

    if (slot >= VP_GXM_DEPTH_POOL_SLOTS || desc == NULL) return -1;
    e = &gxm_depths[slot];
    if (e->used || e->depth_mem != NULL || e->stencil_mem != NULL) return -1;
    e->desc = *desc;
    rc = vp_gxm_depth_desc_build(&e->native_desc, 1u, desc->va,
        render_extent(desc->width, desc->height, desc->width),
        render_extent(desc->width, desc->height, desc->height),
        desc->raw_depth, desc->dim_control, desc->format,
        desc->tile_mode, desc->array_pitch);
    if (rc != VP_GXM_DEPTH_OK) return -1;
    desc = &e->native_desc;
    e->depth_mem = gpu_alloc_purpose((unsigned)desc->depth_bytes, 0, NULL, 1);
    if (e->depth_mem == NULL) return -1;
    if (desc->stencil_bytes != 0u) {
        e->stencil_mem = gpu_alloc_purpose((unsigned)desc->stencil_bytes, 0, NULL, 1);
        if (e->stencil_mem == NULL) {
            depth_entry_destroy(slot);
            return -1;
        }
    }
    format = desc->format_class == VP_GXM_DEPTH_CLASS_DF32 ?
        SCE_GXM_DEPTH_STENCIL_FORMAT_DF32 :
        SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24;
    rc = sceGxmDepthStencilSurfaceInit(&e->surface, format,
            SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, desc->stride_samples,
            e->depth_mem, e->stencil_mem);
    if (rc < 0) {
        glog("VP_GXM_DEPTH_CREATE_FAIL slot=%u rc=%d va=0x%llx %ux%u "
             "stride=%u storage_h=%u\n",
             slot, rc, (unsigned long long)desc->va, desc->width,
             desc->height, desc->stride_samples, desc->storage_height);
        depth_entry_destroy(slot);
        return rc;
    }
    sceGxmDepthStencilSurfaceSetForceStoreMode(&e->surface,
        SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
    sceGxmDepthStencilSurfaceSetForceLoadMode(&e->surface,
        SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
    sceGxmDepthStencilSurfaceSetBackgroundDepth(&e->surface, 1.0f);
    sceGxmDepthStencilSurfaceSetBackgroundStencil(&e->surface, 0u);
    if (gxm_depth_storage_exhausted ||
        gxm_depth_storage_epoch == 0xffffffffu) {
        if (!gxm_depth_storage_exhausted)
            glog("VP_GXM_DEPTH_STORAGE_GENERATION_EXHAUSTED "
                 "action=reject-new-depth-storage\n");
        gxm_depth_storage_exhausted = 1u;
        depth_entry_destroy(slot);
        return -1;
    }
    gxm_depth_storage_epoch++;
    e->storage_generation = gxm_depth_storage_epoch;
    e->used = 1;
    e->depth_valid = 0;
    e->stencil_live = 0;
    e->background_depth = 1.0f;
    e->background_stencil = 0u;
    e->clear_defined = 0u;
    e->content_generation = 0u;
    e->generation_exhausted = 0u;
    return 0;
}

static VPGxmDepthEntry *depth_entry_find(uint64_t va) {
    if (va == 0u) return NULL;
    for (unsigned i = 0; i < VP_GXM_DEPTH_POOL_SLOTS; ++i) {
        VPGxmDepthEntry *entry = &gxm_depths[i];
        if (entry->used && entry->depth_mem != NULL && entry->desc.va == va)
            return entry;
    }
    return NULL;
}

static int depth_desc_same_storage(const VPGxmDepthDesc *a,
                                   const VPGxmDepthDesc *b) {
    return a->va == b->va && a->width == b->width && a->height == b->height &&
           a->layers == b->layers && a->format == b->format &&
           a->stride_samples == b->stride_samples &&
           a->storage_height == b->storage_height;
}

/* Whether an S8D24 identity that would be reused by this descriptor may hold
 * guest-visible stencil content. A shape/format change recreates the storage
 * and explicitly discards old content, so it does not count as live. */
static unsigned depth_live_stencil_for(const VPGxmDepthDesc *desc) {
    unsigned i;
    if (!gxm_depth_pool_ready || desc == NULL ||
        desc->format_class != VP_GXM_DEPTH_CLASS_S8D24)
        return 0u;
    for (i = 0; i < VP_GXM_DEPTH_POOL_SLOTS; i++) {
        if (!gxm_depth_pool.entries[i].used || !gxm_depths[i].used)
            continue;
        if (depth_desc_same_storage(&gxm_depth_pool.entries[i].desc, desc))
            return gxm_depths[i].stencil_live;
    }
    return 0u;
}

static void depth_log_reject(const char *kind, int status, unsigned index,
                             uint64_t color_va, unsigned color_w,
                             unsigned color_h, unsigned depth_test,
                             unsigned depth_write,
                             const VPGxmGuestDepth *g) {
    unsigned n = gxm_depth_reject_logs++;
    if (n < 64u || (n % 512u) == 0u)
        glog("VP_GXM_DEPTH_REJECT kind=%s reason=%s idx=%u color=0x%llx "
             "%ux%u zen=%u zva=0x%llx zfmt=0x%x ztile=0x%x zpitch=0x%x "
             "zsize=%ux%u zdepth=%u zdim=%u depth_test=%u depth_write=%u "
             "n=%u\n", kind, vp_gxm_depth_status_name(status), index,
             (unsigned long long)color_va, color_w, color_h, g->enable,
             (unsigned long long)g->va, g->format, g->tile_mode,
             g->array_pitch, g->width, g->height, g->depth,
             g->dim_control, depth_test, depth_write, n);
}

/* Resolve one guest zeta binding. Returns 0 with binding->enabled possibly 0
 * (guest depth disabled), -2 for a validated refusal, or -1 for allocation /
 * worker failure. Never substitutes the colour surface's former private depth. */
static int depth_binding_acquire(VPGxmDepthBinding *binding,
                                 const VPGxmGuestDepth *g,
                                 unsigned target_width, unsigned target_height,
                                 unsigned fit_width, unsigned fit_height,
                                 unsigned index, uint64_t color_va,
                                 unsigned depth_test, unsigned depth_write,
                                 unsigned depth_write_raw, const char *kind) {
    VPGxmDepthPoolPlan plan;
    VPGxmDepthDesc old_desc;
    int have_old = 0;
    int status, rc;
    unsigned i;

    memset(binding, 0, sizeof(*binding));
    binding->slot = -1;
    status = vp_gxm_depth_desc_build(&binding->desc, g->enable, g->va,
                                     g->width, g->height, g->depth,
                                     g->dim_control, g->format, g->tile_mode,
                                     g->array_pitch);
    if (status == VP_GXM_DEPTH_DISABLED) {
        if (depth_pool_ensure() < 0) {
            gxm_depth_stats.reject_alloc++;
            return -1;
        }
        gxm_depth_stats.disabled++;
        if (depth_test != 0u || depth_write != 0u) {
            gxm_depth_stats.ignored_state++;
            if (gxm_depth_disabled_logs < 16u ||
                (gxm_depth_disabled_logs % 1024u) == 0u)
                glog("VP_GXM_DEPTH_DISABLED kind=%s idx=%u color=0x%llx "
                     "%ux%u zen=%u zva=0x%llx depth_test=%u depth_write=%u "
                     "depth_write_raw=%u action=ignore-depth-state n=%u\n", kind,
                     index, (unsigned long long)color_va, target_width,
                     target_height, g->enable, (unsigned long long)g->va,
                     depth_test, depth_write, depth_write_raw,
                     gxm_depth_disabled_logs);
            gxm_depth_disabled_logs++;
        }
        return 0;
    }
    if (status != VP_GXM_DEPTH_OK) {
        gxm_depth_stats.reject_meta++;
        depth_log_reject(kind, status, index, color_va, target_width,
                         target_height, depth_test, depth_write, g);
        return -2;
    }
    if (!vp_gxm_depth_desc_fits(&binding->desc, fit_width, fit_height)) {
        gxm_depth_stats.reject_target++;
        depth_log_reject(kind, VP_GXM_DEPTH_ERR_TARGET, index, color_va,
                         target_width, target_height, depth_test, depth_write,
                         g);
        return -2;
    }
    if (depth_pool_ensure() < 0) {
        gxm_depth_stats.reject_alloc++;
        glog("VP_GXM_DEPTH_POOL_INIT_FAIL kind=%s idx=%u\n", kind, index);
        return -1;
    }
    rc = vp_gxm_depth_pool_acquire(&gxm_depth_pool, &binding->desc, &plan);
    if (rc != VP_GXM_DEPTH_OK) {
        if (rc == VP_GXM_DEPTH_ERR_BUDGET) gxm_depth_stats.reject_budget++;
        else if (rc == VP_GXM_DEPTH_ERR_POOL) gxm_depth_stats.reject_pool++;
        else gxm_depth_stats.reject_meta++;
        depth_log_reject(kind, rc, index, color_va, target_width,
                         target_height, depth_test, depth_write, g);
        return -2;
    }
    for (i = 0; i < VP_GXM_DEPTH_POOL_SLOTS; i++)
        if ((plan.evict_mask & (1u << i)) != 0u) {
            if (plan.action == VP_GXM_DEPTH_ACTION_RECREATE &&
                (int)i == plan.slot) {
                old_desc = gxm_depths[i].desc;
                have_old = 1;
            }
            depth_entry_destroy(i);
        }
    if (plan.action == VP_GXM_DEPTH_ACTION_REUSE) {
        if (!gxm_depths[plan.slot].used) {
            vp_gxm_depth_pool_release(&gxm_depth_pool, (unsigned)plan.slot);
            gxm_depth_stats.reject_alloc++;
            glog("VP_GXM_DEPTH_REUSE_MISSING kind=%s idx=%u slot=%d\n",
                 kind, index, plan.slot);
            return -2;
        }
        /* Raw guest tiling hints may change without invalidating contents. */
        gxm_depths[plan.slot].desc = binding->desc;
        gxm_depth_stats.reuse++;
    } else {
        rc = depth_entry_create((unsigned)plan.slot, &binding->desc);
        if (rc < 0) {
            vp_gxm_depth_pool_release(&gxm_depth_pool, (unsigned)plan.slot);
            gxm_depth_stats.reject_alloc++;
            return -1;
        }
        if (plan.action == VP_GXM_DEPTH_ACTION_RECREATE) {
            gxm_depth_stats.recreate++;
            if (have_old && (gxm_depth_bind_logs < 32u ||
                             (gxm_depth_bind_logs % 512u) == 0u))
                glog("VP_GXM_DEPTH_RECREATE va=0x%llx %ux%u->%ux%u "
                     "fmt=0x%x->0x%x old_content=undefined slot=%d\n",
                     (unsigned long long)binding->desc.va,
                     old_desc.width, old_desc.height,
                     binding->desc.width, binding->desc.height,
                     old_desc.format, binding->desc.format, plan.slot);
        } else {
            gxm_depth_stats.create++;
        }
    }
    binding->enabled = 1;
    binding->slot = plan.slot;
    binding->entry = &gxm_depths[plan.slot];
    gxm_depth_stats.resolve++;
    if (gxm_depth_stats.resolve % 4096u == 0u) depth_log_stats();
    if (gxm_depth_bind_logs < 64u ||
        (gxm_depth_bind_logs % 512u) == 0u)
        glog("VP_GXM_DEPTH_BIND kind=%s idx=%u action=%s slot=%d "
             "color=0x%llx %ux%u fit=%ux%u zen=%u zva=0x%llx zfmt=0x%x ztile=0x%x "
             "zpitch=0x%x zsize=%ux%u zdepth=%u zdim=%u "
             "class=%u stride=%u storage_h=%u depth_bytes=%llu "
             "stencil_bytes=%llu alloc_bytes=%llu depth_test=%u "
             "depth_write=%u depth_write_raw=%u n=%u\n", kind, index,
             vp_gxm_depth_action_name(plan.action), plan.slot,
             (unsigned long long)color_va, target_width, target_height,
             fit_width, fit_height,
             g->enable, (unsigned long long)g->va, g->format, g->tile_mode,
             g->array_pitch, g->width, g->height, g->depth, g->dim_control,
             binding->desc.format_class, binding->desc.stride_samples,
             binding->desc.storage_height,
             (unsigned long long)binding->desc.depth_bytes,
             (unsigned long long)binding->desc.stencil_bytes,
             (unsigned long long)binding->desc.total_alloc_bytes,
             depth_test, depth_write, depth_write_raw, gxm_depth_bind_logs);
    gxm_depth_bind_logs++;
    return 0;
}

static void depth_entry_clear(VPGxmDepthEntry *e, uint32_t mask,
                              uint32_t depth_clear, uint32_t stencil_clear) {
    if (e == NULL || !e->used) return;
    if ((mask & 1u) != 0u) {
        float depth;
        memcpy(&depth, &depth_clear, sizeof(depth));
        if (!(depth >= 0.0f && depth <= 1.0f)) depth = 1.0f;
        sceGxmDepthStencilSurfaceSetBackgroundDepth(&e->surface, depth);
        e->background_depth = depth;
        /* The next scene starts with ForceLoad disabled and the GL backend
         * clears its depth attachment to this background value.  The clear is
         * recorded as pending; content_generation is deliberately not reset,
         * so the eventual materialized clear advances a monotonic epoch
         * instead of restarting at 1 and aliasing an older view. */
        e->clear_defined = 1u;
        e->depth_valid = 0;
        e->clear_count++;
        gxm_depth_stats.clear_depth++;
    }
    if ((mask & 2u) != 0u) {
        unsigned stencil = stencil_clear & 0xffu;
        sceGxmDepthStencilSurfaceSetBackgroundStencil(&e->surface,
                                                      (unsigned char)stencil);
        e->background_stencil = stencil;
        /* An explicit depth+stencil clear resets stencil to this known
         * background; it no longer carries guest-visible content. */
        e->stencil_live = 0;
        e->clear_count++;
        gxm_depth_stats.clear_stencil++;
    }
}

/* Commit one successfully synchronized depth scene as defined content.  The
 * epoch is monotonic across clears and draws; exhaustion invalidates the
 * identity for sampling until it is recreated and is never wrapped. */
static int depth_entry_commit_content(VPGxmDepthEntry *e, unsigned draw_index,
                                      const char *why) {
    if (e == NULL || !e->used) return -1;
    if (e->generation_exhausted) {
        e->depth_valid = 0u;
        e->content_generation = 0u;
        return -1;
    }
    if (e->content_generation == 0xffffffffu) {
        e->generation_exhausted = 1u;
        e->depth_valid = 0u;
        e->clear_defined = 0u;
        e->content_generation = 0u;
        glog("VP_GXM_DEPTH_GENERATION_EXHAUSTED idx=%u zva=0x%llx why=%s "
             "action=invalidate-until-recreate\n", draw_index,
             (unsigned long long)e->desc.va, why);
        return -1;
    }
    e->depth_valid = 1u;
    e->clear_defined = 0u;
    e->content_generation++;
    if (e->content_generation == 0u) e->content_generation = 1u;
    return 0;
}

/* Turn a recorded clear into real GPU depth content before the first
 * consumer.  This is the only materialization point: the clear itself stays
 * lazy, so a clear that is immediately overwritten by a depth-writing draw
 * costs nothing extra (that scene's ForceLoad-disabled load performs the same
 * clear).  A full-extent depth-only empty scene is used because the guest
 * clear covers the complete zeta surface, which may be larger than the colour
 * target of the clear request.  On failure the recorded state is untouched and
 * the consumer must reject. */
static int depth_clear_materialize_pending(VPGxmDepthEntry *e,
                                           unsigned draw_index,
                                           const char *why) {
    SceGxmRenderTarget *target;
    VPGxmDepthNativeClearResult result;
    uint64_t started, elapsed;
    int rc;

    if (e == NULL || !e->used || e->depth_mem == NULL) return -1;
    if (!e->clear_defined || e->depth_valid) return 0;
    if (gxm_scene_poisoned) return frame_context_reject(why);
    target = target_for(e->native_desc.width, e->native_desc.height);
    if (target == NULL) {
        gxm_depth_stats.clear_materialize_fail++;
        if (gxm_depth_materialize_logs++ < 16u)
            glog("VP_GXM_DEPTH_CLEAR_MATERIALIZE idx=%u zva=0x%llx why=%s "
                 "result=no-render-target %ux%u\n", draw_index,
                 (unsigned long long)e->desc.va, why, e->desc.width,
                 e->desc.height);
        return -1;
    }
    started = sceKernelGetProcessTimeWide();
    rc = vp_gxm_depth_native_clear(gxm_context, target, &e->surface, &result);
    elapsed = sceKernelGetProcessTimeWide() - started;
    gxm_depth_stats.clear_materialize_us += elapsed;
    if (rc < 0) {
        gxm_depth_stats.clear_materialize_fail++;
        if (gxm_depth_materialize_logs++ < 16u)
            glog("VP_GXM_DEPTH_CLEAR_MATERIALIZE idx=%u zva=0x%llx why=%s "
                 "rc=%d begin=%d end=%d notify=%d started=%d synchronized=%d "
                 "us=%llu action=keep-pending-consumer-rejects\n", draw_index,
                 (unsigned long long)e->desc.va, why, rc, result.begin_rc,
                 result.end_rc, result.notify_rc, result.scene_started,
                 result.synchronized, (unsigned long long)elapsed);
        if (result.end_rc < 0 || result.notify_rc < 0)
            return frame_poison_context(result.end_rc < 0 ? result.end_rc
                                                          : result.notify_rc);
        return rc;
    }
    if (depth_entry_commit_content(e, draw_index, why) < 0) {
        gxm_depth_stats.clear_materialize_fail++;
        return -1;
    }
    e->draw_count = 0u;
    gxm_depth_stats.clear_materialize++;
    if (gxm_depth_materialize_logs++ < 16u ||
        (gxm_depth_stats.clear_materialize % 256u) == 0u)
        glog("VP_GXM_DEPTH_CLEAR_MATERIALIZE idx=%u zva=0x%llx why=%s "
             "result=ok depth=%.6f stencil=%u %ux%u gen=%u storage=%u "
             "begin=%d end=%d notify=%d us=%llu materialized=%u fail=%u\n",
             draw_index, (unsigned long long)e->desc.va, why,
             e->background_depth, e->background_stencil, e->desc.width,
             e->desc.height, e->content_generation, e->storage_generation,
             result.begin_rc, result.end_rc, result.notify_rc,
             (unsigned long long)elapsed, gxm_depth_stats.clear_materialize,
             gxm_depth_stats.clear_materialize_fail);
    return 0;
}

static int surface_clear_gpu(VPGxmSurface *s, uint32_t color);
/* Defined with the display blit; draws the solid clear into an open scene. */
static int frame_clear_in_scene(VPGxmSurface *dst);
/* Defined next to frame_clear_in_scene; clears the pending colour tags after a
 * successful in-scene GPU clear. */
static void surface_clear_consumed_in_scene(VPGxmSurface *s);
static int frame_blit_gpu(VPGxmSurface *s, void *dst, unsigned x0, unsigned y0,
                          unsigned w, unsigned h, unsigned dst_width,
                          unsigned dst_height, unsigned dst_pitch,
                          int surface_source);

/* B42 diagnostic: only while the B34 pass capture is due, report which clear
 * branch ran and whether the backing changed, so a dump that still shows
 * pre-clear pixels can be attributed to the clear or to the capture. */
static int b34_dump_due(void); /* defined with the B34 capture below */
static unsigned surface_nonzero_pixels(const VPGxmSurface *s) {
    const uint32_t *p = (const uint32_t *)s->color_mem;
    unsigned n = 0;
    if (p == NULL) return 0;
    for (size_t i = 0, c = (size_t)s->render_width * s->render_height; i < c; ++i)
        if (p[i] != 0u) ++n;
    return n;
}
/* Largest blue byte seen by a GPU sample of the surface through its own texture
 * view: the same view a game draw would read. CPU memory and the GPU texture
 * can disagree after a render-target clear in the emulator, so the diagnostic
 * must ask the GPU, not just color_mem. */
static int surface_gpu_blue_probe(const VPGxmSurface *s, unsigned *max_b) {
    static void *buf;
    unsigned m = 0u;
    int rc;
    *max_b = 0u;
    if (buf == NULL) buf = gpu_alloc(16u * 16u * 4u, 0, NULL);
    if (buf == NULL) return -1;
    memset(buf, 0, 16u * 16u * 4u);
    rc = frame_blit_gpu((VPGxmSurface *)s, buf, 0u, 0u, s->width, s->height,
                        16u, 16u, 16u, 1);
    if (rc == 0) {
        const uint8_t *p = (const uint8_t *)buf;
        for (unsigned i = 0; i < 16u * 16u; ++i)
            if (p[i * 4u + 2u] > m) m = p[i * 4u + 2u];
    }
    *max_b = m;
    return rc;
}
static void surface_clear_trace(const VPGxmSurface *s, uint32_t mask,
                                uint32_t color, unsigned drawn,
                                const char *path, unsigned before,
                                unsigned after, int rc, int gpu_path) {
    static unsigned traces;
    unsigned gpu_b = 0u;
    int gpu_rc = 0;
    if (!b34_dump_due() || traces >= 24u) return;
    traces++;
    if (gpu_path) gpu_rc = surface_gpu_blue_probe(s, &gpu_b);
    glog("VP_GXM_CLEAR_PATH va=0x%llx %ux%u mask=0x%x color=0x%08x "
         "drawn=%u path=%s nonzero_before=%u nonzero_after=%u rc=%d "
         "gpu_probe_blue=%u gpu_probe_rc=%d trace=%u\n",
         (unsigned long long)s->va, s->width, s->height, mask, color, drawn,
         path, before, after, rc, gpu_b, gpu_rc, traces);
}

static int surface_clear(VPGxmSurface *s, uint32_t mask, uint32_t color) {    uint32_t *pixels;
    unsigned count;
    const char *clear_path = "none";
    unsigned traced = 0u, before = 0u;

    if (s == NULL) return -1;
    if (b34_dump_due()) {
        traced = 1u;
        before = surface_nonzero_pixels(s);
    }
    if ((mask & 0x3cu) != 0u && s->color_mem != NULL) {
        pixels = (uint32_t *)s->color_mem;
        count = s->render_width * s->render_height;
        if ((mask & 0x3cu) == 0x3cu) {
            /* A transfer-only fill changes CPU backing but the locked backend
             * can retain a rendered FBO. Clear that live attachment through
             * the GPU before a later blend/depth-preserving scene reloads it. */
            if (s->color_drawn) {
                int rc = surface_clear_gpu(s, color);
                clear_path = "gpu-blit";
                if (rc < 0) {
                    surface_clear_trace(s, mask, color, s->color_drawn,
                                        clear_path, before, before, rc, 1);
                    return rc;
                }
            } else {
            clear_path = "transfer-fill";
            /* GXM transfer strides are byte pitches. Passing the pixel width
             * here compacts each row over the previous one (observed as a
             * clear that only covered the first quarter of the surface and
             * was then magnified by downstream sampling). */
            int rc=sceGxmTransferFill(color,SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR,
                s->color_mem,0,0,s->render_width,s->render_height,
                s->render_width * 4u,NULL,0,NULL);
            if (rc<0) {
                surface_clear_trace(s, mask, color, s->color_drawn, clear_path,
                                    before, before, rc, 0);
                return rc;
            }
            rc=sceGxmTransferFinish();
            if (rc<0) {
                surface_clear_trace(s, mask, color, s->color_drawn, clear_path,
                                    before, before, rc, 0);
                return rc;
            }
            }
        } else {
            clear_path = "partial-cpu";
            for (unsigned i = 0; i < count; i++) {
                uint32_t v = pixels[i];
                uint32_t r = (mask & (1u << 2)) ? (color & 0xffu) : (v & 0xffu);
                uint32_t g = (mask & (1u << 3)) ? (color & 0xff00u) : (v & 0xff00u);
                uint32_t b = (mask & (1u << 4)) ? (color & 0xff0000u) : (v & 0xff0000u);
                uint32_t a = (mask & (1u << 5)) ? (color & 0xff000000u) : (v & 0xff000000u);
                pixels[i] = r | g | b | a;
            }
        }
    }
    if (traced)
        surface_clear_trace(s, mask, color, s->color_drawn, clear_path, before,
                            surface_nonzero_pixels(s), 0,
                            strcmp(clear_path, "gpu-blit") == 0);
    if ((mask & 0x3cu) != 0u) s->color_clear_mask |= mask & 0x3cu;
    s->cleared_count++;
    return 0;
}

/* Finish a deferred full-colour clear outside an open scene. The surface must
 * not be read or written by any open batch; callers flush first. A clear that
 * was recorded through the mode-6 async boundary additionally proves every
 * pending scene complete before the CPU/GPU transfer writes pixels. A failed
 * wait returns the original error and leaves the pending colour and its tag
 * intact; no pixel is written and no GPU reference is released. */
/* VP_HOST_TEST_EXTRACT:clear_materialize_begin */
static int surface_clear_materialize(VPGxmSurface *s, const char *why) {
    uint32_t color;
    int rc;
    if (s == NULL || !s->clear_pending) return 0;
    if (s->clear_pending_async) {
        gxm_clear_defer_materialize_calls++;
        rc = gxm_pending_scene_complete("clear-materialize");
        if (rc < 0) {
            gxm_clear_defer_materialize_failures++;
            return rc;
        }
        s->clear_pending_async = 0u;
    }
    color = s->clear_pending_color;
    s->clear_pending = 0;
    gxm_clear_fusion_materialized++;
    glog("VP_GXM_CLEAR_MATERIALIZE va=0x%llx color=0x%08x why=%s n=%llu\n",
         (unsigned long long)s->va, color, why,
         (unsigned long long)gxm_clear_fusion_materialized);
    rc = surface_clear(s, 0x3cu, color);
    return rc;
}
/* VP_HOST_TEST_EXTRACT:clear_materialize_end */


static void *host_alloc(void *user, SceSize bytes) { (void)user; return malloc(bytes); }
static void host_free(void *user, void *ptr) { (void)user; free(ptr); }

static int ensure_patcher(void) {
    SceGxmShaderPatcherParams params;
    int rc;

    if (patcher != NULL) return 0;
    memset(&params, 0, sizeof(params));
    params.hostAllocCallback = host_alloc;
    params.hostFreeCallback = host_free;
    params.bufferMemSize = VP_GXM_PATCHER_BUFFER;
    params.bufferMem = gpu_alloc(params.bufferMemSize, 0, NULL);
    params.vertexUsseMemSize = VP_GXM_PATCHER_USSE;
    params.vertexUsseMem = gpu_alloc(params.vertexUsseMemSize, 1, &params.vertexUsseOffset);
    params.fragmentUsseMemSize = VP_GXM_PATCHER_USSE;
    params.fragmentUsseMem = gpu_alloc(params.fragmentUsseMemSize, 2, &params.fragmentUsseOffset);
    if (params.bufferMem == NULL || params.vertexUsseMem == NULL ||
        params.fragmentUsseMem == NULL) {
        glog("VP_GXM_PATCHER alloc failed\n");
        return -1;
    }
    rc = sceGxmShaderPatcherCreate(&params, &patcher);
    glog("VP_GXM_PATCHER rc=%d\n", rc);
    return rc;
}

static SceGxmProgram *read_program(const char *path, unsigned *size) {
    FILE *file = fopen(path, "rb");
    long length;
    SceGxmProgram *program;

    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    length = ftell(file);
    if (length < 32 || length > 1024L * 1024L) { fclose(file); return NULL; }
    rewind(file);
    program = (SceGxmProgram *)malloc((size_t)length);
    if (program == NULL || fread(program, 1, (size_t)length, file) != (size_t)length) {
        free(program);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (unsigned)length;
    return program;
}

static void sha_hex(const unsigned char sha[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32u; ++i) {
        out[i * 2u] = digits[sha[i] >> 4];
        out[i * 2u + 1u] = digits[sha[i] & 15u];
    }
    out[64] = 0;
}

static int read_bounded_file(const char *path, size_t limit,
                             unsigned char **out, size_t *out_size) {
    FILE *file;
    long length;
    unsigned char *data;
    if (path == NULL || out == NULL || out_size == NULL) return -1;
    *out = NULL;
    *out_size = 0;
    file = fopen(path, "rb");
    if (file == NULL) return -1;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
        (unsigned long)length > limit) {
        fclose(file);
        return -1;
    }
    rewind(file);
    data = (unsigned char *)malloc((size_t)length);
    if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return -1;
    }
    fclose(file);
    *out = data;
    *out_size = (size_t)length;
    return 0;
}

/* Load the offline index and its per-stage resource sidecar. A missing file
 * selects the legacy hash bundle; a present file is validated as a whole. */
static int static_index_load(const char *dir) {
    char path[VP_GXM_STATIC_PATH];
    char resource_path[VP_GXM_STATIC_PATH];
    char error[160];
    unsigned char *index_bytes = NULL, *resource_bytes = NULL;
    size_t index_size = 0, resource_size = 0;
    FILE *probe;
    int rc;

    /* A reload can change token/variant/resident identity; no hint or plan
     * survives. The generation covers entries that are not touched again. */
    static_pair_hint_reset();
    static_plan_invalidate();
    static_mode = 0;
    static_resources_ready = 0;
    snprintf(path, sizeof(path), "%s/shader-index.bin", dir);
    probe = fopen(path, "rb");
    if (probe == NULL) {
        glog("VP_GXM_STATIC_INDEX detected=0 dir=%s legacy=1\n", dir);
        return 0;
    }
    fclose(probe);
    if (read_bounded_file(path, VP_STATIC_INDEX_MAX_BYTES,
                          &index_bytes, &index_size) < 0) {
        glog("VP_GXM_STATIC_INDEX detected=1 valid=0 error=index-read\n");
        return -1;
    }
    error[0] = 0;
    rc = vp_static_shader_index_parse(index_bytes, index_size, &static_index,
                                      error, sizeof(error));
    if (rc < 0) {
        glog("VP_GXM_STATIC_INDEX detected=1 valid=0 error=%s\n", error);
        free(index_bytes);
        return -1;
    }
    vp_sha256(index_bytes, index_size, static_index_sha256);
    static_index_crc32 = vp_static_index_crc32(index_bytes, index_size);
    free(index_bytes);

    snprintf(resource_path, sizeof(resource_path), "%s/shader-resources.bin", dir);
    if (read_bounded_file(resource_path, VP_RESOURCE_LAYOUT_MAX_BYTES,
                          &resource_bytes, &resource_size) < 0) {
        glog("VP_GXM_RESOURCE_LAYOUT ready=0 path=%s detail=sidecar-missing "
             "action=refuse-static-mode\n", resource_path);
        vp_static_shader_index_free(&static_index);
        return -1;
    }
    vp_sha256(resource_bytes, resource_size, static_resources_sha256);
    error[0] = 0;
    rc = vp_resource_layout_parse(resource_bytes, resource_size,
                                  static_index_sha256, static_index_crc32,
                                  &static_index, &static_resources,
                                  error, sizeof(error));
    free(resource_bytes);
    if (rc != VP_RESOURCE_LAYOUT_OK) {
        glog("VP_GXM_RESOURCE_LAYOUT ready=0 error=%s status=%s\n", error,
             vp_resource_layout_status_name(rc));
        vp_static_shader_index_free(&static_index);
        return -1;
    }
    /* Every compiled variant must resolve to a stage that the sidecar covers;
     * a variant with unknown source metadata is a package error, not a draw
     * time surprise. */
    for (uint32_t i = 0; i < static_index.variant_count; ++i) {
        const VPStaticVariant *variant = &static_index.variants[i];
        const VPStaticStage *stage = &static_index.stages[variant->stage_index];
        if (vp_resource_layout_stage(&static_resources, stage->stage,
                                     stage->sha256) == NULL) {
            glog("VP_GXM_RESOURCE_LAYOUT ready=0 detail=variant-stage-missing "
                 "variant=%u stage=%u\n", i, variant->stage_index);
            vp_resource_layout_free(&static_resources);
            vp_static_shader_index_free(&static_index);
            return -1;
        }
    }
    if (strlen(dir) >= sizeof(static_index_dir)) {
        glog("VP_GXM_STATIC_INDEX invalid=path-too-long\n");
        vp_resource_layout_free(&static_resources);
        vp_static_shader_index_free(&static_index);
        return -1;
    }
    strcpy(static_index_dir, dir);
    static_resources_ready = 1;
    static_mode = 1;
    {
        char index_hex[65], resource_hex[65];
        sha_hex(static_index_sha256, index_hex);
        sha_hex(static_resources_sha256, resource_hex);
        glog("VP_GXM_STATIC_INDEX detected=1 valid=1 stages=%u variants=%u "
             "programs=%u abi=%u sha256=%s\n", static_index.stage_count,
             static_index.variant_count, static_index.program_count,
             static_index.abi_version, index_hex);
        glog("VP_GXM_RESOURCE_LAYOUT ready=1 stages=%u textures=%u variants=%u "
             "sha256=%s parent_sha256=%s action=bounded-per-stage-offset\n",
             static_resources.stage_count, static_resources.texture_count,
             static_index.variant_count, resource_hex, index_hex);
    }
    return 0;
}

int vp_gxm_shader_register_all(const char *dir, unsigned *files,
                               unsigned *registered, int *first_error) {
    char path[256];
    char line[160];
    FILE *index;
    int rc;

    if (files != NULL) *files = 0;
    if (registered != NULL) *registered = 0;
    if (first_error != NULL) *first_error = 0;
    rc = ensure_patcher();
    if (rc < 0) {
        if (first_error != NULL) *first_error = rc;
        return -1;
    }
    rc = static_index_load(dir);
    if (rc < 0) {
        if (first_error != NULL) *first_error = rc;
        return -1;
    }
    if (static_mode) {
        rc = static_preload_observed();
        if (rc == 0) rc = static_register_bringup(0xf9du);
        if (rc < 0) {
            glog("VP_GXM_STATIC_INDEX preload failed rc=%d\n", rc);
            if (first_error != NULL) *first_error = rc;
            return -1;
        }
        if (files != NULL) *files = static_index.variant_count;
        if (registered != NULL) *registered = static_resident_used;
        glog("VP_GXM_STATIC_INDEX ready resident=%u bytes=%u files=%u\n",
             static_resident_used, static_resident_bytes,
             static_index.variant_count);
        return 0;
    }
    snprintf(path, sizeof(path), "%s/index.txt", dir);
    index = fopen(path, "rb");
    if (index == NULL) {
        glog("VP_GXM_SHADER index missing dir=%s\n", dir);
        if (first_error != NULL) *first_error = -1;
        return -1;
    }
    while (fgets(line, sizeof(line), index) != NULL) {
        char *end = line + strlen(line);
        unsigned size = 0;
        unsigned stage = 0;
        unsigned long hash = 0;
        SceGxmShaderPatcherId id = NULL;
        SceGxmProgram *program;
        char *mark;

        while (end > line && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
        if (line[0] == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, line);
        program = read_program(path, &size);
        if (program == NULL) {
            glog("VP_GXM_SHADER read failed file=%s\n", line);
            if (first_error != NULL && *first_error == 0) *first_error = -1;
            continue;
        }
        mark = strstr(line, "_0x");
        if (line[0] == 's' && line[1] >= '0' && line[1] <= '9' && mark != NULL)
            stage = (unsigned)(line[1] - '0');
        if (mark != NULL) hash = strtoul(mark + 3, NULL, 16);
        rc = sceGxmShaderPatcherRegisterProgram(patcher, program, &id);
        if (files != NULL) (*files)++;
        if (rc < 0) {
            if (first_error != NULL && *first_error == 0) *first_error = rc;
            glog("VP_GXM_SHADER stage=%u hash=0x%lx bytes=%u rc=%d\n",
                 stage, hash, size, rc);
            free(program);
        } else {
            if (registered != NULL) (*registered)++;
            if (shader_count < VP_GXM_PROGRAM_STORE) {
                shaders[shader_count].stage = stage;
                shaders[shader_count].viewport_transform = strstr(line, "_vp1_") != NULL;
                shaders[shader_count].hash = hash;
                shaders[shader_count].id = id;
                shaders[shader_count].program = program;
                shader_count++;
            } else {
                free(program);
            }
            glog("VP_GXM_SHADER stage=%u hash=0x%lx bytes=%u rc=0 id=%p\n",
                 stage, hash, size, (void *)id);
        }
    }
    fclose(index);
    glog("VP_GXM_SHADER total files=%u registered=%u\n",
         files != NULL ? *files : 0u, registered != NULL ? *registered : 0u);
    return (first_error != NULL && *first_error < 0) ? -1 : 0;
}

int vp_gxm_shader_has_fragment_sampler(unsigned hash,unsigned viewport_transform,
                                       unsigned sampler_index) {
    char name[8];
    if (sampler_index >= VP_GXM_FRAGMENT_SAMPLER_LIMIT) return 0;
    if (static_mode)
        return static_query_sampler(hash, VP_STATIC_INDEX_STAGE_FRAGMENT,
                                    viewport_transform, sampler_index);
    snprintf(name,sizeof(name),"tex%u",sampler_index);
    for (unsigned i=0;i<shader_count;i++) {
        if (shaders[i].stage!=5u || shaders[i].hash!=(unsigned long)hash ||
            shaders[i].viewport_transform!=viewport_transform) continue;
        return sceGxmProgramFindParameterByName(shaders[i].program,name)!=NULL;
    }
    return 0;
}

int vp_gxm_shader_has_vertex_sampler(unsigned hash,unsigned viewport_transform,
                                     unsigned sampler_index) {
    char name[8];
    if (sampler_index>2u) return 0;
    if (static_mode)
        return static_query_sampler(hash, VP_STATIC_INDEX_STAGE_VERTEX,
                                    viewport_transform, sampler_index);
    snprintf(name,sizeof(name),"tex%u",sampler_index);
    for (unsigned i=0;i<shader_count;i++) {
        if (shaders[i].stage!=1u || shaders[i].hash!=(unsigned long)hash ||
            shaders[i].viewport_transform!=viewport_transform) continue;
        return sceGxmProgramFindParameterByName(shaders[i].program,name)!=NULL;
    }
    return 0;
}

/* Create one vertex/fragment program pair from the registered GXP. The pair is
 * the first title shader (0xf9d): one float4 attribute, a 16-byte stream, and
 * no texture ABI needed for program creation. */
static int create_first_programs(void) {
    unsigned i, j;
    int created = 0;

    for (i = 0; i < shader_count; i++) {
        const VPShaderEntry *vertex = &shaders[i];
        const VPShaderEntry *fragment = NULL;
        const SceGxmProgramParameter *attribute;
        SceGxmVertexAttribute attr;
        SceGxmVertexStream stream;
        SceGxmVertexProgram *vs = NULL;
        SceGxmFragmentProgram *fs = NULL;
        int rc;

        if (vertex->stage != 1u) continue;
        for (j = 0; j < shader_count; j++) {
            if (shaders[j].hash == vertex->hash && shaders[j].stage == 5u &&
                shaders[j].viewport_transform == vertex->viewport_transform)
                fragment = &shaders[j];
        }
        if (fragment == NULL) continue;
        attribute = sceGxmProgramFindParameterByName(vertex->program, "in_attr0");
        if (attribute == NULL) {
            glog("VP_GXM_PROGRAM hash=0x%x in_attr0 missing\n", vertex->hash);
            continue;
        }
        memset(&attr, 0, sizeof(attr));
        attr.streamIndex = 0;
        attr.offset = 0;
        attr.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
        attr.componentCount = 4;
        attr.regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(attribute);
        stream.stride = 16;
        stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
        rc = sceGxmShaderPatcherCreateVertexProgram(patcher, vertex->id, &attr, 1,
                                                    &stream, 1, &vs);
        if (rc < 0) {
            glog("VP_GXM_PROGRAM hash=0x%x vs rc=%d\n", vertex->hash, rc);
            continue;
        }
        rc = sceGxmShaderPatcherCreateFragmentProgram(patcher, fragment->id,
                SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
                NULL, vertex->program, &fs);
        if (rc < 0) {
            glog("VP_GXM_PROGRAM hash=0x%x fs rc=%d\n", vertex->hash, rc);
            continue;
        }
        if (gxm_pair_count < VP_GXM_PROGRAM_STORE) {
            VPGxmProgramPair *pair = &gxm_pairs[gxm_pair_count++];
            pair->hash = vertex->hash;
            pair->viewport_transform = vertex->viewport_transform;
            pair->stride = 16u;
            pair->vertex_id = vertex->id;
            pair->fragment_id = fragment->id;
            pair->vs = vs;
            pair->fs = fs;
            pair->vs_program = vertex->program;
            pair->fs_program = fragment->program;
        }
        if (vertex->hash == 0xf9dul && !vertex->viewport_transform) {
            vertex_program = vs;
            fragment_program = fs;
            gxm_vs_program = vertex->program;
            gxm_fs_program = fragment->program;
        }
        created++;
        glog("VP_GXM_PROGRAM hash=0x%x vs=1 fs=1 total=%d\n", vertex->hash, created);
    }
    return created > 0 ? 0 : -1;
}

static unsigned gxm_rt_w = VP_GXM_WIDTH;
static unsigned gxm_rt_h = VP_GXM_HEIGHT;

/* Recreate the render target/color/depth pair for a pass's true size. The old
 * target and buffers are destroyed/released; a handful of distinct sizes is
 * expected per frame. */
static int gxm_resize_rt(unsigned w, unsigned h) {
    SceGxmRenderTarget *target = NULL;
    SceGxmRenderTargetParams rp;
    SceGxmColorSurface color;
    SceGxmDepthStencilSurface depth;
    void *rt;
    void *dm;
    void *sm;
    int rc;

    if (w == gxm_rt_w && h == gxm_rt_h) return 0;
    if (w < 16u || h < 16u || w > 2048u || h > 2048u) return -1;
    memset(&rp, 0, sizeof(rp));
    rp.width = w;
    rp.height = h;
    rp.scenesPerFrame = 2;
    rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rp.driverMemBlock = -1;
    rc = sceGxmCreateRenderTarget(&rp, &target);
    if (rc < 0) return rc;
    rt = gpu_alloc((size_t)w * h * 4u, 0, NULL);
    dm = gpu_alloc((size_t)w * h * 4u, 0, NULL);
    sm = gpu_alloc((size_t)w * h, 0, NULL);
    if (rt == NULL || dm == NULL || sm == NULL) return -1;
    rc = sceGxmColorSurfaceInit(&color, SCE_GXM_COLOR_FORMAT_A8B8G8R8,
            SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
            SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, w, h, w, rt);
    if (rc < 0) return rc;
    rc = sceGxmDepthStencilSurfaceInit(&depth, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
            SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, w, dm, sm);
    if (rc < 0) return rc;
    if (gxm_target != NULL) sceGxmDestroyRenderTarget(gxm_target);
    gxm_target = target;
    gxm_color = color;
    gxm_depth = depth;
    gxm_rt = rt;
    gxm_rt_w = w;
    gxm_rt_h = h;
    return 0;
}

/* Draw one synthetic full-surface quad through the converted 0xf9d pair. This
 * is the first GXM draw inside the loader: the vertex shader's render_area is
 * set to 1x1 so attribute xy in [0,1] covers the surface, and the fragment
 * samples a 2x2 solid texture at UV (0,0). It does not use the game's vertex
 * data or textures yet. */
/* Material varyings with W=1 must remain register writes through the GXP
 * compiler/translator. Initialize the carrier in every vertex-program path. */
static int init_vertex_uniforms(void *buffer, const SceGxmProgram *program) {
    /* The SDK API returns bytes, not float words. Multiplying by four can
     * write past the end of GXM's reserved ring buffer at a wrap boundary. */
    memset(buffer, 0, sceGxmProgramGetDefaultUniformBufferSize(program));
    const SceGxmProgramParameter *p =
        sceGxmProgramFindParameterByName(program, "vp_varying_one");
    if (p != NULL) {
        const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        return sceGxmSetUniformDataF(buffer, p, 0, 4, one);
    }
    return 0;
}

/* End the scene currently owned by this worker and wait for its completion.
 * Must run before shared staging buffers are reused. The caller must not call
 * this again if it fails: a failed EndScene leaves the context in an explicit
 * failed state rather than silently resetting it. The context is explicit so
 * the local bring-up probe never ends the worker's main scene by accident. */
static int frame_end_scene_timed(SceGxmContext *context, const char *why,
                                 uint64_t *out_end, uint64_t *out_notify,
                                 uint64_t *out_finish) {
    SceGxmNotification notification;
    volatile unsigned *region = sceGxmGetNotificationRegion();
    int pipeline = gxm_scene_pipeline_enabled();
    uint64_t started, finished;
    int rc;

    notification.address = region;
    if (pipeline) {
        /* Never reset the region while a deferred scene may still be running:
         * values only grow, and a wait for a later value implies every earlier
         * scene completed. */
        gxm_notify_seq = gxm_notify_next(gxm_notify_seq);
        notification.value = gxm_notify_seq;
    } else {
        notification.value = 1;
        *region = 0;
    }
    started = gxm_prof_now();
    rc = sceGxmEndScene(context, NULL, &notification);
    finished = gxm_prof_now();
    if (started != 0u) {
        gxm_endscene_calls++;
        gxm_endscene_us += finished - started;
    }
    if (out_end != NULL) *out_end = finished;
    if (rc < 0) return rc;
    started = gxm_prof_now();
    {
        uint64_t fc = vp_gxm_frame_cost_category_begin(
            VP_FRAME_COST_WAIT_NOTIFY);
        rc = sceGxmNotificationWait(&notification);
        vp_gxm_frame_cost_category_end(VP_FRAME_COST_WAIT_NOTIFY, fc);
    }
    finished = gxm_prof_now();
    vp_gpu_wait_record(&gxm_wait_profile, why, started, finished, 1, rc,
                       gxm_pending_scene_count, 0u, 1);
    if (started != 0u) {
        gxm_notify_wait_calls++;
        gxm_notify_wait_us += finished - started;
    }
    if (out_notify != NULL) *out_notify = finished;
    if (rc < 0) return rc;
    /* sceGxmFinish returns void on this SDK; the timestamp still measures the
     * complete synchronous wait before any staging buffer may be reused. */
    started = gxm_prof_now();
    {
        uint64_t fc = vp_gxm_frame_cost_category_begin(
            VP_FRAME_COST_WAIT_FINISH);
        sceGxmFinish(context);
        vp_gxm_frame_cost_category_end(VP_FRAME_COST_WAIT_FINISH, fc);
    }
    finished = gxm_prof_now();
    if (started != 0u) {
        gxm_finish_calls++;
        gxm_finish_us += finished - started;
    }
    if (pipeline) gxm_pending_scene_retire_upto(notification.value);
    if (out_finish != NULL) *out_finish = finished;
    return 0;
}

static int frame_end_scene(SceGxmContext *context) {
    return frame_end_scene_timed(context, "sync-scene", NULL, NULL, NULL);
}


/* Mark the worker's main GXM context unusable after a scene synchronization
 * failure. There is no teardown/reinitialization path yet, so the flag is
 * intentionally not cleared and no resource is silently recreated. */
static int frame_poison_context(int rc) {
    if (vp_texture_direct_active()) vp_texture_direct_fail_submission();
    if (!gxm_scene_poisoned) {
        gxm_scene_poisoned = 1;
        glog("VP_GXM_CONTEXT poison rc=%d action=reject-draw-clear-present "
             "teardown=unavailable\n", rc);
    }
    return rc;
}

static int frame_context_reject(const char *entry) {
    static unsigned reject_logs;
    if (reject_logs++ < 8u)
        glog("VP_GXM_CONTEXT reject entry=%s reason=poisoned\n", entry);
    return -1;
}

/* ---- B34 bounded per-pass capture ---------------------------------------
 *
 * Diagnostic-only readback of render targets and the presented display
 * buffer. It is enabled by ux0:data/mhgu-vita-port/b34-passdump.enable and is
 * inert otherwise. Every call site runs after GPU completion (synchronous
 * EndScene/notification/Finish, transfer Finish, or a batch flush Finish);
 * this code never inserts its own flush and never changes scene boundaries.
 * Per-VA dumps are capped (writes 1, 2 and 8) and the total is bounded so a
 * village run cannot fill the memory card. One JSONL line per dump records the
 * target identity, the writer, and the sampled inputs so producer/consumer
 * edges can be reconstructed offline. */
#define VP_B34_DUMP_FILE_LIMIT 24u
#define VP_B34_DUMP_PRESENT_LIMIT 4u
#define VP_B34_DUMP_VA_LIMIT 40u
#define VP_B34_DUMP_PATH "ux0:data/mhgu-vita-port"
typedef struct {
    uint64_t va;
    unsigned writes;
} VPB34DumpSeen;
typedef struct {
    int parsed;
    int enabled;
    unsigned passdump;
    unsigned live;
    unsigned live_inputs;
    unsigned delay_ms;
    unsigned max_dumps;
    unsigned max_presents;
    unsigned dumps;
    unsigned presents;
    unsigned seq;
    uint64_t start_us;
    unsigned seen_count;
    VPB34DumpSeen seen[VP_B34_DUMP_VA_LIMIT];
} VPB34DumpState;
static VPB34DumpState b34_dump;

static void b34_dump_init(void) {
    char text[512];
    size_t got = 0u;
    FILE *f;
    if (b34_dump.parsed) return;
    b34_dump.parsed = 1;
    b34_dump.delay_ms = 0u;
    b34_dump.max_dumps = VP_B34_DUMP_FILE_LIMIT;
    b34_dump.max_presents = VP_B34_DUMP_PRESENT_LIMIT;
    b34_dump.start_us = sceKernelGetProcessTimeWide();
    f = fopen(VP_B34_DUMP_PATH "/b34-passdump.enable", "rb");
    if (f == NULL) return;
    got = fread(text, 1u, sizeof(text) - 1u, f);
    fclose(f);
    text[got] = 0;
    b34_dump.enabled = 1;
    b34_dump.passdump = 1;
    for (char *line = text; line != NULL && *line != 0;) {
        char *end = strchr(line, '\n');
        char *value;
        if (end != NULL) *end = 0;
        value = strchr(line, '=');
        if (value != NULL) {
            *value++ = 0;
            while (*value == ' ' || *value == '\t') value++;
        }
        if (value != NULL && strcmp(line, "delay_ms") == 0)
            b34_dump.delay_ms = (unsigned)strtoul(value, NULL, 10);
        else if (value != NULL && strcmp(line, "max_dumps") == 0)
            b34_dump.max_dumps = (unsigned)strtoul(value, NULL, 10);
        else if (value != NULL && strcmp(line, "max_presents") == 0)
            b34_dump.max_presents = (unsigned)strtoul(value, NULL, 10);
        line = end != NULL ? end + 1 : NULL;
    }
    if (b34_dump.max_dumps > VP_B34_DUMP_FILE_LIMIT)
        b34_dump.max_dumps = VP_B34_DUMP_FILE_LIMIT;
    if (b34_dump.max_presents > VP_B34_DUMP_PRESENT_LIMIT)
        b34_dump.max_presents = VP_B34_DUMP_PRESENT_LIMIT;
    glog("VP_B34_PASSDUMP enabled=1 delay_ms=%u max_dumps=%u "
         "max_presents=%u\n", b34_dump.delay_ms, b34_dump.max_dumps,
         b34_dump.max_presents);
}

static unsigned b34_live_generation, b34_live_inputs;
static void b34_live_poll(void) {
    static int enabled = -1;
    static uint64_t next_poll;
    static unsigned last, requests;
    FILE *file;
    if (enabled < 0) {
        file = fopen(VP_B34_DUMP_PATH "/capture-live.enable", "rb");
        enabled = file != NULL;
        if (file) fclose(file);
    }
    if (!enabled || requests >= 4u) return;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now < next_poll) return;
    next_poll = now + 100000u;
    file = fopen(VP_B34_DUMP_PATH "/capture-request.txt", "rb");
    if (file) {
        unsigned sequence, inputs;
        int fields = fscanf(file, "%u %u", &sequence, &inputs);
        fclose(file);
        if (fields != 2 || !sequence || sequence == last || inputs > 1u) return;
        last = sequence;
        requests++;
        b34_live_generation = sequence;
        b34_live_inputs = inputs;
        b34_dump.enabled = 1;
        b34_dump.live = 1;
        b34_dump.live_inputs = inputs;
        b34_dump.delay_ms = 0u;
        b34_dump.max_dumps = b34_dump.dumps + (inputs ? 8u : 0u);
        b34_dump.max_presents = b34_dump.presents + 1u;
        b34_dump.seen_count = 0u;
        glog("VP_CAPTURE_REQUEST seq=%u inputs=%u request=%u first_present=%u\n",
             sequence, inputs, requests, b34_dump.presents);
    }
}

static int b34_dump_due(void) {
    uint64_t now;
    b34_dump_init();
    b34_live_poll();
    if (!b34_dump.enabled) return 0;
    now = sceKernelGetProcessTimeWide();
    if (now < b34_dump.start_us) return 0;
    return (now - b34_dump.start_us) / 1000u >= (uint64_t)b34_dump.delay_ms;
}

/* Only a surface/pass readback requires the synchronous scene path. A
 * capture-live request with inputs=0 records the present pair only; that read
 * already happens after gxm_pending_scene_complete("present"). Returning false
 * here therefore restores the asynchronous pipeline for present-only captures
 * and after the requested input dumps complete. The explicit old
 * b34-passdump.enable marker keeps its documented synchronous behaviour. */
static int b34_dump_surface_capture_active(void) {
    b34_dump_init();
    b34_live_poll();
    if (!b34_dump.enabled) return 0;
    if (b34_dump.passdump) return 1;
    return b34_dump.live && b34_dump.live_inputs != 0u &&
           b34_dump.dumps < b34_dump.max_dumps;
}

static unsigned b34_dump_note_writes(uint64_t va) {
    unsigned i;
    for (i = 0; i < b34_dump.seen_count; ++i)
        if (b34_dump.seen[i].va == va) return ++b34_dump.seen[i].writes;
    if (b34_dump.seen_count >= VP_B34_DUMP_VA_LIMIT) return 0u;
    b34_dump.seen[b34_dump.seen_count].va = va;
    b34_dump.seen[b34_dump.seen_count].writes = 1u;
    b34_dump.seen_count++;
    return 1u;
}

static int b34_dump_expect(unsigned writes) {
    return writes == 1u || writes == 2u || writes == 8u;
}

static void b34_dump_inputs(FILE *meta, const VPGxmDrawRequest *req) {
    unsigned printed = 0u;
    fprintf(meta, ",\"inputs\":[");
    if (req == NULL) {
        fprintf(meta, "]");
        return;
    }
    if (req->texture_source != VP_GXM_EXTRA_SOURCE_NONE ||
        req->texture_va != 0u) {
        fprintf(meta, "%s{\"slot\":0,\"va\":\"0x%llx\",\"source\":%u,"
                      "\"w\":%u,\"h\":%u,\"fmt\":%u}",
                printed++ ? "," : "", (unsigned long long)req->texture_va,
                req->texture_source, req->tex_width, req->tex_height,
                req->tex_format);
    }
    for (unsigned e = 0; e < VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u; ++e) {
        const VPGxmExtraTexture *extra = &req->extra_textures[e];
        if (extra->source == VP_GXM_EXTRA_SOURCE_NONE && extra->va == 0u)
            continue;
        fprintf(meta, "%s{\"slot\":%u,\"va\":\"0x%llx\",\"source\":%u,"
                      "\"w\":%u,\"h\":%u,\"fmt\":%u}",
                printed++ ? "," : "", e + 1u,
                (unsigned long long)extra->va, extra->source,
                extra->width, extra->height, extra->tex_format);
    }
    if (req->vertex_texture.source != VP_GXM_EXTRA_SOURCE_NONE ||
        req->vertex_texture.va != 0u)
        fprintf(meta, "%s{\"slot\":\"vs0\",\"va\":\"0x%llx\",\"source\":%u,"
                      "\"w\":%u,\"h\":%u,\"fmt\":%u}",
                printed++ ? "," : "",
                (unsigned long long)req->vertex_texture.va,
                req->vertex_texture.source, req->vertex_texture.width,
                req->vertex_texture.height, req->vertex_texture.tex_format);
    fprintf(meta, "]");
}

static void b34_dump_surface(const char *event, const VPGxmSurface *s,
                             const VPGxmDrawRequest *req,
                             const VPGxmClearRequest *clear) {
    char path[192];
    unsigned writes;
    FILE *file, *meta;
    uint64_t now;

    if (s == NULL || s->color_mem == NULL || s->render_width == 0u ||
        s->render_height == 0u)
        return;
    if (!b34_dump_due()) return;
    if (b34_dump.dumps >= b34_dump.max_dumps) return;
    writes = b34_dump_note_writes(s->va);
    if (writes == 0u || !b34_dump_expect(writes)) return;
    now = sceKernelGetProcessTimeWide();
    snprintf(path, sizeof(path),
             VP_B34_DUMP_PATH "/b34-rt-%03u-%s-0x%llx-%ux%u-f%u.rgba",
             b34_dump.seq, event, (unsigned long long)s->va, s->render_width,
             s->render_height, s->format);
    file = fopen(path, "wb");
    if (file == NULL) {
        glog("VP_B34_PASSDUMP fail=open file=%s\n", path);
        return;
    }
    if (fwrite(s->color_mem, 4u, (size_t)s->render_width * s->render_height, file) !=
        (size_t)s->render_width * s->render_height) {
        glog("VP_B34_PASSDUMP fail=write file=%s\n", path);
        fclose(file);
        return;
    }
    fclose(file);
    meta = fopen(VP_B34_DUMP_PATH "/b34-passes.jsonl", "ab");
    if (meta != NULL) {
        fprintf(meta,
                "{\"seq\":%u,\"t_us\":%llu,\"event\":\"%s\","
                "\"va\":\"0x%llx\",\"w\":%u,\"h\":%u,\"pitch\":%u,"
                "\"format\":%u,\"writes\":%u",
                b34_dump.seq, (unsigned long long)now, event,
                (unsigned long long)s->va, s->render_width, s->render_height, s->render_width,
                s->format, writes);
        if (req != NULL)
            fprintf(meta,
                    ",\"draw_index\":%u,\"hash\":\"0x%x\",\"scope\":%llu,"
                    "\"color_mask\":\"0x%x\",\"depth_test\":%u,"
                    "\"depth_write\":%u,\"viewport\":[%u,%u,%u,%u],"
                    "\"scissor\":%u",
                    req->draw_index, req->hash,
                    (unsigned long long)req->batch_scope, req->color_mask,
                    req->depth_test, req->depth_write, req->viewport_x,
                    req->viewport_y, req->viewport_width,
                    req->viewport_height, req->scissor_enable);
        b34_dump_inputs(meta, req);
        if (clear != NULL)
            fprintf(meta, ",\"clear_mask\":\"0x%x\",\"clear_color\":\"0x%x\"",
                    clear->mask, clear->color);
        fprintf(meta, ",\"file\":\"%s\"}\n",
                strrchr(path, '/') != NULL ? strrchr(path, '/') + 1 : path);
        fclose(meta);
    }
    glog("VP_B34_PASSDUMP seq=%u event=%s va=0x%llx %ux%u writes=%u "
         "bytes=%u\n", b34_dump.seq, event, (unsigned long long)s->va,
         s->render_width, s->render_height, writes,
         s->render_width * s->render_height * 4u);
    b34_dump.seq++;
    b34_dump.dumps++;
}

/* Capture the CPU present buffer together with the exact source surface used
 * to build it. Both reads happen in the worker while the API mutex is held and
 * after the batch/pipeline flush, so no draw can rewrite the source between
 * the two. present_index pairs the records; the source dump is not filtered by
 * the per-VA write thresholds because it is the only way to prove the final
 * scaling edge. */
static void b34_dump_present_pair(const VPGxmSurface *s, const void *buffer,
                                  unsigned width, unsigned height,
                                  unsigned pitch, uint64_t source_va,
                                  unsigned x0, unsigned y0, unsigned w,
                                  unsigned h) {
    char path[192];
    unsigned index;
    uint64_t now;
    FILE *file, *meta;

    if (s == NULL || s->color_mem == NULL || s->render_width == 0u || s->render_height == 0u)
        return;
    if (buffer == NULL || width == 0u || height == 0u) return;
    if (!b34_dump_due()) return;
    if (b34_dump.presents >= b34_dump.max_presents) return;
    x0 = x0 * s->render_width / s->width;
    y0 = y0 * s->render_height / s->height;
    w = w * s->render_width / s->width;
    h = h * s->render_height / s->height;
    index = b34_dump.presents;
    now = sceKernelGetProcessTimeWide();

    snprintf(path, sizeof(path),
             VP_B34_DUMP_PATH "/b34-present-src-%02u-0x%llx-%ux%u-f%u.rgba",
             index, (unsigned long long)s->va, s->render_width, s->render_height,
             s->format);
    file = fopen(path, "wb");
    if (file == NULL) {
        glog("VP_B34_PASSDUMP fail=open file=%s\n", path);
    } else {
        size_t count = (size_t)s->render_width * s->render_height;
        if (fwrite(s->color_mem, 4u, count, file) != count) {
            glog("VP_B34_PASSDUMP fail=write file=%s\n", path);
        }
        fclose(file);
        meta = fopen(VP_B34_DUMP_PATH "/b34-passes.jsonl", "ab");
        if (meta != NULL) {
            fprintf(meta,
                    "{\"seq\":%u,\"t_us\":%llu,\"event\":\"present-source\","
                    "\"present_index\":%u,\"va\":\"0x%llx\",\"w\":%u,\"h\":%u,"
                    "\"pitch\":%u,\"format\":%u,\"rect\":[%u,%u,%u,%u],"
                    "\"file\":\"%s\"}\n",
                    b34_dump.seq, (unsigned long long)now, index,
                    (unsigned long long)s->va, s->render_width, s->render_height, s->render_width,
                    s->format, x0, y0, w, h,
                    strrchr(path, '/') != NULL ? strrchr(path, '/') + 1 : path);
            fclose(meta);
        }
        glog("VP_B34_PASSDUMP seq=%u event=present-source map_index=%u "
             "va=0x%llx %ux%u bytes=%u\n", b34_dump.seq, index,
             (unsigned long long)s->va, s->render_width, s->render_height,
             s->render_width * s->render_height * 4u);
        b34_dump.seq++;
    }

    snprintf(path, sizeof(path), VP_B34_DUMP_PATH "/b34-present-%02u.rgba",
             index);
    file = fopen(path, "wb");
    if (file == NULL) {
        glog("VP_B34_PASSDUMP fail=open file=%s\n", path);
        return;
    }
    if (fwrite(buffer, 4u, (size_t)width * height, file) !=
        (size_t)width * height) {
        glog("VP_B34_PASSDUMP fail=write file=%s\n", path);
        fclose(file);
        return;
    }
    fclose(file);
    meta = fopen(VP_B34_DUMP_PATH "/b34-passes.jsonl", "ab");
    if (meta != NULL) {
        fprintf(meta,
                "{\"seq\":%u,\"t_us\":%llu,\"event\":\"present\","
                "\"present_index\":%u,\"va\":\"0x%llx\",\"w\":%u,\"h\":%u,"
                "\"pitch\":%u,\"format\":8,\"rect\":[%u,%u,%u,%u],"
                "\"file\":\"%s\"}\n",
                b34_dump.seq, (unsigned long long)now, index,
                (unsigned long long)source_va, width, height, pitch, x0, y0,
                w, h, strrchr(path, '/') != NULL ? strrchr(path, '/') + 1
                                                 : path);
        fclose(meta);
    }
    glog("VP_B34_PASSDUMP seq=%u event=present map_index=%u va=0x%llx "
         "%ux%u pitch=%u rect=%u,%u %ux%u\n", b34_dump.seq, index,
         (unsigned long long)source_va, width, height, pitch, x0, y0, w, h);
    b34_dump.seq++;
    b34_dump.presents++;
}

static int run_first_draw(const SceGxmProgram *vs, const SceGxmProgram *fs,
                          uint32_t *pixel_out) {
    SceGxmContext *context = NULL;
    SceGxmRenderTarget *target = NULL;
    SceGxmColorSurface color;
    SceGxmDepthStencilSurface depth;
    SceGxmTexture texture;
    SceGxmContextParams cp;
    SceGxmRenderTargetParams rp;
    void *host_mem = NULL;
    void *rt = NULL;
    void *depth_mem = NULL;
    void *stencil_mem = NULL;
    void *vertex_mem = NULL;
    void *index_mem = NULL;
    void *tex_mem = NULL;
    void *uniform_buffer = NULL;
    const SceGxmProgramParameter *parameter;
    const float area[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    const float scale[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    const float vertices[16] = {0,0,0,0, 1,0,0,0, 0,1,0,0, 1,1,0,0};
    const unsigned short indices[4] = {0, 1, 2, 3};
    unsigned i;
    /* 0 = no owned scene, 1 = scene owned, 2 = EndScene/notification attempted
     * and failed; state 2 must never issue another EndScene. */
    int scene_state = 0;
    int rc;

    if (vs == NULL || fs == NULL || pixel_out == NULL) return -1;

    memset(&cp, 0, sizeof(cp));
    host_mem = calloc(1, SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    cp.hostMem = host_mem;
    cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    cp.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    cp.vdmRingBufferMem = gpu_alloc(cp.vdmRingBufferMemSize, 0, NULL);
    cp.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    cp.vertexRingBufferMem = gpu_alloc(cp.vertexRingBufferMemSize, 0, NULL);
    cp.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    cp.fragmentRingBufferMem = gpu_alloc(cp.fragmentRingBufferMemSize, 0, NULL);
    cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferMem = gpu_alloc(cp.fragmentUsseRingBufferMemSize, 2,
                                             &cp.fragmentUsseRingBufferOffset);
    if (host_mem == NULL || cp.vdmRingBufferMem == NULL || cp.vertexRingBufferMem == NULL ||
        cp.fragmentRingBufferMem == NULL || cp.fragmentUsseRingBufferMem == NULL) {
        glog("VP_GXM_DRAW context alloc failed\n");
        return -1;
    }
    rc = sceGxmCreateContext(&cp, &context);
    if (rc < 0) {
        glog("VP_GXM_DRAW create context rc=%d\n", rc);
        return rc;
    }
    gxm_context = context;
    gxm_main_thread = sceKernelGetThreadId();

    memset(&rp, 0, sizeof(rp));
    rp.width = VP_GXM_WIDTH;
    rp.height = VP_GXM_HEIGHT;
    rp.scenesPerFrame = 2;
    rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rp.driverMemBlock = -1;
    rc = sceGxmCreateRenderTarget(&rp, &target);
    if (rc < 0) {
        glog("VP_GXM_DRAW create target rc=%d\n", rc);
        return rc;
    }
    gxm_target = target;

    rt = gpu_alloc(VP_GXM_BYTES, 0, NULL);
    depth_mem = gpu_alloc(VP_GXM_BYTES, 0, NULL);
    stencil_mem = gpu_alloc(VP_GXM_WIDTH * VP_GXM_HEIGHT, 0, NULL);
    if (rt == NULL || depth_mem == NULL || stencil_mem == NULL) {
        glog("VP_GXM_DRAW surface alloc failed\n");
        return -1;
    }
    gxm_rt = rt;
    rc = sceGxmColorSurfaceInit(&color, SCE_GXM_COLOR_FORMAT_A8B8G8R8,
            SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
            SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, VP_GXM_WIDTH, VP_GXM_HEIGHT,
            VP_GXM_WIDTH, rt);
    if (rc < 0) {
        glog("VP_GXM_DRAW color surface rc=%d\n", rc);
        return rc;
    }
    gxm_color = color;
    rc = sceGxmDepthStencilSurfaceInit(&depth, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
            SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, VP_GXM_WIDTH, depth_mem, stencil_mem);
    if (rc < 0) {
        glog("VP_GXM_DRAW depth surface rc=%d\n", rc);
        return rc;
    }
    gxm_depth = depth;

    vertex_mem = gpu_alloc(sizeof(vertices), 0, NULL);
    index_mem = gpu_alloc(sizeof(indices), 0, NULL);
    tex_mem = gpu_alloc(2 * 2 * 4, 0, NULL);
    if (vertex_mem == NULL || index_mem == NULL || tex_mem == NULL) {
        glog("VP_GXM_DRAW buffer alloc failed\n");
        return -1;
    }
    memcpy(vertex_mem, vertices, sizeof(vertices));
    memcpy(index_mem, indices, sizeof(indices));
    for (i = 0; i < 4; i++) {
        unsigned char *texel = (unsigned char *)tex_mem + i * 4;
        texel[0] = 0x30;
        texel[1] = 0x90;
        texel[2] = 0xf0;
        texel[3] = 0xff;
    }
    rc = sceGxmTextureInitLinear(&texture, tex_mem, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8,
                                 2, 2, 1);
    if (rc < 0) {
        glog("VP_GXM_DRAW texture init rc=%d\n", rc);
        return rc;
    }
    gxm_texture = texture;
    rc = sceGxmBeginScene(context, 0, target, NULL, NULL, NULL, &color, &depth);
    if (rc < 0) {
        glog("VP_GXM_DRAW begin scene rc=%d\n", rc);
        return rc;
    }
    scene_state = 1;
    /* GXM's canonical viewport has a negative y scale; with a positive scale
     * Vita3K collapses the viewport to a single row. */
    sceGxmSetViewport(context, 480.0f, 480.0f, 272.0f, -272.0f, 0.5f, 0.5f);
    sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_NONE, 0, 0, VP_GXM_WIDTH, VP_GXM_HEIGHT);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetFrontStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP,
                              SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, 255, 255);
    sceGxmSetBackStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP,
                             SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, 255, 255);
    sceGxmSetFrontFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
    sceGxmSetBackFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetVertexProgram(context, vertex_program);
    sceGxmSetFragmentProgram(context, fragment_program);

    rc = sceGxmReserveVertexDefaultUniformBuffer(context, &uniform_buffer);
    if (rc < 0) {
        glog("VP_GXM_DRAW reserve vs uniform rc=%d\n", rc);
        goto first_draw_fail;
    }
    rc = init_vertex_uniforms(uniform_buffer, vs);
    if (rc < 0) goto first_draw_fail;
    parameter = sceGxmProgramFindParameterByName(vs, "render_area");
    if (parameter == NULL) {
        glog("VP_GXM_DRAW render_area missing\n");
        rc = -1;
        goto first_draw_fail;
    }
    rc = sceGxmSetUniformDataF(uniform_buffer, parameter, 0, 4, area);
    if (rc < 0) {
        glog("VP_GXM_DRAW render_area rc=%d\n", rc);
        goto first_draw_fail;
    }
    rc = sceGxmReserveFragmentDefaultUniformBuffer(context, &uniform_buffer);
    if (rc < 0) {
        glog("VP_GXM_DRAW reserve fs uniform rc=%d\n", rc);
        goto first_draw_fail;
    }
    memset(uniform_buffer, 0, sceGxmProgramGetDefaultUniformBufferSize(fs));
    parameter = sceGxmProgramFindParameterByName(fs, "fs_cbuf3");
    if (parameter != NULL) {
        rc = sceGxmSetUniformDataF(uniform_buffer, parameter, 0, 4, scale);
        if (rc < 0) {
            glog("VP_GXM_DRAW fs_cbuf3 rc=%d\n", rc);
            goto first_draw_fail;
        }
    }
    parameter = sceGxmProgramFindParameterByName(fs, "tex0");
    if (parameter == NULL) {
        glog("VP_GXM_DRAW tex0 missing\n");
        rc = -1;
        goto first_draw_fail;
    }
    rc = sceGxmSetFragmentTexture(context, sceGxmProgramParameterGetResourceIndex(parameter),
                                  &texture);
    if (rc < 0) {
        glog("VP_GXM_DRAW set texture rc=%d\n", rc);
        goto first_draw_fail;
    }
    rc = sceGxmSetVertexStream(context, 0, vertex_mem);
    if (rc < 0) {
        glog("VP_GXM_DRAW set stream rc=%d\n", rc);
        goto first_draw_fail;
    }
    rc = sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16,
                    index_mem, 4);
    if (rc < 0) {
        glog("VP_GXM_DRAW draw rc=%d\n", rc);
        goto first_draw_fail;
    }
    rc = frame_end_scene(context);
    if (rc < 0) {
        glog("VP_GXM_DRAW end scene rc=%d\n", rc);
        scene_state = 2;
        goto first_draw_fail;
    }
    scene_state = 0;
    *pixel_out = ((const uint32_t *)rt)[0];
    if (0) {
        SceDisplayFrameBuf display;
        memset(&display, 0, sizeof(display));
        display.size = sizeof(display);
        display.base = rt;
        display.pitch = VP_GXM_PITCH;
        display.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        display.width = VP_GXM_WIDTH;
        display.height = VP_GXM_HEIGHT;
    }
    glog("VP_GXM_DRAW rc=0 pixel=0x%08x\n", *pixel_out);
    {
        void *tex2 = gpu_alloc(16, 0, NULL);
        SceGxmTexture t2;
        if (tex2 != NULL) {
            unsigned ti;
            for (ti = 0; ti < 4; ti++) ((uint32_t *)tex2)[ti] = 0xffff00ffu;
            rc = sceGxmTextureInitLinear(&t2, tex2, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8,
                                         2, 2, 1);
            if (rc == 0 &&
                sceGxmBeginScene(context, 0, target, NULL, NULL, NULL, &color, &depth) == 0) {
                scene_state = 1;
                /* GXM's canonical viewport has a negative y scale; with a positive scale
     * Vita3K collapses the viewport to a single row. */
    sceGxmSetViewport(context, 480.0f, 480.0f, 272.0f, -272.0f, 0.5f, 0.5f);
                sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_NONE, 0, 0,
                                    VP_GXM_WIDTH, VP_GXM_HEIGHT);
                sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
                sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
                sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
                sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
                sceGxmSetFrontFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
                sceGxmSetBackFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
                sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
                sceGxmSetVertexProgram(context, vertex_program);
                sceGxmSetFragmentProgram(context, fragment_program);
                rc = sceGxmReserveVertexDefaultUniformBuffer(context, &uniform_buffer);
                if (rc == 0) rc = init_vertex_uniforms(uniform_buffer, vs);
                if (rc == 0) {
                    parameter = sceGxmProgramFindParameterByName(vs, "render_area");
                    if (parameter != NULL)
                        sceGxmSetUniformDataF(uniform_buffer, parameter, 0, 4, area);
                }
                rc = sceGxmReserveFragmentDefaultUniformBuffer(context, &uniform_buffer);
                if (rc == 0) {
                    memset(uniform_buffer, 0, sceGxmProgramGetDefaultUniformBufferSize(fs));
                    parameter = sceGxmProgramFindParameterByName(fs, "fs_cbuf3");
                    if (parameter != NULL)
                        sceGxmSetUniformDataF(uniform_buffer, parameter, 0, 4, scale);
                }
                parameter = sceGxmProgramFindParameterByName(fs, "tex0");
                if (parameter != NULL)
                    sceGxmSetFragmentTexture(context,
                            sceGxmProgramParameterGetResourceIndex(parameter), &t2);
                sceGxmSetVertexStream(context, 0, vertex_mem);
                sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP,
                           SCE_GXM_INDEX_FORMAT_U16, index_mem, 4);
                rc = frame_end_scene(context);
                if (rc < 0) {
                    glog("VP_GXM_SWITCH end scene rc=%d\n", rc);
                    scene_state = 2;
                    goto first_draw_fail;
                }
                scene_state = 0;
                glog("VP_GXM_SWITCH pixel=0x%08x\n",
                     ((const uint32_t *)rt)[(VP_GXM_HEIGHT / 2u) * VP_GXM_WIDTH +
                                            VP_GXM_WIDTH / 2u]);
            }
        }
    }
    gxm_ready = 1;
    return 0;

first_draw_fail:
    {
        int draw_rc = rc;
        if (scene_state == 1) {
            int cleanup_rc = frame_end_scene(context);
            glog("VP_GXM_FIRST_DRAW scene_unwind rc=%d draw_rc=%d context_failed=%d\n",
                 cleanup_rc, draw_rc, cleanup_rc < 0);
        } else if (scene_state == 2) {
            glog("VP_GXM_FIRST_DRAW scene_unwind skipped=end_or_sync_failed "
                 "draw_rc=%d context_failed=1\n", draw_rc);
        }
        return draw_rc;
    }
}

/* Recreate a pair with the draw's real vertex stride. */
static int gxm_rebuild_pair(VPGxmProgramPair *pair, unsigned stride,
                            const uint32_t descriptors[16]) {
    SceGxmVertexAttribute attrs[16];
    SceGxmVertexStream stream;
    SceGxmVertexProgram *vs = NULL;
    SceGxmFragmentProgram *fs = NULL;
    unsigned attr_count = 0;
    int rc;

    memset(attrs, 0, sizeof(attrs));
    for (unsigned i = 0; i < 16u; i++) {
        char name[24];
        const SceGxmProgramParameter *p;
        uint32_t d = descriptors[i];
        VpVertexFormat format;
        SceGxmVertexAttribute *a = &attrs[attr_count];
        snprintf(name, sizeof(name), "in_attr%u", i);
        p = sceGxmProgramFindParameterByName(pair->vs_program, name);
        int has_float = p != NULL;
        int has_uint = 0;
        if (p == NULL) {
            snprintf(name,sizeof(name),"in_attr%u_uint",i);
            p=sceGxmProgramFindParameterByName(pair->vs_program,name);
            has_uint = p != NULL;
        }
        if (p == NULL) continue;
        if (vp_vertex_format_check_input(d, has_float, has_uint) < 0) {
            glog("VP_GXM_ATTRIBUTE type mismatch hash=0x%x attr=%u raw=0x%08x uint=%u\n",
                 pair->hash,i,d,has_uint);
            return -1;
        }
        if (vp_vertex_format_decode(d, stride, &format) < 0) {
            glog("VP_GXM_ATTRIBUTE unsupported hash=0x%x attr=%u raw=0x%08x stride=%u\n",
                 pair->hash, i, d, stride);
            return -1;
        }
        a->offset = (uint16_t)format.source_offset;
        a->streamIndex = 0;
        a->regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(p);
        switch (format.kind) {
        case VP_VERTEX_FORMAT_F32:
            a->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
            a->componentCount = (uint8_t)format.components;
            break;
        case VP_VERTEX_FORMAT_U8N:
            a->format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
            a->componentCount = 4u;
            break;
        case VP_VERTEX_FORMAT_S16N:
            a->format = SCE_GXM_ATTRIBUTE_FORMAT_S16N;
            a->componentCount = 4u;
            break;
        case VP_VERTEX_FORMAT_S8N:
            a->format = SCE_GXM_ATTRIBUTE_FORMAT_S8N;
            a->componentCount = 4u;
            break;
        case VP_VERTEX_FORMAT_U8:
            a->format = SCE_GXM_ATTRIBUTE_FORMAT_U8;
            a->componentCount = 4u;
            break;
        case VP_VERTEX_FORMAT_F16:
            a->format = SCE_GXM_ATTRIBUTE_FORMAT_F16;
            a->componentCount = (uint8_t)format.components;
            break;
        case VP_VERTEX_FORMAT_RGB10A2_UNORM:
            /* The legacy isolated-draw path binds a single vertex stream and
             * has no expansion buffer. Refuse rather than feed packed bits to
             * an F32 fetch; the static path implements the expansion. */
            glog("VP_GXM_ATTRIBUTE packed_unsupported hash=0x%x attr=%u raw=0x%08x\n",
                 pair->hash, i, d);
            return -1;
        default:
            return -1;
        }
        if (format.source_offset > stride || format.source_bytes > stride - format.source_offset)
            return -1;
        glog("VP_GXM_ATTRIBUTE hash=0x%x attr=%u raw=0x%08x offset=%u components=%u format=%u\n",
             pair->hash,i,d,a->offset,a->componentCount,a->format);
        attr_count++;
    }
    stream.stride = (uint16_t)stride;
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    rc = sceGxmShaderPatcherCreateVertexProgram(patcher, pair->vertex_id, attrs, attr_count,
                                                &stream, 1, &vs);
    if (rc < 0) return rc;
    rc = sceGxmShaderPatcherCreateFragmentProgram(patcher, pair->fragment_id,
            SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
            NULL, pair->vs_program, &fs);
    if (rc < 0) return rc;
    pair->vs = vs;
    pair->fs = fs;
    pair->stride = stride;
    memcpy(pair->attributes, descriptors, sizeof(pair->attributes));
    return 0;
}

/* Diagnostic-only scene fingerprint. The selected-frame replay executes one
 * submission in isolation, so any surface filled by guest CPU code outside the
 * submission is empty and the replayed pixels cannot identify the screen. The
 * guest constant buffers are part of the submission itself; their first 64
 * bytes are stable per screen and can be compared with the reference draw
 * capture. Gated by scene-probe.enable; bounded so a long run stays readable. */
static int gxm_scene_probe_state = -1;
static unsigned gxm_scene_probe_lines;
static uint64_t gxm_fnv1a64(const void *data, unsigned bytes) {
    const uint8_t *p = data;
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < bytes; i++) {
        h ^= (uint64_t)p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static int gxm_model_probe_state=-1;
static unsigned gxm_model_probe_count;
static int gxm_model_probe_enabled(void) {
    if (gxm_model_probe_state<0) {
        FILE *marker=fopen("ux0:data/mhgu-vita-port/f8-probe.enable","rb");
        gxm_model_probe_state=marker!=NULL;
        if (marker) fclose(marker);
        glog("VP_GXM_MODEL_PROBE enabled=%d\n",gxm_model_probe_state);
    }
    return gxm_model_probe_state;
}

static void gxm_model_probe_bytes(const char *name,const void *data,size_t bytes) {
    const uint8_t *p=(const uint8_t *)data;
    char prefix[129];
    unsigned n=(unsigned)(bytes<32u?bytes:32u);
    if (!data) {
        glog("VP_GXM_MODEL_PROBE_DATA name=%s bytes=%llu missing=1\n",
             name,(unsigned long long)bytes);
        return;
    }
    if (gxm_model_probe_count == 1u) {
        char path[128];
        snprintf(path,sizeof(path),"ux0:data/mhgu-vita-port/f8-%s.bin",name);
        FILE *capture=fopen(path,"wb");
        if (capture) { fwrite(data,1,bytes,capture); fclose(capture); }
    }
    for (unsigned i=0;i<n;i++) snprintf(prefix+i*2u,3u,"%02x",p[i]);
    prefix[n*2u]=0;
    glog("VP_GXM_MODEL_PROBE_DATA name=%s bytes=%llu fnv=%016llx first=%s\n",
         name,(unsigned long long)bytes,
         (unsigned long long)gxm_fnv1a64(data,(unsigned)(bytes>4096u?4096u:bytes)),
         prefix);
}

static void gxm_model_probe_draw(const VPGxmDrawRequest *req) {
    static const char hex[]="0123456789abcdef";
    char attributes[129];
    if (!req || !gxm_model_probe_enabled() || gxm_model_probe_count>=3u) return;
    ++gxm_model_probe_count;
    for (unsigned i=0;i<16u;i++) {
        uint32_t v=req->attributes[i];
        for (unsigned b=0;b<4u;b++) {
            attributes[i*8u+b*2u]=hex[(v>>(b*8u+4u))&15u];
            attributes[i*8u+b*2u+1u]=hex[(v>>(b*8u))&15u];
        }
    }
    attributes[128]=0;
    glog("VP_GXM_MODEL_PROBE_DRAW idx=%u count=%u stride=%u indexed=%u icount=%u attrs=%s\n",
         req->draw_index,req->count,req->stride,req->indexed,req->index_count,attributes);
    glog("VP_GXM_MODEL_PROBE_STATE idx=%u topology=%u restart=%u/%u target=0x%llx %ux%u "
         "viewport=%u,%u %ux%u scissor=%u (%u,%u)-(%u,%u) depth=%u/%u func=%u "
         "cull=%u face=%u front=%u mask=0x%x blend=%u/%u/%u %u/%u/%u zeta=0x%llx\n",
         req->draw_index,req->topology,req->restart_enabled,req->restart_index,
         (unsigned long long)req->target_va,req->target_width,req->target_height,
         req->viewport_x,req->viewport_y,req->viewport_width,req->viewport_height,
         req->scissor_enable,req->scissor_min_x,req->scissor_min_y,
         req->scissor_max_x,req->scissor_max_y,req->depth_test,req->depth_write,
         req->depth_func,req->cull_enable,req->cull_face,req->front_face,
         req->color_mask,req->blend_color_op,req->blend_color_src,
         req->blend_color_dst,req->blend_alpha_op,req->blend_alpha_src,
         req->blend_alpha_dst,(unsigned long long)req->depth_state.va);
    gxm_model_probe_bytes("vertices",req->vertices,
        (size_t)req->count*req->stride);
    if (req->indexed) {
        size_t index_size=req->index_format==SCE_GXM_INDEX_FORMAT_U32?4u:2u;
        size_t index_bytes=(size_t)req->index_count*index_size;
        gxm_model_probe_bytes("indices",req->indices,index_bytes);
    }
    for (unsigned stage=0;stage<2u;stage++) for (unsigned bank=0;bank<18u;bank++) {
        const VPGxmUniform *u=&req->stage_cbuf[stage][bank];
        if (u->data) {
            char name[24];
            snprintf(name,sizeof(name),"%s_cbuf%u",stage==0u?"vs":"fs",bank);
            gxm_model_probe_bytes(name,u->data,u->size);
        }
    }
    if (req->texels) {
        size_t bytes=vp_texture_upload_bytes(req->tex_width,req->tex_height,req->tex_format);
        gxm_model_probe_bytes("fragment_tex0",req->texels,bytes);
    }
    if (req->vertex_texture.texels) {
        size_t bytes=vp_texture_upload_bytes(req->vertex_texture.width,
            req->vertex_texture.height,req->vertex_texture.tex_format);
        gxm_model_probe_bytes("vertex_tex0",req->vertex_texture.texels,bytes);
    }
    for (unsigned i=0;i<VP_GXM_FRAGMENT_SAMPLER_LIMIT-1u;i++) if (req->extra_textures[i].texels) {
        const VPGxmExtraTexture *t=&req->extra_textures[i];
        size_t bytes=(size_t)t->width*t->height*4u;
        char name[24];
        snprintf(name,sizeof(name),"fragment_tex%u",i+1u);
        gxm_model_probe_bytes(name,t->texels,bytes);
    }
}

static void gxm_scene_probe_uniform(const char *stage, unsigned bank,
                                    const VPGxmUniform *b, unsigned floats) {
    unsigned bytes = floats * 4u;
    unsigned probe_bytes = bytes < 64u ? bytes : 64u;
    if (gxm_scene_probe_state < 0) {
        FILE *marker = fopen("ux0:data/mhgu-vita-port/scene-probe.enable", "rb");
        gxm_scene_probe_state = marker != NULL;
        if (marker != NULL) fclose(marker);
        glog("VP_GXM_SCENE_PROBE enabled=%d\n", gxm_scene_probe_state);
    }
    if (!gxm_scene_probe_state || gxm_scene_probe_lines >= 256u) return;
    gxm_scene_probe_lines++;
    glog("VP_GXM_CBUF_PROBE stage=%s bank=%u floats=%u bytes=%u fnv=%016llx\n",
         stage, bank, floats, b->size,
         (unsigned long long)gxm_fnv1a64(b->data, probe_bytes));
}

static int gxm_creation_dump = -1;
static void creation_dump_bytes(unsigned n, const char *name, const void *data, size_t bytes) {
    if (!data || !bytes) return;
    char path[128];
    snprintf(path,sizeof(path),"ux0:data/mhgu-vita-port/creation-%u-%s.bin",n,name);
    FILE *file=fopen(path,"wb");
    if (file) { fwrite(data,1,bytes,file); fclose(file); }
}
#include "gxm_skin_probe.inc"

static void gxm_creation_probe(const VPGxmDrawRequest *req) {
    static unsigned armed,count;
    gxm_creation_dump = -1;
    if (!gxm_model_probe_enabled() || count>=64u) return;
    if (req->hash==0x17edu) armed=1u;
    if (!armed) return;
    unsigned n=count++;
    if (n<12u) {
        gxm_creation_dump=(int)n;
        unsigned index_bytes=req->index_format==2u?4u:req->index_format==1u?2u:1u;
        creation_dump_bytes(n,"indices",req->indices,(size_t)req->index_count*index_bytes);
        creation_dump_bytes(n,"tex0",req->texels,
            vp_texture_upload_bytes(req->tex_width,req->tex_height,req->tex_format));
        const VPGxmExtraTexture *vt=&req->vertex_texture;
        creation_dump_bytes(n,"vertex-tex0",vt->texels,
            vp_texture_upload_bytes(vt->width,vt->height,vt->tex_format));
        glog("VP_CREATION_TEXTURE n=%u hash=0x%x tex=%ux%u/%u vertex=%ux%u/%u alpha=%u/%u/%f\n",
            n,req->hash,req->tex_width,req->tex_height,req->tex_format,
            vt->width,vt->height,vt->tex_format,req->alpha_test_enable,
            req->alpha_test_func,req->alpha_test_ref);
    }
    FILE *meta=fopen("ux0:data/mhgu-vita-port/creation-draws.jsonl",n?"ab":"wb");
    if (meta) {
        fprintf(meta,"{\"n\":%u,\"hash\":%u,\"count\":%u,\"stride\":%u,"
            "\"cull\":[%u,%u,%u],\"depth\":[%u,%u,%u],\"mask\":%u,\"attributes\":[",
            n,req->hash,req->count,req->stride,req->cull_enable,req->cull_face,
            req->front_face,req->depth_test,req->depth_write,req->depth_func,req->color_mask);
        for (unsigned i=0;i<16u;i++) fprintf(meta,"%s%u",i?",":"",req->attributes[i]);
        fprintf(meta,"]}\n");fclose(meta);
    }
    char path[128];
    snprintf(path,sizeof(path),"ux0:data/mhgu-vita-port/creation-%u-vertices.bin",n);
    FILE *f=fopen(path,"wb");
    if (f) {fwrite(req->vertices,req->stride,req->count,f);fclose(f);}
    for (unsigned stage=0;stage<2u;stage++) for (unsigned bank=0;bank<18u;bank++) {
        const VPGxmUniform *u=&req->stage_cbuf[stage][bank];
        if (!u->data) continue;
        snprintf(path,sizeof(path),"ux0:data/mhgu-vita-port/creation-%u-%s-cbuf%u.bin",
                 n,stage?"fs":"vs",bank);
        f=fopen(path,"wb");
        if (f) {fwrite(u->data,1,u->size<512u?u->size:512u,f);fclose(f);}
    }
}

/* Cg lowers each guest bank into a named array in the default uniform buffer.
 * Its packed resource offsets are compiler-owned: use parameter metadata,
 * rather than binding a raw guest bank as GXM buffer zero. */
static int set_guest_uniforms(void *buffer, const SceGxmProgram *program,
                              const char *prefix, const VPGxmUniform banks[18]) {
    unsigned bank;
    for (bank = 0; bank < 18u; bank++) {
        char name[32];
        const SceGxmProgramParameter *p;
        unsigned floats;
        int rc;
        snprintf(name, sizeof(name), "%s_cbuf%u", prefix, bank);
        p = sceGxmProgramFindParameterByName(program, name);
        if (p == NULL) continue;
        floats = sceGxmProgramParameterGetComponentCount(p) *
                 sceGxmProgramParameterGetArraySize(p);
        if (banks[bank].data == NULL || floats > banks[bank].size / sizeof(float)) {
            glog("VP_GXM_UNIFORM missing=%s need=%u available=%u\n",
                 name, floats * 4u, banks[bank].size);
            return -1;
        }
        if (buffer == NULL) continue;
        gxm_scene_probe_uniform(prefix, bank, &banks[bank], floats);
        rc = sceGxmSetUniformDataF(buffer, p, 0, floats, banks[bank].data);
        {
            static unsigned uniform_success_logs;
            if (rc < 0 || uniform_success_logs < 64u) {
                glog("VP_GXM_UNIFORM name=%s floats=%u rc=%d\n", name, floats, rc);
                if (rc >= 0) ++uniform_success_logs;
            }
        }
        if (rc < 0) return rc;
    }
    return 0;
}

/* textureLodOffset is lowered to normalized UV + offset / texture extent.
 * These uniforms belong to the translated shader, not a guest constant bank.
 * Leaving them zero makes all bone-matrix columns sample the same texel. */
static int set_texture_extent_uniforms(void *buffer, const SceGxmProgram *program,
                                       const VPGxmDrawRequest *req, int vertex) {
    unsigned slots = vertex ? 3u : VP_GXM_FRAGMENT_SAMPLER_LIMIT;
    for (unsigned slot=0;slot<slots;slot++) {
        char name[40];
        unsigned format=0u,packed_value=0u;
        const SceGxmProgramParameter *packed;
        if (vertex && slot==0u) format=req->vertex_texture.tex_format;
        else if (!vertex && slot==0u) format=req->tex_format;
        else if (!vertex) format=req->extra_textures[slot-1u].tex_format;
        if (format==VP_TEXTURE_UPLOAD_RGBA32F_RG32F) {
            if (!vertex) {
                glog("VP_GXM_TEXTURE_PACKED_UNSUPPORTED stage=fs slot=%u\n",slot);
                return -1;
            }
            packed_value=(req->vertex_texture.sampler[0]&7u)==2u ? 2u : 1u;
        }
        snprintf(name,sizeof(name),"tex%u_rgba32f_packed",slot);
        packed=sceGxmProgramFindParameterByName(program,name);
        if (packed_value && packed==NULL) {
            glog("VP_GXM_TEXTURE_PACKED_UNIFORM_MISSING stage=%s slot=%u "
                 "format=0x%x detail=packed-texture-without-selector\n",
                 vertex?"vs":"fs",slot,format);
            return -1;
        }
        if (packed!=NULL) {
            float value=(float)packed_value;
            int rc=sceGxmSetUniformDataF(buffer,packed,0,1,&value);
            if (rc<0) return rc;
        }
        snprintf(name,sizeof(name),"tex%u_inv_size",slot);
        const SceGxmProgramParameter *p=sceGxmProgramFindParameterByName(program,name);
        if (!p) continue;
        unsigned width=0,height=0;
        if (vertex && slot==0u) {
            width=req->vertex_texture.width; height=req->vertex_texture.height;
        } else if (!vertex && slot==0u) {
            width=req->tex_width; height=req->tex_height;
        } else if (!vertex) {
            width=req->extra_textures[slot-1u].width;
            height=req->extra_textures[slot-1u].height;
        }
        if (!width || !height) {
            glog("VP_GXM_TEXTURE_EXTENT missing stage=%s name=%s\n",vertex?"vs":"fs",name);
            return -1;
        }
        float inverse[2]={1.0f/(float)width,1.0f/(float)height};
        int rc=sceGxmSetUniformDataF(buffer,p,0,2,inverse);
        static unsigned logs[2];
        if (rc<0 || logs[vertex != 0]++<8u)
            glog("VP_GXM_TEXTURE_EXTENT stage=%s name=%s size=%ux%u rc=%d\n",
                 vertex?"vs":"fs",name,width,height,rc);
        if (rc<0) return rc;
    }
    return 0;
}

/* Allocate the fixed full-frame staging set exactly once. The legacy
 * gxm_game_draw path keeps its own 64 KiB / 4096-vertex guards, but it shares
 * these buffers, so even a legacy first use must allocate the full-frame
 * capacity. Otherwise a later full-frame draw could copy up to 256 KiB into a
 * 64 KiB allocation. Each pointer is ensured independently so a partial
 * allocation failure can be retried safely. */
static int gxm_game_staging_ensure(void) {
    if (gxm_game_vertices == NULL)
        gxm_game_vertices = gpu_alloc(VP_VERTEX_WINDOW_MAX_BYTES, 0, NULL);
    if (gxm_game_indices == NULL)
        gxm_game_indices = gpu_alloc(VP_VERTEX_WINDOW_MAX_INDEX_BYTES, 0, NULL);
    if (gxm_present == NULL)
        gxm_present = gpu_alloc_purpose(VP_GXM_BYTES, 0, NULL, 1);
    if (gxm_game_vertices == NULL || gxm_game_indices == NULL ||
        gxm_present == NULL)
        return -1;
    return 0;
}

int vp_gxm_game_draw(const void *vertices, unsigned count, unsigned stride,
                     unsigned hash, unsigned viewport_transform,
                     const uint32_t attributes[16],
                     unsigned screen_width, unsigned screen_height,
                     const float *fragment_uniforms,
                     const void *texels, unsigned tex_width, unsigned tex_height,
                     const uint32_t sampler[2],
                     const VPGxmUniform stage_cbuf[2][18],
                     uint32_t *center_pixel, unsigned *nonzero) {
    void *uniform_buffer = NULL;
    const SceGxmProgramParameter *parameter;
    float area[4] = {0, 0, 0, 0};
    float scale[4] = {1, 1, 0, 0};
    unsigned i;
    /* 0 = no owned scene, 1 = scene owned, 2 = EndScene/notification attempted
     * and failed; state 2 must never issue another EndScene. */
    int scene_state = 0;
    int rc;

    if (gxm_scene_poisoned) return frame_context_reject("game-draw");
    if (!gxm_ready || gxm_context == NULL || gxm_target == NULL || gxm_rt == NULL ||
        vertex_program == NULL || fragment_program == NULL || vertices == NULL ||
        center_pixel == NULL || nonzero == NULL ||
        count < 3 || count > 4096 || stride == 0 ||
        (uint64_t)count * stride > 64u * 1024u) {
        return -1;
    }
    {
        unsigned pi;
        const VPGxmProgramPair *pair = NULL;
        for (pi = 0; pi < gxm_pair_count; pi++)
            if (gxm_pairs[pi].hash == hash &&
                gxm_pairs[pi].viewport_transform == viewport_transform) pair = &gxm_pairs[pi];
        if (pair == NULL) {
            glog("VP_GXM_PROGRAM missing hash=0x%x viewport=%u\n", hash, viewport_transform);
            return -1;
        }
        if (pair != NULL) {
            if (pair->stride != stride || memcmp(pair->attributes, attributes, sizeof(pair->attributes))) {
                rc = gxm_rebuild_pair((VPGxmProgramPair *)pair, stride, attributes);
                if (rc < 0) return rc;
            }
            vertex_program = pair->vs;
            fragment_program = pair->fs;
            gxm_vs_program = pair->vs_program;
            gxm_fs_program = pair->fs_program;
        }
    }
    if (gxm_game_staging_ensure() < 0) return -1;
    if (sceGxmProgramFindParameterByName(gxm_fs_program, "tex0") != NULL &&
        (texels == NULL || tex_width == 0u || tex_height == 0u)) {
        glog("VP_GXM_TEXTURE missing real tex0 hash=0x%x\n", hash);
        return -1;
    }
    if (set_guest_uniforms(NULL, gxm_vs_program, "vs", stage_cbuf[0]) < 0 ||
        set_guest_uniforms(NULL, gxm_fs_program, "fs", stage_cbuf[1]) < 0) return -1;
    memcpy(gxm_game_vertices, vertices, (size_t)count * stride);
    for (i = 0; i < count; i++)
        ((unsigned short *)gxm_game_indices)[i] = (unsigned short)i;
    area[0] = (float)screen_width;
    area[1] = (float)screen_height;
    area[2] = 0.0f;
    area[3] = 0.0f;

    rc = gxm_resize_rt(screen_width, screen_height);
    if (rc < 0) return rc;
    memset(gxm_rt, 0, (size_t)gxm_rt_w * gxm_rt_h * 4u);
    rc = sceGxmBeginScene(gxm_context, 0, gxm_target, NULL, NULL, NULL,
                          &gxm_color, &gxm_depth);
    if (rc < 0) {
        glog("VP_GXM_GAME_DRAW begin rc=%d\n", rc);
        return rc;
    }
    scene_state = 1;
    /* Canonical GXM viewport: negated y scale (see the frame executor). */
    sceGxmSetViewport(gxm_context, (float)gxm_rt_w * 0.5f, (float)gxm_rt_w * 0.5f,
                      (float)gxm_rt_h * 0.5f, -(float)gxm_rt_h * 0.5f, 0.5f, 0.5f);
    sceGxmSetRegionClip(gxm_context, SCE_GXM_REGION_CLIP_NONE, 0, 0, gxm_rt_w, gxm_rt_h);
    sceGxmSetFrontDepthFunc(gxm_context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(gxm_context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(gxm_context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(gxm_context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetFrontStencilFunc(gxm_context, SCE_GXM_STENCIL_FUNC_ALWAYS,
                              SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                              SCE_GXM_STENCIL_OP_KEEP, 255, 255);
    sceGxmSetBackStencilFunc(gxm_context, SCE_GXM_STENCIL_FUNC_ALWAYS,
                             SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                             SCE_GXM_STENCIL_OP_KEEP, 255, 255);
    sceGxmSetFrontFragmentProgramEnable(gxm_context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
    sceGxmSetBackFragmentProgramEnable(gxm_context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
    sceGxmSetCullMode(gxm_context, SCE_GXM_CULL_NONE);
    sceGxmSetVertexProgram(gxm_context, vertex_program);
    sceGxmSetFragmentProgram(gxm_context, fragment_program);
    if (texels != NULL && tex_width != 0 && tex_height != 0) {
        if (gxm_game_texture_mem == NULL || gxm_game_texture_width != tex_width ||
            gxm_game_texture_height != tex_height) {
            gxm_game_texture_mem = gpu_alloc(tex_width * tex_height * 4u, 0, NULL);
            if (gxm_game_texture_mem == NULL) {
                rc = -1;
                goto game_draw_fail;
            }
            rc = sceGxmTextureInitLinear(&gxm_game_texture, gxm_game_texture_mem,
                    SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, tex_width, tex_height, 1);
            if (rc < 0) goto game_draw_fail;
            gxm_game_texture_width = tex_width;
            gxm_game_texture_height = tex_height;
        }
        memcpy(gxm_game_texture_mem, texels, (size_t)tex_width * tex_height * 4u);
        if ((sampler[1] & 3u) < 1u || (sampler[1] & 3u) > 2u ||
            ((sampler[1] >> 4) & 3u) < 1u || ((sampler[1] >> 4) & 3u) > 2u ||
            (sampler[0] & 7u) > 2u || ((sampler[0] >> 3) & 7u) > 2u) {
            glog("VP_GXM_SAMPLER unsupported raw=%08x,%08x\n", sampler[0], sampler[1]);
            rc = -1;
            goto game_draw_fail;
        }
        sceGxmTextureSetMagFilter(&gxm_game_texture, (sampler[1] & 3u) == 2u ?
                                    SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMinFilter(&gxm_game_texture, ((sampler[1] >> 4) & 3u) == 2u ?
                                    SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetUAddrMode(&gxm_game_texture, (SceGxmTextureAddrMode)(sampler[0] & 7u));
        sceGxmTextureSetVAddrMode(&gxm_game_texture, (SceGxmTextureAddrMode)((sampler[0] >> 3) & 7u));
        glog("VP_GXM_SAMPLER raw=%08x,%08x\n", sampler[0], sampler[1]);
    }

    rc = sceGxmReserveVertexDefaultUniformBuffer(gxm_context, &uniform_buffer);
    if (rc < 0) goto game_draw_fail;
    rc = init_vertex_uniforms(uniform_buffer, gxm_vs_program);
    if (rc < 0) goto game_draw_fail;
    rc = set_guest_uniforms(uniform_buffer, gxm_vs_program, "vs", stage_cbuf[0]);
    if (rc < 0) goto game_draw_fail;
    parameter = sceGxmProgramFindParameterByName(gxm_vs_program, "render_area");
    if (parameter != NULL) {
        rc = sceGxmSetUniformDataF(uniform_buffer, parameter, 0, 4, area);
        if (rc < 0) goto game_draw_fail;
    }
    rc = sceGxmReserveFragmentDefaultUniformBuffer(gxm_context, &uniform_buffer);
    if (rc < 0) goto game_draw_fail;
    memset(uniform_buffer, 0, sceGxmProgramGetDefaultUniformBufferSize(gxm_fs_program));
    rc = set_guest_uniforms(uniform_buffer, gxm_fs_program, "fs", stage_cbuf[1]);
    if (rc < 0) goto game_draw_fail;
    parameter = sceGxmProgramFindParameterByName(gxm_fs_program, "fs_cbuf3");
    if (parameter != NULL && stage_cbuf[1][3].data == NULL) {
        const float *uniforms = fragment_uniforms != NULL ? fragment_uniforms : scale;
        rc = sceGxmSetUniformDataF(uniform_buffer, parameter, 0, 4, uniforms);
        if (rc < 0) goto game_draw_fail;
    }
    parameter = sceGxmProgramFindParameterByName(gxm_fs_program, "f_mask");
    if (parameter != NULL) {
        if (gxm_mask_mem == NULL) {
            gxm_mask_mem = gpu_alloc(4u, 0, NULL);
            if (gxm_mask_mem != NULL) {
                *(uint32_t *)gxm_mask_mem = 0xffffffffu;
                sceGxmTextureInitLinear(&gxm_mask_texture, gxm_mask_mem,
                        SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, 1, 1, 1);
                sceGxmTextureSetMinFilter(&gxm_mask_texture, SCE_GXM_TEXTURE_FILTER_POINT);
                sceGxmTextureSetMagFilter(&gxm_mask_texture, SCE_GXM_TEXTURE_FILTER_POINT);
            }
        }
        if (gxm_mask_mem != NULL) {
            rc = sceGxmSetFragmentTexture(gxm_context,
                    sceGxmProgramParameterGetResourceIndex(parameter), &gxm_mask_texture);
            glog("VP_GXM_MASK_BIND rc=%d\n", rc);
        }
    }
    parameter = sceGxmProgramFindParameterByName(gxm_fs_program, "tex0");
    if (parameter != NULL) {
        rc = sceGxmSetFragmentTexture(gxm_context,
                sceGxmProgramParameterGetResourceIndex(parameter),
                gxm_game_texture_width != 0 ? &gxm_game_texture : &gxm_texture);
    }
    glog("VP_GXM_GAME_TEX texels=%p w=%u h=%u bind_game=%d\n", texels,
         gxm_game_texture_width, gxm_game_texture_height,
         gxm_game_texture_width != 0 ? 1 : 0);
    if (rc < 0) goto game_draw_fail;
    rc = sceGxmSetVertexStream(gxm_context, 0, gxm_game_vertices);
    if (rc < 0) goto game_draw_fail;
    rc = sceGxmDraw(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP,
                    SCE_GXM_INDEX_FORMAT_U16, gxm_game_indices, count);
    if (rc < 0) {
        glog("VP_GXM_GAME_DRAW draw rc=%d\n", rc);
        goto game_draw_fail;
    }
    rc = frame_end_scene(gxm_context);
    if (rc < 0) {
        glog("VP_GXM_GAME_DRAW end scene rc=%d\n", rc);
        scene_state = 2;
        frame_poison_context(rc);
        goto game_draw_fail;
    }
    scene_state = 0;

    *center_pixel = ((const uint32_t *)gxm_rt)[(gxm_rt_h / 2u) * gxm_rt_w +
                                               gxm_rt_w / 2u];
    *nonzero = 0;
    for (i = 0; i < gxm_rt_w * gxm_rt_h; i += 16u) {
        if (((const uint32_t *)gxm_rt)[i] != 0u) (*nonzero)++;
    }
    {
        static unsigned dumps;
        if (dumps++ == 0u) {
            FILE *f = fopen("ux0:data/mhgu-vita-port/m2-rt.rgba", "wb");
            if (f != NULL) {
                fwrite(gxm_rt, 4u, gxm_rt_w * gxm_rt_h, f);
                fclose(f);
            }
        }
    }
    {
        SceDisplayFrameBuf display;
        for (unsigned y = 0; y < VP_GXM_HEIGHT; y++) {
            unsigned sy = y * gxm_rt_h / VP_GXM_HEIGHT;
            for (unsigned x = 0; x < VP_GXM_WIDTH; x++)
                ((uint32_t *)gxm_present)[y*VP_GXM_WIDTH+x] =
                    ((const uint32_t *)gxm_rt)[sy*gxm_rt_w+x*gxm_rt_w/VP_GXM_WIDTH];
        }
        memset(&display, 0, sizeof(display));
        display.size = sizeof(display);
        display.base = gxm_present;
        display.pitch = VP_GXM_PITCH;
        display.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        display.width = VP_GXM_WIDTH;
        display.height = VP_GXM_HEIGHT;
        rc = sceDisplaySetFrameBuf(&display, SCE_DISPLAY_SETBUF_NEXTFRAME);
        if (rc < 0) return rc;
        sceDisplayWaitVblankStart();
    }
    glog("VP_GXM_GAME_DRAW rc=0 count=%u stride=%u screen=%ux%u center=0x%08x nonzero=%u\n",
         count, stride, screen_width, screen_height, *center_pixel, *nonzero);
    return 0;

game_draw_fail:
    {
        int draw_rc = rc;
        if (scene_state == 1) {
            int cleanup_rc = frame_end_scene(gxm_context);
            if (cleanup_rc < 0) {
                frame_poison_context(cleanup_rc);
                glog("VP_GXM_GAME_DRAW scene_unwind rc=%d draw_rc=%d "
                     "context_failed=1\n", cleanup_rc, draw_rc);
            } else {
                glog("VP_GXM_GAME_DRAW scene_unwind rc=0 draw_rc=%d\n", draw_rc);
            }
        } else if (scene_state == 2) {
            glog("VP_GXM_GAME_DRAW scene_unwind skipped=end_or_sync_failed "
                 "draw_rc=%d context_failed=1\n", draw_rc);
        }
        return draw_rc;
    }
}

/* ---- Full-frame execution (M2 G2/G3/G4) ------------------------------- */

static uint64_t gxm_last_target_va;
static unsigned gxm_last_target_w;
static unsigned gxm_last_target_h;
static unsigned gxm_frame_dumps;
static unsigned gxm_frame_passes;

/* M3 per-draw dumps. Unlike the M2 pass dump this writes the target after
 * every executed draw, so a reference readback of the same draw can be
 * compared one to one. Enabled only by the marker, bounded to 48 files. */
static int gxm_draw_dump_enabled = -1;
static unsigned gxm_draw_dumps;
static FILE *gxm_draw_dump_manifest;

static int draw_dump_enabled(void) {
    if (gxm_draw_dump_enabled < 0) {
        FILE *marker = fopen("ux0:data/mhgu-vita-port/m3-drawdump.enable", "rb");
        gxm_draw_dump_enabled = marker != NULL;
        if (marker != NULL) fclose(marker);
        if (gxm_draw_dump_enabled) {
            gxm_draw_dump_manifest =
                fopen("ux0:data/mhgu-vita-port/m3-draws.jsonl", "wb");
        }
        glog("VP_GXM_DRAWDUMP enabled=%d\n", gxm_draw_dump_enabled);
    }
    return gxm_draw_dump_enabled;
}

static SceGxmPrimitiveType topology_to_gxm(uint32_t topology) {
    switch (topology) {
    case 0u: return SCE_GXM_PRIMITIVE_POINTS;
    case 1u: return SCE_GXM_PRIMITIVE_LINES;
    case 4u: return SCE_GXM_PRIMITIVE_TRIANGLES;
    case 5u: return SCE_GXM_PRIMITIVE_TRIANGLE_STRIP;
    case 6u: return SCE_GXM_PRIMITIVE_TRIANGLE_FAN;
    default: return (SceGxmPrimitiveType)0xffffffffu;
    }
}

static int depth_func_to_gxm(uint32_t op, SceGxmDepthFunc *out) {
    switch (op) {
    case 1u: case 0x200u: *out = SCE_GXM_DEPTH_FUNC_NEVER; return 0;
    case 2u: case 0x201u: *out = SCE_GXM_DEPTH_FUNC_LESS; return 0;
    case 3u: case 0x202u: *out = SCE_GXM_DEPTH_FUNC_EQUAL; return 0;
    case 4u: case 0x203u: *out = SCE_GXM_DEPTH_FUNC_LESS_EQUAL; return 0;
    case 5u: case 0x204u: *out = SCE_GXM_DEPTH_FUNC_GREATER; return 0;
    case 6u: case 0x205u: *out = SCE_GXM_DEPTH_FUNC_NOT_EQUAL; return 0;
    case 7u: case 0x206u: *out = SCE_GXM_DEPTH_FUNC_GREATER_EQUAL; return 0;
    case 8u: case 0x207u: *out = SCE_GXM_DEPTH_FUNC_ALWAYS; return 0;
    default: return -1;
    }
}

/* VP_HOST_TEST_EXTRACT:blend_ops_begin */
static int blend_factor_to_gxm(uint32_t factor, SceGxmBlendFactor *out) {
    switch (factor) {
    case 1u: case 0x4000u: *out = SCE_GXM_BLEND_FACTOR_ZERO; return 0;
    case 2u: case 0x4001u: *out = SCE_GXM_BLEND_FACTOR_ONE; return 0;
    case 3u: case 0x4300u: *out = SCE_GXM_BLEND_FACTOR_SRC_COLOR; return 0;
    case 4u: case 0x4301u: *out = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; return 0;
    case 5u: case 0x4302u: *out = SCE_GXM_BLEND_FACTOR_SRC_ALPHA; return 0;
    case 6u: case 0x4303u: *out = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; return 0;
    case 7u: case 0x4304u: *out = SCE_GXM_BLEND_FACTOR_DST_ALPHA; return 0;
    case 8u: case 0x4305u: *out = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; return 0;
    case 9u: case 0x4306u: *out = SCE_GXM_BLEND_FACTOR_DST_COLOR; return 0;
    case 0xau: case 0x4307u: *out = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR; return 0;
    case 0xbu: case 0x4308u: *out = SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE; return 0;
    default: return -1;
    }
}

static int blend_op_to_gxm(uint32_t op, SceGxmBlendFunc *out) {
    switch (op) {
    case 1u: case 0x8006u: *out = SCE_GXM_BLEND_FUNC_ADD; return 0;
    case 2u: case 0x800au: *out = SCE_GXM_BLEND_FUNC_SUBTRACT; return 0;
    case 3u: case 0x800bu: *out = SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT; return 0;
    case 4u: case 0x8007u: *out = SCE_GXM_BLEND_FUNC_MIN; return 0;
    case 5u: case 0x8008u: *out = SCE_GXM_BLEND_FUNC_MAX; return 0;
    default: return -1;
    }
}
/* VP_HOST_TEST_EXTRACT:blend_ops_end */

/* VP_HOST_TEST_EXTRACT:color_mask_begin */
static uint8_t color_mask_to_gxm(uint32_t mask) {
    uint8_t out = 0;
    if ((mask & 0x1u) != 0u) out |= SCE_GXM_COLOR_MASK_R;
    if ((mask & 0x10u) != 0u) out |= SCE_GXM_COLOR_MASK_G;
    if ((mask & 0x100u) != 0u) out |= SCE_GXM_COLOR_MASK_B;
    if ((mask & 0x1000u) != 0u) out |= SCE_GXM_COLOR_MASK_A;
    return out;
}
/* VP_HOST_TEST_EXTRACT:color_mask_end */

/* VP_HOST_TEST_EXTRACT:blend_build_begin */
static int build_blend_info(const VPGxmDrawRequest *req, SceGxmBlendInfo *info) {
    SceGxmBlendFunc color_func, alpha_func;
    SceGxmBlendFactor color_src, color_dst, alpha_src, alpha_dst;

    memset(info, 0, sizeof(*info));
    info->colorMask = color_mask_to_gxm(req->color_mask);
    if (blend_op_to_gxm(req->blend_color_op, &color_func) < 0 ||
        blend_op_to_gxm(req->blend_alpha_op, &alpha_func) < 0 ||
        blend_factor_to_gxm(req->blend_color_src, &color_src) < 0 ||
        blend_factor_to_gxm(req->blend_color_dst, &color_dst) < 0 ||
        blend_factor_to_gxm(req->blend_alpha_src, &alpha_src) < 0 ||
        blend_factor_to_gxm(req->blend_alpha_dst, &alpha_dst) < 0) {
        glog("VP_GXM_BLEND unsupported op=%u,%u src=%u,%u dst=%u,%u mask=0x%x\n",
             req->blend_color_op, req->blend_alpha_op,
             req->blend_color_src, req->blend_alpha_src,
             req->blend_color_dst, req->blend_alpha_dst, req->color_mask);
        return -1;
    }
    info->colorFunc = color_func;
    info->alphaFunc = alpha_func;
    info->colorSrc = color_src;
    info->colorDst = color_dst;
    info->alphaSrc = alpha_src;
    info->alphaDst = alpha_dst;
    return 0;
}
/* VP_HOST_TEST_EXTRACT:blend_build_end */

/* Number of sampler parameters declared by a GXP program. NVN may leave a
 * texture bound from an earlier draw even when the current fragment program
 * never samples it, so an unresolvable tex0 is only fatal when the program
 * actually declares a sampler. */
static unsigned program_sampler_count(const SceGxmProgram *program) {
    unsigned count, i, samplers = 0;

    if (program == NULL) return 0;
    count = sceGxmProgramGetParameterCount(program);
    for (i = 0; i < count; i++) {
        const SceGxmProgramParameter *parameter =
                sceGxmProgramGetParameter(program, i);
        if (parameter != NULL &&
            sceGxmProgramParameterGetCategory(parameter) ==
                    SCE_GXM_PARAMETER_CATEGORY_SAMPLER)
            samplers++;
    }
    return samplers;
}

/* Validate the declared samplers against the bounded tex0..tex3 fragment ABI.
 * Fragment programs may name tex0..tex3 exactly once each; vertex programs
 * support only their existing tex0 resource. A name outside that set, a
 * duplicate slot or more than four fragment samplers is refused before any
 * texture unit is bound. Returns 0 with the declared count, or -1. */
static int program_sampler_layout(const SceGxmProgram *program, unsigned stage,
                                  unsigned *count_out) {
    unsigned count, i, samplers = 0, seen = 0;

    if (count_out != NULL) *count_out = 0;
    if (program == NULL) return 0;
    if (stage != VP_STATIC_INDEX_STAGE_VERTEX &&
        stage != VP_STATIC_INDEX_STAGE_FRAGMENT)
        return -1;
    count = sceGxmProgramGetParameterCount(program);
    for (i = 0; i < count; i++) {
        const SceGxmProgramParameter *parameter =
                sceGxmProgramGetParameter(program, i);
        const char *name;
        unsigned slot;
        if (parameter == NULL ||
            sceGxmProgramParameterGetCategory(parameter) !=
                    SCE_GXM_PARAMETER_CATEGORY_SAMPLER)
            continue;
        name = sceGxmProgramParameterGetName(parameter);
        if (vp_gxm_sampler_name_slot(name,
                stage == VP_STATIC_INDEX_STAGE_VERTEX ? 1u : 0u, &slot) < 0)
            return -1;
        if (seen & (1u << slot)) return -1;
        seen |= 1u << slot;
        samplers++;
    }
    if (samplers > (stage == VP_STATIC_INDEX_STAGE_VERTEX
                        ? 1u : VP_GXM_FRAGMENT_SAMPLER_LIMIT))
        return -1;
    if (count_out != NULL) *count_out = samplers;
    return 0;
}

/* Viewport y convention. GXM's default viewport and the guest's Maxwell
 * viewport both use a negative y scale for a top-down window, but the first
 * frame executor passed a positive scale. The three-way marker exists for the
 * M3 controlled comparison; the canonical convention is the default. */
static int gxm_viewport_mode = -1;
static int viewport_mode(void) {
    if (gxm_viewport_mode < 0) {
        FILE *file;
        char line[32];
        gxm_viewport_mode = 1;
        file = fopen("ux0:data/mhgu-vita-port/m3-viewport.enable", "rb");
        if (file != NULL) {
            if (fgets(line, sizeof(line), file) != NULL) {
                if (strncmp(line, "legacy", 6) == 0) gxm_viewport_mode = 0;
                else if (strncmp(line, "offset", 6) == 0) gxm_viewport_mode = 2;
            }
            fclose(file);
        }
        glog("VP_GXM_VIEWPORT mode=%s\n",
             gxm_viewport_mode == 0 ? "legacy" :
             gxm_viewport_mode == 2 ? "offset" : "canonical");
    }
    return gxm_viewport_mode;
}

/* Add one displayed draw's viewport (clipped by the scissor, when enabled) to
 * the surface's present rectangle. Coordinates are in surface pixels. */
static void display_rect_add(VPGxmSurface *target, const VPGxmDrawRequest *req) {
    unsigned x0, y0, x1, y1;

    if (!req->display_target || req->color_mask == 0u) return;
    x0 = req->viewport_x;
    y0 = req->viewport_y;
    x1 = req->viewport_width != 0u ? x0 + req->viewport_width : target->width;
    y1 = req->viewport_height != 0u ? y0 + req->viewport_height : target->height;
    if (req->scissor_enable != 0u) {
        unsigned sx0 = req->scissor_min_x, sy0 = req->scissor_min_y;
        unsigned sx1 = req->scissor_max_x, sy1 = req->scissor_max_y;
        if (sx0 > x0) x0 = sx0;
        if (sy0 > y0) y0 = sy0;
        if (sx1 < x1) x1 = sx1;
        if (sy1 < y1) y1 = sy1;
    }
    if (x0 > target->width) x0 = target->width;
    if (y0 > target->height) y0 = target->height;
    if (x1 > target->width) x1 = target->width;
    if (y1 > target->height) y1 = target->height;
    if (x0 >= x1 || y0 >= y1) return;
    if (!target->disp_valid) {
        target->disp_x0 = x0;
        target->disp_y0 = y0;
        target->disp_x1 = x1;
        target->disp_y1 = y1;
        target->disp_valid = 1u;
    } else {
        if (x0 < target->disp_x0) target->disp_x0 = x0;
        if (y0 < target->disp_y0) target->disp_y0 = y0;
        if (x1 > target->disp_x1) target->disp_x1 = x1;
        if (y1 > target->disp_y1) target->disp_y1 = y1;
    }
    target->present_draws++;
}

/* ---- static index residency and GXM object cache ------------------------ */

static void static_log_batch_hist(void);

static void static_log_stats(void) {
    glog("VP_GXM_STATIC_STATS index_hits=%u index_misses=%u gxp_loads=%u "
         "registrations=%u unregistrations=%u pair_hits=%u pair_hint_hits=%u "
         "pair_misses=%u "
         "pair_evictions=%u resident_evictions=%u reflections=%u draw_ok=%u "
         "draw_fail=%u query_unobserved=%u batch_encoded=%u batch_completed=%u "
         "batch_failed=%u batch_flushes=%u batch_sync_fallbacks=%u "
         "batch_max=%u batch_bytes_peak=%u batch_uniform_peak=%u "
         "batch_reset_bytes_avoided=%llu reflect_name_formats_avoided=%llu "
         "reflect_lookups_avoided=%llu resident=%u/%u bytes=%u/%u peak=%u\n",
         static_stats.index_hits, static_stats.index_misses,
         static_stats.gxp_loads, static_stats.registrations,
         static_stats.unregistrations, static_stats.pair_hits,
         static_stats.pair_hint_hits,
         static_stats.pair_misses, static_stats.pair_evictions,
         static_stats.resident_evictions, static_stats.reflections,
         static_stats.draw_ok, static_stats.draw_fail,
         static_stats.query_unobserved, static_stats.batch_encoded,
         static_stats.batch_completed, static_stats.batch_failed,
         static_stats.batch_flushes, static_stats.batch_sync_fallbacks,
         static_stats.batch_max, static_stats.batch_bytes_peak,
         static_stats.batch_uniform_peak,
         (unsigned long long)static_stats.batch_reset_bytes_avoided,
         (unsigned long long)static_stats.reflect_name_formats_avoided,
         (unsigned long long)static_stats.reflect_lookups_avoided,
         static_resident_used,
         VP_GXM_RESIDENT_LIMIT, static_resident_bytes,
         VP_GXM_RESIDENT_BYTES, static_stats.resident_peak_bytes);
    glog("VP_GXM_SHARED_PREP_STATS enabled=%d shader_plan_hits=%u "
         "shader_plan_misses=%u shader_plan_builds=%u "
         "shader_plan_invalidations=%u shader_plan_vs=%u shader_plan_fs=%u "
         "shader_slots_vs=%u,%u,%u,%u,%u shader_slots_fs=%u,%u,%u,%u,%u "
         "sampler_plan_hits=%u sampler_plan_misses=%u "
         "pair_prepared_hits=%u uniform_plan_reuses=%u "
         "blend_plan_reuses=%u generation=%u\n",
         gxm_shared_prepare_active(), static_plan_counts.hits,
         static_plan_counts.misses, static_plan_counts.builds,
         static_plan_counts.invalidations, static_plan_stage_calls[0],
         static_plan_stage_calls[1],
         static_plan_slot_calls[0][0], static_plan_slot_calls[0][1],
         static_plan_slot_calls[0][2], static_plan_slot_calls[0][3],
         static_plan_slot_calls[0][4],
         static_plan_slot_calls[1][0], static_plan_slot_calls[1][1],
         static_plan_slot_calls[1][2], static_plan_slot_calls[1][3],
         static_plan_slot_calls[1][4],
         gxm_sampler_plan_hits, gxm_sampler_plan_misses,
         static_shared_pair_hits, static_shared_uniform_reuses,
         static_shared_blend_reuses, static_plan_generation);
    if (gxm_direct_seen || vp_texture_direct_active()) {
        glog("VP_GXM_TEXTURE_PIPELINE_STATS active=%d direct_binds=%llu "
             "direct_bytes_avoided=%llu direct_retains=%llu "
             "direct_releases=%llu direct_fail=%llu direct_quarantined=%llu "
             "staging_memcpy_bytes=%llu copy_path_us=%llu upload_fill_bytes=%llu "
             "deferred_scenes=%llu notif_poll_hits=%llu "
             "notif_real_waits=%llu notify_calls=%llu combined_drains=%llu "
             "pending_scenes=%u\n",
             vp_texture_direct_active(),
             (unsigned long long)gxm_direct_binds,
             (unsigned long long)gxm_direct_bytes_avoided,
             (unsigned long long)gxm_direct_retains,
             (unsigned long long)gxm_direct_releases,
             (unsigned long long)gxm_direct_bind_failures,
             (unsigned long long)gxm_direct_quarantined,
             (unsigned long long)gxm_tex_bytes,
             (unsigned long long)gxm_tex_us,
             (unsigned long long)gxm_upload_fill_bytes,
             (unsigned long long)gxm_deferred_scenes,
             (unsigned long long)gxm_notify_poll_hits,
             (unsigned long long)gxm_notify_real_waits,
             (unsigned long long)gxm_notify_wait_calls,
             (unsigned long long)gxm_combined_drains,
             gxm_pending_scene_count);
        glog("VP_GXM_TEXTURE_REGION_STATS physical_bytes=%u peak_bytes=%u "
             "kernel_allocs=%llu kernel_frees=%llu suballocs=%llu "
             "subfrees=%llu live=%u failed=%u budget=%u memory_mode=%u "
             "mapped_physical_bytes=%u cached_physical_bytes=%u "
             "mapped_allocated_bytes=%u cached_allocated_bytes=%u\n",
             (unsigned)gxm_texture_region_arena.physical_bytes,
             (unsigned)gxm_texture_region_arena.peak_bytes,
             (unsigned long long)gxm_texture_region_arena.kernel_allocs,
             (unsigned long long)gxm_texture_region_arena.kernel_frees,
             (unsigned long long)gxm_texture_region_arena.suballocs,
             (unsigned long long)gxm_texture_region_arena.subfrees,
             gxm_texture_region_arena.live, gxm_texture_region_arena.failed,
             (unsigned)gxm_texture_region_arena.budget,
             __atomic_load_n(&vp_engine_texture_memory_mode, __ATOMIC_ACQUIRE),
             (unsigned)gxm_texture_region_arena
                 .class_physical[VP_TEXTURE_REGION_CLASS_MAPPED],
             (unsigned)gxm_texture_region_arena
                 .class_physical[VP_TEXTURE_REGION_CLASS_CPU_CACHED],
             (unsigned)gxm_texture_region_arena
                 .class_allocated[VP_TEXTURE_REGION_CLASS_MAPPED],
             (unsigned)gxm_texture_region_arena
                 .class_allocated[VP_TEXTURE_REGION_CLASS_CPU_CACHED]);
    }
    if (gxm_clear_defer_seen) {
        unsigned pending_async = 0u;
        for (unsigned i = 0u; i < VP_GXM_SURFACE_LIMIT; ++i)
            if (gxm_surfaces[i].used && gxm_surfaces[i].clear_pending_async)
                pending_async++;
        glog("VP_GXM_CLEAR_DEFER_STATS enabled=%d tries=%llu accept=%llu "
             "fallback=%llu submit_fail=%llu scene_submits=%llu "
             "coalesced=%llu materialize_calls=%llu materialize_fail=%llu "
             "in_scene=%llu pending_async=%u\n",
             gxm_clear_defer_reported == 1,
             (unsigned long long)gxm_clear_defer_tries,
             (unsigned long long)gxm_clear_defer_accepts,
             (unsigned long long)gxm_clear_defer_fallbacks,
             (unsigned long long)gxm_clear_defer_submit_failures,
             (unsigned long long)gxm_clear_defer_scene_submits,
             (unsigned long long)gxm_clear_defer_coalesced,
             (unsigned long long)gxm_clear_defer_materialize_calls,
             (unsigned long long)gxm_clear_defer_materialize_failures,
             (unsigned long long)gxm_clear_defer_in_scene, pending_async);
    }
    static_log_batch_hist();
    glog("VP_GXM_CLEAR_FUSION_STATS deferred=%llu combined_deferred=%llu "
         "in_scene=%llu materialized=%llu blue_first=%llu\n",
         (unsigned long long)gxm_clear_fusion_deferred,
         (unsigned long long)gxm_clear_fusion_combined,
         (unsigned long long)gxm_clear_fusion_in_scene,
         (unsigned long long)gxm_clear_fusion_materialized,
         (unsigned long long)gxm_clear_fusion_blue);
    glog("VP_GXM_DRAW_REFUSE_STATS total=%llu "
         "s0=%llu s1=%llu s2=%llu s3=%llu s4=%llu s5=%llu s6=%llu "
         "s7=%llu s8=%llu s9=%llu s10=%llu s11=%llu s12=%llu "
         "s13=%llu s14=%llu s15=%llu\n",
         (unsigned long long)gxm_draw_refuse_total,
         (unsigned long long)gxm_draw_refuse_stage[0],
         (unsigned long long)gxm_draw_refuse_stage[1],
         (unsigned long long)gxm_draw_refuse_stage[2],
         (unsigned long long)gxm_draw_refuse_stage[3],
         (unsigned long long)gxm_draw_refuse_stage[4],
         (unsigned long long)gxm_draw_refuse_stage[5],
         (unsigned long long)gxm_draw_refuse_stage[6],
         (unsigned long long)gxm_draw_refuse_stage[7],
         (unsigned long long)gxm_draw_refuse_stage[8],
         (unsigned long long)gxm_draw_refuse_stage[9],
         (unsigned long long)gxm_draw_refuse_stage[10],
         (unsigned long long)gxm_draw_refuse_stage[11],
         (unsigned long long)gxm_draw_refuse_stage[12],
         (unsigned long long)gxm_draw_refuse_stage[13],
         (unsigned long long)gxm_draw_refuse_stage[14],
         (unsigned long long)gxm_draw_refuse_stage[15]);
    glog("VP_GXM_STAGE9_STATS primary_depth=%llu primary_format=%llu "
         "primary_bytes=%llu primary_uniforms=%llu vertex_tex=%llu "
         "tex_missing=%llu extra_feedback=%u extra_missing=%u "
         "extra_uninit=%u extra_view=%u extra_format=%u extra_sampler=%u "
         "extra_layout=%u shadow=%u\n",
         (unsigned long long)gxm_stage9_reason[VP_GXM_S9_PRIMARY_DEPTH],
         (unsigned long long)gxm_stage9_reason[VP_GXM_S9_PRIMARY_FORMAT],
         (unsigned long long)gxm_stage9_reason[VP_GXM_S9_PRIMARY_BYTES],
         (unsigned long long)gxm_stage9_reason[VP_GXM_S9_PRIMARY_UNIFORMS],
         (unsigned long long)gxm_stage9_reason[VP_GXM_S9_VERTEX_TEX],
         (unsigned long long)gxm_stage9_reason[VP_GXM_S9_TEX_MISSING],
         gxm_extra_reject_feedback, gxm_extra_reject_missing,
         gxm_extra_reject_uninitialized, gxm_extra_reject_view,
         gxm_extra_reject_format, gxm_extra_reject_sampler,
         gxm_extra_reject_layout, gxm_shadow_rejects);
    {
        unsigned printed = 0u;
        while (printed < 12u) {
            unsigned pick = VP_GXM_REFUSE_HASH_LIMIT, i;
            for (i = 0; i < gxm_refuse_hash_used; ++i) {
                unsigned already = 0u, j;
                for (j = 0; j < printed; ++j)
                    if (gxm_refuse_hash_order[j] == i) already = 1u;
                if (already) continue;
                if (pick == VP_GXM_REFUSE_HASH_LIMIT ||
                    gxm_refuse_hash[i].count > gxm_refuse_hash[pick].count)
                    pick = i;
            }
            if (pick == VP_GXM_REFUSE_HASH_LIMIT) break;
            gxm_refuse_hash_order[printed++] = pick;
            glog("VP_GXM_REFUSE_TOP hash=0x%x stage=%u count=%llu\n",
                 gxm_refuse_hash[pick].hash, gxm_refuse_hash[pick].stage,
                 (unsigned long long)gxm_refuse_hash[pick].count);
        }
    }
    glog("VP_GXM_BATCH_PERF flushes=%llu end_sum=%llu end_max=%llu "
         "notify_sum=%llu notify_max=%llu finish_sum=%llu finish_max=%llu "
         "total_sum=%llu total_max=%llu\n",
         (unsigned long long)gxm_batch_perf.flushes,
         (unsigned long long)gxm_batch_perf.end_sum,
         (unsigned long long)gxm_batch_perf.end_max,
         (unsigned long long)gxm_batch_perf.notify_sum,
         (unsigned long long)gxm_batch_perf.notify_max,
         (unsigned long long)gxm_batch_perf.finish_sum,
         (unsigned long long)gxm_batch_perf.finish_max,
         (unsigned long long)gxm_batch_perf.total_sum,
         (unsigned long long)gxm_batch_perf.total_max);
    glog("VP_GXM_COMPLETION endscene_calls=%llu endscene_api_us=%llu "
         "notify_calls=%llu notify_block_us=%llu finish_calls=%llu "
         "finish_block_us=%llu pending_complete_calls=%llu "
         "pending_block_us=%llu pipeline_age_sum_us=%llu pipeline_age_max_us=%llu\n",
         (unsigned long long)gxm_endscene_calls,
         (unsigned long long)gxm_endscene_us,
         (unsigned long long)gxm_notify_wait_calls,
         (unsigned long long)gxm_notify_wait_us,
         (unsigned long long)gxm_finish_calls,
         (unsigned long long)gxm_finish_us,
         (unsigned long long)gxm_pending_complete_calls,
         (unsigned long long)gxm_pending_wait_us,
         (unsigned long long)gxm_pipeline_age_us,
         (unsigned long long)gxm_pipeline_age_max);
    for (unsigned class_id = 0u; class_id < VP_SCENE_CLASS_COUNT; ++class_id)
        glog("VP_GXM_SCENE_CLASS class=%s flushes=%llu draws=%llu "
             "notify_us=%llu finish_us=%llu total_us=%llu max_us=%llu\n",
             gxm_scene_class_names[class_id],
             (unsigned long long)gxm_scene_classes[class_id].flushes,
             (unsigned long long)gxm_scene_classes[class_id].draws,
             (unsigned long long)gxm_scene_classes[class_id].notify_us,
             (unsigned long long)gxm_scene_classes[class_id].finish_us,
             (unsigned long long)gxm_scene_classes[class_id].total_us,
             (unsigned long long)gxm_scene_classes[class_id].total_max);
    glog("VP_GXM_BLEND_CENSUS opaque=%llu blended=%llu\n",
         (unsigned long long)gxm_draws_opaque,
         (unsigned long long)gxm_draws_blended);
    uint64_t encode_profile_us = sceKernelGetProcessTimeWide();
    for (unsigned kind = 0u; kind < 2u; ++kind)
        glog("VP_GXM_BATCH_ENCODE_PERF kind=%s t_us=%llu draws=%llu prep_us=%llu "
             "begin_us=%llu encode_us=%llu draw_us=%llu setup_us=%llu "
             "vertex_us=%llu index_us=%llu texture_us=%llu nested_flush_us=%llu "
             "nested_pending_us=%llu nested_notify_us=%llu "
             "nested_finish_us=%llu "
             "validate_us=%llu pair_us=%llu feedback_us=%llu admit_us=%llu "
             "resolve_us=%llu pair_flush_us=%llu admit_flush_us=%llu "
             "prj_us=%llu reserve_us=%llu uniform_us=%llu policy_us=%llu\n",
             kind ? "depth-only" : "colour",
             (unsigned long long)encode_profile_us,
             (unsigned long long)gxm_batch_encode_perf[kind].draws,
             (unsigned long long)gxm_batch_encode_perf[kind].prep_us,
             (unsigned long long)gxm_batch_encode_perf[kind].begin_us,
             (unsigned long long)gxm_batch_encode_perf[kind].encode_us,
             (unsigned long long)gxm_batch_encode_perf[kind].draw_us,
             (unsigned long long)gxm_batch_encode_perf[kind].setup_us,
             (unsigned long long)gxm_batch_encode_perf[kind].vertex_us,
             (unsigned long long)gxm_batch_encode_perf[kind].index_us,
             (unsigned long long)gxm_batch_encode_perf[kind].texture_us,
             (unsigned long long)gxm_batch_encode_perf[kind].nested_flush_us,
             (unsigned long long)gxm_batch_encode_perf[kind].nested_pending_us,
             (unsigned long long)gxm_batch_encode_perf[kind].nested_notify_us,
             (unsigned long long)gxm_batch_encode_perf[kind].nested_finish_us,
             (unsigned long long)gxm_batch_encode_perf[kind].validate_us,
             (unsigned long long)gxm_batch_encode_perf[kind].pair_us,
             (unsigned long long)gxm_batch_encode_perf[kind].feedback_us,
             (unsigned long long)gxm_batch_encode_perf[kind].admit_us,
             (unsigned long long)gxm_batch_encode_perf[kind].resolve_us,
             (unsigned long long)gxm_batch_encode_perf[kind].pair_flush_us,
             (unsigned long long)gxm_batch_encode_perf[kind].admit_flush_us,
             (unsigned long long)gxm_batch_encode_perf[kind].prj_us,
             (unsigned long long)gxm_batch_encode_perf[kind].reserve_us,
             (unsigned long long)gxm_batch_encode_perf[kind].uniform_us,
             (unsigned long long)gxm_batch_encode_perf[kind].policy_us);
    glog("VP_GXM_API_PERF submits=%llu notify_sum_us=%llu notify_max_us=%llu "
         "wait_sum_us=%llu wait_max_us=%llu direct_draws=%llu stream_entries=%llu\n",
         (unsigned long long)gxm_api_perf.submits,
         (unsigned long long)gxm_api_perf.notify_sum,
         (unsigned long long)gxm_api_perf.notify_max,
         (unsigned long long)gxm_api_perf.wait_sum,
         (unsigned long long)gxm_api_perf.wait_max,
         (unsigned long long)gxm_api_perf.direct_draws,
         (unsigned long long)gxm_api_perf.stream_entries);
    glog("VP_GXM_XFER vertex_bytes=%llu vertex_us=%llu "
         "texture_bytes=%llu texture_us=%llu\n",
         (unsigned long long)gxm_xfer_bytes, (unsigned long long)gxm_xfer_us,
         (unsigned long long)gxm_tex_bytes, (unsigned long long)gxm_tex_us);
    glog("VP_GXM_STRIP_JOIN draws=%llu segments=%llu\n",
         (unsigned long long)gxm_strip_draws, (unsigned long long)gxm_strip_segments);
    glog("VP_GXM_INDEX_FAST no_restart=%llu remap_copy=%llu ramp_copy=%llu "
         "ramp_bind=%llu desc_hits=%llu desc_misses=%llu\n",
         (unsigned long long)gxm_index_norestart,
         (unsigned long long)gxm_index_remap_copy,
         (unsigned long long)gxm_index_ramp_copy,
         (unsigned long long)gxm_index_ramp_bind,
         (unsigned long long)gxm_texture_desc_hits,
         (unsigned long long)gxm_texture_desc_misses);
    glog("VP_GXM_UPLOAD_CACHE hits=%llu copies=%llu reused_bytes=%llu capacity=%u "
         "owned_hits=%llu owned_bytes=%llu memo_hits=%llu skip_hits=%llu\n",
         (unsigned long long)gxm_upload_hits, (unsigned long long)gxm_upload_copies,
         (unsigned long long)gxm_upload_saved_bytes, gxm_upload_capacity,
         (unsigned long long)gxm_upload_owned_hits,
         (unsigned long long)gxm_upload_owned_bytes,
         (unsigned long long)gxm_upload_memo_hits,
         (unsigned long long)gxm_upload_skip_hits);
    glog("VP_GXM_SNAPSHOT hits=%llu saved_bytes=%llu owned_hits=%llu "
         "owned_bytes=%llu owned_retirements=%llu owned_retired_assocs=%llu "
         "skip_hits=%llu\n",
         (unsigned long long)gxm_snapshot_hits,
         (unsigned long long)gxm_snapshot_saved_bytes,
         (unsigned long long)gxm_snapshot_owned_hits,
         (unsigned long long)gxm_snapshot_owned_bytes,
         (unsigned long long)gxm_owned_retirements,
         (unsigned long long)gxm_owned_retired_assocs,
         (unsigned long long)gxm_snapshot_skip_hits);
}

static int static_reflection_build(const SceGxmProgram *program, unsigned stage,
                                   VPStaticReflection *refl) {
    const char *prefix = stage == VP_STATIC_INDEX_STAGE_VERTEX ? "vs" : "fs";
    unsigned i;
    memset(refl, 0, sizeof(*refl));
    refl->default_uniform_bytes = sceGxmProgramGetDefaultUniformBufferSize(program);
    for (i = 0; i < 18u; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "%s_cbuf%u", prefix, i);
        refl->cbuf[i] = sceGxmProgramFindParameterByName(program, name);
        if (refl->cbuf[i] != NULL)
            refl->cbuf_float_counts[i] =
                sceGxmProgramParameterGetComponentCount(refl->cbuf[i]) *
                sceGxmProgramParameterGetArraySize(refl->cbuf[i]);
    }
    for (i = 0; i < VP_GXM_FRAGMENT_SAMPLER_LIMIT; ++i) {
        char name[40];
        snprintf(name, sizeof(name), "tex%u", i);
        refl->tex[i] = sceGxmProgramFindParameterByName(program, name);
        snprintf(name, sizeof(name), "tex%u_inv_size", i);
        refl->tex_inv_size[i] = sceGxmProgramFindParameterByName(program, name);
        snprintf(name, sizeof(name), "tex%u_rgba32f_packed", i);
        refl->tex_rgba32f_packed[i] =
            sceGxmProgramFindParameterByName(program, name);
        snprintf(name, sizeof(name), "tex%u_shadow_extent", i);
        refl->shadow_extent[i] = sceGxmProgramFindParameterByName(program, name);
        snprintf(name, sizeof(name), "tex%u_shadow_state", i);
        refl->shadow_state[i] = sceGxmProgramFindParameterByName(program, name);
        snprintf(name, sizeof(name), "tex%u_shadow_border", i);
        refl->shadow_border[i] = sceGxmProgramFindParameterByName(program, name);
        {
            unsigned present = (refl->shadow_extent[i] != NULL ? 1u : 0u) +
                               (refl->shadow_state[i] != NULL ? 1u : 0u) +
                               (refl->shadow_border[i] != NULL ? 1u : 0u);
            /* The helper is only defined for fragment textures.  A complete
             * trio without the sampler, or any trio in a vertex program, is an
             * incomplete contract and is refused at draw time. */
            if (present != 0u) {
                if (stage == VP_STATIC_INDEX_STAGE_FRAGMENT &&
                    present == 3u && refl->tex[i] != NULL)
                    refl->shadow_complete_mask |= 1u << i;
                else
                    refl->shadow_incomplete_mask |= 1u << i;
            }
        }
    }
    refl->render_area = sceGxmProgramFindParameterByName(program, "render_area");
    refl->fragcoord_scale = sceGxmProgramFindParameterByName(program, "vp_fragcoord_scale");
    refl->varying_one = sceGxmProgramFindParameterByName(program, "vp_varying_one");
    refl->alpha_test = sceGxmProgramFindParameterByName(program, "vp_alpha_test");
    refl->face_state = sceGxmProgramFindParameterByName(program, "vp_face_state");
    refl->f_mask = sceGxmProgramFindParameterByName(program, "f_mask");
    for (i = 0; i < 16u; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "in_attr%u", i);
        refl->attr[i] = sceGxmProgramFindParameterByName(program, name);
        snprintf(name, sizeof(name), "in_attr%u_uint", i);
        refl->attr_uint[i] = sceGxmProgramFindParameterByName(program, name);
    }
    if (program_sampler_layout(program, stage, &refl->sampler_count) < 0) {
        refl->sampler_unsupported = 1u;
        return -1;
    }
    static_stats.reflections++;
    return 0;
}

static int static_resident_find(uint32_t stage_index, uint16_t variant_index) {
    if (variant_index >= VP_STATIC_INDEX_MAX_VARIANTS) return -1;
    unsigned slot = static_resident_slots[variant_index];
    if (slot == 0u) return -1;
    const VPStaticResident *resident = &static_residents[slot - 1u];
    return resident->used && resident->stage_index == stage_index &&
           resident->variant_index == variant_index ? (int)slot - 1 : -1;
}

static int static_resident_evict_one(void) {
    unsigned i, victim = VP_GXM_RESIDENT_LIMIT;
    uint32_t oldest = 0xffffffffu;
    for (i = 0; i < VP_GXM_RESIDENT_LIMIT; ++i) {
        const VPStaticResident *resident = &static_residents[i];
        if (!resident->used || resident->refs != 0u) continue;
        if (victim == VP_GXM_RESIDENT_LIMIT || resident->last_use < oldest) {
            victim = i;
            oldest = resident->last_use;
        }
    }
    if (victim == VP_GXM_RESIDENT_LIMIT) return -1;
    {
        VPStaticResident *resident = &static_residents[victim];
        int rc;
        /* Unregistering a program releases GXM objects a deferred scene may
         * still reference. */
        rc = gxm_pending_scene_complete("resident-evict");
        if (rc < 0) return rc;
        rc = sceGxmShaderPatcherUnregisterProgram(patcher, resident->id);
        if (rc < 0) {
            glog("VP_GXM_STATIC_UNREGISTER stage=%u variant=%u rc=%d\n",
                 resident->stage_index, resident->variant_index, rc);
            return -1;
        }
        glog("VP_GXM_STATIC_EVICT stage=%u variant=%u bytes=%u\n",
             resident->stage_index, resident->variant_index, resident->bytes);
        static_stats.unregistrations++;
        static_stats.resident_evictions++;
        static_resident_bytes -= resident->bytes;
        static_resident_slots[resident->variant_index] = 0u;
        free(resident->program);
        memset(resident, 0, sizeof(*resident));
        static_resident_used--;
        /* A previously declared sampler may become unresolved even though no
         * memory changed; plans must not keep the old declared/reflection. */
        static_plan_invalidate();
    }
    return 0;
}

static int static_resident_ensure(uint32_t stage_index, uint16_t variant_index,
                                  VPStaticResident **out) {
    const VPStaticVariant *variant;
    VPStaticResident *resident;
    char name[64];
    char path[VP_GXM_STATIC_PATH + 96u];
    uint8_t *bytes;
    long length;
    FILE *file;
    int slot, rc;

    if (out == NULL) return -1;
    *out = NULL;
    slot = static_resident_find(stage_index, variant_index);
    if (slot >= 0) {
        resident = &static_residents[slot];
        resident->last_use = ++static_clock;
        static_stats.index_hits++;
        *out = resident;
        return 0;
    }
    static_stats.index_misses++;
    if (variant_index >= static_index.variant_count) return -1;
    variant = &static_index.variants[variant_index];
    if (variant->stage_index != stage_index ||
        (variant->flags & VP_STATIC_INDEX_VARIANT_COMPILED) == 0u)
        return -1;
    if (variant->gxp_bytes > VP_GXM_RESIDENT_BYTES) {
        glog("VP_GXM_STATIC_GXP too_large stage=%u variant=%u bytes=%u\n",
             stage_index, variant_index, variant->gxp_bytes);
        return -1;
    }
    while (static_resident_used >= VP_GXM_RESIDENT_LIMIT ||
           static_resident_bytes + variant->gxp_bytes > VP_GXM_RESIDENT_BYTES) {
        if (static_resident_evict_one() < 0) {
            glog("VP_GXM_STATIC_RESIDENT limit stage=%u variant=%u used=%u bytes=%u\n",
                 stage_index, variant_index, static_resident_used,
                 static_resident_bytes);
            return -1;
        }
    }
    for (slot = 0; slot < (int)VP_GXM_RESIDENT_LIMIT; ++slot)
        if (!static_residents[slot].used) break;
    if (slot == (int)VP_GXM_RESIDENT_LIMIT) return -1;

    if (vp_static_variant_name(variant, name, sizeof(name)) < 0) return -1;
    snprintf(path, sizeof(path), "%s/shaders/%s", static_index_dir, name);
    file = fopen(path, "rb");
    if (file == NULL) {
        glog("VP_GXM_STATIC_GXP open_failed stage=%u variant=%u file=%s\n",
             stage_index, variant_index, name);
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        (uint32_t)length != variant->gxp_bytes) {
        glog("VP_GXM_STATIC_GXP size_mismatch stage=%u variant=%u file=%s "
             "expected=%u\n", stage_index, variant_index, name, variant->gxp_bytes);
        fclose(file);
        return -1;
    }
    rewind(file);
    bytes = (uint8_t *)malloc(variant->gxp_bytes);
    if (bytes == NULL || fread(bytes, 1, variant->gxp_bytes, file) !=
                         variant->gxp_bytes) {
        glog("VP_GXM_STATIC_GXP read_failed stage=%u variant=%u file=%s\n",
             stage_index, variant_index, name);
        free(bytes);
        fclose(file);
        return -1;
    }
    fclose(file);
    if (vp_static_index_crc32(bytes, variant->gxp_bytes) != variant->gxp_crc32) {
        glog("VP_GXM_STATIC_GXP crc_mismatch stage=%u variant=%u file=%s\n",
             stage_index, variant_index, name);
        free(bytes);
        return -1;
    }
    resident = &static_residents[slot];
    memset(resident, 0, sizeof(*resident));
    rc = sceGxmShaderPatcherRegisterProgram(patcher,
            (const SceGxmProgram *)bytes, &resident->id);
    if (rc < 0) {
        glog("VP_GXM_STATIC_GXP register_failed stage=%u variant=%u rc=%d file=%s\n",
             stage_index, variant_index, rc, name);
        free(bytes);
        return -1;
    }
    resident->used = 1;
    resident->stage_index = stage_index;
    resident->variant_index = variant_index;
    resident->program = (SceGxmProgram *)bytes;
    resident->has_f_mask =
        sceGxmProgramFindParameterByName(resident->program, "f_mask") != NULL;
    resident->bytes = variant->gxp_bytes;
    resident->last_use = ++static_clock;
    if (static_reflection_build(resident->program,
                                static_index.stages[stage_index].stage,
                                &resident->refl) < 0) {
        glog("VP_GXM_STATIC_SAMPLER_UNSUPPORTED stage=%u variant=%u file=%s "
             "samplers=%u limit=%u action=refuse\n", stage_index, variant_index,
             name, program_sampler_count(resident->program),
             VP_GXM_FRAGMENT_SAMPLER_LIMIT);
        sceGxmShaderPatcherUnregisterProgram(patcher, resident->id);
        free(bytes);
        memset(resident, 0, sizeof(*resident));
        return -1;
    }
    static_resident_slots[variant_index] = (uint16_t)(slot + 1);
    static_resident_used++;
    static_resident_bytes += resident->bytes;
    if (static_resident_bytes > static_stats.resident_peak_bytes)
        static_stats.resident_peak_bytes = static_resident_bytes;
    static_stats.gxp_loads++;
    static_stats.registrations++;
    /* A stage that had no resident (and therefore queried as undeclared) can
     * now declare samplers. Bump before any later draw can consult a plan. */
    static_plan_invalidate();
    glog("VP_GXM_STATIC_RESIDENT stage=%u variant=%u bytes=%u resident=%u/%u "
         "total_bytes=%u\n", stage_index, variant_index, resident->bytes,
         static_resident_used, VP_GXM_RESIDENT_LIMIT, static_resident_bytes);
    *out = resident;
    return 0;
}

static int static_query_sampler(unsigned token, unsigned stage,
                                unsigned viewport, unsigned sampler) {
    const VPStaticProgram *program;
    const VPStaticVariant *variant;
    uint16_t stage_index;
    int resident;
    if (!static_mode || sampler >= VP_GXM_FRAGMENT_SAMPLER_LIMIT) return 0;
    program = vp_static_program_for_token(&static_index, token);
    if (program == NULL) {
        static_stats.query_unobserved++;
        return 0;
    }
    stage_index = stage == VP_STATIC_INDEX_STAGE_VERTEX ? program->vertex_stage
                                                        : program->fragment_stage;
    if (stage_index == VP_STATIC_INDEX_STAGE_NONE) return 0;
    variant = vp_static_variant_for(&static_index, stage_index, viewport);
    resident = -1;
    if (variant != NULL)
        resident = static_resident_find(stage_index,
                                        (uint16_t)(variant - static_index.variants));
    if (resident < 0) {
        /* Texture consumption is a property of the stage content, not of the
         * float/integer input specialization. Accept any resident variant for
         * this stage/viewport when the generic one is not loaded yet. */
        const VPStaticStage *entry = &static_index.stages[stage_index];
        uint32_t i;
        for (i = 0; i < entry->variant_count; ++i) {
            const VPStaticVariant *candidate = &static_index.variants[
                entry->variant_start + i];
            if ((candidate->viewport_mask & (1u << viewport)) == 0u) continue;
            resident = static_resident_find(stage_index,
                    (uint16_t)(candidate - static_index.variants));
            if (resident >= 0) break;
        }
    }
    if (resident < 0) {
        static_stats.query_unobserved++;
        return 0;
    }
    return static_residents[resident].refl.tex[sampler] != NULL;
}

int vp_gxm_static_resources_active(void) {
    if (!static_mode) return 0;
    return static_resources_ready ? 1 : -1;
}

int vp_gxm_shader_resource(unsigned token, unsigned gpu_stage,
                           unsigned viewport_transform, unsigned slot,
                           VPResourceTexture *out) {
    const VPStaticProgram *program;
    const VPStaticStage *stage;
    const VPResourceStage *resource_stage;
    uint16_t stage_index;
    unsigned stage_type;

    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    if (gpu_stage != 0u && gpu_stage != 4u) return -1;
    if (slot >= VP_GXM_FRAGMENT_SAMPLER_LIMIT) return -1;
    if (!static_mode) return -3;
    if (!static_resources_ready) return -2;
    program = vp_static_program_for_token(&static_index, token);
    if (program == NULL) return -4;
    stage_index = gpu_stage == 0u ? program->vertex_stage
                                  : program->fragment_stage;
    if (stage_index == VP_STATIC_INDEX_STAGE_NONE ||
        stage_index >= static_index.stage_count)
        return -4;
    stage = &static_index.stages[stage_index];
    stage_type = stage->stage;
    if (stage_type != VP_STATIC_INDEX_STAGE_VERTEX &&
        stage_type != VP_STATIC_INDEX_STAGE_FRAGMENT)
        return -4;
    /* Present only when the actual registered reflection uses this slot. */
    if (!static_query_sampler(token, stage_type, viewport_transform, slot))
        return 1;
    resource_stage = vp_resource_layout_stage(&static_resources, stage_type,
                                              stage->sha256);
    if (resource_stage == NULL) return -5;
    if (vp_resource_layout_texture(&static_resources, resource_stage, slot,
                                   out) < 0)
        return -6;
    return 0;
}

/* Plan callbacks deliberately call the original query functions. A plan miss
 * therefore has exactly the legacy return/failure semantics; each later hit
 * copies value fields and issues no resident lookup again. The query path and
 * resident registration are serialized by gpu_execution_gate (the decoder
 * entry holds it around inline GXM work), so the generation comparison is a
 * stale-result check, not a lock. */
static int static_plan_declared(void *user, uint32_t token, unsigned gpu_stage,
                                unsigned viewport, unsigned slot) {
    (void)user;
    return gpu_stage == 0u
               ? vp_gxm_shader_has_vertex_sampler(token, viewport, slot)
               : vp_gxm_shader_has_fragment_sampler(token, viewport, slot);
}

static int static_plan_active(void *user) {
    (void)user;
    return vp_gxm_static_resources_active();
}

static int static_plan_resource(void *user, uint32_t token, unsigned gpu_stage,
                                unsigned viewport, unsigned slot,
                                VPResourceTexture *out) {
    (void)user;
    return vp_gxm_shader_resource(token, gpu_stage, viewport, slot, out);
}

static const VPGxmShaderPlanOps static_plan_ops = {
    NULL, static_plan_declared, static_plan_active, static_plan_resource
};

int vp_gxm_shader_resource_plan(unsigned token, unsigned gpu_stage,
                                unsigned viewport_transform,
                                VPGxmShaderResourcePlan *out) {
    unsigned stage_index;
    unsigned slot;
    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    if (gpu_stage != 0u && gpu_stage != 4u) return -1;
    /* Legacy mode keeps the original per-slot calls: reporting a synthetic
     * plan here would silently disable the legacy hash-bundle resolution. */
    if (!static_mode) return -3;
    if (vp_gxm_shader_plan_cached(static_plan_cache,
                                  VP_GXM_SHADER_PLAN_CACHE_ENTRIES,
                                  static_plan_generation, &static_plan_ops,
                                  token, gpu_stage, viewport_transform, out,
                                  &static_plan_counts) < 0)
        return -1;
    stage_index = gpu_stage == 0u ? 0u : 1u;
    static_plan_stage_calls[stage_index]++;
    for (slot = 0; slot < VP_GXM_FRAGMENT_SAMPLER_LIMIT; ++slot)
        if ((out->declared_mask & (1u << slot)) != 0u)
            static_plan_slot_calls[stage_index][slot]++;
    return 0;
}

static int static_preload_observed(void) {
    uint32_t i;
    unsigned loaded = 0, observed = 0;
    for (i = 0; i < static_index.program_count; ++i) {
        const VPStaticProgram *program = &static_index.programs[i];
        unsigned viewport;
        if ((program->flags & VP_STATIC_INDEX_PROGRAM_OBSERVED) == 0u) continue;
        observed++;
        for (viewport = 0; viewport < 2u; ++viewport) {
            uint16_t stages[2];
            unsigned role;
            stages[0] = program->vertex_stage;
            stages[1] = program->fragment_stage;
            for (role = 0; role < 2u; ++role) {
                const VPStaticVariant *variant;
                VPStaticResident *resident;
                if (stages[role] == VP_STATIC_INDEX_STAGE_NONE) continue;
                variant = vp_static_variant_for(&static_index, stages[role], viewport);
                if (variant == NULL) {
                    glog("VP_GXM_STATIC_PRELOAD missing token=%u role=%u "
                         "stage=%u viewport=%u\n", program->token, role,
                         stages[role], viewport);
                    return -1;
                }
                if (static_resident_ensure(stages[role],
                        (uint16_t)(variant - static_index.variants), &resident) < 0)
                    return -1;
                loaded++;
            }
        }
    }
    glog("VP_GXM_STATIC_PRELOAD observed_programs=%u requests=%u resident=%u "
         "bytes=%u\n", observed, loaded, static_resident_used,
         static_resident_bytes);
    return 0;
}

/* Stage the bring-up token (0xf9d) into the legacy shader table so the existing
 * first-draw and display-present code keeps using the same reviewed pair. */
static int static_register_bringup(unsigned token) {
    const VPStaticProgram *program = vp_static_program_for_token(&static_index, token);
    const VPStaticVariant *vs_variant, *fs_variant;
    VPStaticResident *vs_res, *fs_res;
    if (program == NULL || program->vertex_stage == VP_STATIC_INDEX_STAGE_NONE ||
        program->fragment_stage == VP_STATIC_INDEX_STAGE_NONE) {
        glog("VP_GXM_STATIC_BRINGUP token=0x%x missing\n", token);
        return -1;
    }
    vs_variant = vp_static_variant_for(&static_index, program->vertex_stage, 0u);
    fs_variant = vp_static_variant_for(&static_index, program->fragment_stage, 0u);
    if (vs_variant == NULL || fs_variant == NULL) {
        glog("VP_GXM_STATIC_BRINGUP token=0x%x missing viewport0 variant\n", token);
        return -1;
    }
    if (static_resident_ensure(program->vertex_stage,
            (uint16_t)(vs_variant - static_index.variants), &vs_res) < 0 ||
        static_resident_ensure(program->fragment_stage,
            (uint16_t)(fs_variant - static_index.variants), &fs_res) < 0)
        return -1;
    if (shader_count + 2u > VP_GXM_PROGRAM_STORE) return -1;
    memset(&shaders[shader_count], 0, sizeof(shaders[shader_count]));
    shaders[shader_count].stage = 1u;
    shaders[shader_count].viewport_transform = 0u;
    shaders[shader_count].hash = token;
    shaders[shader_count].id = vs_res->id;
    shaders[shader_count].program = vs_res->program;
    shader_count++;
    memset(&shaders[shader_count], 0, sizeof(shaders[shader_count]));
    shaders[shader_count].stage = 5u;
    shaders[shader_count].viewport_transform = 0u;
    shaders[shader_count].hash = token;
    shaders[shader_count].id = fs_res->id;
    shaders[shader_count].program = fs_res->program;
    shader_count++;
    glog("VP_GXM_STATIC_BRINGUP token=0x%x vs_stage=%u fs_stage=%u\n",
         token, program->vertex_stage, program->fragment_stage);
    return 0;
}

static int static_build_pair(VPStaticPair *pair, const VPStaticResident *vs_res,
                             const VPStaticResident *fs_res,
                             const VPGxmDrawRequest *req,
                             const SceGxmBlendInfo *blend) {
    SceGxmVertexAttribute attrs[16];
    SceGxmVertexStream streams[2];
    SceGxmVertexProgram *vs = NULL;
    SceGxmFragmentProgram *fs = NULL;
    unsigned i, attr_count = 0, packed_count = 0;
    uint32_t packed_mask = 0;
    int rc;

    memset(attrs, 0, sizeof(attrs));
    for (i = 0; i < 16u; ++i) {
        uint32_t d = req->attributes[i];
        int has_float = vs_res->refl.attr[i] != NULL;
        int has_uint = vs_res->refl.attr_uint[i] != NULL;
        const SceGxmProgramParameter *p;
        VpVertexFormat format;
        SceGxmVertexAttribute *a = &attrs[attr_count];
        if (!has_float && !has_uint) continue;
        if (vp_vertex_format_check_input(d, has_float, has_uint) < 0) {
            glog("VP_GXM_STATIC_ATTRIBUTE type mismatch stage=%u attr=%u raw=0x%08x "
                 "uint=%u\n", vs_res->stage_index, i, d, (unsigned)has_uint);
            return -1;
        }
        p = has_uint ? vs_res->refl.attr_uint[i] : vs_res->refl.attr[i];
        if (vp_vertex_format_decode(d, req->stride, &format) < 0) {
            glog("VP_GXM_STATIC_ATTRIBUTE unsupported stage=%u attr=%u raw=0x%08x "
                 "stride=%u\n", vs_res->stage_index, i, d, req->stride);
            return -1;
        }
        a->regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(p);
        if (format.kind == VP_VERTEX_FORMAT_RGB10A2_UNORM) {
            if (packed_count == VP_VERTEX_PACK_SLOTS || packed_count >= 16u) {
                glog("VP_GXM_STATIC_ATTRIBUTE pack_limit stage=%u attr=%u\n",
                     vs_res->stage_index, i);
                return -1;
            }
            a->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
            a->componentCount = 4u;
            a->streamIndex = 1u;
            a->offset = (uint16_t)(packed_count * 16u);
            packed_mask |= 1u << i;
            ++packed_count;
            {
                /* Pair rebuilds are expected while materials stream in; keep
                 * the first occurrences with full identity, then only periodic
                 * evidence. No per-draw I/O is added on the successful path. */
                static unsigned packed_attr_logs;
                if (packed_attr_logs < 16u ||
                    (packed_attr_logs % 256u) == 0u)
                    glog("VP_GXM_STATIC_ATTRIBUTE packed stage=%u attr=%u raw=0x%08x "
                         "slot=%u source_offset=%u n=%u\n", vs_res->stage_index, i, d,
                         packed_count - 1u, format.source_offset, packed_attr_logs);
                packed_attr_logs++;
            }
            ++attr_count;
            continue;
        }
        a->offset = (uint16_t)format.source_offset;
        a->streamIndex = 0;
        switch (format.kind) {
        case VP_VERTEX_FORMAT_F32:
            a->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
            a->componentCount = (uint8_t)format.components;
            break;
        case VP_VERTEX_FORMAT_U8N:
            a->format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
            a->componentCount = 4u;
            break;
        case VP_VERTEX_FORMAT_S16N:
            a->format = SCE_GXM_ATTRIBUTE_FORMAT_S16N;
            a->componentCount = 4u;
            break;
        case VP_VERTEX_FORMAT_S8N:
            a->format = SCE_GXM_ATTRIBUTE_FORMAT_S8N;
            a->componentCount = 4u;
            break;
        case VP_VERTEX_FORMAT_U8:
            a->format = SCE_GXM_ATTRIBUTE_FORMAT_U8;
            a->componentCount = 4u;
            break;
        case VP_VERTEX_FORMAT_F16:
            a->format = SCE_GXM_ATTRIBUTE_FORMAT_F16;
            a->componentCount = (uint8_t)format.components;
            break;
        default:
            return -1;
        }
        attr_count++;
    }
    memset(streams, 0, sizeof(streams));
    streams[0].stride = (uint16_t)req->stride;
    streams[0].indexSource = req->indexed && req->index_format == 2u
                           ? SCE_GXM_INDEX_SOURCE_INDEX_32BIT
                           : SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    streams[1].stride = (uint16_t)(16u * packed_count);
    streams[1].indexSource = streams[0].indexSource;
    rc = sceGxmShaderPatcherCreateVertexProgram(patcher, vs_res->id, attrs,
                                                attr_count, streams,
                                                packed_count != 0u ? 2u : 1u, &vs);
    if (rc < 0) {
        glog("VP_GXM_STATIC_PAIR vs_create stage=%u rc=%d attrs=%u stride=%u\n",
             vs_res->stage_index, rc, attr_count, req->stride);
        return rc;
    }
    rc = sceGxmShaderPatcherCreateFragmentProgram(patcher, fs_res->id,
            SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
            blend, vs_res->program, &fs);
    if (rc < 0) {
        glog("VP_GXM_STATIC_PAIR fs_create stage=%u rc=%d\n",
             fs_res->stage_index, rc);
        sceGxmShaderPatcherReleaseVertexProgram(patcher, vs);
        return rc;
    }
    pair->vs = vs;
    pair->fs = fs;
    pair->vs_program = vs_res->program;
    pair->fs_program = fs_res->program;
    pair->vs_resident = (unsigned)(vs_res - static_residents);
    pair->fs_resident = (unsigned)(fs_res - static_residents);
    pair->vs_stage = vs_res->stage_index;
    pair->fs_stage = fs_res->stage_index;
    pair->vs_variant = vs_res->variant_index;
    pair->fs_variant = fs_res->variant_index;
    pair->viewport = (uint16_t)(req->viewport_transform & 1u);
    pair->index_source = (uint16_t)streams[0].indexSource;
    pair->stride = req->stride;
    pair->color_mask = blend->colorMask;
    pair->packed_mask = packed_mask;
    pair->packed_stream_stride = (uint16_t)(16u * packed_count);
    memcpy(pair->attributes, req->attributes, sizeof(pair->attributes));
    pair->blend[0] = req->blend_color_op;
    pair->blend[1] = req->blend_color_src;
    pair->blend[2] = req->blend_color_dst;
    pair->blend[3] = req->blend_alpha_op;
    pair->blend[4] = req->blend_alpha_src;
    pair->blend[5] = req->blend_alpha_dst;
    /* The validated GXM result and the two ring sizes are properties of this
     * immutable pair. A later hit may reuse them; release/rebuild clears them
     * with the rest of the slot, so no address-based identity is involved. */
    pair->blend_info = *blend;
    pair->vs_default_uniform_bytes = vs_res->refl.default_uniform_bytes;
    pair->fs_default_uniform_bytes = fs_res->refl.default_uniform_bytes;
    pair->fs_face_state = fs_res->refl.face_state != NULL ? 1u : 0u;
    pair->last_use = ++static_clock;
    pair->used = 1;
    memset(&pair->base, 0, sizeof(pair->base));
    pair->base.hash = req->hash;
    pair->base.viewport_transform = pair->viewport;
    pair->base.stride = pair->stride;
    pair->base.vertex_id = vs_res->id;
    pair->base.fragment_id = fs_res->id;
    pair->base.vs = vs;
    pair->base.fs = fs;
    pair->base.vs_program = vs_res->program;
    pair->base.fs_program = fs_res->program;
    memcpy(pair->base.attributes, req->attributes, sizeof(pair->base.attributes));
    static_residents[pair->vs_resident].refs++;
    static_residents[pair->fs_resident].refs++;
    return 0;
}

/* VP_HOST_TEST_EXTRACT:pair_match_begin */
static int static_pair_inputs_match(const VPStaticPair *pair,
                                    const VPGxmDrawRequest *req,
                                    unsigned index_source) {
    return pair->index_source == index_source && pair->stride == req->stride &&
        pair->color_mask == color_mask_to_gxm(req->color_mask) &&
        !memcmp(pair->attributes, req->attributes, sizeof(pair->attributes)) &&
        pair->blend[0] == req->blend_color_op &&
        pair->blend[1] == req->blend_color_src &&
        pair->blend[2] == req->blend_color_dst &&
        pair->blend[3] == req->blend_alpha_op &&
        pair->blend[4] == req->blend_alpha_src &&
        pair->blend[5] == req->blend_alpha_dst;
}
/* VP_HOST_TEST_EXTRACT:pair_match_end */

static void static_release_pair(VPStaticPair *pair) {
    if (pair == NULL) return;
    /* The hint owns no reference; forget it before the slot becomes free. */
    static_pair_hint_forget(pair);
    if (!pair->used) return;
    /* Releasing the pair releases its GXM vertex/fragment programs. */
    (void)gxm_pending_scene_complete("pair-release");
    if (pair->vs != NULL) sceGxmShaderPatcherReleaseVertexProgram(patcher, pair->vs);
    if (pair->fs != NULL) sceGxmShaderPatcherReleaseFragmentProgram(patcher, pair->fs);
    if (pair->vs_resident < VP_GXM_RESIDENT_LIMIT &&
        static_residents[pair->vs_resident].used &&
        static_residents[pair->vs_resident].refs > 0u)
        static_residents[pair->vs_resident].refs--;
    if (pair->fs_resident < VP_GXM_RESIDENT_LIMIT &&
        static_residents[pair->fs_resident].used &&
        static_residents[pair->fs_resident].refs > 0u)
        static_residents[pair->fs_resident].refs--;
    memset(pair, 0, sizeof(*pair));
}

/* VP_HOST_TEST_EXTRACT:pair_probe_begin */
/* The hints are only keys back to slots produced by the original resolution
 * path. Every candidate runs the same full input comparison as the scan;
 * resident references held by used slots keep their programs alive. */
static int static_pair_hint_probe(const VPGxmDrawRequest *req,
                                  uint32_t vs_token, uint32_t fs_token,
                                  unsigned viewport, unsigned index_source,
                                  VPGxmProgramPair **out,
                                  VPStaticPair **owner) {
    for (unsigned i = 0; i < VP_STATIC_PAIR_HINTS; ++i) {
        VPStaticPairHint *hint = &static_pair_hints[i];
        VPStaticPair *pair = hint->pair;

        if (pair == NULL || !pair->used) continue;
        if (hint->vs_token != vs_token || hint->fs_token != fs_token ||
            hint->viewport != viewport)
            continue;
        if (!static_pair_inputs_match(pair, req, index_source)) continue;
        pair->last_use = ++static_clock;
        hint->age = ++static_pair_hint_clock;
        static_stats.pair_hits++;
        static_stats.pair_hint_hits++;
        *out = &pair->base;
        *owner = pair;
        return 1;
    }
    return 0;
}
/* VP_HOST_TEST_EXTRACT:pair_probe_end */

/* Non-mutating cache probe for an already-built pair. Used while a batch is
 * open: a hit only refreshes LRU age and statistics, so no resident or pair
 * can be registered/evicted before the open scene flushes. A miss returns 1
 * and the caller must flush before calling static_pair_get, which may mutate
 * both caches. */
static int static_pair_cached(const VPGxmDrawRequest *req,
                              VPGxmProgramPair **out, VPStaticPair **owner) {
    const VPStaticProgram *vs_program, *fs_program;
    const VPStaticVariant *vs_variant, *fs_variant;
    SceGxmBlendInfo blend;
    unsigned vs_token, fs_token, viewport, index_source;
    uint32_t required_uint_mask = 0;
    int vs_ambiguous = 0, fs_ambiguous = 0;
    int vs_resident, fs_resident;
    unsigned i;

    if (out == NULL || owner == NULL) return -1;
    *out = NULL;
    *owner = NULL;
    vs_token = req->vs_token;
    fs_token = req->fs_token;
    if (vs_token == 0u || fs_token == 0u) return 1;
    viewport = req->viewport_transform & 1u;
    index_source = req->indexed && req->index_format == 2u
                 ? SCE_GXM_INDEX_SOURCE_INDEX_32BIT
                 : SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if (static_pair_hint_probe(req, vs_token, fs_token, viewport, index_source,
                               out, owner)) {
        if (gxm_shared_prepare_active()) static_shared_pair_hits++;
        return 0;
    }
    vs_program = vp_static_program_for_token(&static_index, vs_token);
    fs_program = vp_static_program_for_token(&static_index, fs_token);
    if (vs_program == NULL || vs_program->vertex_stage == VP_STATIC_INDEX_STAGE_NONE ||
        fs_program == NULL || fs_program->fragment_stage == VP_STATIC_INDEX_STAGE_NONE)
        return 1;
    for (i = 0; i < 16u; ++i) {
        uint32_t descriptor = req->attributes[i];
        if (descriptor != 0u && ((descriptor >> 27) & 7u) == 4u)
            required_uint_mask |= 1u << i;
    }
    vs_variant = vp_static_variant_select(&static_index, vs_program->vertex_stage,
                                          viewport, required_uint_mask, &vs_ambiguous);
    fs_variant = vp_static_variant_select(&static_index, fs_program->fragment_stage,
                                          viewport, 0u, &fs_ambiguous);
    if (vs_ambiguous || fs_ambiguous ||
        vs_variant == NULL || fs_variant == NULL)
        return 1;
    vs_resident = static_resident_find(vs_program->vertex_stage,
                                       (uint16_t)(vs_variant - static_index.variants));
    fs_resident = static_resident_find(fs_program->fragment_stage,
                                       (uint16_t)(fs_variant - static_index.variants));
    if (vs_resident < 0 || fs_resident < 0) return 1;
    /* A matching pair already ran this conversion when it was created; its
     * stored blend_info is part of the immutable pair. Shared mode therefore
     * skips the repeated build on a hit. A miss still reaches static_pair_get,
     * which performs the original conversion and reports its exact failure. */
    if (!gxm_shared_prepare_active() && build_blend_info(req, &blend) < 0)
        return 1;
    for (i = 0; i < VP_GXM_STATIC_PAIR_LIMIT; ++i) {
        VPStaticPair *pair = &static_pairs[i];
        if (!pair->used) continue;
        if (pair->vs_resident == (unsigned)vs_resident &&
            pair->fs_resident == (unsigned)fs_resident &&
            static_pair_inputs_match(pair, req, index_source)) {
            pair->last_use = ++static_clock;
            static_stats.pair_hits++;
            if (gxm_shared_prepare_active()) static_shared_pair_hits++;
            /* Established only after the original resolution matched. */
            static_pair_hint_set(pair, vs_token, fs_token, viewport);
            *out = &pair->base;
            *owner = pair;
            return 0;
        }
    }
    return 1;
}

static int static_pair_get(const VPGxmDrawRequest *req, VPGxmProgramPair **out,
                           VPStaticPair **owner) {
    const VPStaticProgram *vs_program, *fs_program;
    const VPStaticVariant *vs_variant, *fs_variant;
    VPStaticResident *vs_res, *fs_res;
    SceGxmBlendInfo blend;
    unsigned vs_token, fs_token, viewport, index_source;
    unsigned i, victim;
    VPPairCacheScan scan;
    uint32_t required_uint_mask = 0;
    int vs_ambiguous = 0, fs_ambiguous = 0;
    int rc;

    if (out == NULL || owner == NULL) return -1;
    *out = NULL;
    *owner = NULL;
    vs_token = req->vs_token;
    fs_token = req->fs_token;
    /* Both stage identities are required. Falling back to the legacy VS-only
     * hash would silently pair a fragment program whose token was never
     * observed, which is exactly what this index exists to prevent. */
    if (vs_token == 0u || fs_token == 0u) {
        glog("VP_GXM_STATIC_MISS missing-token idx=%u vs=0x%x fs=0x%x\n",
             req->draw_index, vs_token, fs_token);
        return -1;
    }
    vs_program = vp_static_program_for_token(&static_index, vs_token);
    fs_program = vp_static_program_for_token(&static_index, fs_token);
    if (vs_program == NULL || vs_program->vertex_stage == VP_STATIC_INDEX_STAGE_NONE) {
        glog("VP_GXM_STATIC_MISS vs-token=0x%x idx=%u\n", vs_token, req->draw_index);
        return -1;
    }
    if (fs_program == NULL || fs_program->fragment_stage == VP_STATIC_INDEX_STAGE_NONE) {
        glog("VP_GXM_STATIC_MISS fs-token=0x%x idx=%u\n", fs_token, req->draw_index);
        return -1;
    }
    viewport = req->viewport_transform & 1u;
    index_source = req->indexed && req->index_format == 2u
                 ? SCE_GXM_INDEX_SOURCE_INDEX_32BIT
                 : SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    for (i = 0; i < 16u; ++i) {
        uint32_t descriptor = req->attributes[i];
        if (descriptor != 0u && ((descriptor >> 27) & 7u) == 4u)
            required_uint_mask |= 1u << i;
    }
    vs_variant = vp_static_variant_select(&static_index, vs_program->vertex_stage,
                                          viewport, required_uint_mask, &vs_ambiguous);
    fs_variant = vp_static_variant_select(&static_index, fs_program->fragment_stage,
                                          viewport, 0u, &fs_ambiguous);
    if (vs_ambiguous || fs_ambiguous) {
        glog("VP_GXM_STATIC_MISS ambiguous_specialization idx=%u vs=0x%x fs=0x%x "
             "viewport=%u uint_mask=0x%x\n", req->draw_index, vs_token, fs_token,
             viewport, required_uint_mask);
        return -1;
    }
    if (vs_variant == NULL || fs_variant == NULL) {
        glog("VP_GXM_STATIC_MISS variant idx=%u vs=0x%x fs=0x%x viewport=%u "
             "vs_stage=%u fs_stage=%u\n", req->draw_index, vs_token, fs_token,
             viewport, vs_program->vertex_stage, fs_program->fragment_stage);
        return -1;
    }
    if (static_resident_ensure(vs_program->vertex_stage,
            (uint16_t)(vs_variant - static_index.variants), &vs_res) < 0 ||
        static_resident_ensure(fs_program->fragment_stage,
            (uint16_t)(fs_variant - static_index.variants), &fs_res) < 0)
        return -1;
    if (build_blend_info(req, &blend) < 0) return -1;

    /* Scan the whole cache: a failed build leaves a hole that must not hide a
     * later live match, and a free slot is tracked independently of the LRU
     * victim. Selecting a free slot early also made a full cache always evict
     * slot 0 because the victim was frozen on the first iteration. */
    vp_pair_cache_scan_begin(&scan);
    for (i = 0; i < VP_GXM_STATIC_PAIR_LIMIT; ++i) {
        VPStaticPair *pair = &static_pairs[i];
        int is_free = !pair->used;
        int is_hit = !is_free &&
            pair->vs_resident == (unsigned)(vs_res - static_residents) &&
            pair->fs_resident == (unsigned)(fs_res - static_residents) &&
            static_pair_inputs_match(pair, req, index_source);
        if (vp_pair_cache_scan_slot(&scan, i, is_free, is_hit,
                                    pair->last_use) == VP_PAIR_CACHE_SCAN_HIT) {
            pair->last_use = ++static_clock;
            static_stats.pair_hits++;
            if (gxm_shared_prepare_active()) static_shared_pair_hits++;
            static_pair_hint_set(pair, vs_token, fs_token, viewport);
            *out = &pair->base;
            *owner = pair;
            return 0;
        }
    }
    static_stats.pair_misses++;
    victim = vp_pair_cache_scan_victim(&scan);
    if (victim >= VP_GXM_STATIC_PAIR_LIMIT) return -1;
    if (static_pairs[victim].used) {
        static_release_pair(&static_pairs[victim]);
        static_stats.pair_evictions++;
    }
    rc = static_build_pair(&static_pairs[victim], vs_res, fs_res, req, &blend);
    if (rc < 0) {
        memset(&static_pairs[victim], 0, sizeof(static_pairs[victim]));
        return rc;
    }
    static_pair_hint_set(&static_pairs[victim], vs_token, fs_token, viewport);
    *out = &static_pairs[victim].base;
    *owner = &static_pairs[victim];
    return 0;
}

/* Cached-parameter variants of the per-draw bindings. These avoid
 * FindParameterByName on the hot path; the handles were resolved once when the
 * GXP was registered. */
static int static_set_guest_uniforms(void *buffer, const VPStaticReflection *refl,
                                     unsigned stage, const VPGxmUniform banks[18]) {
    unsigned bank;
    for (bank = 0; bank < 18u; ++bank) {
        const SceGxmProgramParameter *p = refl->cbuf[bank];
        unsigned floats;
        int rc;
        if (p == NULL) continue;
        floats = refl->cbuf_float_counts[bank];
        if (banks[bank].data == NULL || floats > banks[bank].size / sizeof(float)) {
            glog("VP_GXM_STATIC_UNIFORM missing bank=%u stage=%u need=%u available=%u\n",
                 bank, stage, floats * 4u, banks[bank].size);
            return -1;
        }
        if (buffer == NULL) continue;
        gxm_scene_probe_uniform(stage == 0u ? "vs" : "fs", bank, &banks[bank], floats);
        rc = sceGxmSetUniformDataF(buffer, p, 0, floats, banks[bank].data);
        if (rc < 0) return rc;
    }
    return 0;
}

static int static_set_texture_extent(void *buffer, const VPStaticReflection *refl,
                                     const VPGxmDrawRequest *req, int vertex) {
    unsigned slot;
    unsigned slots = vertex ? 3u : VP_GXM_FRAGMENT_SAMPLER_LIMIT;
    for (slot = 0; slot < slots; ++slot) {
        const SceGxmProgramParameter *p = refl->tex_inv_size[slot];
        const SceGxmProgramParameter *packed = refl->tex_rgba32f_packed[slot];
        unsigned width = 0u, height = 0u;
        unsigned format = 0u, packed_value = 0u;
        float inverse[2];
        if (vertex && slot == 0u) format = req->vertex_texture.tex_format;
        else if (!vertex && slot == 0u) format = req->tex_format;
        else if (!vertex) format = req->extra_textures[slot - 1u].tex_format;
        if (format == VP_TEXTURE_UPLOAD_RGBA32F_RG32F) {
            if (!vertex) {
                glog("VP_GXM_STATIC_PACKED_UNSUPPORTED slot=%u\n", slot);
                return -1;
            }
            packed_value = (req->vertex_texture.sampler[0]&7u)==2u ? 2u : 1u;
        }
        if (packed_value != 0u && packed == NULL) {
            glog("VP_GXM_STATIC_PACKED_UNIFORM_MISSING stage=%s slot=%u "
                 "format=0x%x detail=packed-texture-without-selector\n",
                 vertex ? "vs" : "fs", slot, format);
            return -1;
        }
        if (packed != NULL) {
            float value = (float)packed_value;
            if (sceGxmSetUniformDataF(buffer, packed, 0, 1, &value) < 0)
                return -1;
        }
        if (p == NULL) continue;
        if (vertex && slot == 0u) {
            width = req->vertex_texture.width;
            height = req->vertex_texture.height;
        } else if (!vertex && slot == 0u) {
            width = req->tex_width;
            height = req->tex_height;
        } else if (!vertex) {
            width = req->extra_textures[slot - 1u].width;
            height = req->extra_textures[slot - 1u].height;
        }
        if (!width || !height) {
            glog("VP_GXM_STATIC_EXTENT missing stage=%s slot=%u\n",
                 vertex ? "vs" : "fs", slot);
            return -1;
        }
        inverse[0] = 1.0f / (float)width;
        inverse[1] = 1.0f / (float)height;
        if (sceGxmSetUniformDataF(buffer, p, 0, 2, inverse) < 0) return -1;
    }
    return 0;
}

static int static_init_vertex_uniforms(void *buffer, const VPStaticReflection *refl) {
    static const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (refl->varying_one != NULL)
        return sceGxmSetUniformDataF(buffer, refl->varying_one, 0, 4, one);
    return 0;
}

static void frame_release_fs(SceGxmFragmentProgram *fs, int owned) {
    if (owned && fs != NULL)
        sceGxmShaderPatcherReleaseFragmentProgram(patcher, fs);
}

/* Bounded extra-sampler diagnostics and the per-draw descriptor copy. A GPU
 * producer's SceGxmTexture belongs to the surface table and is shared by every
 * consumer; each draw samples through its own aligned copy with the current
 * consumer sampler applied, so binding a different TSC cannot mutate the
 * producer or another consumer's view. */
typedef struct {
    _Alignas(16) SceGxmTexture texture;
} VPGxmAlignedTexture;

/* One encoded draw whose GPU work is not complete yet. All guest-sourced
 * pointers in req have been replaced by owned staging copies or cleared; the
 * guest may free its pushbuffer, vertices and textures as soon as the worker
 * submission returns. Descriptors and their staging pages live until the
 * arena's completion notification. */
/* VP_HOST_TEST_EXTRACT:direct_refs_begin */
#define VP_GXM_DIRECT_TEXTURE_MAX (VP_GXM_FRAGMENT_SAMPLER_LIMIT + 1u)
#define VP_GXM_DIRECT_QUARANTINE_MAX 64u
typedef struct {
    unsigned count;
    VPTexturePoolToken token[VP_GXM_DIRECT_TEXTURE_MAX];
} VPGxmDirectRefs;
static VPTexturePoolToken gxm_direct_quarantine[VP_GXM_DIRECT_QUARANTINE_MAX];
static unsigned gxm_direct_quarantine_count;
static uint64_t gxm_direct_quarantined;
static uint64_t gxm_direct_releases;

static int gxm_direct_refs_add(VPGxmDirectRefs *refs, VPTexturePoolToken token) {
    if (refs == NULL || refs->count >= VP_GXM_DIRECT_TEXTURE_MAX) return -1;
    refs->token[refs->count++] = token;
    return 0;
}

static unsigned gxm_direct_refs_release(VPGxmDirectRefs *refs) {
    unsigned released;
    if (refs == NULL) return 0u;
    released = refs->count;
    for (unsigned i = 0; i < refs->count; ++i) {
        vp_texture_direct_release_gpu(refs->token[i]);
        gxm_direct_releases++;
    }
    refs->count = 0u;
    return released;
}

/* A failed EndScene/notification cannot prove the GPU stopped reading these
 * payloads. Move the tokens out of the entry before any memset/reset so they
 * stay retained; the process is poisoned and the pool must not reuse them. */
static unsigned gxm_direct_refs_quarantine(VPGxmDirectRefs *refs) {
    unsigned moved = 0u;
    if (refs == NULL) return 0u;
    for (unsigned i = 0; i < refs->count; ++i) {
        gxm_direct_quarantined++;
        if (gxm_direct_quarantine_count < VP_GXM_DIRECT_QUARANTINE_MAX)
            gxm_direct_quarantine[gxm_direct_quarantine_count++] =
                refs->token[i];
        /* A dropped token still leaves the slot's gpu_refs incremented: the
         * payload is leaked, never reused or released by mistake. */
        moved++;
    }
    refs->count = 0u;
    return moved;
}

/* Production rollback policy shared by the draw-failure paths and its host
 * regression. A draw that never reached sceGxmDraw can always release. A draw
 * that did reach it may release only after the scene completion was proven
 * (EndScene + notification + Finish all succeeded). */
typedef enum {
    VP_GXM_DIRECT_ROLLBACK_RELEASE = 0,
    VP_GXM_DIRECT_ROLLBACK_QUARANTINE = 1
} VPGxmDirectRollback;
static VPGxmDirectRollback gxm_direct_rollback_decision(
        int draw_consumed, int scene_completion_proven) {
    return draw_consumed && !scene_completion_proven
               ? VP_GXM_DIRECT_ROLLBACK_QUARANTINE
               : VP_GXM_DIRECT_ROLLBACK_RELEASE;
}
static void gxm_direct_rollback_refs(VPGxmDirectRefs *refs,
                                     VPGxmDirectRollback decision) {
    if (decision == VP_GXM_DIRECT_ROLLBACK_QUARANTINE)
        gxm_direct_refs_quarantine(refs);
    else
        gxm_direct_refs_release(refs);
}
/* VP_HOST_TEST_EXTRACT:direct_refs_end */

typedef struct {
    int used;
    VPGxmDrawRequest req;
    size_t reserve_bytes;
    size_t uniform_bytes;
    size_t staging_base;
    unsigned char *staging_page;
    size_t staged_bytes;
    unsigned snapshot_base;
    uint64_t t_start;
    uint64_t t_encoded;
    VPGxmSurface *target;
    VPGxmSurface *primary_producer;
    VPGxmDepthEntry *depth_entry;
    unsigned commit_mask;
    int depth_enabled;
    int depth_only;
    int depth_written;
    int primary_depth_view;
    int extra_depth_view[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
    int vertex_depth_view;
    int primary_cpu_texture;
    int extra_cpu_texture[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
    int vertex_cpu_texture;
    int extra_bound[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
    VPGxmSurface *extra_producer[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
    VPGxmAlignedTexture primary_depth_storage;
    VPGxmAlignedTexture extra_depth_storage[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
    VPGxmAlignedTexture vertex_depth_storage;
    SceGxmTexture primary_texture;
    SceGxmTexture primary_surface_texture;
    SceGxmTexture extra_texture[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
    SceGxmTexture extra_surface_texture[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
    SceGxmTexture vertex_texture;
    /* Decoder-owned direct bindings retained for this draw's GPU lifetime.
     * Released only after a proven scene completion (or quarantined on an
     * unknown failure). */
    VPGxmDirectRefs direct;
    unsigned direct_consumed;
} VPGxmBatchEntry;

/* VP_HOST_TEST_EXTRACT:clear_reason_enum_begin */
enum {
    GXM_BATCH_REASON_SUBMIT = 0,
    GXM_BATCH_REASON_LIMIT,
    GXM_BATCH_REASON_BUDGET,
    GXM_BATCH_REASON_SCOPE,
    GXM_BATCH_REASON_RT,
    GXM_BATCH_REASON_DEPTH,
    GXM_BATCH_REASON_MODE,
    GXM_BATCH_REASON_INELIGIBLE,
    GXM_BATCH_REASON_CLEAR,
    GXM_BATCH_REASON_PRESENT,
    GXM_BATCH_REASON_READBACK,
    GXM_BATCH_REASON_RESOURCE,
    GXM_BATCH_REASON_FAILURE,
    GXM_BATCH_REASON_LEGACY,
    /* Mode-6 clear defer: submit the open scene and record the clear without
     * proving the scene complete. The recorded surface tag carries that
     * obligation to the next materialization point. Deliberately absent from
     * gxm_batch_reason_needs_sync. */
    GXM_BATCH_REASON_CLEAR_DEFER,
    GXM_BATCH_REASON_COUNT
};
/* VP_HOST_TEST_EXTRACT:clear_reason_enum_end */
_Static_assert(GXM_BATCH_REASON_COUNT <= VP_GXM_BATCH_REASON_LIMIT,
               "batch reason histogram capacity");

typedef struct {
    VPBatchPolicy policy;
    VPBatchPolicyStats stats;
    unsigned char *staging;
    size_t staging_size;
    size_t staging_used;
    unsigned staging_page;
    size_t uniform_bytes;
    /* In-flight scenes retain their own descriptors and staging pages. */
    VPTextureSnapshots *textures;
    VPGxmBatchEntry *entries;
    unsigned count;
    /* Compact producer identities of the open scene. The batch policy fixes
     * one colour target and one depth attachment identity per scene, so the
     * dependency check only needs these two VAs instead of every entry. They
     * are recorded only when a pushed entry meets the producer predicates and
     * are cleared by every scene reset. */
    uint64_t producer_color_va;
    uint64_t producer_depth_va;
    int scene_open;
    int depth_only;
    VPGxmSurface *target;
    SceGxmRenderTarget *render_target;
    VPGxmDepthEntry *depth_entry;
    VPGxmDepthDesc depth_desc;
    int depth_enabled;
    unsigned render_width;
    unsigned render_height;
    uint64_t scope;
    uint64_t opened_us;
    unsigned reason_counts[GXM_BATCH_REASON_COUNT];
    unsigned flush_logs;
} VPGxmBatch;

static VPGxmBatch gxm_batch;
static VPGxmBatchEntry *gxm_batch_current_entry;
/* checks counts every query; entry_scan_bound sums the old scan's maximum
 * entry count (hits could return earlier). source_scans counts actual scans. */
static uint64_t gxm_hazard_checks, gxm_hazard_entry_scan_bound;
static uint64_t gxm_hazard_source_scans;
static uint64_t gxm_hazard_color, gxm_hazard_depth, gxm_hazard_both;

/* ---- B85 bounded scene completion pipeline ------------------------------
 *
 * A batched scene is submitted by sceGxmEndScene; its GPU work then runs while
 * the worker keeps encoding the next scene. The GPU consumes submitted scenes
 * in order, so a later scene may already encode a draw that samples the
 * previous scene's output. What must not happen before the notification is:
 * overwriting the staged bytes/descriptors the GPU still reads,
 * unpinning upload-cache pages, freeing or resizing guest resources, and any
 * CPU readback. Those paths call gxm_pending_scene_complete() first.
 *
 * The guest's memory is otherwise safe because every guest-sourced pointer was
 * already copied into the arena at encode time. Ordinary batches pipeline by
 * default; scene-pipeline.disable selects synchronous completion. */
static VPGxmBatchEntry gxm_batch_entry_sets[VP_GXM_BATCH_ARENAS][VP_BATCH_MAX_DRAWS];
static VPTextureSnapshots gxm_batch_texture_sets[VP_GXM_BATCH_ARENAS];
static unsigned char *gxm_batch_staging_sets[VP_GXM_BATCH_ARENAS][VP_GXM_BATCH_STAGING_PAGES];
static unsigned gxm_batch_arena;
static int gxm_batch_arena_ready;

static int gxm_scene_pipeline_state = -1;
static int gxm_pipeline_capture_sync = -1;
static unsigned gxm_pipeline_capture_logs;
static int gxm_scene_pipeline_enabled(void) {
    if (gxm_scene_pipeline_state < 0) {
        FILE *marker = fopen("ux0:data/mhgu-vita-port/scene-pipeline.disable", "rb");
        gxm_scene_pipeline_state = marker == NULL;
        if (marker != NULL) fclose(marker);
        glog("VP_GXM_PIPELINE enabled=%d source=%s\n", gxm_scene_pipeline_state,
             marker != NULL ? "disable-marker" : "default");
    }
    return gxm_scene_pipeline_state;
}

static void gxm_batch_arena_init(void) {
    if (gxm_batch_arena_ready) return;
    gxm_batch_arena_ready = 1;
    gxm_batch_arena = 0u;
    gxm_batch.entries = gxm_batch_entry_sets[0];
    gxm_batch.textures = &gxm_batch_texture_sets[0];
    gxm_batch.staging = gxm_batch_staging_sets[0][0];
}

static void gxm_batch_arena_set(unsigned arena) {
    gxm_batch_arena = arena % VP_GXM_BATCH_ARENAS;
    gxm_batch.entries = gxm_batch_entry_sets[gxm_batch_arena];
    gxm_batch.textures = &gxm_batch_texture_sets[gxm_batch_arena];
    gxm_batch.staging_page = 0u;
    gxm_batch.staging = gxm_batch_staging_sets[gxm_batch_arena][0];
    gxm_batch.staging_size = gxm_batch.staging != NULL
        ? VP_GXM_BATCH_PAGE_BYTES : 0u;
}

/* Worker-owned; reset after every BeginScene, including after auxiliary draws. */
static struct {
    int valid;
    float viewport[6];
    unsigned clip[5];
    SceGxmDepthFunc depth_func;
    int bias_factor, bias_units;
    unsigned depth_write;
    SceGxmCullMode cull;
    /* Repeats within one scene are common; the context keeps the binding
     * until EndScene. Owned (non-static) fragment programs are never cached:
     * their address is freed after the draw. */
    SceGxmVertexProgram *vertex_program;
    SceGxmFragmentProgram *fragment_program;
} gxm_draw_state;
static uint64_t gxm_state_sets, gxm_state_skips;

static void frame_set_cull(SceGxmCullMode cull) {
    if (!gxm_draw_state.valid || gxm_draw_state.cull != cull) {
        sceGxmSetCullMode(gxm_context, cull);
        gxm_draw_state.cull = cull;
        gxm_state_sets++;
    } else gxm_state_skips++;
}

/* Worker-only override for the opt-in production-executor diagnostic. */
static int gxm_batch_test_mode = -1;
/* Clear fusion stays in its legacy standalone path during the self test. */
static int gxm_batch_test_active(void) {
    return gxm_batch_test_mode >= 0;
}

static int gxm_batch_flush_worker(unsigned reason);
static int surface_recycle_slot(void) {
    unsigned victim = 0;
    static unsigned evictions;
    for (unsigned i = 1; i < VP_GXM_SURFACE_LIMIT; ++i)
        if (gxm_surfaces[i].last_use < gxm_surfaces[victim].last_use)
            victim = i;
    /* Entries contain producer pointers: finish both the open batch and all
     * deferred scenes before reusing their surface slot or GPU allocation. */
    if (gxm_batch_flush_worker(GXM_BATCH_REASON_RESOURCE) < 0 ||
        gxm_pending_scene_complete("surface-recycle") < 0)
        return -1;
    if (evictions < 16u || (evictions % 256u) == 0u)
        glog("VP_GXM_SURFACE_RECYCLE slot=%u va=0x%llx size=%ux%u n=%u\n",
             victim, (unsigned long long)gxm_surfaces[victim].va,
             gxm_surfaces[victim].width, gxm_surfaces[victim].height, evictions);
    evictions++;
    surface_release(&gxm_surfaces[victim]);
    return (int)victim;
}
static int gxm_batch_finish_aborted_worker(const char *why, int draw_rc);
static void *gxm_batch_entry_alloc(VPGxmBatchEntry *entry, size_t bytes,
                                   size_t align);
static size_t gxm_batch_align_size(size_t bytes);
static size_t gxm_batch_reserve_for(const VPGxmDrawRequest *req,
                                    const VPStaticPair *pair, int depth_only,
                                    size_t uniform_bytes);
static int gxm_batch_aux_disabled(void);
static int static_pair_cached(const VPGxmDrawRequest *req,
                              VPGxmProgramPair **out, VPStaticPair **owner);

static int gxm_extra_reject(unsigned draw_index, unsigned slot,
                            const char *reason,
                            const VPGxmExtraTexture *extra, uint64_t target_va) {
    gxm_extra_rejects++;
    if (strstr(reason, "feedback") != NULL) gxm_extra_reject_feedback++;
    else if (strstr(reason, "uninitialized") != NULL) gxm_extra_reject_uninitialized++;
    else if (strstr(reason, "missing") != NULL) gxm_extra_reject_missing++;
    else if (strstr(reason, "sampler") != NULL) gxm_extra_reject_sampler++;
    else if (strstr(reason, "layout") != NULL) gxm_extra_reject_layout++;
    else if (strstr(reason, "format") != NULL || strcmp(reason, "header") == 0 ||
             strcmp(reason, "component-type") == 0 || strcmp(reason, "swizzle") == 0)
        gxm_extra_reject_format++;
    else gxm_extra_reject_view++;
    if (gxm_extra_reject_logs < 48u || (gxm_extra_reject_logs % 256u) == 0u)
        glog("VP_GXM_EXTRA_TEX_REJECT idx=%u slot=%u reason=%s resolve=%d "
             "producer=0x%llx target=0x%llx view=%ux%u ticfmt=%u word0=%08x "
             "type=%u header=%u depth=%u block_d=%u mips=%u storage=%u "
             "consumed_gpu:%u consumed_cpu:%u reject:%u\n",
             draw_index, slot, reason, extra->resolve_rc,
             (unsigned long long)extra->va, (unsigned long long)target_va,
             extra->width, extra->height, extra->tic_format, extra->swizzle,
             extra->texture_type, extra->tic_header, extra->depth,
             extra->block_depth, extra->max_mip_level, (unsigned)extra->storage,
             gxm_extra_gpu_binds, gxm_extra_cpu_binds, gxm_extra_rejects);
    gxm_extra_reject_logs++;
    return -2;
}

/* Log the source whose content was actually consumed by a successful draw
 * (bind + sceGxmDraw + EndScene/notification/Finish). First 12 of each source,
 * then every 256th use. */
static void gxm_extra_consume_log(unsigned draw_index, unsigned slot,
                                  const VPGxmExtraTexture *extra,
                                  const VPGxmSurface *producer,
                                  uint64_t target_va, unsigned *source_count) {
    unsigned n = (*source_count)++;
    if (n >= 12u && (n % 256u) != 0u) return;
    if (producer != NULL)
        glog("VP_GXM_EXTRA_TEX_CONSUME idx=%u slot=%u source=gpu producer=0x%llx "
             "target=0x%llx view=%ux%u surface=%ux%u ticfmt=%u word0=%08x "
             "draws=%u color_drawn=%u clear_mask=0x%x samples=%u "
             "consumed_gpu:%u consumed_cpu:%u reject:%u\n", draw_index, slot,
             (unsigned long long)extra->va, (unsigned long long)target_va,
             extra->width, extra->height, producer->width, producer->height,
             extra->tic_format, extra->swizzle, producer->draw_count,
             producer->color_drawn, producer->color_clear_mask,
             producer->sampled_count, gxm_extra_gpu_binds,
             gxm_extra_cpu_binds, gxm_extra_rejects);
    else
        glog("VP_GXM_EXTRA_TEX_CONSUME idx=%u slot=%u source=cpu va=0x%llx "
             "target=0x%llx view=%ux%u upload_fmt=%u cube=%u mips=%u "
             "consumed_gpu:%u consumed_cpu:%u reject:%u\n", draw_index, slot,
             (unsigned long long)extra->va, (unsigned long long)target_va,
             extra->width, extra->height, extra->tex_format, extra->cube,
             extra->mip_count, gxm_extra_gpu_binds, gxm_extra_cpu_binds,
             gxm_extra_rejects);
}

/* Single mapping from the checked upload identity to GXM. The enum semantics
 * were confirmed in the actual VitaSDK gxm.h: SWIZZLE1_R is (R,0,0,1),
 * SWIZZLE2_GR is (R,G,0,1), and UBC4/SBC4/UBC5/SBC5 select unsigned/signed
 * block formats. Unknown bits, signedness and swizzle combinations are
 * refusals rather than channel-replication guesses. */
static int frame_gxm_texture_format(unsigned upload,SceGxmTextureFormat *out) {
    static const SceGxmTextureFormat ubc4[8]={
        SCE_GXM_TEXTURE_FORMAT_UBC4_R,
        SCE_GXM_TEXTURE_FORMAT_UBC4_000R,
        SCE_GXM_TEXTURE_FORMAT_UBC4_111R,
        SCE_GXM_TEXTURE_FORMAT_UBC4_RRRR,
        SCE_GXM_TEXTURE_FORMAT_UBC4_0RRR,
        SCE_GXM_TEXTURE_FORMAT_UBC4_1RRR,
        SCE_GXM_TEXTURE_FORMAT_UBC4_R000,
        SCE_GXM_TEXTURE_FORMAT_UBC4_R111
    };
    static const SceGxmTextureFormat sbc4[8]={
        SCE_GXM_TEXTURE_FORMAT_SBC4_R,
        SCE_GXM_TEXTURE_FORMAT_SBC4_000R,
        SCE_GXM_TEXTURE_FORMAT_SBC4_111R,
        SCE_GXM_TEXTURE_FORMAT_SBC4_RRRR,
        SCE_GXM_TEXTURE_FORMAT_SBC4_0RRR,
        SCE_GXM_TEXTURE_FORMAT_SBC4_1RRR,
        SCE_GXM_TEXTURE_FORMAT_SBC4_R000,
        SCE_GXM_TEXTURE_FORMAT_SBC4_R111
    };
    static const SceGxmTextureFormat ubc5[6]={
        SCE_GXM_TEXTURE_FORMAT_UBC5_GR,
        SCE_GXM_TEXTURE_FORMAT_UBC5_00GR,
        SCE_GXM_TEXTURE_FORMAT_UBC5_GRRR,
        SCE_GXM_TEXTURE_FORMAT_UBC5_RGGG,
        SCE_GXM_TEXTURE_FORMAT_UBC5_GRGR,
        SCE_GXM_TEXTURE_FORMAT_UBC5_00RG
    };
    static const SceGxmTextureFormat sbc5[6]={
        SCE_GXM_TEXTURE_FORMAT_SBC5_GR,
        SCE_GXM_TEXTURE_FORMAT_SBC5_00GR,
        SCE_GXM_TEXTURE_FORMAT_SBC5_GRRR,
        SCE_GXM_TEXTURE_FORMAT_SBC5_RGGG,
        SCE_GXM_TEXTURE_FORMAT_SBC5_GRGR,
        SCE_GXM_TEXTURE_FORMAT_SBC5_00RG
    };
    if (out==NULL) return -1;
    if (upload==0u) { *out=SCE_GXM_TEXTURE_FORMAT_A8B8G8R8; return 0; }
    if (upload==VP_TEXTURE_UPLOAD_RGBA16F) {
        *out=SCE_GXM_TEXTURE_FORMAT_F16F16F16F16_ABGR;
        return 0;
    }
    if (upload==VP_TEXTURE_UPLOAD_RGBA32F_RG32F) {
        *out=SCE_GXM_TEXTURE_FORMAT_F32F32_GR;
        return 0;
    }
    if (upload==0x24u) { *out=SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR; return 0; }
    if (upload==0x124u) { *out=SCE_GXM_TEXTURE_FORMAT_UBC1_1BGR; return 0; }
    if (upload==0x26u) { *out=SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR; return 0; }
    if (upload==0x126u) { *out=SCE_GXM_TEXTURE_FORMAT_UBC3_1BGR; return 0; }
    if ((upload&~((unsigned)0xffu|VP_TEXTURE_UPLOAD_SIGNED|
                  VP_TEXTURE_UPLOAD_SWIZZLE_MASK))!=0u) return -1;
    unsigned base=upload&0xffu;
    unsigned code=(upload&VP_TEXTURE_UPLOAD_SWIZZLE_MASK)>>
                  VP_TEXTURE_UPLOAD_SWIZZLE_SHIFT;
    int sign=(upload&VP_TEXTURE_UPLOAD_SIGNED)!=0u;
    if (base==0x27u) {
        if (code>7u) return -1;
        *out=sign?sbc4[code]:ubc4[code];
        return 0;
    }
    if (base==0x28u) {
        if (code>5u) return -1;
        *out=sign?sbc5[code]:ubc5[code];
        return 0;
    }
    return -1;
}

/* Maxwell TSC address modes 0..4 map onto GXM sampler modes. Mode 3 is
 * clamp-to-border. The VitaSDK exposes no GXM border-colour setter, so the
 * native sampler uses GXM's default border; that gap is logged with the
 * guest's border words the first few times it is used. */
static int gxm_texture_addr_mode(unsigned raw, SceGxmTextureAddrMode *out) {
    switch (raw & 7u) {
    case 0u: *out = SCE_GXM_TEXTURE_ADDR_REPEAT; return 0;
    case 1u: *out = SCE_GXM_TEXTURE_ADDR_MIRROR; return 0;
    case 2u: *out = SCE_GXM_TEXTURE_ADDR_CLAMP; return 0;
    case 3u: *out = SCE_GXM_TEXTURE_ADDR_CLAMP_FULL_BORDER; return 0;
    case 4u: *out = SCE_GXM_TEXTURE_ADDR_MIRROR_CLAMP; return 0;
    default: return -1;
    }
}

static unsigned gxm_addr_border_logs;

static void gxm_note_addr_border(unsigned raw_u, unsigned raw_v,
                                 const uint32_t tsc[8], unsigned draw_index) {
    if (raw_u != 3u && raw_v != 3u) return;
    if (gxm_addr_border_logs >= 8u) return;
    gxm_addr_border_logs++;
    glog("VP_GXM_SAMPLER border-mode idx=%u u=%u v=%u border=%08x,%08x,%08x,%08x "
         "native=CLAMP_FULL_BORDER border_colour=default\n", draw_index,
         raw_u, raw_v, tsc[4], tsc[5], tsc[6], tsc[7]);
}

/* All frame_draw_internal calls run on the single GXM worker; the precheck and
 * the later apply therefore share this cache without another lock. The key is
 * the full sampler word pair plus primary-vs-mip kind. */
static const VPGxmSamplerPlan *gxm_sampler_plan_for(
        unsigned kind, const uint32_t sampler[2]) {
    return vp_gxm_sampler_plan_cache_get(gxm_sampler_plan_cache,
                                         VP_GXM_SAMPLER_PLAN_CACHE_ENTRIES,
                                         kind, sampler[0], sampler[1],
                                         &gxm_sampler_plan_hits,
                                         &gxm_sampler_plan_misses);
}

static int frame_apply_extra_sampler(SceGxmTexture *texture,
                                     const VPGxmExtraTexture *extra,
                                     unsigned draw_index, unsigned slot) {
    SceGxmTextureAddrMode u_mode, v_mode;
    if (gxm_shared_prepare_active()) {
        const VPGxmSamplerPlan *plan =
            gxm_sampler_plan_for(VP_GXM_SAMPLER_PLAN_MIP, extra->sampler);
        if (plan == NULL || plan->rc < 0) {
            glog("VP_GXM_FRAME_DRAW idx=%u unsupported tex%u sampler=%08x,%08x\n",
                draw_index, slot, extra->sampler[0], extra->sampler[1]);
            return -2;
        }
        gxm_note_addr_border(plan->raw_u, plan->raw_v, extra->tsc_words,
                             draw_index);
        sceGxmTextureSetMinFilter(texture, plan->min_filter
            ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMagFilter(texture, plan->mag_filter
            ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMipFilter(texture, plan->mip_filter
            ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED
            : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
        sceGxmTextureSetUAddrMode(texture, (SceGxmTextureAddrMode)plan->addr_u);
        sceGxmTextureSetVAddrMode(texture, (SceGxmTextureAddrMode)plan->addr_v);
        return 0;
    }
    if ((extra->sampler[1]&3u)<1u || (extra->sampler[1]&3u)>2u ||
        ((extra->sampler[1]>>4)&3u)<1u || ((extra->sampler[1]>>4)&3u)>2u ||
        gxm_texture_addr_mode(extra->sampler[0],&u_mode)<0 ||
        gxm_texture_addr_mode(extra->sampler[0]>>3,&v_mode)<0) {
        glog("VP_GXM_FRAME_DRAW idx=%u unsupported tex%u sampler=%08x,%08x\n",
            draw_index, slot, extra->sampler[0], extra->sampler[1]);
        return -2;
    }
    gxm_note_addr_border(extra->sampler[0]&7u, (extra->sampler[0]>>3)&7u,
                         extra->tsc_words, draw_index);
    sceGxmTextureSetMinFilter(texture,
        ((extra->sampler[1]>>4)&3u)==2u?SCE_GXM_TEXTURE_FILTER_LINEAR:SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(texture,
        (extra->sampler[1]&3u)==2u?SCE_GXM_TEXTURE_FILTER_LINEAR:SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMipFilter(texture,
        ((extra->sampler[1]>>6)&3u)!=0u ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED :
                                         SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
    sceGxmTextureSetUAddrMode(texture, u_mode);
    sceGxmTextureSetVAddrMode(texture, v_mode);
    return 0;
}

/* Apply the validated guest TSC to the primary fragment sampler. Logging and
 * rejection stay with this helper so the cached and uncached paths behave the
 * same for unsupported sampler combinations. */
static int gxm_apply_primary_sampler(SceGxmTexture *texture,
                                     const VPGxmDrawRequest *req) {
    SceGxmTextureAddrMode u_mode, v_mode;
    if (gxm_shared_prepare_active()) {
        const VPGxmSamplerPlan *plan =
            gxm_sampler_plan_for(VP_GXM_SAMPLER_PLAN_PRIMARY, req->sampler);
        if (plan == NULL || plan->rc < 0) {
            glog("VP_GXM_SAMPLER unsupported primary hash=0x%x raw=%08x,%08x\n",
                 req->hash,req->sampler[0],req->sampler[1]);
            return -2;
        }
        gxm_note_addr_border(plan->raw_u, plan->raw_v, req->tex_tsc,
                             req->draw_index);
        /* Primary deliberately does not set a mip filter; only the extra and
         * vertex paths apply their planned mip state. */
        sceGxmTextureSetMagFilter(texture, plan->mag_filter
            ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMinFilter(texture, plan->min_filter
            ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetUAddrMode(texture, (SceGxmTextureAddrMode)plan->addr_u);
        sceGxmTextureSetVAddrMode(texture, (SceGxmTextureAddrMode)plan->addr_v);
        return 0;
    }
    if ((req->sampler[1]&3u)<1u || (req->sampler[1]&3u)>2u ||
        ((req->sampler[1]>>4)&3u)<1u || ((req->sampler[1]>>4)&3u)>2u ||
        gxm_texture_addr_mode(req->sampler[0],&u_mode)<0 ||
        gxm_texture_addr_mode(req->sampler[0]>>3,&v_mode)<0) {
        glog("VP_GXM_SAMPLER unsupported primary hash=0x%x raw=%08x,%08x\n",
             req->hash,req->sampler[0],req->sampler[1]);
        return -2;
    }
    gxm_note_addr_border(req->sampler[0]&7u, (req->sampler[0]>>3)&7u,
                         req->tex_tsc, req->draw_index);
    /* Texture initialization defaults to point sampling. The full-frame
     * path must apply the current TSC to tex0 as well as extra samplers;
     * otherwise the 64px skin atlas becomes visibly blocky. */
    sceGxmTextureSetMagFilter(texture,(req->sampler[1]&3u)==2u ?
        SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMinFilter(texture,((req->sampler[1]>>4)&3u)==2u ?
        SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(texture,u_mode);
    sceGxmTextureSetVAddrMode(texture,v_mode);
    return 0;
}

/* ---- B106 bounded CPU texture-descriptor cache ---------------------------
 *
 * The batch path used to rebuild the same SceGxmTexture for every draw that
 * samples the same owned backing with the same layout and sampler. A GXM
 * descriptor contains the GPU address, not payload ownership, so the CPU copy
 * stays valid when an arena slot is reused for identical content. The key is
 * exact; content versions, VAs, guest RAM assumptions and hashes are not used.
 * sampler_kind separates the primary path from the extra/vertex path because
 * the former deliberately does not configure a mip filter while the latter
 * two do; without it the same backing/layout/sampler could hit the wrong
 * descriptor. Direct-mapped indexing keeps the probe at fixed cost and the
 * full key is still compared before a hit is accepted. Every
 * sceGxmSetFragmentTexture/SetVertexTexture call is still issued: skipping the
 * setters left Vita3K's resource dirty state stale. Producer and depth-view
 * descriptors are not cached here. */
#define VP_GXM_TEXTURE_DESC_CACHE_ENTRIES 32u
#define VP_GXM_TEXTURE_DESC_PRIMARY 0u
#define VP_GXM_TEXTURE_DESC_EXTRA 1u

typedef struct {
    unsigned valid;
    const void *backing;
    unsigned width, height;
    unsigned format;
    unsigned cube;
    unsigned mip_count;
    unsigned sampler_kind;
    uint32_t sampler[2];
    SceGxmTexture texture;
} VPGxmTextureDescEntry;

static VPGxmTextureDescEntry gxm_texture_desc_cache[VP_GXM_TEXTURE_DESC_CACHE_ENTRIES];

static unsigned gxm_texture_desc_index(const void *backing, unsigned width,
                                       unsigned height,
                                       SceGxmTextureFormat format,
                                       unsigned cube, unsigned mip_count,
                                       unsigned sampler_kind,
                                       const uint32_t sampler[2]) {
    uintptr_t bits = (uintptr_t)backing;
    uint32_t mix = (uint32_t)(bits ^ (bits >> 16));
    mix ^= width ^ (height << 12) ^ ((unsigned)format << 5) ^ (cube << 7) ^
           (mip_count << 9) ^ (sampler_kind << 1) ^ sampler[0] ^
           (sampler[1] << 3);
    return mix & (VP_GXM_TEXTURE_DESC_CACHE_ENTRIES - 1u);
}

static int gxm_texture_desc_lookup(const void *backing, unsigned width,
                                   unsigned height, SceGxmTextureFormat format,
                                   unsigned cube, unsigned mip_count,
                                   unsigned sampler_kind,
                                   const uint32_t sampler[2],
                                   SceGxmTexture *out) {
    const VPGxmTextureDescEntry *e = &gxm_texture_desc_cache[
        gxm_texture_desc_index(backing, width, height, format, cube, mip_count,
                               sampler_kind, sampler)];
    if (!e->valid || e->backing != backing || e->width != width ||
        e->height != height || e->format != (unsigned)format ||
        e->cube != cube || e->mip_count != mip_count ||
        e->sampler_kind != sampler_kind ||
        e->sampler[0] != sampler[0] || e->sampler[1] != sampler[1])
        return 0;
    *out = e->texture;
    gxm_texture_desc_hits++;
    return 1;
}

static void gxm_texture_desc_store(const void *backing, unsigned width,
                                   unsigned height, SceGxmTextureFormat format,
                                   unsigned cube, unsigned mip_count,
                                   unsigned sampler_kind,
                                   const uint32_t sampler[2],
                                   const SceGxmTexture *texture) {
    VPGxmTextureDescEntry *e = &gxm_texture_desc_cache[
        gxm_texture_desc_index(backing, width, height, format, cube, mip_count,
                               sampler_kind, sampler)];
    e->valid = 1u;
    e->backing = backing;
    e->width = width;
    e->height = height;
    e->format = (unsigned)format;
    e->cube = cube;
    e->mip_count = mip_count;
    e->sampler_kind = sampler_kind;
    e->sampler[0] = sampler[0];
    e->sampler[1] = sampler[1];
    e->texture = *texture;
    gxm_texture_desc_misses++;
}

static unsigned gxm_depth_view_binds, gxm_depth_view_rejects, gxm_depth_view_logs;
static unsigned gxm_depth_view_consumes;
static unsigned gxm_shadow_binds;

typedef struct {
    const SceGxmProgramParameter *extent;
    const SceGxmProgramParameter *state;
    const SceGxmProgramParameter *border;
    unsigned present;
} VPGxmShadowParams;

static void static_shadow_params(const VPStaticReflection *refl, unsigned slot,
                                 VPGxmShadowParams *out) {
    memset(out, 0, sizeof(*out));
    if (refl == NULL || slot >= VP_GXM_FRAGMENT_SAMPLER_LIMIT) return;
    if ((refl->shadow_complete_mask & (1u << slot)) == 0u) return;
    out->extent = refl->shadow_extent[slot];
    out->state = refl->shadow_state[slot];
    out->border = refl->shadow_border[slot];
    out->present = 3u;
}

/* Legacy (non-index) path: no cached reflection.  The bounded tex0..tex3 ABI
 * makes the lookup cheap enough for diagnostics; production shadow programs
 * use the static index.  vertex=1 always returns an incomplete contract because
 * the helper is fragment-only. */
static void legacy_shadow_params(const SceGxmProgram *program, unsigned slot,
                                 int vertex, VPGxmShadowParams *out) {
    char name[40];
    memset(out, 0, sizeof(*out));
    if (program == NULL || slot >= VP_GXM_FRAGMENT_SAMPLER_LIMIT) return;
    snprintf(name, sizeof(name), "tex%u_shadow_extent", slot);
    out->extent = sceGxmProgramFindParameterByName(program, name);
    snprintf(name, sizeof(name), "tex%u_shadow_state", slot);
    out->state = sceGxmProgramFindParameterByName(program, name);
    snprintf(name, sizeof(name), "tex%u_shadow_border", slot);
    out->border = sceGxmProgramFindParameterByName(program, name);
    out->present = (out->extent != NULL ? 1u : 0u) +
                   (out->state != NULL ? 1u : 0u) +
                   (out->border != NULL ? 1u : 0u);
    if (out->present != 0u && (vertex ||
        out->present != 3u ||
        sceGxmProgramFindParameterByName(program, slot == 0u ? "tex0" :
            slot == 1u ? "tex1" : slot == 2u ? "tex2" : "tex3") == NULL))
        out->present = 0x80000000u | out->present;
}

static int static_shadow_layout_check(const VPStaticReflection *fs,
                                      const VPStaticReflection *vs,
                                      unsigned draw_index) {
    unsigned mask = (fs != NULL ? fs->shadow_incomplete_mask : 0u) |
                    (vs != NULL ? vs->shadow_incomplete_mask : 0u);
    if (mask == 0u) return 0;
    gxm_shadow_rejects++;
    glog("VP_GXM_SHADOW_REJECT idx=%u reason=incomplete-contract stage=fragment-or-vertex "
         "mask=0x%x action=refuse\n", draw_index, mask);
    return -1;
}

static int legacy_shadow_layout_check(const SceGxmProgram *fs,
                                      const SceGxmProgram *vs,
                                      unsigned draw_index) {
    for (unsigned slot = 0; slot < VP_GXM_FRAGMENT_SAMPLER_LIMIT; ++slot) {
        VPGxmShadowParams params;
        legacy_shadow_params(fs, slot, 0, &params);
        if (params.present != 0u && params.present != 3u) {
            gxm_shadow_rejects++;
            glog("VP_GXM_SHADOW_REJECT idx=%u reason=incomplete-contract "
                 "stage=fragment slot=%u present=0x%x action=refuse\n",
                 draw_index, slot, params.present);
            return -1;
        }
        legacy_shadow_params(vs, slot, 1, &params);
        if (params.present != 0u) {
            gxm_shadow_rejects++;
            glog("VP_GXM_SHADOW_REJECT idx=%u reason=vertex-shadow-unsupported "
                 "slot=%u present=0x%x action=refuse\n", draw_index, slot,
                 params.present & 0x7fffffffu);
            return -1;
        }
    }
    return 0;
}

/* Return 1 for software shadow, 0 for ordinary, -1 for an explicit mismatch.
 * The decision uses the sidecar shader descriptor depth flag, never the TIC
 * depth/layer field (an ordinary 2D texture also has TIC depth 1). */
static int shadow_slot_mode(const VPGxmShadowParams *params,
                            const VPGxmExtraTexture *identity,
                            unsigned source_is_depth, const char **reason) {
    return vp_gxm_shadow_source_mode(identity->descriptor_depth,
                                     params->present, source_is_depth, reason);
}

/* Bind the three float4 helper uniforms for one software-shadow sampler.  The
 * parameters were resolved once for the static path; the legacy path uses the
 * bounded texN names.  Missing parameters are refused rather than left at the
 * zero-filled default, which would silently select compare Never/wrap Repeat. */
static int frame_set_shadow_uniforms(void *buffer, const SceGxmProgram *program,
                                     const VPStaticReflection *refl,
                                     unsigned slot,
                                     const VPGxmDepthViewPlan *plan,
                                     unsigned draw_index) {
    const SceGxmProgramParameter *extent, *state, *border;
    int rc;
    if (plan == NULL ||
        plan->sampler_mode != VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW)
        return -1;
    if (refl != NULL) {
        if (slot >= VP_GXM_FRAGMENT_SAMPLER_LIMIT ||
            (refl->shadow_complete_mask & (1u << slot)) == 0u)
            return -1;
        extent = refl->shadow_extent[slot];
        state = refl->shadow_state[slot];
        border = refl->shadow_border[slot];
    } else {
        VPGxmShadowParams params;
        legacy_shadow_params(program, slot, 0, &params);
        if (params.present != 3u) return -1;
        extent = params.extent;
        state = params.state;
        border = params.border;
    }
    if (extent == NULL || state == NULL || border == NULL) return -1;
    rc = sceGxmSetUniformDataF(buffer, extent, 0, 4, plan->shadow_extent);
    if (rc < 0) return rc;
    rc = sceGxmSetUniformDataF(buffer, state, 0, 4, plan->shadow_state);
    if (rc < 0) return rc;
    rc = sceGxmSetUniformDataF(buffer, border, 0, 4, plan->shadow_border);
    if (rc < 0) return rc;
    if (gxm_shadow_binds <= 16u)
        glog("VP_GXM_SHADOW_UNIFORMS idx=%u slot=%u extent=%.1f,%.1f,%.9f,%.9f "
             "state=%u,%u,%u,%u border=%.6f mode=software-shadow\n",
             draw_index, slot, (double)plan->shadow_extent[0],
             (double)plan->shadow_extent[1], (double)plan->shadow_extent[2],
             (double)plan->shadow_extent[3], plan->shadow_compare_func,
             plan->shadow_filter_code, (unsigned)plan->shadow_state[2],
             (unsigned)plan->shadow_state[3],
             (double)plan->shadow_border[0]);
    return 0;
}

/* Shared native wrapper for every depth-sourced sampler slot.  The view is
 * created over the pool entry's own depth_mem; guest depth bytes are never
 * decoded or filled by the CPU.  The returned descriptor lives in *storage and
 * must remain valid until the synchronous EndScene/Finish completes. */
static int depth_texture_view(unsigned draw_index, unsigned slot,
                              const VPGxmExtraTexture *identity,
                              unsigned shadow_mode,
                              VPGxmDepthViewPlan *out_plan,
                              VPGxmAlignedTexture *storage,
                              SceGxmTexture **out) {
    VPGxmDepthEntry *entry;
    VPGxmDepthViewInput input;
    VPGxmDepthViewPlan plan;
    unsigned content_state;
    int rc;

    if (out == NULL) return -1;
    *out = NULL;
    if (identity == NULL || storage == NULL) return -1;
    entry = depth_entry_find(identity->va);
    if (entry == NULL) {
        gxm_depth_view_rejects++;
        if (gxm_depth_view_logs++ < 24u)
            glog("VP_GXM_DEPTH_VIEW_REJECT idx=%u slot=%u va=0x%llx "
                 "reason=missing-owned-depth-entry consumed=%u reject=%u\n",
                 draw_index, slot, (unsigned long long)identity->va,
                 gxm_depth_view_binds, gxm_depth_view_rejects);
        return -1;
    }
    /* A guest clear precedes this consumer.  Materialize it on the GPU now;
     * if that fails the entry stays CLEARED and the planner below refuses it.
     * No placeholder depth is ever published. */
    if (depth_clear_materialize_pending(entry, draw_index,
                                        "depth-view-consume") < 0 &&
        entry->clear_defined && !entry->depth_valid) {
        gxm_depth_view_rejects++;
        if (gxm_depth_view_logs++ < 48u)
            glog("VP_GXM_DEPTH_VIEW_REJECT idx=%u slot=%u va=0x%llx "
                 "reason=clear-materialize-failed consumed=%u reject=%u\n",
                 draw_index, slot, (unsigned long long)identity->va,
                 gxm_depth_view_binds, gxm_depth_view_rejects);
        return -1;
    }
    content_state = entry->depth_valid ? VP_GXM_DEPTH_CONTENT_VALID :
                    entry->clear_defined ? VP_GXM_DEPTH_CONTENT_CLEARED :
                                           VP_GXM_DEPTH_CONTENT_NONE;
    memset(&input, 0, sizeof(input));
    input.va = identity->va;
    input.tic = identity->tic_words;
    input.tsc = identity->tsc_words;
    input.expected_descriptor_type = identity->descriptor_type;
    input.descriptor_depth = identity->descriptor_depth;
    input.sampler_mode = shadow_mode
                             ? VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW
                             : VP_GXM_DEPTH_SAMPLER_MODE_ORDINARY;
    input.shadow_contract_complete = shadow_mode ? 1u : 0u;
    input.usage_flags = identity->usage_flags;
    input.entry_va = entry->desc.va;
    input.entry_width = entry->desc.width;
    input.entry_height = entry->desc.height;
    input.entry_format_class = entry->desc.format_class;
    input.entry_stride_samples = entry->desc.stride_samples;
    input.content_state = content_state;
    input.content_generation = entry->content_generation;
    input.entry_storage_generation = entry->storage_generation;
    rc = vp_gxm_depth_view_plan(&input, &plan);
    if (rc != VP_GXM_DEPTH_VIEW_OK) {
        static unsigned reject_logs;
        gxm_depth_view_rejects++;
        if (rc == VP_GXM_DEPTH_VIEW_ERR_SHADOW_POLICY) gxm_shadow_rejects++;
        if (reject_logs < 32u || (reject_logs % 128u) == 0u)
            glog("VP_GXM_DEPTH_VIEW_REJECT idx=%u slot=%u va=0x%llx "
                 "reason=%s shadow_status=%s detail=planner mode=%s state=%u "
                 "gen=%u entry=%ux%u ticfmt=%u consumed=%u reject=%u "
                 "tsc=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
                 draw_index, slot, (unsigned long long)identity->va,
                 vp_gxm_depth_view_status_name(rc),
                 vp_gxm_shadow_status_name(plan.shadow_status),
                 shadow_mode ? "software-shadow" : "ordinary", content_state,
                 entry->content_generation, entry->desc.width,
                 entry->desc.height, identity->tic_words[0] & 0x7fu,
                 gxm_depth_view_binds, gxm_depth_view_rejects,
                 identity->tsc_words[0], identity->tsc_words[1],
                 identity->tsc_words[2], identity->tsc_words[3],
                 identity->tsc_words[4], identity->tsc_words[5],
                 identity->tsc_words[6], identity->tsc_words[7]);
        reject_logs++;
        return rc;
    }
    memset(storage, 0, sizeof(*storage));
    /* Validate the original TIC against the logical image before selecting
     * the physical sampling view. Guest descriptor bytes stay unchanged. */
    plan.width = entry->native_desc.width;
    plan.height = entry->native_desc.height;
    rc = vp_gxm_depth_native_init(&storage->texture, entry->depth_mem, &plan);
    if (rc < 0) {
        gxm_depth_view_rejects++;
        if (gxm_depth_view_logs++ < 48u)
            glog("VP_GXM_DEPTH_VIEW_REJECT idx=%u slot=%u va=0x%llx "
                 "reason=gxm-native-init rc=0x%08x %ux%u format=%u\n",
                 draw_index, slot, (unsigned long long)identity->va,
                 (unsigned)rc, plan.width, plan.height, plan.format);
        return rc;
    }
    gxm_depth_view_binds++;
    if (shadow_mode) gxm_shadow_binds++;
    if (gxm_depth_view_logs++ < 24u || (gxm_depth_view_binds % 256u) == 0u)
        glog("VP_GXM_DEPTH_VIEW_BIND idx=%u slot=%u va=0x%llx %ux%u "
             "format=%u class=%u ticfmt=%u rsource=%u usage=0x%02x gen=%u "
             "mode=%s filters=%u/%u addr=%u/%u shadow_compare=%u "
             "shadow_filter=%u wrap=%u/%u border_used=%u border=%.6f "
             "binds=%u shadow_binds=%u reject=%u\n", draw_index, slot,
             (unsigned long long)identity->va, plan.width, plan.height,
             plan.format, plan.depth_class, plan.tic_format,
             plan.source_component, identity->usage_flags,
             plan.content_generation, shadow_mode ? "software-shadow" : "ordinary",
             plan.min_filter, plan.mag_filter, plan.addr_u, plan.addr_v,
             plan.shadow_compare_func, plan.shadow_filter_code,
             (unsigned)plan.shadow_state[2], (unsigned)plan.shadow_state[3],
             plan.shadow_border_used, (double)plan.shadow_border[0],
             gxm_depth_view_binds, gxm_shadow_binds, gxm_depth_view_rejects);
    if (out_plan != NULL) *out_plan = plan;
    *out = &storage->texture;
    return 0;
}

static void gxm_stage_sha_hex(unsigned stage_index, char *out, size_t out_size) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char *sha;
    if (out == NULL || out_size < 2u) return;
    out[0] = '-';
    out[1] = 0;
    if (!static_mode || stage_index >= static_index.stage_count ||
        out_size < 65u)
        return;
    sha = static_index.stages[stage_index].sha256;
    for (unsigned i = 0; i < 32u; i++) {
        out[i * 2u] = hex[sha[i] >> 4];
        out[i * 2u + 1u] = hex[sha[i] & 15u];
    }
    out[64] = 0;
}

/* ---- bounded intra-submission draw batching ------------------------------
 *
 * The guest decodes a whole GPFIFO submission during one synchronous ioctl.
 * frame_draw_internal validates and encodes each draw on the worker; when the
 * request is quiet, static-indexed and compatible with the open scene, the
 * draw is issued into that scene instead of finishing it. Guest-sourced
 * vertices, indices and CPU textures are copied into a bounded 4 MiB mapped
 * arena before the worker returns. The submission end calls
 * vp_gxm_batch_flush before retiring its fence, so encoding is never reported
 * as GPU completion.
 *
 * Resource mutation while a batch is open is forbidden. The draw path flushes
 * before pair-cache builds/evictions, depth-pool acquisition of a different
 * identity and any surface allocation/resize. Same-identity draws reuse the
 * pinned scene target and depth entry; extra GPU producers and depth views
 * are only read, not reallocated, until the scene finishes.
 */
/* VP_HOST_TEST_EXTRACT:clear_reason_name_begin */
static const char *gxm_batch_reason_name(unsigned reason) {
    static const char *const names[GXM_BATCH_REASON_COUNT] = {
        "submit-end", "limit", "budget", "scope", "rt-change", "depth-change",
        "mode-change", "ineligible", "clear", "present", "readback",
        "resource", "failure", "legacy", "clear-defer"
    };
    return reason < GXM_BATCH_REASON_COUNT ? names[reason] : "unknown";
}
/* VP_HOST_TEST_EXTRACT:clear_reason_name_end */

/* Aggregate batch-shape evidence. The per-flush log is sampled, so these
 * counters are the authoritative histogram for the whole run. */
static void static_log_batch_hist(void) {
    unsigned i;
    glog("VP_GXM_STATE_CACHE sets=%llu skipped=%llu\n",
         (unsigned long long)gxm_state_sets,
         (unsigned long long)gxm_state_skips);
    glog("VP_GXM_BATCH_HIST size=");
    for (i = 1; i <= VP_BATCH_MAX_DRAWS; ++i)
        glog("%s%u:%u", i == 1u ? "" : ",", i,
             static_stats.batch_size_hist[i]);
    glog(" reasons=");
    for (i = 0; i < GXM_BATCH_REASON_COUNT; ++i)
        glog("%s%s:%u", i == 0u ? "" : ",", gxm_batch_reason_name(i),
             static_stats.batch_reason_hist[i]);
    glog(" sync_fallbacks=%u failed=%u pre_reject=face:%u,alpha:%u,sampler:%u\n",
         static_stats.batch_sync_fallbacks,
         static_stats.batch_failed, gxm_batch_pre_reject[0],
         gxm_batch_pre_reject[1], gxm_batch_pre_reject[2]);
    glog("VP_GXM_BATCH_DETAIL ineligible=disabled:%llu,quiet:%llu,view:%llu,"
         "hazard:%llu,feedback:%llu "
         "budget=uniform:%llu,single:%llu,used:%llu\n",
         (unsigned long long)gxm_batch_inelig[VP_GXM_BATCH_INELIG_DISABLED],
         (unsigned long long)gxm_batch_inelig[VP_GXM_BATCH_INELIG_QUIET],
         (unsigned long long)gxm_batch_inelig[VP_GXM_BATCH_INELIG_DEPTH_VIEW],
         (unsigned long long)gxm_batch_inelig[VP_GXM_BATCH_INELIG_HAZARD],
         (unsigned long long)gxm_batch_inelig[VP_GXM_BATCH_INELIG_FEEDBACK],
         (unsigned long long)gxm_batch_budget[0],
         (unsigned long long)gxm_batch_budget[1],
         (unsigned long long)gxm_batch_budget[2]);
    glog("VP_GXM_BATCH_HAZARD checks=%llu color=%llu depth=%llu both=%llu "
         "entry_scan_bound=%llu source_scans=%llu\n",
         (unsigned long long)gxm_hazard_checks,
         (unsigned long long)gxm_hazard_color,
         (unsigned long long)gxm_hazard_depth,
         (unsigned long long)gxm_hazard_both,
         (unsigned long long)gxm_hazard_entry_scan_bound,
         (unsigned long long)gxm_hazard_source_scans);
}

static unsigned gxm_batch_flush_reason_for(unsigned decision) {
    switch (decision) {
    case VP_BATCH_FLUSH_SCOPE: return GXM_BATCH_REASON_SCOPE;
    case VP_BATCH_FLUSH_RT: return GXM_BATCH_REASON_RT;
    case VP_BATCH_FLUSH_DEPTH: return GXM_BATCH_REASON_DEPTH;
    case VP_BATCH_FLUSH_MODE: return GXM_BATCH_REASON_MODE;
    case VP_BATCH_FLUSH_LIMIT: return GXM_BATCH_REASON_LIMIT;
    case VP_BATCH_FLUSH_BUDGET: return GXM_BATCH_REASON_BUDGET;
    default: return GXM_BATCH_REASON_RESOURCE;
    }
}

/* Depth-sourced samplers are legal in a shared scene as long as no pending
 * depth clear needs a GPU materialization during preparation. A pending clear
 * (clear_defined && !depth_valid) would issue a transfer/Finish while the batch
 * scene is open, so that draw takes the synchronous path and the batch is
 * flushed before it. */
static int gxm_batch_depth_view_safe(uint64_t va) {
    VPGxmDepthEntry *entry;
    if (va == 0u) return 0;
    entry = depth_entry_find(va);
    if (entry == NULL) return 0;
    if (entry->clear_defined && !entry->depth_valid) return 0;
    return 1;
}

static int gxm_batch_depth_views_ready(const VPGxmDrawRequest *req) {
    unsigned e;
    if (req->texture_source == VP_GXM_EXTRA_SOURCE_DEPTH &&
        !gxm_batch_depth_view_safe(req->texture_va))
        return 0;
    if (req->vertex_texture.source == VP_GXM_EXTRA_SOURCE_DEPTH &&
        !gxm_batch_depth_view_safe(req->vertex_texture.va))
        return 0;
    for (e = 0; e < VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u; ++e)
        if (req->extra_textures[e].source == VP_GXM_EXTRA_SOURCE_DEPTH &&
            !gxm_batch_depth_view_safe(req->extra_textures[e].va))
            return 0;
    return 1;
}

/* A draw that samples an image produced earlier inside the still-open scene
 * must not join it: the producer's GPU writes are not guaranteed visible to a
 * later draw before EndScene, and a depth producer may still be pending. Flush
 * the scene first; the synchronous path then observes the completed content.
 *
 * The scene's colour and depth identities are fixed by the batch policy, and
 * the per-entry scan only ever compared those two VAs, so one source scan
 * against the compact identities is sufficient. Only actual depth writes
 * record a depth producer. */
static void gxm_batch_note_sampled_va(uint64_t va, int *color_hit,
                                      int *depth_hit) {
    if (va == 0u) return;
    if (va == gxm_batch.producer_color_va) *color_hit = 1;
    if (va == gxm_batch.producer_depth_va) *depth_hit = 1;
}

static int gxm_batch_sample_hazard(const VPGxmDrawRequest *req) {
    int color_hit = 0, depth_hit = 0;
    unsigned e;

    gxm_hazard_checks++;
    if (gxm_batch.count == 0u) return 0;
    gxm_hazard_entry_scan_bound += gxm_batch.count;
    if (gxm_batch.producer_color_va == 0u &&
        gxm_batch.producer_depth_va == 0u)
        return 0;
    gxm_hazard_source_scans++;
    if (req->texture_source != VP_GXM_EXTRA_SOURCE_NONE)
        gxm_batch_note_sampled_va(req->texture_va, &color_hit, &depth_hit);
    if (req->vertex_texture.source != VP_GXM_EXTRA_SOURCE_NONE)
        gxm_batch_note_sampled_va(req->vertex_texture.va, &color_hit,
                                  &depth_hit);
    for (e = 0; e < VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u; ++e)
        if (req->extra_textures[e].source != VP_GXM_EXTRA_SOURCE_NONE)
            gxm_batch_note_sampled_va(req->extra_textures[e].va, &color_hit,
                                      &depth_hit);
    if (!color_hit && !depth_hit) return 0;
    if (color_hit && depth_hit)
        gxm_hazard_both++;
    else if (color_hit)
        gxm_hazard_color++;
    else
        gxm_hazard_depth++;
    return 1;
}

/* Record successfully encoded colour/depth writes after the policy push. */
static void gxm_batch_note_pushed_entry(const VPGxmBatchEntry *entry) {
    if (entry->target != NULL && !entry->depth_only &&
        (entry->commit_mask & VP_GXM_DRAW_COMMIT_COLOR_DRAWN) != 0u)
        gxm_batch.producer_color_va = entry->target->va;
    if (entry->depth_entry != NULL &&
        entry->depth_written)
        gxm_batch.producer_depth_va = entry->depth_entry->desc.va;
}

static int gxm_batch_aux_disabled(void) {    static int disabled = -1;
    if (disabled < 0) {
        FILE *marker;
        disabled = 0;
        marker = fopen("ux0:data/mhgu-vita-port/f8-probe.enable", "rb");
        if (marker != NULL) { disabled = 1; fclose(marker); }
        marker = fopen("ux0:data/mhgu-vita-port/m3-drawdump.enable", "rb");
        if (marker != NULL) { disabled = 1; fclose(marker); }
        marker = fopen("ux0:data/mhgu-vita-port/scene-probe.enable", "rb");
        if (marker != NULL) { disabled = 1; fclose(marker); }
        if (disabled)
            glog("VP_GXM_BATCH disabled=1 reason=diagnostic-marker\n");
    }
    return disabled;
}

/* Bounded batching is an explicit experimental opt-in. The disable marker wins
 * so an interrupted run cannot leave the candidate enabled by accident. */
static int gxm_batch_enabled(void) {
    static int enabled = -1;
    if (gxm_batch_test_mode >= 0) return gxm_batch_test_mode;
    if (enabled < 0) {
        FILE *marker;
        enabled = 0;
        marker = fopen("ux0:data/mhgu-vita-port/bounded-batch.disable", "rb");
        if (marker != NULL) {
            fclose(marker);
            glog("VP_GXM_BATCH_MODE enabled=0 marker=disable\n");
        } else {
            marker = fopen("ux0:data/mhgu-vita-port/bounded-batch.enable", "rb");
            if (marker != NULL) {
                fclose(marker);
                enabled = 1;
                glog("VP_GXM_BATCH_MODE enabled=1 marker=enable\n");
            } else {
                glog("VP_GXM_BATCH_MODE enabled=0 marker=absent\n");
            }
        }
    }
    return enabled;
}

/* VP_HOST_TEST_EXTRACT:pair_uniform_begin */
static size_t gxm_batch_align_size(size_t bytes) {
    if (bytes == 0u) return 0u;
    if (bytes > (size_t)-1 - 15u) return (size_t)-1;
    return (bytes + 15u) & ~(size_t)15u;
}

static size_t gxm_batch_add_size(size_t total, size_t bytes) {
    size_t aligned;
    if (bytes == 0u) return total;
    aligned = gxm_batch_align_size(bytes);
    if (aligned == (size_t)-1 || total > (size_t)-1 - aligned)
        return (size_t)-1;
    return total + aligned;
}

/* Uniform-ring bytes for one pair from the immutable sizes captured at pair
 * creation. The extra fragment buffer depends on this draw's live cull/face
 * state and still uses the same saturating addition as the original path. */
static size_t static_pair_uniform_bytes(const VPStaticPair *pair,
                                        const VPGxmDrawRequest *req) {
    size_t total = 0u;
    if (pair == NULL || req == NULL) return 0u;
    if (pair->vs_program != NULL)
        total = gxm_batch_add_size(total, pair->vs_default_uniform_bytes);
    if (pair->fs_program != NULL) {
        total = gxm_batch_add_size(total, pair->fs_default_uniform_bytes);
        if (pair->fs_face_state != 0u && !req->cull_enable &&
            (req->cull_face == 0x404u || req->cull_face == 0x405u ||
             req->cull_face == 0x408u))
            total = gxm_batch_add_size(total, pair->fs_default_uniform_bytes);
    }
    return total;
}
/* VP_HOST_TEST_EXTRACT:pair_uniform_end */

static VPTextureSnapshotKey gxm_snapshot_key(uint64_t va, unsigned w,
    unsigned h, unsigned format, unsigned cube, unsigned mips) {
    VPTextureSnapshotKey key = {va, w, h, format, cube, mips};
    return key;
}

static void gxm_upload_capacity_init(void) {
    if (gxm_upload_capacity_state >= 0) return;
    {
        FILE *marker = fopen(
            "ux0:data/mhgu-vita-port/upload-cache-32m.enable", "rb");
        if (marker != NULL) {
            fclose(marker);
            gxm_upload_capacity = VP_UPLOAD_CACHE_MAX_BYTES;
        }
    }
    gxm_upload_capacity_state = 1;
    glog("VP_GXM_UPLOAD_CACHE capacity=%u\n", gxm_upload_capacity);
}

/* B115 experiment: the marker deliberately removes the exact-content check
 * from all three caches (decoded RAW, GXM upload cache, scene snapshot).
 * The default path keeps the contract in texture_snapshot.h: an unmarked
 * lookup must compare current bytes. This switch is read once and logged. */
static int gxm_texture_skip_bytes_state = -1;
int vp_gxm_texture_skip_bytes_enabled(void) {
    if (gxm_texture_skip_bytes_state < 0) {
        FILE *marker = fopen(
            "ux0:data/mhgu-vita-port/texture-identity-skip-bytes.enable", "rb");
        gxm_texture_skip_bytes_state = marker != NULL;
        if (marker != NULL) fclose(marker);
        glog("VP_TEX_IDENTITY skip_bytes=%d source=%s\n",
             gxm_texture_skip_bytes_state,
             gxm_texture_skip_bytes_state ? "marker" : "default");
    }
    return gxm_texture_skip_bytes_state;
}

static const void *gxm_snapshot_find_key(const VPTextureSnapshots *cache,
                                         const VPTextureSnapshotKey *key,
                                         size_t bytes) {
    if (!bytes) return NULL;
    for (unsigned i = 0; i < cache->count; ++i) {
        const VPTextureSnapshot *e = &cache->entries[i];
        if (e->bytes == bytes && vp_texture_snapshot_key_equal(&e->key, key))
            return e->data;
    }
    return NULL;
}

static const void *gxm_snapshot_find_current(const VPTextureSnapshots *cache,
                                             const VPTextureSnapshotKey *key,
                                             const void *current, size_t bytes) {
    if (!vp_gxm_texture_skip_bytes_enabled())
        return vp_texture_snapshot_find(cache, key, current, bytes);
    const void *hit = gxm_snapshot_find_key(cache, key, bytes);
    if (hit != NULL) gxm_snapshot_skip_hits++;
    return hit;
}

static VPGxmUpload *gxm_upload_find(const VPTextureSnapshotKey *key,
                                   const void *source, size_t bytes,
                                   const void *owned_source) {
    if (!source || !bytes) return NULL;
    for (unsigned i = 0; i < VP_UPLOAD_CACHE_SLOTS; ++i) {
        VPGxmUpload *entry = &gxm_uploads[i];
        const VPTextureSnapshot *v = &entry->snapshot;
        if (v->data && v->bytes == bytes && v->key.va == key->va &&
            v->key.width == key->width && v->key.height == key->height &&
            v->key.format == key->format && v->key.cube == key->cube &&
            v->key.mips == key->mips) {
            if (vp_texture_snapshot_owned_match(v, key, owned_source, bytes)) {
                /* Association hit, not a byte comparison. The decoder retires
                 * this link before rewriting/freeing the owned allocation. */
                gxm_upload_owned_hits++;
                gxm_upload_owned_bytes += bytes;
                return entry;
            }
            if (vp_gxm_texture_skip_bytes_enabled()) {
                /* Experiment: layout+size only. No association is recorded. */
                gxm_upload_skip_hits++;
                return entry;
            }
            if (vp_texture_bytes_equal(v->data, source, bytes)) {
                /* A full comparison just proved the association. An unmarked
                 * caller never records one: its pointer is not decoder-owned. */
                if (owned_source != NULL)
                    entry->snapshot.owned_decode_source = owned_source;
                return entry;
            }
        }
    }
    return NULL;
}

static VPGxmUpload *gxm_upload_prepare(const VPTextureSnapshotKey *key,
                                      const void *source, size_t bytes,
                                      const void *owned_source) {
    VPGxmUpload *entry;
    gxm_upload_capacity_init();
    entry = gxm_upload_find(key, source, bytes, owned_source);
    if (entry) {
        entry->pinned = 1u;
        entry->stamp = ++gxm_upload_stamp;
        return entry;
    }
    /* Only populate between scenes. An open-scene miss uses owned staging. */
    if (!gxm_upload_memory && !vp_texture_direct_active() &&
        !gxm_batch.scene_open)
        gxm_upload_memory = gpu_alloc(gxm_upload_capacity, 0, NULL);
    if (!gxm_upload_memory || gxm_batch.scene_open || !source || !bytes ||
        bytes > gxm_upload_capacity) return NULL;
    unsigned pages = (bytes + VP_UPLOAD_PAGE_BYTES - 1u) / VP_UPLOAD_PAGE_BYTES;
    unsigned capacity_pages = gxm_upload_capacity / VP_UPLOAD_PAGE_BYTES;
    for (;;) {
        unsigned first = 0u, run = 0u;
        entry = NULL;
        for (unsigned i = 0; i < VP_UPLOAD_CACHE_SLOTS; ++i)
            if (!gxm_uploads[i].snapshot.data) { entry = &gxm_uploads[i]; break; }
        for (unsigned i = 0; i < capacity_pages; ++i) {
            run = gxm_upload_pages[i] ? 0u : run + 1u;
            if (run == pages) { first = i + 1u - pages; break; }
        }
        if (entry && run == pages) {
            void *data = gxm_upload_memory + first * VP_UPLOAD_PAGE_BYTES;
            memcpy(data, source, bytes);
            gxm_upload_fill_bytes += bytes;
            memset(gxm_upload_pages + first, 1, pages);
            *entry = (VPGxmUpload){{*key, data, bytes, owned_source}, first,
                                  pages, ++gxm_upload_stamp, 1u, 0u};
            gxm_upload_copies++;
            return entry;
        }
        VPGxmUpload *old = NULL;
        for (unsigned i = 0; i < VP_UPLOAD_CACHE_SLOTS; ++i) {
            VPGxmUpload *v = &gxm_uploads[i];
            if (v->snapshot.data && !v->pinned && !v->pending &&
                (!old || v->stamp < old->stamp))
                old = v;
        }
        if (!old) return NULL;
        memset(gxm_upload_pages + old->first_page, 0, old->pages);
        memset(old, 0, sizeof(*old));
    }
}

static size_t gxm_snapshot_reserve(const VPTextureSnapshotKey *key,
                                  const void *source, size_t bytes,
                                  const void *owned_source) {
    const void *hit = NULL;
    VPGxmUpload *entry = gxm_upload_prepare(key, source, bytes, owned_source);
    if (entry != NULL) {
        /* prepare already pinned/stamped this entry for the draw. The memo
         * lets the encoder reuse it without a second 80-slot search. */
        if (owned_source != NULL &&
            gxm_snapshot_memo_count < VP_SNAPSHOT_MEMO_MAX) {
            VPGxmSnapshotMemo *memo =
                &gxm_snapshot_memo[gxm_snapshot_memo_count++];
            memo->key = *key;
            memo->source = source;
            memo->bytes = bytes;
            memo->owned = NULL;
            memo->upload = entry;
        }
        return 0u;
    }
    if (owned_source != NULL)
        hit = vp_texture_snapshot_find_owned(gxm_batch.textures, key,
                                             owned_source, bytes);
    if (hit == NULL) {
        hit = gxm_snapshot_find_current(gxm_batch.textures, key, source, bytes);
        if (hit != NULL && owned_source != NULL) {
            /* The full comparison above proved this association. In skip mode
             * no association is recorded, so a later content change cannot be
             * mistaken for an owned-source hit. */
            if (!vp_gxm_texture_skip_bytes_enabled())
                vp_texture_snapshot_associate_owned(gxm_batch.textures, key, hit,
                                                    bytes, owned_source);
        }
    } else {
        gxm_snapshot_owned_hits++;
        gxm_snapshot_owned_bytes += bytes;
    }
    if (hit == NULL) return bytes;
    if (owned_source != NULL &&
        gxm_snapshot_memo_count < VP_SNAPSHOT_MEMO_MAX) {
        VPGxmSnapshotMemo *memo = &gxm_snapshot_memo[gxm_snapshot_memo_count++];
        memo->key = *key;
        memo->source = source;
        memo->bytes = bytes;
        memo->owned = hit;
        memo->upload = NULL;
    }
    return 0u;
}

static int gxm_snapshot_memo_lookup(const VPTextureSnapshotKey *key,
                                    const void *source, size_t bytes,
                                    const void **owned,
                                    VPGxmUpload **upload) {
    for (unsigned i = 0; i < gxm_snapshot_memo_count; ++i) {
        const VPGxmSnapshotMemo *memo = &gxm_snapshot_memo[i];
        if ((memo->owned != NULL || memo->upload != NULL) &&
            memo->bytes == bytes &&
            memo->source == source && memo->key.va == key->va &&
            memo->key.width == key->width && memo->key.height == key->height &&
            memo->key.format == key->format && memo->key.cube == key->cube &&
            memo->key.mips == key->mips) {
            *owned = memo->owned;
            *upload = memo->upload;
            return 1;
        }
    }
    return 0;
}

/* Deep direct-binding contract. The payload bytes produced by the decoder are
 * exactly the bytes the copy path would memcpy into its own GXM backing, so a
 * direct binding is only allowed when the same descriptor format and byte
 * count validate. Primary textures are never cubes in the existing primary
 * descriptor path. */
static int gxm_direct_candidate(const void *texels, unsigned owned,
                                unsigned width, unsigned height,
                                unsigned format, unsigned cube,
                                unsigned mip_count, int primary,
                                SceGxmTextureFormat *fmt_out,
                                size_t *bytes_out) {
    SceGxmTextureFormat fmt;
    size_t bytes;
    if (!vp_texture_direct_active() || !owned || texels == NULL ||
        width == 0u || height == 0u)
        return 0;
    if (primary && cube) return 0;
    if (frame_gxm_texture_format(format, &fmt) < 0) return 0;
    bytes = cube ? vp_texture_cube_upload_bytes_mips(width, height, mip_count)
                 : vp_texture_upload_bytes(width, height, format);
    if (bytes == 0u || !vp_texture_direct_compatible(texels, bytes)) return 0;
    if (cube && (mip_count == 0u ||
                 mip_count > vp_texture_cube_mip_count(width, height)))
        return 0;
    if (fmt_out != NULL) *fmt_out = fmt;
    if (bytes_out != NULL) *bytes_out = bytes;
    return 1;
}

static void *gxm_batch_texture_copy(VPGxmBatchEntry *entry,
    const VPTextureSnapshotKey *key, const void *source, size_t bytes,
    const void *owned_source);

/* Direct sources are retained through the decoder's live-slot identity, never
 * through an address range. A failed retain is an internal contract failure:
 * in deep mode the draw fails explicitly instead of silently copying. */
static void *gxm_batch_texture_bind(VPGxmBatchEntry *entry,
    const VPTextureSnapshotKey *key, const void *source, size_t bytes,
    const void *owned_source, int direct_eligible) {
    if (direct_eligible) {
        VPTexturePoolToken token;
        if (!vp_texture_direct_active() ||
            !vp_texture_direct_retain_gpu(source, &token)) {
            gxm_direct_bind_failures++;
            glog("VP_GXM_DIRECT_BIND_FAIL bytes=%u action=draw-fail\n",
                 (unsigned)bytes);
            vp_texture_direct_fail_submission();
            return NULL;
        }
        if (gxm_direct_refs_add(&entry->direct, token) < 0) {
            vp_texture_direct_release_gpu(token);
            gxm_direct_bind_failures++;
            glog("VP_GXM_DIRECT_BIND_FAIL bytes=%u reason=token-limit\n",
                 (unsigned)bytes);
            vp_texture_direct_fail_submission();
            return NULL;
        }
        gxm_direct_binds++;
        gxm_direct_retains++;
        gxm_direct_bytes_avoided += bytes;
        gxm_direct_seen = 1;
        return (void *)source;
    }
    return gxm_batch_texture_copy(entry, key, source, bytes, owned_source);
}

static void *gxm_batch_texture_copy(VPGxmBatchEntry *entry,
    const VPTextureSnapshotKey *key, const void *source, size_t bytes,
    const void *owned_source) {
    uint64_t started = gxm_prof_now();
    const void *memo_owned = NULL;
    VPGxmUpload *memo_upload = NULL;
    VPGxmUpload *upload;
    if (owned_source != NULL &&
        gxm_snapshot_memo_lookup(key, source, bytes, &memo_owned,
                                 &memo_upload)) {
        /* Admission already proved this exact decoder-owned source against
         * this exact snapshot or upload entry in this draw. */
        if (memo_upload != NULL) {
            /* Keep the copy-site LRU order while reusing the pinned entry. */
            memo_upload->pinned = 1u;
            memo_upload->stamp = ++gxm_upload_stamp;
            gxm_upload_memo_hits++;
            gxm_upload_hits++;
            gxm_upload_saved_bytes += bytes;
            gxm_tex_us += gxm_prof_now() - started;
            return (void *)memo_upload->snapshot.data;
        }
        gxm_snapshot_owned_hits++;
        gxm_snapshot_owned_bytes += bytes;
        gxm_tex_us += gxm_prof_now() - started;
        return (void *)memo_owned;
    }
    upload = gxm_upload_find(key, source, bytes, owned_source);
    if (upload) {
        upload->pinned = 1u;
        upload->stamp = ++gxm_upload_stamp;
        gxm_upload_hits++;
        gxm_upload_saved_bytes += bytes;
        gxm_tex_us += gxm_prof_now() - started;
        return (void *)upload->snapshot.data;
    }
    const void *hit = NULL;
    if (owned_source != NULL)
        hit = vp_texture_snapshot_find_owned(gxm_batch.textures, key,
                                             owned_source, bytes);
    if (hit != NULL) {
        gxm_snapshot_owned_hits++;
        gxm_snapshot_owned_bytes += bytes;
    } else {
        hit = gxm_snapshot_find_current(gxm_batch.textures, key, source, bytes);
        if (hit != NULL && owned_source != NULL) {
            /* The full comparison above proved this association. In skip mode
             * no association is recorded (see gxm_snapshot_reserve). */
            if (!vp_gxm_texture_skip_bytes_enabled())
                vp_texture_snapshot_associate_owned(gxm_batch.textures, key, hit,
                                                    bytes, owned_source);
        }
    }
    if (hit) {
        gxm_snapshot_hits++;
        gxm_snapshot_saved_bytes += bytes;
        gxm_tex_us += gxm_prof_now() - started;
        return (void *)hit;
    }
    void *copy = gxm_batch_entry_alloc(entry, bytes, 16u);
    if (!copy) return NULL;
    memcpy(copy, source, bytes);
    gxm_tex_us += gxm_prof_now() - started;
    gxm_tex_bytes += bytes;
    vp_texture_snapshot_remember_owned(gxm_batch.textures, key, copy, bytes,
                                      owned_source);
    return copy;
}

/* B105 decoder update event. Only pointer associations are cleared; pinned
 * upload pages, staged snapshot bytes and their GPU backing stay valid until
 * their normal completion. A pointer reused after this never inherits the old
 * proof and must be compared/copied again before a new association exists. */
void vp_gxm_texture_owned_retire(const void *source) {
    unsigned cleared = 0;
    if (source == NULL) return;
    gxm_owned_retirements++;
    for (unsigned i = 0; i < VP_UPLOAD_CACHE_SLOTS; ++i) {
        if (gxm_uploads[i].snapshot.owned_decode_source == source) {
            gxm_uploads[i].snapshot.owned_decode_source = NULL;
            cleared++;
        }
    }
    for (unsigned arena = 0; arena < VP_GXM_BATCH_ARENAS; ++arena)
        cleared += vp_texture_snapshot_retire_owned(
            &gxm_batch_texture_sets[arena], source);
    for (unsigned i = 0; i < gxm_snapshot_memo_count; ++i) {
        if (gxm_snapshot_memo[i].source == source) {
            gxm_snapshot_memo[i].source = NULL;
            gxm_snapshot_memo[i].owned = NULL;
            gxm_snapshot_memo[i].upload = NULL;
            cleared++;
        }
    }
    gxm_owned_retired_assocs += cleared;
}

/* The SDK allocates one fresh default uniform buffer per reserved stage from
 * its ring. Account the real per-program size of both stages so the batch
 * cannot silently exhaust the ring before EndScene. */
static size_t gxm_batch_uniform_for(const VPGxmDrawRequest *req,
                                    const VPStaticPair *pair) {
    size_t total = 0u;
    if (pair == NULL) return 0u;
    /* Mode 3 uses the pair's immutable sizes; this skips the repeated
     * resident reflection reads without changing the live face condition. */
    if (gxm_shared_prepare_active()) {
        static_shared_uniform_reuses++;
        return static_pair_uniform_bytes(pair, req);
    }
    if (pair->vs_program != NULL)
        total = gxm_batch_add_size(total,
            static_residents[pair->vs_resident].refl.default_uniform_bytes);
    if (pair->fs_program != NULL) {
        size_t bytes = static_residents[pair->fs_resident].refl.default_uniform_bytes;
        total = gxm_batch_add_size(total, bytes);
        /* Only the two-face draw path reserves a second fragment buffer. */
        if (static_residents[pair->fs_resident].refl.face_state != NULL &&
            !req->cull_enable &&
            (req->cull_face == 0x404u || req->cull_face == 0x405u ||
             req->cull_face == 0x408u))
            total = gxm_batch_add_size(total, bytes);
    }
    return total;
}

/* The submitter scanned the real index buffer once while planning the vertex
 * window. A valid plan with zero markers proves this request has no restarts,
 * so strip joining, marker splitting and the 2x index reservation can be
 * skipped. Unplanned requests and plans with markers keep the raw guest
 * restart_enabled value. */
static int gxm_effective_restart(const VPGxmDrawRequest *req) {
    if (req->index_plan_valid && req->index_plan_restart_count == 0u) return 0;
    return req->restart_enabled != 0u;
}

/* Shared GPU-visible indices live for the context lifetime. */
static uint16_t *gxm_index_ramp;

static const uint16_t *gxm_index_ramp_get(void) {
    if (gxm_index_ramp == NULL) {
        gxm_index_ramp = gpu_alloc(VP_VERTEX_WINDOW_MAX_VERTICES * sizeof(uint16_t),
                                   0, NULL);
        if (gxm_index_ramp == NULL) return NULL;
        for (unsigned i = 0; i < VP_VERTEX_WINDOW_MAX_VERTICES; ++i)
            gxm_index_ramp[i] = (uint16_t)i;
    }
    return gxm_index_ramp;
}

static size_t gxm_batch_reserve_for(const VPGxmDrawRequest *req,
                                    const VPStaticPair *pair, int depth_only,
                                    size_t uniform_bytes) {
    size_t total = 0u;
    (void)depth_only;
    /* Probes below describe this draw only; the encoded copy consumes them. */
    gxm_snapshot_memo_count = 0u;
    total = gxm_batch_add_size(total, (size_t)req->count * req->stride);
    if (pair != NULL && pair->packed_mask != 0u)
        total = gxm_batch_add_size(total,
                    (size_t)req->count * pair->packed_stream_stride);
    if (req->indexed) {
        unsigned out_size = req->index_format == 2u ? 4u : 2u;
        size_t index_bytes = (size_t)req->index_count * out_size;
        if (gxm_effective_restart(req) && req->topology == 5u &&
            req->index_count <= VP_VERTEX_WINDOW_MAX_INDEX_COUNT / 2u) index_bytes *= 2u;
        total = gxm_batch_add_size(total, index_bytes);
    }
    if (req->texture_source != VP_GXM_EXTRA_SOURCE_DEPTH &&
        req->texels != NULL && req->tex_width != 0u && req->tex_height != 0u &&
        (req->texture_va == 0u || surface_find(req->texture_va) == NULL) &&
        !gxm_direct_candidate(req->texels, req->texels_owned,
            req->tex_width, req->tex_height, req->tex_format, 0u, 1u, 1,
            NULL, NULL))
    {
        VPTextureSnapshotKey key = gxm_snapshot_key(req->texture_va,
            req->tex_width, req->tex_height, req->tex_format, 0u, 1u);
        size_t bytes = vp_texture_upload_bytes(req->tex_width, req->tex_height,
                                               req->tex_format);
        total = gxm_batch_add_size(total,
            gxm_snapshot_reserve(&key, req->texels, bytes,
                req->texels_owned ? req->texels : NULL));
    }
    for (unsigned e = 0; e < VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u; ++e) {
        const VPGxmExtraTexture *extra = &req->extra_textures[e];
        size_t bytes;
        if (extra->source != VP_GXM_EXTRA_SOURCE_CPU ||
            extra->texels == NULL || extra->width == 0u || extra->height == 0u)
            continue;
        if (gxm_direct_candidate(extra->texels, extra->texels_owned,
                extra->width, extra->height, extra->tex_format, extra->cube,
                extra->cube ? extra->mip_count : 1u, 0, NULL, NULL))
            continue;
        bytes = extra->cube
            ? vp_texture_cube_upload_bytes_mips(extra->width, extra->height,
                                                extra->mip_count)
            : vp_texture_upload_bytes(extra->width, extra->height,
                                      extra->tex_format);
        VPTextureSnapshotKey key = gxm_snapshot_key(extra->va, extra->width,
            extra->height, extra->tex_format, extra->cube,
            extra->cube ? extra->mip_count : 1u);
        total = gxm_batch_add_size(total,
            gxm_snapshot_reserve(&key, extra->texels, bytes,
                extra->texels_owned ? extra->texels : NULL));
    }
    {
        const VPGxmExtraTexture *extra = &req->vertex_texture;
        /* The batch vertex-texture path copies every non-depth source that has
         * texels; a GPU-classified descriptor may still carry its CPU snapshot.
         * Reserve the same bytes the copy will consume. */
        if (extra->source != VP_GXM_EXTRA_SOURCE_DEPTH &&
            extra->texels != NULL && extra->width != 0u && extra->height != 0u &&
            !gxm_direct_candidate(extra->texels, extra->texels_owned,
                extra->width, extra->height, extra->tex_format, extra->cube,
                extra->cube ? extra->mip_count : 1u, 0, NULL, NULL)) {
            size_t bytes = extra->cube
                ? vp_texture_cube_upload_bytes_mips(extra->width, extra->height,
                                                    extra->mip_count)
                : vp_texture_upload_bytes(extra->width, extra->height,
                                          extra->tex_format);
            VPTextureSnapshotKey key = gxm_snapshot_key(extra->va, extra->width,
                extra->height, extra->tex_format, extra->cube,
                extra->cube ? extra->mip_count : 1u);
            total = gxm_batch_add_size(total,
                gxm_snapshot_reserve(&key, extra->texels, bytes,
                    extra->texels_owned ? extra->texels : NULL));
        }
    }
    total = gxm_batch_add_size(total, uniform_bytes);
    return total;
}

static int gxm_batch_ensure_staging(void) {
    gxm_batch_arena_init();
    if (gxm_batch.staging != NULL) return 0;
    gxm_upload_capacity_init();
    gxm_batch.staging_size = VP_GXM_BATCH_PAGE_BYTES;
    gxm_batch.staging = gpu_alloc((unsigned)gxm_batch.staging_size, 0, NULL);
    if (gxm_batch.staging == NULL) {
        gxm_batch.staging_size = 0u;
        glog("VP_GXM_BATCH staging_alloc_failed bytes=%u arena=%u\n",
             VP_GXM_BATCH_PAGE_BYTES, gxm_batch_arena);
        return -1;
    }
    gxm_batch_staging_sets[gxm_batch_arena][gxm_batch.staging_page] = gxm_batch.staging;
    gxm_batch.staging_used = 0u;
    glog("VP_GXM_BATCH staging_ready bytes=%u draws=%u arena=%u\n",
         (unsigned)gxm_batch.staging_size, VP_BATCH_MAX_DRAWS, gxm_batch_arena);
    return 0;
}

static int gxm_batch_next_staging_page(void) {
    unsigned next = gxm_batch.staging_page + 1u;
    if (next >= VP_GXM_BATCH_STAGING_PAGES) return -1;
    unsigned char *page = gxm_batch_staging_sets[gxm_batch_arena][next];
    if (page == NULL) {
        page = gpu_alloc(VP_GXM_BATCH_PAGE_BYTES, 0, NULL);
        if (page == NULL) return -1;
        gxm_batch_staging_sets[gxm_batch_arena][next] = page;
    }
    gxm_batch.staging_page = next;
    gxm_batch.staging = page;
    gxm_batch.staging_size = VP_GXM_BATCH_PAGE_BYTES;
    gxm_batch.staging_used = 0u;
    return 0;
}

static void *gxm_batch_entry_alloc(VPGxmBatchEntry *entry, size_t bytes,
                                   size_t align) {
    size_t aligned = gxm_batch_align_size(bytes);
    size_t at;
    (void)align;
    if (entry == NULL || !entry->used || entry->staging_page == NULL)
        return NULL;
    if (aligned == (size_t)-1) return NULL;
    if (entry->staged_bytes > entry->reserve_bytes ||
        aligned > entry->reserve_bytes - entry->staged_bytes) {
        glog("VP_GXM_BATCH staging_entry_overflow reserve=%u staged=%u need=%u "
             "draw=%u\n", (unsigned)entry->reserve_bytes,
             (unsigned)entry->staged_bytes, (unsigned)bytes,
             entry->req.draw_index);
        return NULL;
    }
    at = entry->staging_base + entry->staged_bytes;
    if (at > gxm_batch.staging_size || aligned > gxm_batch.staging_size - at)
        return NULL;
    entry->staged_bytes += aligned;
    return entry->staging_page + at;
}

/* The decision parameter comes from gxm_direct_rollback_decision: release is
 * only legal when no draw was issued or scene completion was proven; otherwise
 * the leases are quarantined and stay retained. */
static void gxm_batch_discard_current_impl(VPGxmDirectRollback decision) {
    VPGxmBatchEntry *entry = gxm_batch_current_entry;
    if (entry == NULL) return;
    if (entry->direct.count != 0u)
        gxm_direct_rollback_refs(&entry->direct, decision);
    if (entry->staging_base <= gxm_batch.staging_used)
        gxm_batch.staging_used = entry->staging_base;
    if (entry->uniform_bytes <= gxm_batch.uniform_bytes)
        gxm_batch.uniform_bytes -= entry->uniform_bytes;
    else
        gxm_batch.uniform_bytes = 0u;
    gxm_batch.textures->count = entry->snapshot_base;
    memset(entry, 0, sizeof(*entry));
    gxm_batch_current_entry = NULL;
}

static void gxm_batch_discard_current(void) {
    gxm_batch_discard_current_impl(VP_GXM_DIRECT_ROLLBACK_RELEASE);
}

static void gxm_batch_discard_current_decision(int draw_consumed,
                                               int scene_completion_proven) {
    gxm_batch_discard_current_impl(
        gxm_direct_rollback_decision(draw_consumed,
                                     scene_completion_proven));
}

static void gxm_batch_reset_scene_state(void) {
    gxm_batch_arena_init();
    /* A reset that still owns a current entry is a failure path (successful
     * flushes publish and clear the pointer first). The entry may have issued
     * part of a two-pass draw, so retain its direct leases in quarantine; a
     * later memset or arena reuse must never silently drop them. */
    if (gxm_batch_current_entry != NULL) {
        if (gxm_batch_current_entry->direct.count != 0u)
            gxm_direct_refs_quarantine(&gxm_batch_current_entry->direct);
        gxm_batch_current_entry = NULL;
    }
    /* The memo points into this arena's staging; no entry may survive a reset
     * even though the current call order always re-runs the reserve first. */
    gxm_snapshot_memo_count = 0u;
    for (unsigned i = 0; i < VP_UPLOAD_CACHE_SLOTS; ++i)
        gxm_uploads[i].pinned = 0u;
    /* Every reader of the entry array is bounded by gxm_batch.count, and each
     * admission memsets its own entry before setting used. Entries beyond the
     * active count, including the other arena's stale slots, are never read, so
     * the whole-array clear is not needed. */
    static_stats.batch_reset_bytes_avoided += (uint64_t)sizeof(gxm_batch_entry_sets[0]);
    gxm_batch.count = 0u;
    gxm_batch.producer_color_va = 0u;
    gxm_batch.producer_depth_va = 0u;
    gxm_batch.textures->count = 0u;
    gxm_batch.staging_page = 0u;
    gxm_batch.staging = gxm_batch_staging_sets[gxm_batch_arena][0];
    gxm_batch.staging_size = gxm_batch.staging != NULL ? VP_GXM_BATCH_PAGE_BYTES : 0u;
    gxm_batch.staging_used = 0u;
    gxm_batch.uniform_bytes = 0u;
    gxm_batch.scene_open = 0;
    gxm_batch.depth_only = 0;
    gxm_batch.target = NULL;
    gxm_batch.render_target = NULL;
    gxm_batch.depth_entry = NULL;
    memset(&gxm_batch.depth_desc, 0, sizeof(gxm_batch.depth_desc));
    gxm_batch.depth_enabled = 0;
    gxm_batch.render_width = 0u;
    gxm_batch.render_height = 0u;
    gxm_batch.scope = 0u;
    gxm_batch.opened_us = 0u;
}

/* Publish one encoded draw only after the scene's EndScene/notification/Finish
 * succeeded. This mirrors the synchronous success tail: producer content
 * generations, surface draw/display metadata and sampler-consumption counters
 * become visible here, never at encode time. */
static void gxm_batch_publish_entry(VPGxmBatchEntry *entry) {
    static unsigned commit_logs;
    VPGxmDrawRequest *req = &entry->req;
    unsigned e;

    if (!entry->used) return;
    if (entry->depth_enabled && entry->depth_entry != NULL) {
        if ((entry->commit_mask & VP_GXM_DRAW_COMMIT_DEPTH_VALID) != 0u)
            depth_entry_commit_content(entry->depth_entry, req->draw_index,
                                       "batch-commit");
        if ((entry->commit_mask & VP_GXM_DRAW_COMMIT_DEPTH_DRAW_COUNT) != 0u)
            entry->depth_entry->draw_count++;
        if (entry->depth_entry->desc.format_class == VP_GXM_DEPTH_CLASS_S8D24 &&
            req->depth_state.stencil_enable != 0u) {
            entry->depth_entry->stencil_live = 1u;
            gxm_depth_stats.stencil_unsupported++;
            if (gxm_stencil_unsupported_logs < 16u ||
                (gxm_stencil_unsupported_logs % 512u) == 0u)
                glog("VP_GXM_STENCIL_UNSUPPORTED idx=%u zva=0x%llx "
                     "format=0x%x action=guest-state-not-emulated "
                     "stencil_live=1 batch=1 n=%u\n", req->draw_index,
                     (unsigned long long)req->depth_state.va,
                     req->depth_state.format, gxm_stencil_unsupported_logs);
            gxm_stencil_unsupported_logs++;
        }
    }
    if (entry->target != NULL) {
        if ((entry->commit_mask & VP_GXM_DRAW_COMMIT_COLOR_DRAWN) != 0u)
            entry->target->color_drawn = 1u;
        if ((entry->commit_mask & VP_GXM_DRAW_COMMIT_COLOR_DRAW_COUNT) != 0u)
            entry->target->draw_count++;
        if ((entry->commit_mask & VP_GXM_DRAW_COMMIT_DISPLAY_RECT) != 0u)
            display_rect_add(entry->target, req);
        if ((entry->commit_mask & VP_GXM_DRAW_COMMIT_LAST_TARGET) != 0u) {
            gxm_last_target_va = entry->target->va;
            gxm_last_target_w = entry->target->width;
            gxm_last_target_h = entry->target->height;
        }
    }
    if (entry->primary_producer != NULL)
        entry->primary_producer->sampled_count++;
    for (e = 0; e < VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u; ++e) {
        if (!entry->extra_bound[e]) continue;
        if (entry->extra_depth_view[e]) {
            gxm_depth_view_consumes++;
            if (gxm_depth_view_consumes <= 16u ||
                (gxm_depth_view_consumes % 256u) == 0u)
                glog("VP_GXM_DEPTH_VIEW_CONSUME idx=%u slot=%u source=depth "
                     "va=0x%llx view=%ux%u consumes=%u draws=%u batch=1\n",
                     req->draw_index, e + 1u,
                     (unsigned long long)req->extra_textures[e].va,
                     req->extra_textures[e].width,
                     req->extra_textures[e].height,
                     gxm_depth_view_consumes, gxm_depth_view_binds);
        } else if (entry->extra_producer[e] != NULL) {
            entry->extra_producer[e]->sampled_count++;
            gxm_extra_consume_log(req->draw_index, e + 1u,
                                  &req->extra_textures[e],
                                  entry->extra_producer[e], req->target_va,
                                  &gxm_extra_gpu_binds);
        } else {
            gxm_extra_consume_log(req->draw_index, e + 1u,
                                  &req->extra_textures[e], NULL,
                                  req->target_va, &gxm_extra_cpu_binds);
        }
    }
    if (entry->primary_depth_view) {
        gxm_depth_view_consumes++;
        if (gxm_depth_view_consumes <= 16u ||
            (gxm_depth_view_consumes % 256u) == 0u)
            glog("VP_GXM_DEPTH_VIEW_CONSUME idx=%u slot=0 source=depth "
                 "va=0x%llx view=%ux%u consumes=%u draws=%u batch=1\n",
                 req->draw_index, (unsigned long long)req->texture_va,
                 req->tex_width, req->tex_height, gxm_depth_view_consumes,
                 gxm_depth_view_binds);
    }
    if (entry->vertex_depth_view) {
        gxm_depth_view_consumes++;
        if (gxm_depth_view_consumes <= 16u ||
            (gxm_depth_view_consumes % 256u) == 0u)
            glog("VP_GXM_DEPTH_VIEW_CONSUME idx=%u stage=vertex slot=0 "
                 "source=depth va=0x%llx view=%ux%u consumes=%u draws=%u "
                 "batch=1\n", req->draw_index,
                 (unsigned long long)req->vertex_texture.va,
                 req->vertex_texture.width, req->vertex_texture.height,
                 gxm_depth_view_consumes, gxm_depth_view_binds);
    }
    if (commit_logs < 48u || (commit_logs % 256u) == 0u)
        glog("VP_GXM_BATCH_COMMIT idx=%u hash=0x%x depth_only=%d target=0x%llx "
             "mask=0x%x commit=0x%x n=%u\n", req->draw_index, req->hash,
             entry->depth_only ? 1 : 0,
             (unsigned long long)(entry->target != NULL ? entry->target->va : 0u),
             req->color_mask, entry->commit_mask, commit_logs);
    commit_logs++;
}

/* VP_HOST_TEST_EXTRACT:pending_state_begin */
/* One notification address per arena: NotificationWait tests equality. */
#define VP_GXM_PENDING_DIRECT_MAX (128u * VP_GXM_DIRECT_TEXTURE_MAX)
typedef struct {
    int used;
    unsigned arena;
    uint32_t notify_value;
    uint64_t submit_us;
    unsigned draws;
    unsigned pin_count;
    unsigned cls;
    unsigned char pins[VP_UPLOAD_CACHE_SLOTS];
    unsigned direct_count;
    VPTexturePoolToken direct[VP_GXM_PENDING_DIRECT_MAX];
} VPGxmPendingScene;
static VPGxmPendingScene gxm_pending_scenes[VP_GXM_BATCH_ARENAS];
static unsigned gxm_pending_scene_count;
/* VP_HOST_TEST_EXTRACT:pending_state_end */

static int gxm_scene_pipeline_active(void) {
    int capture;
    if (!gxm_scene_pipeline_enabled()) return 0;
    if (gxm_batch_test_mode >= 0) return 0;
    /* Capture state is the only diagnostic condition that changes the scene
     * mode. A present-only capture (inputs=0) keeps the pipeline asynchronous
     * because its readback happens after the present drain; pass-level input
     * dumps force the synchronous path while they are due. */
    capture = b34_dump_surface_capture_active();
    if (capture != gxm_pipeline_capture_sync) {
        gxm_pipeline_capture_sync = capture;
        if (gxm_pipeline_capture_logs++ < 16u)
            glog("VP_GXM_PIPELINE_CAPTURE sync=%d passdump=%u "
                 "live_inputs=%u dumps=%u/%u presents=%u/%u "
                 "detail=surface-capture-only\n",
                 capture, b34_dump.passdump, b34_dump.live_inputs,
                 b34_dump.dumps, b34_dump.max_dumps, b34_dump.presents,
                 b34_dump.max_presents);
    }
    return capture ? 0 : 1;
}

/* Reasons after which the caller mutates guest-visible resources or reads GPU
 * output on the CPU. Those must not return while a submitted scene is still
 * executing. CLEAR_DEFER is intentionally not listed: it only submits the
 * open scene, and the per-surface tag keeps the completion obligation. */
/* VP_HOST_TEST_EXTRACT:clear_reason_sync_begin */
static int gxm_batch_reason_needs_sync(unsigned reason) {
    switch (reason) {
    case GXM_BATCH_REASON_SUBMIT:
    case GXM_BATCH_REASON_CLEAR:
    case GXM_BATCH_REASON_PRESENT:
    case GXM_BATCH_REASON_READBACK:
    case GXM_BATCH_REASON_RESOURCE:
    case GXM_BATCH_REASON_LEGACY:
    case GXM_BATCH_REASON_FAILURE:
        return 1;
    default:
        return 0;
    }
}
/* VP_HOST_TEST_EXTRACT:clear_reason_sync_end */

static void gxm_pending_scene_record(unsigned arena, uint32_t value,
                                     uint64_t submit_us, unsigned draws,
                                     unsigned cls) {
    VPGxmPendingScene *pending = &gxm_pending_scenes[arena];
    memset(pending, 0, sizeof(*pending));
    gxm_pending_scene_count++;
    pending->used = 1;
    pending->arena = arena;
    pending->notify_value = value;
    pending->submit_us = submit_us;
    pending->draws = draws;
    pending->cls = cls;
    pending->direct_count = 0u;
    for (unsigned i = 0; i < VP_UPLOAD_CACHE_SLOTS; ++i) {
        if (!gxm_uploads[i].pinned) continue;
        gxm_uploads[i].pending++;
        pending->pins[i] = 1u;
        pending->pin_count++;
    }
}

/* VP_HOST_TEST_EXTRACT:pending_functions_begin */
/* Wait for one pending scene. A failure poisons the context and returns the
 * original error without retiring the scene or releasing its upload pins: the
 * failed wait does not prove the GPU stopped reading them. */
static int gxm_pending_scene_wait(VPGxmPendingScene *pending, const char *why) {
    SceGxmNotification notification;
    volatile unsigned *address;
    uint64_t started, finished, blocked = 0u, now = 0u;
    int rc;
    int real_wait;

    address = sceGxmGetNotificationRegion() + 1u + pending->arena;
    notification.address = address;
    notification.value = pending->notify_value;
    started = gxm_prof_now();
    /* The SDK wait is an equality wait on this address. Once the acquire load
     * already observes the pending value, no further SVC can block and the
     * duplicate call is skipped. Pending scenes are retired on success and an
     * arena is drained before reuse, so the value cannot belong to an older
     * generation of the same arena. */
    if (vp_texture_direct_active() &&
        gxm_notify_observe(address) == pending->notify_value) {
        gxm_notify_poll_hits++;
        rc = 0;
        real_wait = 0;
    } else {
        uint64_t fc = vp_gxm_frame_cost_category_begin(
            VP_FRAME_COST_WAIT_NOTIFY);
        gxm_notify_real_waits++;
        rc = sceGxmNotificationWait(&notification);
        vp_gxm_frame_cost_category_end(VP_FRAME_COST_WAIT_NOTIFY, fc);
        real_wait = 1;
    }
    finished = gxm_prof_now();
    vp_gpu_wait_record(&gxm_wait_profile,
                       gxm_wait_reclaim_active ? "texture-pool" : why,
                       started, finished, real_wait, rc,
                       gxm_pending_scene_count, pending->draws, 0);
    if (started != 0u && real_wait) {
        blocked = finished - started;
        gxm_notify_wait_calls++;
        gxm_notify_wait_us += blocked;
        gxm_pending_complete_calls++;
        gxm_pending_wait_us += blocked;
    }
    if (rc < 0) {
        glog("VP_GXM_PIPELINE wait_fail why=%s value=%u draws=%u rc=%d "
             "action=poison\n", why, pending->notify_value,
             pending->draws, rc);
        frame_poison_context(rc);
        return rc;
    }
    now = gxm_prof_now();
    {
        /* Age since EndScene is not blocking: it includes the CPU work the
         * worker performed while the scene executed. */
        uint64_t age = now > pending->submit_us
                           ? now - pending->submit_us : 0u;
        gxm_pipeline_waits++;
        gxm_pipeline_age_us += age;
        if (age > gxm_pipeline_age_max) gxm_pipeline_age_max = age;
        if (pending->cls < VP_SCENE_CLASS_COUNT) {
            unsigned cls = pending->cls;
            gxm_scene_classes[cls].notify_us += blocked;
            gxm_scene_classes[cls].total_us += blocked;
            if (blocked > gxm_scene_classes[cls].total_max)
                gxm_scene_classes[cls].total_max = blocked;
        }
        if (gxm_pipeline_waits <= 32u || (gxm_pipeline_waits % 256u) == 0u)
            glog("VP_GXM_PIPELINE_DONE why=%s arena=%u value=%u draws=%u "
                 "pins=%u since_submit_us=%llu notify_block_us=%llu "
                 "waits=%llu age_sum_us=%llu age_max_us=%llu\n",
                 why, pending->arena, pending->notify_value,
                 pending->draws, pending->pin_count,
                 (unsigned long long)age, (unsigned long long)blocked,
                 (unsigned long long)gxm_pipeline_waits,
                 (unsigned long long)gxm_pipeline_age_us,
                 (unsigned long long)gxm_pipeline_age_max);
    }
    return 0;
}

static void gxm_pending_scene_retire_one(VPGxmPendingScene *pending) {
    if (pending == NULL || !pending->used) return;
    for (unsigned i = 0; i < VP_UPLOAD_CACHE_SLOTS; ++i)
        if (pending->pins[i] && gxm_uploads[i].pending)
            gxm_uploads[i].pending--;
    /* Direct leases belong to the GPU work proven complete by the successful
     * notification wait that led here. A failed wait never reaches this. */
    for (unsigned i = 0; i < pending->direct_count; ++i) {
        vp_texture_direct_release_gpu(pending->direct[i]);
        gxm_direct_releases++;
    }
    pending->direct_count = 0u;
    memset(pending, 0, sizeof(*pending));
    gxm_pending_scene_count--;
}

/* Retire every pending scene whose notification the completed value proves
 * finished. The comparison is the same serial-number test used to choose the
 * newest scene, so a wrapped value cannot release a newer scene early. */
static void gxm_pending_scene_retire_upto(uint32_t value) {
    for (unsigned arena = 0; arena < VP_GXM_BATCH_ARENAS; ++arena) {
        VPGxmPendingScene *pending = &gxm_pending_scenes[arena];
        if (!pending->used || !gxm_notify_reached(value, pending->notify_value))
            continue;
        gxm_pending_scene_retire_one(pending);
    }
}

/* Wait only before an arena is reused or CPU-visible work requires it. */
static int gxm_pending_scene_complete_arena(unsigned arena, const char *why) {
    VPGxmPendingScene *pending = &gxm_pending_scenes[arena];
    int rc;

    if (!pending->used) return 0;
    rc = gxm_pending_scene_wait(pending, why);
    if (rc < 0) return rc;
    gxm_pending_scene_retire_one(pending);
    return 0;
}

/* Drain every pending scene. Scenes share one context and are submitted in
 * notification order, so waiting for the newest pending value proves every
 * older scene completed. The per-arena wait remains for arena reuse. */
static int gxm_pending_scene_complete(const char *why) {
    int newest = -1;
    int rc;

    for (unsigned arena = 0; arena < VP_GXM_BATCH_ARENAS; ++arena) {
        VPGxmPendingScene *pending = &gxm_pending_scenes[arena];
        if (!pending->used) continue;
        if (newest < 0 ||
            gxm_notify_newer(pending->notify_value,
                             gxm_pending_scenes[newest].notify_value))
            newest = (int)arena;
    }
    if (newest < 0) return 0;
    rc = gxm_pending_scene_wait(&gxm_pending_scenes[newest], why);
    if (rc < 0) return rc;
    gxm_pending_scene_retire_upto(gxm_pending_scenes[newest].notify_value);
    return 0;
}
/* VP_HOST_TEST_EXTRACT:pending_functions_end */

/* Direct-lease handoff helpers. A successful synchronous scene already waited
 * for GPU completion; a deferred scene keeps its leases in the pending record
 * until the notification wait proves completion. */
static void gxm_batch_release_direct_entries(unsigned arena, unsigned draws) {
    if (arena >= VP_GXM_BATCH_ARENAS) return;
    if (draws > VP_BATCH_MAX_DRAWS) draws = VP_BATCH_MAX_DRAWS;
    for (unsigned i = 0; i < draws; ++i)
        gxm_direct_refs_release(&gxm_batch_entry_sets[arena][i].direct);
}

static void gxm_batch_quarantine_direct_entries(unsigned arena, unsigned draws) {
    if (arena >= VP_GXM_BATCH_ARENAS) return;
    if (draws > VP_BATCH_MAX_DRAWS) draws = VP_BATCH_MAX_DRAWS;
    for (unsigned i = 0; i < draws; ++i)
        gxm_direct_refs_quarantine(&gxm_batch_entry_sets[arena][i].direct);
}

/* Move every direct token out of the arena's committed entries before the
 * arena can be reused. The pending capacity is exactly draws *
 * VP_GXM_DIRECT_TEXTURE_MAX; an unexpected overflow quarantines the remaining
 * tokens rather than letting a later memento drop them. */
static int gxm_batch_move_direct_to_pending(unsigned arena, unsigned draws,
                                            VPGxmPendingScene *pending) {
    if (arena >= VP_GXM_BATCH_ARENAS || pending == NULL) return -1;
    if (draws > VP_BATCH_MAX_DRAWS) draws = VP_BATCH_MAX_DRAWS;
    for (unsigned i = 0; i < draws; ++i) {
        VPGxmDirectRefs *refs = &gxm_batch_entry_sets[arena][i].direct;
        for (unsigned j = 0; j < refs->count; ++j) {
            if (pending->direct_count >= VP_GXM_PENDING_DIRECT_MAX) {
                gxm_direct_refs_quarantine(refs);
                glog("VP_GXM_DIRECT_PENDING_OVERFLOW arena=%u draws=%u "
                     "action=quarantine\n", arena, draws);
                return -1;
            }
            pending->direct[pending->direct_count++] = refs->token[j];
        }
        refs->count = 0u;
    }
    return 0;
}

/* Complete a scene whose draws were all encoded successfully. Called on
 * submission end and on every API/resource boundary. The outer wrapper is the
 * single span owner; every early return in the implementation is balanced. */
static int gxm_batch_flush_worker_impl(unsigned reason) {
    uint64_t t_start, t_end = 0u, t_notify = 0u, t_finished = 0u;
    unsigned pending, committed, i;
    int rc;
    int pipeline;
    int deferred = 0;
    int needs_sync;
    int combine;
    uint32_t deferred_value = 0u;

    if (reason >= GXM_BATCH_REASON_COUNT) reason = GXM_BATCH_REASON_RESOURCE;
    gxm_batch_arena_init();
    pipeline = gxm_scene_pipeline_active();
    needs_sync = gxm_batch_reason_needs_sync(reason);
    combine = vp_texture_direct_active();
    gxm_batch.reason_counts[reason]++;
    static_stats.batch_reason_hist[reason]++;
    /* In pipeline mode a CPU boundary submits the current scene first and then
     * proves every earlier scene with one wait for the newest notification.
     * An empty scene has nothing to submit, so it still drains here. */
    if (gxm_pending_scene_count &&
        (!pipeline || (needs_sync && (!combine || !gxm_batch.scene_open)))) {
        /* CPU-visible boundaries drain pending work before touching backing. */
        int wait_rc = gxm_pending_scene_complete(gxm_batch_reason_name(reason));
        if (wait_rc < 0) {
            unsigned dropped = vp_batch_policy_abort(&gxm_batch.policy,
                                                     &gxm_batch.stats);
            static_stats.batch_failed += dropped;
            gxm_perf_global_fail += dropped;
            gxm_batch_quarantine_direct_entries(gxm_batch_arena,
                                                gxm_batch.count);
            glog("VP_GXM_PIPELINE_FAIL reason=%s pending_wait=%d dropped=%u\n",
                 gxm_batch_reason_name(reason), wait_rc, dropped);
            gxm_batch_reset_scene_state();
            return wait_rc;
        }
    }
    if (!gxm_batch.scene_open) {
        if (gxm_batch.policy.open) {
            unsigned dropped = vp_batch_policy_abort(&gxm_batch.policy,
                                                     &gxm_batch.stats);
            static_stats.batch_failed += dropped;
            gxm_perf_global_fail += dropped;
            gxm_scene_poisoned = 1;
            glog("VP_GXM_BATCH_STATE_FAIL reason=%s pending_without_scene=%u\n",
                 gxm_batch_reason_name(reason), dropped);
            gxm_batch_quarantine_direct_entries(gxm_batch_arena,
                                                gxm_batch.count);
            gxm_batch_reset_scene_state();
            return -1;
        }
        gxm_batch_discard_current();
        return 0;
    }
    pending = gxm_batch.policy.open ? gxm_batch.policy.draws : 0u;
    if (pending != gxm_batch.count) {
        int cleanup = frame_end_scene_timed(gxm_context, "failure", &t_end, &t_notify,
                                            &t_finished);
        gxm_scene_poisoned = 1;
        vp_batch_policy_abort(&gxm_batch.policy, &gxm_batch.stats);
        static_stats.batch_failed += pending;
        gxm_perf_global_fail += pending;
        glog("VP_GXM_BATCH_STATE_FAIL reason=%s pending=%u entries=%u "
             "cleanup_rc=%d action=poison-no-publish\n",
             gxm_batch_reason_name(reason), pending, gxm_batch.count, cleanup);
        /* cleanup may have ended the scene; without a successful completion
         * the GPU-held leases stay quarantined. */
        gxm_batch_quarantine_direct_entries(gxm_batch_arena, gxm_batch.count);
        gxm_batch_reset_scene_state();
        return -1;
    }
    t_start = gxm_prof_now();
    if (pipeline) {
        /* Submit without draining; wait on arena reuse or a CPU boundary. */
        SceGxmNotification notification;
        notification.address = sceGxmGetNotificationRegion() + 1u + gxm_batch_arena;
        *notification.address = 0u;
        gxm_notify_seq = gxm_notify_next(gxm_notify_seq);
        notification.value = gxm_notify_seq;
        rc = sceGxmEndScene(gxm_context, NULL, &notification);
        t_end = gxm_prof_now();
        if (t_start != 0u) {
            gxm_endscene_calls++;
            gxm_endscene_us += t_end - t_start;
        }
        t_notify = t_end;
        t_finished = t_end;
        if (rc >= 0) {
            deferred = 1;
            deferred_value = notification.value;
        }
    } else {
        rc = frame_end_scene_timed(gxm_context, gxm_batch_reason_name(reason),
                                   &t_end, &t_notify, &t_finished);
    }
    if (rc < 0) {
        frame_poison_context(rc);
        vp_batch_policy_abort(&gxm_batch.policy, &gxm_batch.stats);
        static_stats.batch_failed += pending;
        static_stats.batch_flushes++;
        gxm_perf_global_fail += pending;
        glog("VP_GXM_BATCH_FLUSH_FAIL reason=%s pending=%u end_rc=%d "
             "action=poison-no-publish dropped=%u\n",
             gxm_batch_reason_name(reason), pending, rc, pending);
        /* EndScene failed: the GPU may have consumed some of these textures.
         * Never release the direct leases on an unproven boundary. */
        gxm_batch_quarantine_direct_entries(gxm_batch_arena, pending);
        gxm_batch_reset_scene_state();
        return rc;
    }
    gxm_batch_perf.flushes++;
    if (!deferred) {
        /* frame_end_scene_timed already waited for the notification and
         * sceGxmFinish, so every direct payload is safe to release. */
        gxm_batch_release_direct_entries(gxm_batch_arena, pending);
    }
    {
        uint64_t end_us = t_end - t_start, notify_us = t_notify - t_end;
        uint64_t finish_us = t_finished - t_notify;
        uint64_t total_us = t_finished - t_start;
        gxm_batch_perf.end_sum += end_us;
        gxm_batch_perf.notify_sum += notify_us;
        gxm_batch_perf.finish_sum += finish_us;
        gxm_batch_perf.total_sum += total_us;
        if (end_us > gxm_batch_perf.end_max) gxm_batch_perf.end_max = end_us;
        if (notify_us > gxm_batch_perf.notify_max)
            gxm_batch_perf.notify_max = notify_us;
        if (finish_us > gxm_batch_perf.finish_max)
            gxm_batch_perf.finish_max = finish_us;
        if (total_us > gxm_batch_perf.total_max)
            gxm_batch_perf.total_max = total_us;
    }
    {
        unsigned cls = gxm_scene_class_of(gxm_batch.render_width,
                                          gxm_batch.render_height,
                                          gxm_batch.depth_only);
        gxm_scene_classes[cls].flushes++;
        gxm_scene_classes[cls].draws += pending;
        gxm_scene_classes[cls].notify_us += t_notify - t_end;
        gxm_scene_classes[cls].finish_us += t_finished - t_notify;
        gxm_scene_classes[cls].total_us += t_finished - t_start;
        if (t_finished - t_start > gxm_scene_classes[cls].total_max)
            gxm_scene_classes[cls].total_max = t_finished - t_start;
    }
    for (i = 0; i < gxm_batch.count; ++i) {
        gxm_batch_publish_entry(&gxm_batch.entries[i]);
        /* Captures disable the asynchronous pipeline; their readback is
         * synchronized and represents the target after the whole scene. */
        if (!deferred && !gxm_batch.entries[i].depth_only &&
            gxm_batch.entries[i].target != NULL)
            b34_dump_surface("batch-flush",
                             gxm_batch.entries[i].target,
                             &gxm_batch.entries[i].req, NULL);
    }
    {
        unsigned batch_bytes = (unsigned)gxm_batch.policy.bytes_used;
        committed = vp_batch_policy_commit(&gxm_batch.policy, &gxm_batch.stats);
        static_stats.batch_completed += committed;
        if (committed <= VP_BATCH_MAX_DRAWS)
            static_stats.batch_size_hist[committed]++;
        static_stats.batch_flushes++;
        gxm_perf_global_ok += committed;
        if (gxm_batch.stats.max_bytes > static_stats.batch_bytes_peak)
            static_stats.batch_bytes_peak = gxm_batch.stats.max_bytes;
        if (gxm_batch.stats.max_batch > static_stats.batch_max)
            static_stats.batch_max = gxm_batch.stats.max_batch;
        if (gxm_batch.flush_logs < 48u || (gxm_batch.flush_logs % 64u) == 0u)
            glog("VP_GXM_BATCH_FLUSH reason=%s draws=%u completed=%u bytes=%u "
                 "end_us=%llu notify_us=%llu finish_us=%llu total_us=%llu "
                 "flushes=%u encoded=%u completed_total=%u failed=%u syncs=%u "
                 "max=%u peak_bytes=%u n=%u\n",
                 gxm_batch_reason_name(reason), committed, committed,
                 batch_bytes,
                 (unsigned long long)(t_end - t_start),
                 (unsigned long long)(t_notify - t_end),
                 (unsigned long long)(t_finished - t_notify),
                 (unsigned long long)(t_finished - t_start),
                 gxm_batch.stats.flushes, gxm_batch.stats.encoded,
                 gxm_batch.stats.completed, gxm_batch.stats.failed,
                 gxm_batch.stats.sync_fallbacks, gxm_batch.stats.max_batch,
                 gxm_batch.stats.max_bytes, gxm_batch.flush_logs);
    }
    gxm_batch.flush_logs++;
    if (deferred) {
        gxm_pending_scene_record(gxm_batch_arena, deferred_value, t_end,
                                 committed,
                                 gxm_scene_class_of(gxm_batch.render_width,
                                                    gxm_batch.render_height,
                                                    gxm_batch.depth_only));
        gxm_deferred_scenes++;
        if (gxm_batch_move_direct_to_pending(
                gxm_batch_arena, committed,
                &gxm_pending_scenes[gxm_batch_arena]) < 0) {
            frame_poison_context(-1);
            gxm_batch_reset_scene_state();
            return -1;
        }
    }
    if (pipeline) {
        unsigned next = (gxm_batch_arena + 1u) % VP_GXM_BATCH_ARENAS;
        if (combine && deferred && needs_sync) {
            /* The combined wait below covers this arena as well because GPU
             * scenes complete in submission order. */
            gxm_combined_drains++;
        } else {
            rc = gxm_pending_scene_complete_arena(next, "arena-reuse");
            if (rc < 0) return rc;
        }
        gxm_batch_arena_set(next);
    }
    gxm_batch_reset_scene_state();
    if (deferred && needs_sync) {
        /* One wait for the newest notification proves every earlier scene,
         * including the current one submitted above. */
        rc = gxm_pending_scene_complete(gxm_batch_reason_name(reason));
        if (rc < 0) return rc;
    }
    return 0;
}

static int gxm_batch_flush_worker(unsigned reason) {
    uint64_t fc = vp_gxm_frame_cost_category_begin(VP_FRAME_COST_GXM_FLUSH);
    int rc = gxm_batch_flush_worker_impl(reason);
    vp_gxm_frame_cost_category_end(VP_FRAME_COST_GXM_FLUSH, fc);
    return rc;
}

/* ---- Deep texture-pool GPU backing and drain --------------------------- */

typedef struct {
    void *base;
    SceUID uid;
    int mapped;
} VPGxmTextureRegion;
static VPGxmTextureRegion gxm_texture_regions[VP_TEXTURE_REGION_CHUNKS];

/* Class 0 is the decoded GPU payload: uncached main memory mapped for GPU
 * read. Class 1 is a CPU-only raw snapshot: ordinary cached main memory that
 * is never passed to sceGxmMapMemory and never bound to a texture unit. */
static void *gxm_texture_chunk_create(void *user, size_t bytes,
                                      unsigned allocation_class) {
    unsigned slot;
    SceUID uid;
    void *base = NULL;
    int mapped = allocation_class == VP_TEXTURE_REGION_CLASS_MAPPED;
    SceKernelMemBlockType type = mapped
        ? SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE
        : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW;
    (void)user;
    if (allocation_class >= VP_TEXTURE_REGION_CLASSES) return NULL;
    for (slot = 0; slot < VP_TEXTURE_REGION_CHUNKS; ++slot)
        if (!gxm_texture_regions[slot].base) break;
    if (slot == VP_TEXTURE_REGION_CHUNKS) return NULL;
    uid = sceKernelAllocMemBlock("vpTexPool", type, bytes, NULL);
    if (uid < 0) {
        glog("VP_TEX_POOL_ALLOC_FAIL bytes=%u class=%u mapped=%d rc=%d kind=chunk\n",
             (unsigned)bytes, allocation_class, mapped, uid);
        return NULL;
    }
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base ||
        (mapped && sceGxmMapMemory(base, bytes, SCE_GXM_MEMORY_ATTRIB_READ) < 0)) {
        sceKernelFreeMemBlock(uid);
        return NULL;
    }
    gxm_texture_regions[slot] = (VPGxmTextureRegion){base, uid, mapped};
    return base;
}

static int gxm_texture_chunk_release(void *user, void *base) {
    (void)user;
    for (unsigned slot = 0; slot < VP_TEXTURE_REGION_CHUNKS; ++slot) {
        VPGxmTextureRegion *region = &gxm_texture_regions[slot];
        int rc;
        if (region->base != base) continue;
        if (region->mapped) {
            /* No lease names this payload. Remove persistent driver bindings
             * before unmapping, including currently inactive sampler units. */
            for (unsigned unit = 0; unit < SCE_GXM_MAX_TEXTURE_UNITS; ++unit) {
                rc = sceGxmSetFragmentTexture(gxm_context, unit, &gxm_texture);
                if (rc < 0) return rc;
                rc = sceGxmSetVertexTexture(gxm_context, unit, &gxm_texture);
                if (rc < 0) return rc;
            }
            rc = sceGxmUnmapMemory(base);
            if (rc < 0) return rc;
        }
        rc = sceKernelFreeMemBlock(region->uid);
        if (rc < 0) return rc;
        memset(region, 0, sizeof(*region));
        return 0;
    }
    return -1;
}

int vp_gxm_texture_regions_init(size_t budget_bytes) {
    return vp_texture_regions_configure(&gxm_texture_region_arena, budget_bytes,
        NULL, gxm_texture_chunk_create, gxm_texture_chunk_release);
}

void *vp_gxm_texture_region_create(void *user, size_t bytes,
                                   unsigned allocation_class) {
    (void)user;
    return vp_texture_regions_alloc(&gxm_texture_region_arena, bytes,
                                    allocation_class);
}

int vp_gxm_texture_region_release(void *user, void *base) {
    (void)user;
    return vp_texture_regions_free(&gxm_texture_region_arena, base);
}

void vp_gxm_texture_region_stats(VPGxmTextureRegionStats *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->budget_bytes = gxm_texture_region_arena.budget;
    out->physical_bytes = gxm_texture_region_arena.physical_bytes;
    out->peak_bytes = gxm_texture_region_arena.peak_bytes;
    out->mapped_physical_bytes =
        gxm_texture_region_arena.class_physical[VP_TEXTURE_REGION_CLASS_MAPPED];
    out->cached_physical_bytes =
        gxm_texture_region_arena.class_physical[VP_TEXTURE_REGION_CLASS_CPU_CACHED];
    out->mapped_allocated_bytes =
        gxm_texture_region_arena.class_allocated[VP_TEXTURE_REGION_CLASS_MAPPED];
    out->cached_allocated_bytes =
        gxm_texture_region_arena.class_allocated[VP_TEXTURE_REGION_CLASS_CPU_CACHED];
    out->kernel_allocs = gxm_texture_region_arena.kernel_allocs;
    out->kernel_frees = gxm_texture_region_arena.kernel_frees;
    out->suballocs = gxm_texture_region_arena.suballocs;
    out->subfrees = gxm_texture_region_arena.subfrees;
    out->live = gxm_texture_region_arena.live;
    out->failed = gxm_texture_region_arena.failed;
}

int vp_gxm_texture_regions_reset(void) {
    return vp_texture_regions_reset(&gxm_texture_region_arena);
}

/* Same 32-present endpoint as the frame-cost snapshot. All byte counts are
 * actual allocations or mapped bytes, never the configured budget presented as
 * occupancy. Sampled only while the deep pipeline is applied; the budget field
 * is the explicit configuration so a reader can compare it with the measured
 * physical bytes. */
static void gxm_texture_memory_log(uint64_t now, uint64_t frames) {
    VPGuestTextureMemoryPerf perf;
    VPGxmTextureRegionStats regions;
    SceKernelFreeMemorySizeInfo memory;
    int memory_rc;

    vp_guest_texture_memory_perf(&perf);
    if (!perf.applied) return;
    vp_gxm_texture_region_stats(&regions);
    memset(&memory, 0, sizeof(memory));
    memory.size = sizeof(memory);
    memory_rc = sceKernelGetFreeMemorySize(&memory);
    glog("VP_TEX_MEMORY_METRICS frames=%llu t_us=%llu memory_mode=%u split=%d "
         "budget=%u raw_class=%u raw_cached=%d raw_mapped=%d decoded_mapped=%d "
         "raw_cached_bytes=%u raw_cached_alloc_bytes=%u "
         "decoded_mapped_bytes=%u decoded_mapped_alloc_bytes=%u "
         "physical_bytes=%u peak_bytes=%u region_kernel_allocs=%llu "
         "region_kernel_frees=%llu region_live=%u region_failed=%u "
         "pool_used_bytes=%u pool_peak_used_bytes=%u pool_live=%u "
         "pool_peak_live=%u pool_evictions=%llu pool_alloc_map=%llu "
         "pool_free_map=%llu pressure=%llu nomem=%llu gpu_refs=%u cpu_pins=%u "
         "user_free_rc=%d user_free=%u cdram_free=%u\n",
         (unsigned long long)frames, (unsigned long long)now,
         perf.memory_mode, perf.memory_mode == 2u ? 1 : 0,
         (unsigned)perf.budget_bytes, perf.raw_class,
         perf.memory_mode == 2u ? 1 : 0, perf.memory_mode == 2u ? 0 : 1, 1,
         (unsigned)regions.cached_physical_bytes,
         (unsigned)regions.cached_allocated_bytes,
         (unsigned)regions.mapped_physical_bytes,
         (unsigned)regions.mapped_allocated_bytes,
         (unsigned)regions.physical_bytes, (unsigned)regions.peak_bytes,
         (unsigned long long)regions.kernel_allocs,
         (unsigned long long)regions.kernel_frees, regions.live, regions.failed,
         (unsigned)perf.used_bytes, (unsigned)perf.peak_used_bytes,
         perf.live_slots, perf.peak_live_slots,
         (unsigned long long)perf.evictions,
         (unsigned long long)perf.alloc_map_calls,
         (unsigned long long)perf.free_map_calls,
         (unsigned long long)perf.pressure_events,
         (unsigned long long)perf.nomem_events, perf.gpu_refs, perf.cpu_pins,
         memory_rc, (unsigned)memory.size_user, (unsigned)memory.size_cdram);
}

int vp_gxm_texture_copy_cache_reset(void) {
    if (!gxm_upload_memory) return 0;
    for (unsigned i = 0; i < VP_UPLOAD_CACHE_SLOTS; ++i)
        if (gxm_uploads[i].pending || gxm_uploads[i].pinned) return -1;
    for (unsigned i = 0; i < block_count; ++i) {
        if (blocks[i].ptr != gxm_upload_memory) continue;
        for (unsigned unit = 0; unit < SCE_GXM_MAX_TEXTURE_UNITS; ++unit) {
            int rc = sceGxmSetFragmentTexture(gxm_context, unit, &gxm_texture);
            if (rc < 0) return rc;
            rc = sceGxmSetVertexTexture(gxm_context, unit, &gxm_texture);
            if (rc < 0) return rc;
        }
        int rc = sceGxmUnmapMemory(gxm_upload_memory);
        if (rc < 0) return rc;
        rc = sceKernelFreeMemBlock(blocks[i].id);
        if (rc < 0) return rc;
        for (unsigned j = i + 1u; j < block_count; ++j)
            blocks[j - 1u] = blocks[j];
        block_count--;
        gxm_upload_memory = NULL;
        memset(gxm_uploads, 0, sizeof(gxm_uploads));
        memset(gxm_upload_pages, 0, sizeof(gxm_upload_pages));
        gxm_snapshot_memo_count = 0u;
        glog("VP_GXM_UPLOAD_CACHE_RELEASE bytes=%u reason=direct-pipeline\n",
             gxm_upload_capacity);
        return 0;
    }
    return -1;
}

void vp_gxm_texture_descriptor_cache_reset(void) {
    memset(gxm_texture_desc_cache, 0, sizeof(gxm_texture_desc_cache));
}

static int gxm_texture_pipeline_reclaim_worker(void) {
    int rc;
    if (gxm_scene_poisoned) return -1;
    /* Inline on the GXM worker: no second worker wait can deadlock here. The
     * RESOURCE boundary submits the open scene and waits for the newest
     * notification, which also proves every older deferred scene. */
    gxm_wait_reclaim_active = 1u;
    rc = gxm_batch_flush_worker(GXM_BATCH_REASON_RESOURCE);
    gxm_wait_reclaim_active = 0u;
    if (rc < 0) return rc;
    rc = gxm_pending_scene_complete("texture-pool");
    if (rc < 0) return rc;
    return 0;
}

static void gxm_texture_pipeline_reclaim_cb(void *opaque) {
    int *rc = (int *)opaque;
    *rc = gxm_texture_pipeline_reclaim_worker();
}

int vp_gxm_texture_pipeline_reclaim(void) {
    if (vp_gxm_worker_is_current())
        return gxm_texture_pipeline_reclaim_worker();
    {
        int rc = 0;
        int exec_rc = vp_gxm_worker_execute(gxm_texture_pipeline_reclaim_cb,
                                            &rc);
        if (exec_rc < 0) return exec_rc;
        return rc;
    }
}

/* A draw-local refusal happened after earlier draws were already encoded into
 * the scene. Those earlier draws really executed, so end the scene and publish
 * them; the refused draw itself was never issued or added to the pending set.
 * If EndScene/Finish fails, poison and drop every pending commit. */
static int gxm_batch_finish_aborted_worker(const char *why, int draw_rc) {
    uint64_t t_end = 0u, t_notify = 0u, t_finished = 0u;
    unsigned pending, committed, i;
    int rc;

    if (gxm_pending_scene_count) {
        rc = gxm_pending_scene_complete("abort");
        if (rc < 0) {
            /* The earlier pending scene could not be proven complete; the
             * open scene's direct leases must stay retained. */
            gxm_batch_quarantine_direct_entries(gxm_batch_arena,
                                                gxm_batch.count);
            return rc;
        }
    }
    if (!gxm_batch.scene_open) {
        gxm_batch_discard_current();
        return 0;
    }
    pending = gxm_batch.policy.open ? gxm_batch.policy.draws : 0u;
    rc = frame_end_scene_timed(gxm_context, "abort", &t_end, &t_notify, &t_finished);
    if (rc < 0) {
        frame_poison_context(rc);
        vp_batch_policy_abort(&gxm_batch.policy, &gxm_batch.stats);
        static_stats.batch_failed += pending;
        static_stats.batch_flushes++;
        gxm_perf_global_fail += pending;
        glog("VP_GXM_BATCH_ABORT reason=%s draw_rc=%d pending=%u end_rc=%d "
             "action=poison-no-publish\n", why, draw_rc, pending, rc);
        gxm_batch_quarantine_direct_entries(gxm_batch_arena, pending);
        gxm_batch_reset_scene_state();
        return -1;
    }
    /* frame_end_scene_timed waited for GPU completion above. */
    gxm_batch_release_direct_entries(gxm_batch_arena, pending);
    for (i = 0; i < gxm_batch.count; ++i)
        gxm_batch_publish_entry(&gxm_batch.entries[i]);
    committed = vp_batch_policy_commit(&gxm_batch.policy, &gxm_batch.stats);
    static_stats.batch_completed += committed;
    if (committed <= VP_BATCH_MAX_DRAWS)
        static_stats.batch_size_hist[committed]++;
    static_stats.batch_flushes++;
    gxm_perf_global_ok += committed;
    glog("VP_GXM_BATCH_ABORT reason=%s draw_rc=%d pending=%u completed=%u "
         "end_rc=0 action=publish-prior-only\n", why, draw_rc, pending,
         committed);
    gxm_batch_discard_current();
    gxm_batch_reset_scene_state();
    return 0;
}

/* ---- Deliberate visual deviation: terrain vertex-colour floor ------------
 * The village terrain's own vertex colours contain exact zeros. That was
 * verified byte for byte in the original data (v01a00.mod holds the whole
 * 24-byte runtime record with colour 000000ff), and the ground fragment
 * program multiplies the albedo by attr3/(1.001-attr3) with no additive term,
 * so those vertices render as exact (0,0,0) -- the large pure-black terrain
 * patches under eaves and on the rocky slope.
 *
 * The original data is therefore correct and the port renders it faithfully;
 * this is an explicit, user-approved deviation from it. When the marker
 * ux0:data/mhgu-vita-port/vcolor-floor.txt exists the RGB bytes of the vertex
 * colour attribute are clamped to a floor on the vertex copy that the draw
 * path has already staged (no extra copy, no guest-memory write). It applies
 * only to draws that write full colour (mask 0x1111), never to the blue-only
 * shadow/silhouette pass, and only to the verified terrain/prop vertex layout.
 * Without the marker nothing changes and behaviour is bit-for-bit the same.
 */
static int gxm_vcolor_floor_state = -1;
static const unsigned gxm_vcolor_floor_attr = 3u;
static float gxm_vcolor_floor_factor;
static uint8_t gxm_vcolor_floor_byte;
static unsigned gxm_vcolor_floor_logs;

static void gxm_vcolor_floor_init(void) {
    char text[256];
    size_t got = 0u;
    FILE *f;
    if (gxm_vcolor_floor_state >= 0) return;
    gxm_vcolor_floor_state = 0;
    f = fopen("ux0:data/mhgu-vita-port/vcolor-floor.txt", "rb");
    if (f == NULL) return;
    got = fread(text, 1u, sizeof(text) - 1u, f);
    fclose(f);
    text[got] = 0;
    for (char *line = text; line != NULL && *line != 0;) {
        char *end = strchr(line, '\n');
        char *value;
        if (end != NULL) *end = 0;
        value = strchr(line, '=');
        if (value != NULL) {
            *value++ = 0;
            while (*value == ' ' || *value == '\t') value++;
            if (strcmp(line, "factor") == 0)
                gxm_vcolor_floor_factor = (float)atof(value);
        }
        line = end != NULL ? end + 1 : NULL;
    }
    if (!(gxm_vcolor_floor_factor > 0.0f)) return;
    if (gxm_vcolor_floor_factor > 0.9f) gxm_vcolor_floor_factor = 0.9f;
    /* attr = f/(1+f) is the exact inverse of the shader's x/(1.001-x). */
    gxm_vcolor_floor_byte = (uint8_t)((gxm_vcolor_floor_factor /
        (1.0f + gxm_vcolor_floor_factor)) * 255.0f + 0.5f);
    gxm_vcolor_floor_state = 1;
    glog("VP_GXM_VCOLOR_FLOOR enabled factor=%.4f byte=%u layout=terrain-v24\n",
         (double)gxm_vcolor_floor_factor, (unsigned)gxm_vcolor_floor_byte);
}

/* Accept only the verified terrain/prop layout: F32x3 position at 0, F16x2 uv
 * at 16, U8N4 colour at 20, stride 24. Anything else keeps its data untouched. */
static int gxm_vcolor_floor_layout(const VPGxmDrawRequest *req,
                                   VpVertexFormat *color) {
    VpVertexFormat pos, uv;
    if (req->stride != 24u) return 0;
    if (vp_vertex_format_decode(req->attributes[0], req->stride, &pos) < 0 ||
        pos.kind != VP_VERTEX_FORMAT_F32 || pos.components != 3u ||
        pos.source_offset != 0u)
        return 0;
    if (vp_vertex_format_decode(req->attributes[2], req->stride, &uv) < 0 ||
        uv.kind != VP_VERTEX_FORMAT_F16 ||
        uv.components != 2u || uv.source_offset != 16u)
        return 0;
    if (vp_vertex_format_decode(req->attributes[gxm_vcolor_floor_attr],
                                req->stride, color) < 0 ||
        color->kind != VP_VERTEX_FORMAT_U8N || color->source_offset != 20u)
        return 0;
    return 1;
}

/* B60 gate census: one line per distinct fragment token, emitted after the
 * vertex scan so it carries the observed minimum colour byte and the clamp
 * count. A fixed first-N probe is useless here: the menu/prop pipelines spend
 * the budget long before the terrain program is first drawn. */
static uint32_t gxm_vcolor_floor_seen[256];
static unsigned gxm_vcolor_floor_seen_n;

static int gxm_vcolor_floor_hash_new(uint32_t hash) {
    unsigned i;
    for (i = 0; i < gxm_vcolor_floor_seen_n; ++i)
        if (gxm_vcolor_floor_seen[i] == hash) return 0;
    if (gxm_vcolor_floor_seen_n >= 256u) return 0;
    gxm_vcolor_floor_seen[gxm_vcolor_floor_seen_n++] = hash;
    return 1;
}

/* B59 attributed the pure-black terrain patches to hash 0x12a4 (unlit ground).
 * Whatever the gate decides for that program must be visible in the log of any
 * run that reaches it, independent of the distinct-pipeline census. */
static void gxm_vcolor_floor_terrain_note(const VPGxmDrawRequest *req,
                                          const char *reason, int layout_ok,
                                          unsigned minc, uint64_t clamped) {
    static int terrain_logs;
    if (req->hash != 0x12a4u || terrain_logs >= 4) return;
    terrain_logs++;
    glog("VP_GXM_VCOLOR_FLOOR terrain idx=%u n=%u stride=%u mask=0x%x "
         "reason=%s lay=%d minc=%u clamped=%llu byte=%u\n",
         req->draw_index, req->count, req->stride, req->color_mask, reason,
         layout_ok, minc, (unsigned long long)clamped,
         (unsigned)gxm_vcolor_floor_byte);
}

static void gxm_vcolor_floor_apply(void *verts, const VPGxmDrawRequest *req) {
    VpVertexFormat color;
    uint8_t *base = (uint8_t *)verts;
    uint64_t clamped = 0u;
    uint32_t v;
    unsigned minc = 255u;
    int rgb_mask, layout_ok, first;
    if (gxm_vcolor_floor_state < 0) gxm_vcolor_floor_init();
    if (gxm_vcolor_floor_state != 1 || base == NULL || req->count == 0u)
        return;
    /* Require all three colour channels: the blue-only screen-space
     * shadow/silhouette pass (mask 0x100) is excluded, every full-colour
     * pipeline (0x1111 and anything else with RGB set) is admitted. */
    rgb_mask = (req->color_mask & 0x111u) == 0x111u;
    first = gxm_vcolor_floor_hash_new(req->hash);
    if (!rgb_mask) {
        if (first)
            glog("VP_GXM_VCOLOR_FLOOR gate hash=0x%x idx=%u n=%u stride=%u "
                 "mask=0x%x rgb=0 reason=mask a0=%08x a2=%08x a3=%08x\n",
                 req->hash, req->draw_index, req->count, req->stride,
                 req->color_mask, req->attributes[0], req->attributes[2],
                 req->attributes[3]);
        gxm_vcolor_floor_terrain_note(req, "mask", 0, 255u, 0u);
        return;
    }
    layout_ok = gxm_vcolor_floor_layout(req, &color);
    if (!layout_ok) {
        /* Bounded evidence for the unlit-ground program only: keep the probe
         * narrow so a run stays readable, and fall back to a direct decode of
         * attr3 when the full layout signature does not match. */
        if (req->hash == 0x12a4u &&
            vp_vertex_format_decode(req->attributes[3], req->stride,
                                    &color) == 0 &&
            color.kind == VP_VERTEX_FORMAT_U8N &&
            color.source_offset + 4u <= req->stride) {
            static unsigned relaxed_logs;
            if (relaxed_logs < 8u) {
                relaxed_logs++;
                glog("VP_GXM_VCOLOR_FLOOR relaxed idx=%u hash=0x%x n=%u "
                     "stride=%u mask=0x%x a0=%08x a2=%08x a3=%08x off=%u\n",
                     req->draw_index, req->hash, req->count, req->stride,
                     req->color_mask, req->attributes[0], req->attributes[2],
                     req->attributes[3], color.source_offset);
            }
        } else {
            static unsigned miss_logs;
            if (first)
                glog("VP_GXM_VCOLOR_FLOOR gate hash=0x%x idx=%u n=%u "
                     "stride=%u mask=0x%x rgb=1 lay=0 reason=layout "
                     "a0=%08x a2=%08x a3=%08x\n",
                     req->hash, req->draw_index, req->count, req->stride,
                     req->color_mask, req->attributes[0], req->attributes[2],
                     req->attributes[3]);
            if (miss_logs < 8u && req->stride == 24u && req->count >= 300u) {
                miss_logs++;
                glog("VP_GXM_VCOLOR_FLOOR miss idx=%u hash=0x%x n=%u "
                     "stride=%u mask=0x%x a0=%08x a2=%08x a3=%08x\n",
                     req->draw_index, req->hash, req->count, req->stride,
                     req->color_mask, req->attributes[0], req->attributes[2],
                     req->attributes[3]);
            }
            gxm_vcolor_floor_terrain_note(req, "layout", 0, 255u, 0u);
            return;
        }
    }
    for (v = 0u; v < req->count; ++v) {
        uint8_t *p = base + (size_t)v * req->stride + color.source_offset;
        unsigned m = p[0] < p[1] ? p[0] : p[1];
        if (p[2] < m) m = p[2];
        if (m < minc) minc = m;
        if (p[0] < gxm_vcolor_floor_byte) { p[0] = gxm_vcolor_floor_byte; ++clamped; }
        if (p[1] < gxm_vcolor_floor_byte) { p[1] = gxm_vcolor_floor_byte; ++clamped; }
        if (p[2] < gxm_vcolor_floor_byte) { p[2] = gxm_vcolor_floor_byte; ++clamped; }
    }
    if (first)
        glog("VP_GXM_VCOLOR_FLOOR draw hash=0x%x idx=%u n=%u stride=%u "
             "mask=0x%x lay=%d minc=%u clamped=%llu byte=%u\n",
             req->hash, req->draw_index, req->count, req->stride,
             req->color_mask, layout_ok, minc,
             (unsigned long long)clamped, (unsigned)gxm_vcolor_floor_byte);
    /* B59 attributed the pure-black terrain patches to this exact program
     * (unlit ground, hash 0x12a4). Its record must never be lost to the
     * distinct-pipeline cap. */
    gxm_vcolor_floor_terrain_note(req, "scanned", layout_ok, minc, clamped);
    if (clamped != 0u && gxm_vcolor_floor_logs < 32u) {
        gxm_vcolor_floor_logs++;
        glog("VP_GXM_VCOLOR_FLOOR idx=%u hash=0x%x n=%u clamped=%llu byte=%u\n",
             req->draw_index, req->hash, req->count,
             (unsigned long long)clamped, (unsigned)gxm_vcolor_floor_byte);
    }
}

/* A draw that samples a surface whose deferred clear is still pending must see
 * the cleared content. Flush the open batch, materialize outside a scene, then
 * let the draw continue (and possibly open a new batch). */
static int draw_materialize_pending_samples(VPGxmDrawRequest *req) {
    VPGxmSurface *pending[VP_GXM_FRAGMENT_SAMPLER_LIMIT + 1u];
    unsigned n = 0u, i, e;
    if (req->texture_source == VP_GXM_EXTRA_SOURCE_GPU) {
        VPGxmSurface *s = surface_find(req->texture_va);
        if (s != NULL && s->clear_pending) pending[n++] = s;
    }
    for (e = 0u; e < VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u; ++e) {
        const VPGxmExtraTexture *extra = &req->extra_textures[e];
        if (extra->source == VP_GXM_EXTRA_SOURCE_GPU) {
            VPGxmSurface *s = surface_find(extra->va);
            unsigned seen = 0u;
            if (s == NULL || !s->clear_pending) continue;
            for (i = 0u; i < n; ++i) if (pending[i] == s) seen = 1u;
            if (!seen && n < VP_GXM_FRAGMENT_SAMPLER_LIMIT + 1u) pending[n++] = s;
        }
    }
    if (req->vertex_texture.source == VP_GXM_EXTRA_SOURCE_GPU) {
        VPGxmSurface *s = surface_find(req->vertex_texture.va);
        unsigned seen = 0u;
        if (s != NULL && s->clear_pending) {
            for (i = 0u; i < n; ++i) if (pending[i] == s) seen = 1u;
            if (!seen && n < VP_GXM_FRAGMENT_SAMPLER_LIMIT + 1u) pending[n++] = s;
        }
    }
    if (n == 0u) return 0;
    if (gxm_batch.policy.open || gxm_batch.scene_open) {
        int rc = gxm_batch_flush_worker(GXM_BATCH_REASON_READBACK);
        if (rc < 0) return rc;
    }
    for (i = 0u; i < n; ++i) {
        int rc = surface_clear_materialize(pending[i], "sample");
        if (rc < 0) return rc;
    }
    return 0;
}

static uint64_t gxm_feedback_effect_skips;

static int frame_draw_internal_body(VPGxmDrawRequest *req) {    VPGxmProgramPair *pair = NULL;
    VPStaticPair *static_pair = NULL;
    VPGxmBatchEntry *batch_entry = NULL;
    int batch_candidate = 0;
    int admission_flushed = 0;
    size_t batch_reserve = 0u;
    size_t batch_uniform = 0u;
    size_t batch_uniform_base = 0u;
    unsigned batch_decision = VP_BATCH_ACCEPT;
    const void *vertex_stream = NULL;
    const void *packed_stream = NULL;
    SceGxmTexture *vertex_bind_texture = NULL;
    int frame_fs_owned = 1;
    uint64_t t_start=gxm_prof_now(),t_prepared=0;
    uint64_t t_vertex=0u,t_indices=0u,t_textures=0u;
    uint64_t t_validate=0u,t_pair=0u,t_feedback=0u,t_admit=0u,t_resolve=0u;
    uint64_t flush_pair0=0u,flush_fb0=0u,pair_flush_us=0u,admit_flush_us=0u;
    uint64_t t_admit0=0u,t_reserve1=0u,t_uniform=0u;
    uint64_t nested_flush_before=gxm_batch_perf.total_sum;
    uint64_t nested_pending_before=gxm_pending_wait_us;
    uint64_t nested_notify_before=gxm_notify_wait_us;
    uint64_t nested_finish_before=gxm_finish_us;
    uint64_t t_begin=0,t_encoded=0,t_drawn=0,t_end=0,t_notify=0,t_finished=0;
    VPGxmSurface *target = NULL;
    SceGxmRenderTarget *render_target;
    SceGxmBlendInfo blend;
    SceGxmFragmentProgram *frame_fs = NULL;
    SceGxmPrimitiveType prim;
    SceGxmDepthFunc depth_func = SCE_GXM_DEPTH_FUNC_ALWAYS;
    SceGxmCullMode cull = SCE_GXM_CULL_NONE;
    void *uniform_buffer = NULL;
    const SceGxmProgramParameter *parameter;
    float area[4];
    void *texture = NULL;
    const void *index_source;
    SceGxmIndexFormat index_format;
    unsigned index_count = req->count;
    int strip_joined = 0;
    int effective_restart = 0;
    unsigned joined_segments = 0u;
    unsigned packed_stream_stride = 0;
    unsigned before_rgb = 0u, before_alpha = 0u, after_rgb = 0u, after_alpha = 0u;
    unsigned depth_write = req->depth_write;
    unsigned render_width = 0u, render_height = 0u;
    unsigned commit_mask;
    int depth_only = 0;
    int movie_trace = 0;
    int model_trace = 0;
    /* 0 = no owned scene, 1 = scene owned, 2 = EndScene/notification attempted
     * and failed; state 2 must never issue another EndScene. */
    int scene_state = 0;
    /* Two-pass FACE handling: [0] draws back faces (predicate 0), [1] front. */
    int face_two_pass = 0;
    SceGxmCullMode face_pass_cull[2] = {SCE_GXM_CULL_NONE, SCE_GXM_CULL_NONE};
    int rc;
    int depth_status;
    VPGxmDepthBinding depth_binding;
    SceGxmDepthStencilSurface *depth_surface;
    VPGxmAlignedTexture frame_extra_surface[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
    SceGxmTexture *frame_extra_bind[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u] = {NULL, NULL, NULL};
    VPGxmSurface *frame_extra_producer[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u] = {NULL, NULL, NULL};
    int frame_extra_depth[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u] = {0, 0, 0};
    VPGxmAlignedTexture frame_primary_depth_view;
    VPGxmAlignedTexture frame_extra_depth_view[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
    VPGxmAlignedTexture frame_vertex_depth_view;
    VPGxmDepthViewPlan frame_primary_depth_plan;
    VPGxmDepthViewPlan frame_extra_depth_plan[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
    VPGxmDepthViewPlan frame_vertex_depth_plan;
    int frame_primary_shadow = 0;
    int frame_extra_shadow[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u] = {0, 0, 0};
    int frame_vertex_shadow = 0;
    SceGxmTexture *frame_primary_depth_bind = NULL;
    SceGxmTexture *frame_vertex_depth_bind = NULL;

    memset(&frame_primary_depth_plan, 0, sizeof(frame_primary_depth_plan));
    memset(frame_extra_depth_plan, 0, sizeof(frame_extra_depth_plan));
    memset(&frame_vertex_depth_plan, 0, sizeof(frame_vertex_depth_plan));

    gxm_batch_arena_init();
    gxm_last_draw_class = 0u;
    gxm_draw_stage = 0u;
    req->batch_encoded = 0u;
    req->batch_fallback = 0u;
    memset(&depth_binding, 0, sizeof(depth_binding));
    depth_binding.slot = -1;

    if (gxm_scene_poisoned) return frame_context_reject("frame-draw");
    if (!gxm_ready || gxm_context == NULL || req->vertices == NULL ||
        req->count < 3u ||
        req->count > VP_VERTEX_WINDOW_MAX_VERTICES ||
        req->stride == 0u ||
        (uint64_t)req->count * req->stride > VP_VERTEX_WINDOW_MAX_BYTES) {
        glog("VP_GXM_FRAME_DRAW reject idx=%u count=%u stride=%u\n",
             req->draw_index, req->count, req->stride);
        return -1;
    }
    /* This locked game's four-tap feedback bloom pass samples its own RT.
     * Omit the optional effect: restoring it exposed alternating noisy views. */
    if (req->vs_token == 0x1828u && req->fs_token == 0x1828u &&
        req->count == 4u && req->target_width <= 320u &&
        req->target_height <= 180u) {
        gxm_feedback_effect_skips++;
        gxm_last_draw_class = 2u;
        if (gxm_feedback_effect_skips <= 4u ||
            (gxm_feedback_effect_skips % 256u) == 0u)
            glog("VP_GXM_EFFECT_OMIT token=0x1828 reason=feedback-bloom skips=%llu\n",
                 (unsigned long long)gxm_feedback_effect_skips);
        return 0;
    }
    gxm_draw_stage = 1u;
    /* Diagnostic cull/depth override must be known before a fully masked draw
     * is classified: with depth write forced off it is a side-effect-free
     * no-op rather than a refused depth write. */
    if (req->hash == 0xf8u) {
        static int f8_state_override = -1;
        if (f8_state_override < 0) {
            FILE *marker = fopen("ux0:data/mhgu-vita-port/f8-state-override.enable", "rb");
            f8_state_override = marker != NULL;
            if (marker != NULL) fclose(marker);
            glog("VP_GXM_F8_STATE_OVERRIDE enabled=%d\n", f8_state_override);
        }
        if (f8_state_override) {
            cull = SCE_GXM_CULL_NONE;
            depth_func = SCE_GXM_DEPTH_FUNC_ALWAYS;
            depth_write = 0u;
            glog("VP_GXM_F8_STATE_OVERRIDE_APPLY idx=%u action=cull-none-depth-always-depth-write-off\n",
                 req->draw_index);
        }
    }
    /* Maxwell/GL semantics: depth_test_enable=0 disables depth writes too,
     * even when the raw write bit is set (yuzu gl_rasterizer
     * SyncDepthTestState disables the depth mask in that case). The normalized
     * value and the masked-draw classification are decided together by the
     * host-testable planner below, before any texture, surface or pool state
     * is touched. */
    /* Validate the guest zeta metadata even when the colour mask is zero: an
     * enabled but malformed depth binding must not be silently ignored. */
    gxm_draw_stage = 2u;
    depth_status = vp_gxm_depth_desc_build(&depth_binding.desc,
            req->depth_state.enable, req->depth_state.va,
            req->depth_state.width, req->depth_state.height,
            req->depth_state.depth, req->depth_state.dim_control,
            req->depth_state.format, req->depth_state.tile_mode,
            req->depth_state.array_pitch);
    if (depth_status != VP_GXM_DEPTH_OK &&
        depth_status != VP_GXM_DEPTH_DISABLED) {
        gxm_depth_stats.reject_meta++;
        depth_log_reject("draw", depth_status, req->draw_index, req->target_va,
                         req->target_width, req->target_height, req->depth_test,
                         depth_write, &req->depth_state);
        if (req->color_mask == 0u) gxm_last_draw_class = 1u;
        return -2;
    }
    {
        VPGxmDepthDrawPlanInput plan_in;
        VPGxmDepthDrawPlan plan;
        int plan_rc;
        memset(&plan_in, 0, sizeof(plan_in));
        plan_in.color_mask = req->color_mask;
        plan_in.depth_test = req->depth_test;
        plan_in.depth_write_raw = depth_write;
        plan_in.depth_func = req->depth_func;
        plan_in.stencil_enable = req->depth_state.stencil_enable;
        plan_in.depth_status = depth_status;
        plan_in.depth_format_class = depth_binding.desc.format_class;
        plan_in.color_width = req->target_width;
        plan_in.color_height = req->target_height;
        plan_in.depth_width = req->depth_state.width;
        plan_in.depth_height = req->depth_state.height;
        plan_rc = vp_gxm_depth_draw_plan(&plan_in, &plan);
        if (plan_rc != VP_GXM_DEPTH_OK) {
            glog("VP_GXM_FRAME_DRAW reject idx=%u depth-plan rc=%d\n",
                 req->draw_index, plan_rc);
            return -2;
        }
        if (plan.effective_write != depth_write) {
            if (gxm_depth_write_normalized_logs < 16u ||
                (gxm_depth_write_normalized_logs % 1024u) == 0u)
                glog("VP_GXM_DEPTH_WRITE_NORMALIZED idx=%u raw=%u "
                     "depth_test=0 effective=0 action=write-disabled n=%u\n",
                     req->draw_index, depth_write,
                     gxm_depth_write_normalized_logs);
            gxm_depth_write_normalized_logs++;
            depth_write = plan.effective_write;
        }
        if (plan.refuse) {
            static unsigned mask_zero_logs;
            gxm_depth_stats.masked_refuse++;
            if (mask_zero_logs < 32u || (mask_zero_logs % 512u) == 0u)
                glog("VP_GXM_FRAME_MASK_ZERO idx=%u hash=0x%x action=refuse "
                     "reason=%s color=0x%llx %ux%u zen=%u zva=0x%llx "
                     "zfmt=0x%x ztile=0x%x zpitch=0x%x zsize=%ux%u "
                     "zdepth=%u zdim=%u depth_test=%u depth_write=%u "
                     "stencil=%u mask_log_n=%u\n",
                     req->draw_index, req->hash, plan.reason,
                     (unsigned long long)req->target_va, req->target_width,
                     req->target_height, req->depth_state.enable,
                     (unsigned long long)req->depth_state.va,
                     req->depth_state.format, req->depth_state.tile_mode,
                     req->depth_state.array_pitch, req->depth_state.width,
                     req->depth_state.height, req->depth_state.depth,
                     req->depth_state.dim_control, req->depth_test,
                     depth_write, req->depth_state.stencil_enable,
                     mask_zero_logs);
            mask_zero_logs++;
            gxm_last_draw_class = 1u;
            return -2;
        }
        if (plan.noop) {
            static unsigned mask_zero_logs;
            gxm_depth_stats.masked_noop++;
            if (mask_zero_logs < 32u || (mask_zero_logs % 512u) == 0u)
                glog("VP_GXM_FRAME_MASK_ZERO idx=%u hash=0x%x action=noop "
                     "reason=%s color=0x%llx %ux%u zen=%u zva=0x%llx "
                     "zfmt=0x%x ztile=0x%x zpitch=0x%x zsize=%ux%u "
                     "zdepth=%u zdim=%u depth_test=%u depth_write=%u "
                     "stencil=%u mask_log_n=%u\n",
                     req->draw_index, req->hash, plan.reason,
                     (unsigned long long)req->target_va, req->target_width,
                     req->target_height, req->depth_state.enable,
                     (unsigned long long)req->depth_state.va,
                     req->depth_state.format, req->depth_state.tile_mode,
                     req->depth_state.array_pitch, req->depth_state.width,
                     req->depth_state.height, req->depth_state.depth,
                     req->depth_state.dim_control, req->depth_test, depth_write,
                     req->depth_state.stencil_enable, mask_zero_logs);
            mask_zero_logs++;
            gxm_last_draw_class = 2u;
            return 0;
        }
        if (plan.depth_only) {
            /* Real depth production into the validated zeta identity. The
             * stale Maxwell RT0 binding (target_va/size) is never used as the
             * render target or render extent. */
            depth_only = 1;
            gxm_last_draw_class = 1u;
            render_width = plan.render_width;
            render_height = plan.render_height;
        }
    }
    if (req->hash == 0xca2u) {
        static unsigned movie_requests;
        unsigned n = __sync_fetch_and_add(&movie_requests, 1u);
        movie_trace = n < 12u || (n % 60u) == 0u;
        if (movie_trace) {
            const VPGxmExtraTexture *t1 = &req->extra_textures[0];
            const VPGxmExtraTexture *t2 = &req->extra_textures[1];
            glog("VP_GXM_MOVIE_INPUT n=%u idx=%u target=0x%llx %ux%u "
                 "tex0=0x%llx %ux%u fmt=%u data=%p samp=%08x,%08x "
                 "tex1=0x%llx %ux%u fmt=%u data=%p samp=%08x,%08x "
                 "tex2=0x%llx %ux%u fmt=%u data=%p samp=%08x,%08x\n",
                 n, req->draw_index, (unsigned long long)req->target_va,
                 req->target_width, req->target_height,
                 (unsigned long long)req->texture_va, req->tex_width,
                 req->tex_height, req->tex_format, req->texels,
                 req->sampler[0], req->sampler[1],
                 (unsigned long long)t1->va, t1->width, t1->height,
                 t1->tex_format, t1->texels, t1->sampler[0], t1->sampler[1],
                 (unsigned long long)t2->va, t2->width, t2->height,
                 t2->tex_format, t2->texels, t2->sampler[0], t2->sampler[1]);
        }
    }
    if (req->hash == 0xf8u) {
        static unsigned model_requests;
        unsigned n = __sync_fetch_and_add(&model_requests, 1u);
        model_trace = n < 12u || (n % 60u) == 0u;
        if (model_trace) {
            const VPGxmExtraTexture *t1 = &req->extra_textures[0];
            const VPGxmExtraTexture *t2 = &req->extra_textures[1];
            glog("VP_GXM_MODEL_INPUT n=%u idx=%u target=0x%llx %ux%u "
                 "count=%u stride=%u indexed=%u icount=%u tex0=0x%llx %ux%u fmt=%u data=%p "
                 "tex1=0x%llx %ux%u fmt=%u data=%p cube=%u mips=%u "
                 "tex2=0x%llx %ux%u fmt=%u data=%p cube=%u mips=%u\n",
                 n, req->draw_index, (unsigned long long)req->target_va,
                 req->target_width, req->target_height, req->count, req->stride,
                 req->indexed, req->index_count,
                 (unsigned long long)req->texture_va, req->tex_width,
                 req->tex_height, req->tex_format, req->texels,
                 (unsigned long long)t1->va, t1->width, t1->height,
                 t1->tex_format, t1->texels, t1->cube, t1->mip_count,
                 (unsigned long long)t2->va, t2->width, t2->height,
                 t2->tex_format, t2->texels, t2->cube, t2->mip_count);
        }
        if (model_trace) gxm_model_probe_draw(req);
    }
    /* Conservative eligibility: only the quiet frame path, with no diagnostic
     * dump active, may encode into the shared scene. Draws that fail validation
     * before this point never disturb an already-open batch. */
    flush_pair0 = gxm_batch_perf.total_sum;
    t_validate = gxm_prof_now();
    gxm_draw_stage = 3u;
    if (gxm_clear_fusion_enabled()) {
        rc = draw_materialize_pending_samples(req);
        if (rc < 0) return rc;
    }
    {
        int cand_base = static_mode && gxm_batch_enabled() &&
                        !gxm_batch_aux_disabled();
        int cand_quiet = req->quiet && !movie_trace && !model_trace;
        int cand_view = cand_base && cand_quiet &&
                        gxm_batch_depth_views_ready(req);
        int cand_hazard = cand_view && !gxm_batch_sample_hazard(req);
        int cand_feedback = depth_only || req->target_va == 0u ||
                            req->texture_va != req->target_va;
        batch_candidate = cand_base && cand_quiet && cand_view &&
                          cand_hazard && cand_feedback;
        if (!batch_candidate &&
            (gxm_batch.policy.open || gxm_batch.scene_open)) {
            unsigned why = !cand_base ? VP_GXM_BATCH_INELIG_DISABLED
                         : !cand_quiet ? VP_GXM_BATCH_INELIG_QUIET
                         : !cand_view ? VP_GXM_BATCH_INELIG_DEPTH_VIEW
                         : !cand_hazard ? VP_GXM_BATCH_INELIG_HAZARD
                         : VP_GXM_BATCH_INELIG_FEEDBACK;
            gxm_batch_inelig[why]++;
        }
    }
    if (gxm_batch.policy.open || gxm_batch.scene_open) {
        if (!batch_candidate) {
            rc = gxm_batch_flush_worker(GXM_BATCH_REASON_INELIGIBLE);
            if (rc < 0) return rc;
            /* The producer is complete now; its consumer can start a new
             * shared scene instead of being forced into a one-draw scene. */
            batch_candidate = static_mode && gxm_batch_enabled() &&
                !gxm_batch_aux_disabled() && req->quiet &&
                !movie_trace && !model_trace &&
                gxm_batch_depth_views_ready(req) &&
                (depth_only || req->target_va == 0u ||
                 req->texture_va != req->target_va);
        } else {
            int planned_depth_attachment = depth_status == VP_GXM_DEPTH_OK &&
                !(req->depth_test == 0u && depth_write == 0u &&
                  req->depth_state.stencil_enable == 0u);
            unsigned reason = VP_BATCH_ACCEPT;
            if (depth_only && !planned_depth_attachment)
                batch_candidate = 0;
            if (!batch_candidate) {
                rc = gxm_batch_flush_worker(GXM_BATCH_REASON_INELIGIBLE);
                if (rc < 0) return rc;
            } else if (gxm_batch.policy.open && planned_depth_attachment &&
                       (depth_binding.desc.raw_depth != gxm_batch.depth_desc.raw_depth ||
                        depth_binding.desc.dim_control != gxm_batch.depth_desc.dim_control ||
                        depth_binding.desc.tile_mode != gxm_batch.depth_desc.tile_mode ||
                        depth_binding.desc.array_pitch != gxm_batch.depth_desc.array_pitch)) {
                rc = gxm_batch_flush_worker(GXM_BATCH_REASON_DEPTH);
                if (rc < 0) return rc;
            } else if (!vp_batch_policy_identity_matches(
                    &gxm_batch.policy, depth_only,
                    req->target_va, req->target_width, req->target_height,
                    req->target_format, planned_depth_attachment,
                    depth_binding.desc.va, depth_binding.desc.format,
                    depth_binding.desc.width, depth_binding.desc.height,
                    &reason)) {
                unsigned flush_reason = reason == VP_BATCH_FLUSH_RT
                    ? GXM_BATCH_REASON_RT
                    : reason == VP_BATCH_FLUSH_DEPTH
                        ? GXM_BATCH_REASON_DEPTH
                        : GXM_BATCH_REASON_MODE;
                rc = gxm_batch_flush_worker(flush_reason);
                if (rc < 0) return rc;
            }
        }
    }
    if (static_mode) {
        /* While a scene is open, only a non-mutating pair-cache hit is legal.
         * A miss flushes the scene before resident registration or eviction. */
        int cached = static_pair_cached(req, &pair, &static_pair);
        if (cached != 0) {
            if (gxm_batch.policy.open || gxm_batch.scene_open) {
                rc = gxm_batch_flush_worker(GXM_BATCH_REASON_RESOURCE);
                if (rc < 0) return rc;
            }
            rc = static_pair_get(req, &pair, &static_pair);
            if (rc < 0) return -2;
        }
        static_current_pair = static_pair;
    } else {
        for (unsigned pi = 0; pi < gxm_pair_count; pi++)
            if (gxm_pairs[pi].hash == req->hash &&
                gxm_pairs[pi].viewport_transform == req->viewport_transform)
                pair = &gxm_pairs[pi];
        if (pair == NULL) {
            glog("VP_GXM_FRAME_DRAW idx=%u missing hash=0x%x viewport=%u\n",
                 req->draw_index, req->hash, req->viewport_transform);
            return -2;
        }
    }
    t_pair = gxm_prof_now();
    pair_flush_us = gxm_batch_perf.total_sum - flush_pair0;
    gxm_draw_stage = 4u;
    /* Refuse an unsupported sampler layout before any scene state or texture
     * unit is touched. Static residents are validated when their GXP is
     * registered; the legacy path needs the same explicit check per program. */
    if (static_mode) {
        if (static_residents[static_pair->fs_resident].refl.sampler_unsupported ||
            static_residents[static_pair->vs_resident].refl.sampler_unsupported) {
            glog("VP_GXM_FRAME_DRAW idx=%u hash=0x%x sampler-layout-unsupported "
                 "fragment=%u vertex=%u limit=%u\n", req->draw_index, req->hash,
                 static_residents[static_pair->fs_resident].refl.sampler_unsupported,
                 static_residents[static_pair->vs_resident].refl.sampler_unsupported,
                 VP_GXM_FRAGMENT_SAMPLER_LIMIT);
            return -2;
        }
    } else if (program_sampler_layout(pair->fs_program,
                                      VP_STATIC_INDEX_STAGE_FRAGMENT, NULL) < 0 ||
               program_sampler_layout(pair->vs_program,
                                      VP_STATIC_INDEX_STAGE_VERTEX, NULL) < 0) {
        glog("VP_GXM_FRAME_DRAW idx=%u hash=0x%x sampler-layout-unsupported "
             "stage=fragment-or-vertex limit=%u\n", req->draw_index, req->hash,
             VP_GXM_FRAGMENT_SAMPLER_LIMIT);
        return -2;
    }
    /* A helper uniform trio is an all-or-nothing ABI.  A partial trio (or a
     * vertex-stage trio) is refused before any texture or scene state is
     * touched; it is never completed with zero defaults. */
    if (static_mode) {
        if (static_shadow_layout_check(
                &static_residents[static_pair->fs_resident].refl,
                &static_residents[static_pair->vs_resident].refl,
                req->draw_index) < 0)
            return -2;
    } else if (legacy_shadow_layout_check(pair->fs_program, pair->vs_program,
                                          req->draw_index) < 0) {
        return -2;
    }
    /* Depth-texture attachment identity is checked after the real pool binding
     * is known.  A stale request field or a depth state that has no effect must
     * not reject, and depth-test-enabled/write-disabled is still an attachment. */
    gxm_draw_stage = 5u;

    if (depth_only &&
        (static_mode ? static_residents[static_pair->fs_resident].has_f_mask != 0u
                     : sceGxmProgramFindParameterByName(pair->fs_program, "f_mask") != NULL)) {
        /* f_mask is a colour-path diagnostic texture with no guest descriptor
         * in this request. Binding its white fallback to a real depth producer
         * would fake a sampled resource. */
        glog("VP_GXM_FRAME_TEX_REJECT idx=%u reason=missing-real-resource "
             "stage=fragment slot=f_mask cbuf_off=none va=0x0 depth_only=1\n",
             req->draw_index);
        return -2;
    }
    gxm_skin_probe(req);
    gxm_creation_probe(req);
    if (!static_mode && (pair->stride != req->stride ||
        memcmp(pair->attributes, req->attributes, sizeof(pair->attributes)))) {
        rc = gxm_rebuild_pair(pair, req->stride, req->attributes);
        if (rc < 0) {
            glog("VP_GXM_FRAME_DRAW idx=%u pair rebuild rc=%d\n", req->draw_index, rc);
            return -2;
        }
    }
    prim = topology_to_gxm(req->topology);
    if ((uint32_t)prim == 0xffffffffu) {
        glog("VP_GXM_FRAME_DRAW idx=%u unsupported topology=%u\n",
             req->draw_index, req->topology);
        return -2;
    }
    /* A bound extra sampler that reads the surface this draw writes has no
     * defined read/write order in this backend. Reject before the target
     * surface or any scene is touched; never invent a hazard order. A
     * depth-only draw has no colour surface, so its stale RT0 address is not
     * a feedback target and must not reject a real depth producer. */
    for (unsigned e = 0; !depth_only && e < VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u; e++) {
        const SceGxmProgramParameter *extra_parameter;
        const VPGxmExtraTexture *extra = &req->extra_textures[e];
        if (static_mode) {
            extra_parameter =
                static_residents[static_pair->fs_resident].refl.tex[e + 1u];
            static_stats.reflect_name_formats_avoided++;
        } else {
            char name[8];
            snprintf(name, sizeof(name), "tex%u", e + 1u);
            extra_parameter = sceGxmProgramFindParameterByName(pair->fs_program,
                                                               name);
        }
        if (extra_parameter == NULL || extra->va == 0u ||
            extra->va != req->target_va)
            continue;
        /* frame_fs is not created yet at this point; release defensively so a
         * future reorder cannot leak an owned fragment program on this path. */
        frame_release_fs(frame_fs, frame_fs_owned);
        return gxm_extra_reject(req->draw_index, e + 1u, "feedback-alias",
                                extra, req->target_va);
    }
    if (static_mode && static_pair != NULL && gxm_shared_prepare_active()) {
        /* Pair hits already ran and stored this conversion, and the pair's
         * creation check covered the same six guest enums plus color mask. */
        blend = static_pair->blend_info;
        static_shared_blend_reuses++;
    } else if (build_blend_info(req, &blend) < 0) return -2;
    /* Census only: Maxwell factor 1 is ZERO and 2 is ONE, so ONE/ZERO is the
     * opaque colour-blend case. */
    if (req->blend_color_src == 2u && req->blend_color_dst == 1u)
        gxm_draws_opaque++;
    else
        gxm_draws_blended++;
    t_feedback = gxm_prof_now();
    flush_fb0 = gxm_batch_perf.total_sum;
    if (batch_candidate) {
        const VPStaticReflection *fs_refl =
            &static_residents[static_pair->fs_resident].refl;
        uint32_t alpha_func = req->alpha_test_func;
        int pre_reject = 0;
        if (alpha_func >= 0x200u && alpha_func <= 0x207u) alpha_func -= 0x1ffu;
        /* These are the draw-local checks that would otherwise fail after the
         * shared scene is already open and force an abort/flush. Refuse batch
         * membership instead; the synchronous path still performs the exact
         * same check and reports the same failure. */
        if (fs_refl->face_state != NULL && req->cull_enable &&
            (req->cull_face != 0x404u && req->cull_face != 0x405u)) {
            pre_reject = 1;
            gxm_batch_pre_reject_why = 0u;
        }
        if (req->alpha_test_enable &&
            (fs_refl->alpha_test == NULL || alpha_func < 1u || alpha_func > 8u)) {
            pre_reject = 1;
            gxm_batch_pre_reject_why = 1u;
        }
        if (fs_refl->tex[0] != NULL) {
            if (gxm_shared_prepare_active()) {
                /* Same cached plan the apply below will use; this only decides
                 * batch membership, it does not bind or modify any texture. */
                const VPGxmSamplerPlan *plan = gxm_sampler_plan_for(
                    VP_GXM_SAMPLER_PLAN_PRIMARY, req->sampler);
                if (plan == NULL || plan->rc < 0) {
                    pre_reject = 1;
                    gxm_batch_pre_reject_why = 2u;
                }
            } else {
                SceGxmTextureAddrMode u_mode, v_mode;
                if ((req->sampler[1] & 3u) < 1u || (req->sampler[1] & 3u) > 2u ||
                    ((req->sampler[1] >> 4) & 3u) < 1u ||
                    ((req->sampler[1] >> 4) & 3u) > 2u ||
                    gxm_texture_addr_mode(req->sampler[0], &u_mode) < 0 ||
                    gxm_texture_addr_mode(req->sampler[0] >> 3, &v_mode) < 0) {
                    pre_reject = 1;
                    gxm_batch_pre_reject_why = 2u;
                }
            }
        }
        if (pre_reject) {
            req->batch_fallback = 1u;
            static_stats.batch_sync_fallbacks++;
            gxm_batch_pre_reject[gxm_batch_pre_reject_why]++;
            if (gxm_batch.policy.open || gxm_batch.scene_open) {
                rc = gxm_batch_flush_worker(GXM_BATCH_REASON_INELIGIBLE);
                if (rc < 0) return rc;
            }
            batch_candidate = 0;
        }
    }
    /* Allocate/retire targets before admitting this draw. Recycling after
     * admission would flush a policy entry whose GPU draw is not encoded yet. */
    if (!depth_only) {
        VPGxmSurface *existing = surface_find(req->target_va);
        if (existing == NULL || existing->width != req->target_width ||
            existing->height != req->target_height) {
            if (gxm_batch_flush_worker(GXM_BATCH_REASON_RESOURCE) < 0) return -1;
            (void)surface_find(req->texture_va);
            for (unsigned e = 0; e < VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u; ++e)
                (void)surface_find(req->extra_textures[e].va);
            (void)surface_find(req->vertex_texture.va);
            if (surface_get(req->target_va, req->target_width,
                            req->target_height, req->target_format, "draw") == NULL)
                return -1;
        }
    }
    t_admit0 = gxm_prof_now();
    if (batch_candidate) {
        int planned_depth_attachment = depth_status == VP_GXM_DEPTH_OK &&
            !(req->depth_test == 0u && depth_write == 0u &&
              req->depth_state.stencil_enable == 0u);
        batch_uniform_base = gxm_batch_uniform_for(req, static_pair);
        batch_uniform = batch_uniform_base;
        if (gxm_clear_fusion_enabled() && req->target_va != 0u) {
            VPGxmSurface *ct = surface_find(req->target_va);
            /* One extra vertex/fragment default buffer for the in-scene clear;
             * this is uniform-ring budget only, not staged bytes. */
            if (ct != NULL && ct->clear_pending)
                batch_uniform = gxm_batch_add_size(batch_uniform, 256u);
        }
        t_uniform = gxm_prof_now();
        /* reserve_for takes the base uniform bytes the old staging accounting
         * used; the clear charge above only narrows the uniform budget. It
         * re-runs only the snapshot probes after a flush. */
        batch_reserve = gxm_batch_reserve_for(req, static_pair, depth_only,
                                              batch_uniform_base);
        t_reserve1 = gxm_prof_now();
        if (batch_uniform > VP_GXM_BATCH_UNIFORM_BUDGET ||
            (gxm_batch.policy.open &&
             batch_uniform > VP_GXM_BATCH_UNIFORM_BUDGET -
                                gxm_batch.uniform_bytes)) {
            if (batch_uniform > VP_GXM_BATCH_UNIFORM_BUDGET) {
                /* A single draw exceeds the conservative ring share; keep the
                 * synchronous fallback rather than gambling on ring capacity. */
                req->batch_fallback = 1u;
                static_stats.batch_sync_fallbacks++;
            }
            if (gxm_batch.policy.open || gxm_batch.scene_open) {
                gxm_batch_budget[0]++;
                rc = gxm_batch_flush_worker(GXM_BATCH_REASON_BUDGET);
                if (rc < 0) return rc;
                admission_flushed = 1;
            }
            if (batch_uniform > VP_GXM_BATCH_UNIFORM_BUDGET)
                batch_candidate = 0;
        }
        /* A flush invalidates shared snapshots; only then is the reserve
         * different. Without a flush the first result is still exact. */
        if (admission_flushed)
            batch_reserve = gxm_batch_reserve_for(req, static_pair, depth_only,
                                                  batch_uniform_base);
        if (!batch_candidate) {
            /* The single draw exceeds the uniform-ring share; the synchronous
             * path below owns it now. */
        } else if (batch_reserve > VP_GXM_BATCH_PAGE_BYTES) {
            req->batch_fallback = 1u;
            if (gxm_batch.policy.open || gxm_batch.scene_open) {
                gxm_batch_budget[1]++;
                rc = gxm_batch_flush_worker(GXM_BATCH_REASON_BUDGET);
                if (rc < 0) return rc;
            }
            batch_candidate = 0;
        } else if (gxm_batch_ensure_staging() < 0) {
            req->batch_fallback = 1u;
            if (gxm_batch.policy.open || gxm_batch.scene_open) {
                rc = gxm_batch_flush_worker(GXM_BATCH_REASON_RESOURCE);
                if (rc < 0) return rc;
            }
            batch_candidate = 0;
        } else {
            if (gxm_batch.policy.open || gxm_batch.scene_open) {
                batch_decision = vp_batch_policy_admit(
                    &gxm_batch.policy, req->batch_scope, depth_only,
                    req->target_va, req->target_width, req->target_height,
                    req->target_format, planned_depth_attachment,
                    depth_binding.desc.va, depth_binding.desc.format,
                    depth_binding.desc.width, depth_binding.desc.height,
                    batch_reserve, VP_BATCH_MAX_DRAWS,
                    VP_BATCH_STAGING_BUDGET);
                if (batch_decision != VP_BATCH_ACCEPT) {
                    /* The open scene's staging bytes plus this reserve exceed
                     * the arena; the draw flushes the scene and retries. */
                    if (batch_decision == VP_BATCH_FLUSH_BUDGET)
                        gxm_batch_budget[2]++;
                    rc = gxm_batch_flush_worker(
                        gxm_batch_flush_reason_for(batch_decision));
                    if (rc < 0) return rc;
                    batch_reserve = gxm_batch_reserve_for(req, static_pair,
                                                          depth_only,
                                                          batch_uniform_base);
                }
            }
            if (gxm_batch_ensure_staging() < 0) return -1;
            if (batch_reserve > gxm_batch.staging_size - gxm_batch.staging_used &&
                gxm_batch_next_staging_page() < 0) {
                gxm_batch_budget[2]++;
                rc = gxm_batch_flush_worker(GXM_BATCH_REASON_BUDGET);
                if (rc < 0 || gxm_batch_ensure_staging() < 0) return -1;
                batch_reserve = gxm_batch_reserve_for(req, static_pair, depth_only,
                                                      batch_uniform_base);
            }
            if (!gxm_batch.policy.open || !gxm_batch.scene_open)
                batch_decision = vp_batch_policy_admit(
                    &gxm_batch.policy, req->batch_scope, depth_only,
                    req->target_va, req->target_width, req->target_height,
                    req->target_format, planned_depth_attachment,
                    depth_binding.desc.va, depth_binding.desc.format,
                    depth_binding.desc.width, depth_binding.desc.height,
                    batch_reserve, VP_BATCH_MAX_DRAWS,
                    VP_BATCH_STAGING_BUDGET);
            if (batch_decision == VP_BATCH_ACCEPT &&
                gxm_batch.policy.draws < VP_BATCH_MAX_DRAWS &&
                batch_reserve <= gxm_batch.staging_size -
                                  gxm_batch.staging_used) {
                VPGxmBatchEntry *entry =
                    &gxm_batch.entries[gxm_batch.policy.draws];
                size_t base = gxm_batch.staging_used;
                if (entry->used && entry->direct.count != 0u)
                    gxm_direct_refs_quarantine(&entry->direct);
                memset(entry, 0, sizeof(*entry));
                entry->used = 1;
                entry->reserve_bytes = batch_reserve;
                entry->uniform_bytes = batch_uniform;
                entry->staging_base = base;
                entry->staging_page = gxm_batch.staging;
                entry->snapshot_base = gxm_batch.textures->count;
                entry->t_start = t_start;
                gxm_batch.staging_used = base + batch_reserve;
                gxm_batch.uniform_bytes += batch_uniform;
                if (gxm_batch.uniform_bytes > static_stats.batch_uniform_peak)
                    static_stats.batch_uniform_peak =
                        (unsigned)gxm_batch.uniform_bytes;
                gxm_batch_current_entry = entry;
                batch_entry = entry;
            } else {
                req->batch_fallback = 1u;
                if (gxm_batch.policy.open || gxm_batch.scene_open) {
                    if (batch_decision == VP_BATCH_FLUSH_BUDGET ||
                        batch_decision == VP_BATCH_REFUSE_TOO_LARGE)
                        gxm_batch_budget[2]++;
                    rc = gxm_batch_flush_worker(
                        batch_decision == VP_BATCH_REFUSE_TOO_LARGE
                            ? GXM_BATCH_REASON_BUDGET
                            : gxm_batch_flush_reason_for(batch_decision));
                    if (rc < 0) return rc;
                }
                batch_candidate = 0;
            }
        }
    }
    t_admit = gxm_prof_now();
    admit_flush_us = gxm_batch_perf.total_sum - flush_fb0;
    if (t_admit0 != 0u && t_uniform != 0u) {
        unsigned pk = depth_only ? 1u : 0u;
        gxm_batch_encode_perf[pk].prj_us += t_admit0 - t_feedback;
        gxm_batch_encode_perf[pk].uniform_us += t_uniform - t_admit0;
        gxm_batch_encode_perf[pk].reserve_us += t_reserve1 - t_uniform;
        gxm_batch_encode_perf[pk].policy_us += t_admit - t_reserve1;
    }
    gxm_draw_stage = 6u;
    if (depth_only) {
        /* No colour surface is created or looked up for a depth-only draw; the
         * guest's stale RT0 colour binding is not a render extent. */
        if (batch_entry != NULL && gxm_batch.policy.open &&
            gxm_batch.scene_open) {
            render_target = gxm_batch.render_target;
            render_width = gxm_batch.render_width;
            render_height = gxm_batch.render_height;
            depth_binding.desc = gxm_batch.depth_desc;
            depth_binding.entry = gxm_batch.depth_entry;
            depth_binding.slot = gxm_batch.depth_entry != NULL
                ? (int)(gxm_batch.depth_entry - gxm_depths) : -1;
            depth_binding.enabled = gxm_batch.depth_enabled;
        } else {
            render_target = target_for(
                render_extent(render_width, render_height, render_width),
                render_extent(render_width, render_height, render_height));
            if (render_target == NULL) {
                glog("VP_GXM_FRAME_DRAW idx=%u depth-only target %ux%u unavailable\n",
                     req->draw_index, render_width, render_height);
                return -1;
            }
            rc = depth_binding_acquire(&depth_binding, &req->depth_state,
                    req->target_width, req->target_height,
                    render_width, render_height,
                    req->draw_index, req->target_va, req->depth_test, depth_write,
                    req->depth_write, "draw-depth-only");
            if (rc < 0) return rc;
        }
        if (!depth_binding.enabled) {
            glog("VP_GXM_FRAME_DRAW idx=%u depth-only without enabled zeta\n",
                 req->draw_index);
            return -2;
        }
    } else {
        if (batch_entry != NULL && gxm_batch.policy.open &&
            gxm_batch.scene_open) {
            target = gxm_batch.target;
            if (target == NULL) return -1;
            render_width = gxm_batch.render_width;
            render_height = gxm_batch.render_height;
            render_target = gxm_batch.render_target;
            depth_binding.desc = gxm_batch.depth_desc;
            depth_binding.entry = gxm_batch.depth_entry;
            depth_binding.slot = gxm_batch.depth_entry != NULL
                ? (int)(gxm_batch.depth_entry - gxm_depths) : -1;
            depth_binding.enabled = gxm_batch.depth_enabled;
            rc = 0;
        } else {
            target = surface_get(req->target_va, req->target_width, req->target_height,
                                 req->target_format, "draw");
            if (target == NULL) return -1;
            render_width = target->width;
            render_height = target->height;
            render_target = target_for(target->render_width, target->render_height);
            if (render_target == NULL) return -1;
            before_rgb = req->quiet && !movie_trace && !model_trace ? 0u : surface_nonzero_rgb(target);
            before_alpha = req->quiet && !movie_trace && !model_trace ? 0u : surface_nonzero_alpha(target);

            rc = depth_binding_acquire(&depth_binding, &req->depth_state,
                    req->target_width, req->target_height,
                    req->target_width, req->target_height,
                    req->draw_index, req->target_va, req->depth_test, depth_write,
                    req->depth_write, "draw");
        }
        if (rc < 0) {
            /* Bounded state census at the exact colour-path refusal: the raw
             * RT0 fields are included because a target-shape failure is only
             * meaningful together with the effective-attachment decision and
             * the active viewport/scissor. */
            static unsigned reject_state_logs;
            if (reject_state_logs < 32u ||
                (reject_state_logs % 512u) == 0u)
                glog("VP_GXM_DEPTH_REJECT_STATE idx=%u hash=0x%x vs=0x%x "
                     "fs=0x%x target=0x%llx %ux%u fmt=%u color_mask=0x%x "
                     "raw=0x%x common=0x%x rt_control=0x%x active_rt=%u "
                     "color_attachment=%u zva=0x%llx zsize=%ux%u zfmt=0x%x "
                     "zen=%u depth_test=%u depth_write=%u vp=%u,%u %ux%u "
                     "scissor=%u,%u,%u,%u,%u fits=%d n=%u\n",
                     req->draw_index, req->hash, req->vs_token, req->fs_token,
                     (unsigned long long)req->target_va, req->target_width,
                     req->target_height, req->target_format, req->color_mask,
                     req->color_mask_raw, req->color_mask_common,
                     req->rt_control, req->active_rt_index,
                     req->color_attachment,
                     (unsigned long long)req->depth_state.va,
                     req->depth_state.width, req->depth_state.height,
                     req->depth_state.format, req->depth_state.enable,
                     req->depth_test, depth_write, req->viewport_x,
                     req->viewport_y, req->viewport_width,
                     req->viewport_height, req->scissor_enable,
                     req->scissor_min_x, req->scissor_max_x,
                     req->scissor_min_y, req->scissor_max_y,
                     vp_gxm_depth_desc_fits(&depth_binding.desc,
                                            req->target_width,
                                            req->target_height),
                     reject_state_logs);
            reject_state_logs++;
            return rc;
        }
    }

    /* A depth surface with no test, write or stencil effect is not attached at
     * all.  GXM gets the initialized-disabled surface below, so a depth view of
     * the same VA would not create a read/write image alias. */
    t_resolve = gxm_prof_now();
    t_vertex=t_resolve;
    gxm_draw_stage = 7u;
    if (depth_binding.enabled && req->depth_test == 0u && depth_write == 0u &&
        req->depth_state.stencil_enable == 0u) {
        static unsigned depth_detach_logs;
        if (depth_detach_logs++ < 16u)
            glog("VP_GXM_DEPTH_DETACH idx=%u zva=0x%llx reason=no-test-write-stencil "
                 "surface=disabled action=detach n=%u\n", req->draw_index,
                 (unsigned long long)depth_binding.desc.va, depth_detach_logs - 1u);
        depth_binding.enabled = 0;
    }
    /* The attached image and a sampled image must never be the same depth
     * identity without a proven feedback protocol.  This applies even when the
     * depth write mask is off: a depth test still reads the attachment while the
     * sampler reads the same allocation.  Only declared, actually resolved
     * sampler slots participate; undeclared/stale request fields are zeroed by
     * the resolver and cannot reject. */
    if (depth_binding.enabled) {
        const SceGxmProgramParameter *primary_declared = static_mode ?
            static_residents[static_pair->fs_resident].refl.tex[0] :
            sceGxmProgramFindParameterByName(pair->fs_program, "tex0");
        if (primary_declared != NULL &&
            req->texture_source != VP_GXM_EXTRA_SOURCE_NONE &&
            req->texture_va == depth_binding.desc.va) {
            glog("VP_GXM_FRAME_TEX_REJECT idx=%u reason=depth-attachment-feedback "
                 "stage=fragment slot=tex0 source=%u va=0x%llx zva=0x%llx "
                 "depth_test=%u depth_write=%u stencil=%u\n", req->draw_index,
                 req->texture_source, (unsigned long long)req->texture_va,
                 (unsigned long long)depth_binding.desc.va, req->depth_test,
                 depth_write, req->depth_state.stencil_enable);
            return -2;
        }
        for (unsigned e = 0; e < VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u; e++) {
            const SceGxmProgramParameter *declared = static_mode ?
                static_residents[static_pair->fs_resident].refl.tex[e + 1u] :
                sceGxmProgramFindParameterByName(pair->fs_program,
                    e == 0u ? "tex1" : e == 1u ? "tex2" : "tex3");
            const VPGxmExtraTexture *extra = &req->extra_textures[e];
            if (declared == NULL || extra->source == VP_GXM_EXTRA_SOURCE_NONE ||
                extra->va != depth_binding.desc.va)
                continue;
            glog("VP_GXM_FRAME_TEX_REJECT idx=%u reason=depth-attachment-feedback "
                 "stage=fragment slot=tex%u source=%u va=0x%llx zva=0x%llx "
                 "depth_test=%u depth_write=%u stencil=%u\n", req->draw_index,
                 e + 1u, extra->source, (unsigned long long)extra->va,
                 (unsigned long long)depth_binding.desc.va, req->depth_test,
                 depth_write, req->depth_state.stencil_enable);
            return -2;
        }
        {
            const SceGxmProgramParameter *declared = static_mode ?
                static_residents[static_pair->vs_resident].refl.tex[0] :
                sceGxmProgramFindParameterByName(pair->vs_program, "tex0");
            const VPGxmExtraTexture *extra = &req->vertex_texture;
            if (declared != NULL && extra->source != VP_GXM_EXTRA_SOURCE_NONE &&
                extra->va == depth_binding.desc.va) {
                glog("VP_GXM_FRAME_TEX_REJECT idx=%u "
                     "reason=depth-attachment-feedback stage=vertex slot=tex0 "
                     "source=%u va=0x%llx zva=0x%llx depth_test=%u depth_write=%u "
                     "stencil=%u\n", req->draw_index, extra->source,
                     (unsigned long long)extra->va,
                     (unsigned long long)depth_binding.desc.va, req->depth_test,
                     depth_write, req->depth_state.stencil_enable);
                return -2;
            }
        }
    }

    if (static_mode) {
        /* The pair cache key includes the blend state; this object is reused
         * until evicted at a draw boundary (the draw path finishes the GPU
         * work synchronously). */
        frame_fs = pair->fs;
        frame_fs_owned = 0;
        rc = 0;
    } else {
        rc = sceGxmShaderPatcherCreateFragmentProgram(patcher, pair->fragment_id,
                SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
                &blend, pair->vs_program, &frame_fs);
    }
    if (rc < 0) {
        glog("VP_GXM_FRAME_DRAW idx=%u fs rc=%d mask=0x%x blend=%u/%u %u/%u\n",
             req->draw_index, rc, req->color_mask, req->blend_color_src,
             req->blend_color_dst, req->blend_alpha_src, req->blend_alpha_dst);
        return -2;
    }
    if (req->depth_test != 0u && depth_func_to_gxm(req->depth_func, &depth_func) < 0) {
        glog("VP_GXM_FRAME_DRAW idx=%u unsupported depth func=%u\n",
             req->draw_index, req->depth_func);
        frame_release_fs(frame_fs, frame_fs_owned);
        return -2;
    }
    if (req->cull_enable != 0u) {
        /* cull_face: 0x404 front, 0x405 back, 0x408 both. front_face:
         * 0x900 clockwise, 0x901 counter-clockwise. */
        if (req->cull_face == 0x408u) cull = SCE_GXM_CULL_CW; /* no both-side cull */
        else if (req->cull_face == 0x405u)
            cull = req->front_face == 0x901u ? SCE_GXM_CULL_CW : SCE_GXM_CULL_CCW;
        else if (req->cull_face == 0x404u)
            cull = req->front_face == 0x901u ? SCE_GXM_CULL_CCW : SCE_GXM_CULL_CW;
        else
            cull = req->cull_face == 0u ? SCE_GXM_CULL_NONE : SCE_GXM_CULL_NONE;
    }

    /* Prepare vertex and index buffers on the GPU side. The allocation is the
     * measured full-frame capacity even if a legacy draw created it first. */
    t_indices=gxm_prof_now();
    gxm_draw_stage = 8u;
    if (gxm_game_staging_ensure() < 0) {
        frame_release_fs(frame_fs, frame_fs_owned);
        return -1;
    }
    {
        size_t vertex_bytes = (size_t)req->count * req->stride;
        uint64_t xfer_t0 = gxm_prof_now();
        if (batch_entry != NULL) {
            void *dst = gxm_batch_entry_alloc(batch_entry, vertex_bytes, 16u);
            if (dst == NULL) {
                frame_release_fs(frame_fs, frame_fs_owned);
                return -1;
            }
            memcpy(dst, req->vertices, vertex_bytes);
            vertex_stream = dst;
        } else {
            memcpy(gxm_game_vertices, req->vertices, vertex_bytes);
            vertex_stream = gxm_game_vertices;
        }
        gxm_xfer_us += gxm_prof_now() - xfer_t0;
        gxm_xfer_bytes += vertex_bytes;
        gxm_vcolor_floor_apply((void *)vertex_stream, req);
    }
    if (static_mode && static_pair != NULL && static_pair->packed_mask != 0u) {
        int packed;
        size_t packed_bytes = (size_t)req->count * static_pair->packed_stream_stride;
        void *packed_dst;
        size_t packed_capacity;
        if (packed_bytes > VP_VERTEX_WINDOW_MAX_BYTES) {
            glog("VP_GXM_FRAME_DRAW idx=%u packed_stream_over_budget count=%u "
                 "stride=%u bytes=%llu capacity=%u mask=0x%x\n",
                 req->draw_index, req->count, static_pair->packed_stream_stride,
                 (unsigned long long)packed_bytes, VP_VERTEX_WINDOW_MAX_BYTES,
                 static_pair->packed_mask);
            frame_release_fs(frame_fs, frame_fs_owned);
            return -2;
        }
        if (batch_entry != NULL) {
            packed_dst = gxm_batch_entry_alloc(batch_entry, packed_bytes, 16u);
            packed_capacity = packed_bytes;
        } else {
            if (gxm_game_vertices_packed == NULL) {
                gxm_game_vertices_packed = gpu_alloc(VP_VERTEX_WINDOW_MAX_BYTES, 0, NULL);
                if (gxm_game_vertices_packed == NULL) {
                    glog("VP_GXM_FRAME_DRAW idx=%u packed_alloc_failed\n", req->draw_index);
                    frame_release_fs(frame_fs, frame_fs_owned);
                    return -1;
                }
            }
            packed_dst = gxm_game_vertices_packed;
            packed_capacity = VP_VERTEX_WINDOW_MAX_BYTES;
        }
        if (packed_dst == NULL) {
            frame_release_fs(frame_fs, frame_fs_owned);
            return -1;
        }
        packed = vp_vertex_expand_rgb10a2(req->vertices, req->count, req->stride,
                req->attributes, static_pair->packed_mask,
                packed_dst, packed_capacity,
                &packed_stream_stride);
        if (packed < 0 || packed_stream_stride != static_pair->packed_stream_stride) {
            glog("VP_GXM_FRAME_DRAW idx=%u packed_expand_failed mask=0x%x stride=%u\n",
                 req->draw_index, static_pair->packed_mask, packed_stream_stride);
            frame_release_fs(frame_fs, frame_fs_owned);
            return -2;
        }
        packed_stream = packed_dst;
    } else {
        packed_stream = NULL;
    }
    if (req->indexed) {
        unsigned index_size;
        unsigned out_size;
        uint32_t canonical_restart;
        void *index_dst;
        size_t index_capacity;
        size_t index_bytes;
        if (req->indices == NULL || req->index_count < 3u ||
            req->index_count > VP_VERTEX_WINDOW_MAX_INDEX_COUNT) {
            glog("VP_GXM_FRAME_DRAW idx=%u indexed without indices icount=%u\n",
                 req->draw_index, req->index_count);
            frame_release_fs(frame_fs, frame_fs_owned);
            return -2;
        }
        index_size = req->index_format == 0u ? 1u :
                     req->index_format == 1u ? 2u :
                     req->index_format == 2u ? 4u : 0u;
        if (index_size == 0u) {
            glog("VP_GXM_FRAME_DRAW idx=%u bad index format=%u\n",
                 req->draw_index, req->index_format);
            frame_release_fs(frame_fs, frame_fs_owned);
            return -2;
        }
        /* Stage every index format into the aligned GXM buffer. Real indices
         * are rebased by the raw window minimum; restart markers become the
         * canonical marker for the GXM index width, so the splitter below
         * cannot confuse a rebased real index with a restart. */
        out_size = req->index_format == 2u ? 4u : 2u;
        canonical_restart = out_size == 4u ? 0xffffffffu : 0xffffu;
        index_bytes = (size_t)req->index_count * out_size;
        effective_restart = gxm_effective_restart(req);
        if (!effective_restart) gxm_index_norestart++;
        strip_joined = effective_restart && prim == SCE_GXM_PRIMITIVE_TRIANGLE_STRIP &&
                       req->index_count <= VP_VERTEX_WINDOW_MAX_INDEX_COUNT / 2u;
        if (strip_joined) index_bytes *= 2u;
        if (batch_entry != NULL) {
            index_dst = gxm_batch_entry_alloc(batch_entry, index_bytes, 16u);
            index_capacity = index_bytes;
        } else {
            index_dst = gxm_game_indices;
            index_capacity = VP_VERTEX_WINDOW_MAX_INDEX_BYTES;
        }
        int remap_rc = -1;
        if (index_dst != NULL) {
            if (strip_joined) {
                remap_rc = vp_gpu_strip_remap(req->indices, req->index_count, index_size,
                    req->restart_index, req->index_window_first, index_dst,
                    index_capacity, out_size, &joined_segments);
            } else remap_rc = vp_vertex_window_remap(req->indices, req->index_count, index_size,
                effective_restart, req->restart_index, req->index_window_first,
                index_dst, index_capacity, out_size, canonical_restart);
            if (remap_rc >= 0 && index_size == out_size &&
                req->index_window_first == 0u && !effective_restart)
                gxm_index_remap_copy++;
        }
        if (remap_rc < 0) {
            glog("VP_GXM_FRAME_DRAW idx=%u index remap failed fmt=%u "
                 "window_first=%u restart=%u/%u icount=%u\n",
                 req->draw_index, req->index_format, req->index_window_first,
                 req->restart_enabled, req->restart_index, req->index_count);
            frame_release_fs(frame_fs, frame_fs_owned);
            return -2;
        }
        index_source = index_dst;
        index_format = out_size == 4u ? SCE_GXM_INDEX_FORMAT_U32
                                      : SCE_GXM_INDEX_FORMAT_U16;
        index_count = strip_joined ? (unsigned)remap_rc : req->index_count;
    } else {
        index_source = gxm_index_ramp_get();
        if (index_source == NULL) {
            frame_release_fs(frame_fs, frame_fs_owned);
            return -1;
        }
        gxm_index_ramp_bind++;
        index_format = SCE_GXM_INDEX_FORMAT_U16;
    }

    t_textures=gxm_prof_now();
    gxm_draw_stage = 9u;
    {
        VPGxmExtraTexture primary_identity;
        int primary_shadow_mode = 0;
        int primary_sampler_used;
        const char *shadow_reason = NULL;
        memset(&primary_identity, 0, sizeof(primary_identity));
        primary_identity.va = req->texture_va;
        memcpy(primary_identity.tic_words, req->tex_tic,
               sizeof(primary_identity.tic_words));
        memcpy(primary_identity.tsc_words, req->tex_tsc,
               sizeof(primary_identity.tsc_words));
        primary_identity.descriptor_type = req->tex_descriptor_type;
        primary_identity.descriptor_depth = req->tex_descriptor_depth;
        primary_identity.usage_flags = req->tex_usage_flags;
        primary_identity.tic_format = req->tex_tic_format;
        primary_identity.tic_header = req->tex_header;
        primary_identity.texture_type = req->tex_texture_type;
        primary_identity.depth = req->tex_depth;
        primary_identity.block_depth = req->tex_block_depth;
        primary_identity.max_mip_level = req->tex_max_mip_level;
        primary_identity.block_height = req->tex_block_height;
        primary_identity.swizzle = req->tex_swizzle;
        primary_identity.width = req->tex_width;
        primary_identity.height = req->tex_height;
        parameter = static_mode
                        ? static_residents[static_pair->fs_resident].refl.tex[0]
                        : sceGxmProgramFindParameterByName(pair->fs_program, "tex0");
        primary_sampler_used = parameter != NULL;
        if (parameter != NULL) {
            VPGxmShadowParams params;
            if (static_mode)
                static_shadow_params(
                    &static_residents[static_pair->fs_resident].refl, 0, &params);
            else
                legacy_shadow_params(pair->fs_program, 0, 0, &params);
            primary_shadow_mode = shadow_slot_mode(
                &params, &primary_identity,
                req->texture_source == VP_GXM_EXTRA_SOURCE_DEPTH,
                &shadow_reason);
            if (primary_shadow_mode < 0) {
                gxm_shadow_rejects++;
                glog("VP_GXM_SHADOW_REJECT idx=%u stage=fragment slot=0 "
                     "reason=%s source=%u tic_depth=%u desc_depth=%u "
                     "ticfmt=0x%x tic_compare=%u contract=%u va=0x%llx\n",
                     req->draw_index, shadow_reason, req->texture_source,
                     req->tex_depth, req->tex_descriptor_depth,
                     req->tex_tic_format, (req->tex_tsc[0] >> 9) & 1u,
                     params.present, (unsigned long long)req->texture_va);
                frame_release_fs(frame_fs, frame_fs_owned);
                return -2;
            }
            frame_primary_shadow = primary_shadow_mode;
        }

    /* Texture: a surface produced by an earlier pass, a CPU-decoded texture,
     * a real owned depth allocation, or no source. */
    if (primary_sampler_used && req->texture_source == VP_GXM_EXTRA_SOURCE_DEPTH) {
        if (depth_binding.enabled &&
            depth_binding.desc.va == primary_identity.va) {
            rc = gxm_extra_reject(req->draw_index, 0u, "feedback-depth-attachment",
                                  &primary_identity, req->target_va);
            frame_release_fs(frame_fs, frame_fs_owned);
            return rc;
        }
        rc = depth_texture_view(req->draw_index, 0u, &primary_identity,
                                (unsigned)primary_shadow_mode,
                                &frame_primary_depth_plan,
                                batch_entry != NULL
                                    ? &batch_entry->primary_depth_storage
                                    : &frame_primary_depth_view,
                                &frame_primary_depth_bind);
        if (rc != 0) {
            gxm_stage9_note(VP_GXM_S9_PRIMARY_DEPTH, req->draw_index);
            frame_release_fs(frame_fs, frame_fs_owned);
            return -2;
        }
        if (batch_entry != NULL) batch_entry->primary_depth_view = 1;
        texture = frame_primary_depth_bind;
    }
    if (primary_sampler_used && texture == NULL && req->texture_va != 0u) {
        VPGxmSurface *source = surface_find(req->texture_va);
        if (source != NULL) {
            if (batch_entry != NULL) {
                batch_entry->primary_surface_texture = source->texture;
                texture = &batch_entry->primary_surface_texture;
                batch_entry->primary_producer = source;
            } else {
                texture = &source->texture;
                source->sampled_count++;
            }
            if (!req->quiet)
                glog("VP_GXM_FRAME_TEX idx=%u source=surface va=0x%llx %ux%u draws=%u "
                     "samples=%u\n", req->draw_index,
                     (unsigned long long)source->va, source->width, source->height,
                     source->draw_count, source->sampled_count);
        }
    }
    if (primary_sampler_used && texture == NULL && req->texels != NULL && req->tex_width != 0u &&
        req->tex_height != 0u) {
        SceGxmTextureFormat fmt;
        if (frame_gxm_texture_format(req->tex_format,&fmt)<0) {
            glog("VP_GXM_FRAME_DRAW idx=%u unsupported texture format "
                 "upload=0x%x %ux%u\n",req->draw_index,req->tex_format,
                 req->tex_width,req->tex_height);
            gxm_stage9_note(VP_GXM_S9_PRIMARY_FORMAT, req->draw_index);
            frame_release_fs(frame_fs, frame_fs_owned);
            return -2;
        }
        size_t texture_bytes=vp_texture_upload_bytes(req->tex_width,req->tex_height,req->tex_format);
        if (!texture_bytes) {
            gxm_stage9_note(VP_GXM_S9_PRIMARY_BYTES, req->draw_index);
            frame_release_fs(frame_fs, frame_fs_owned);
            return -2;
        }
        if (batch_entry != NULL) {
            VPTextureSnapshotKey key = gxm_snapshot_key(req->texture_va,
                req->tex_width, req->tex_height, req->tex_format, 0u, 1u);
            int direct_eligible = gxm_direct_candidate(
                req->texels, req->texels_owned, req->tex_width,
                req->tex_height, req->tex_format, 0u, 1u, 1, NULL, NULL);
            void *tex_mem = gxm_batch_texture_bind(batch_entry, &key,
                req->texels, texture_bytes,
                req->texels_owned ? req->texels : NULL, direct_eligible);
            if (tex_mem == NULL) {
                frame_release_fs(frame_fs, frame_fs_owned);
                return -1;
            }
            texture = &batch_entry->primary_texture;
            if (!gxm_texture_desc_lookup(tex_mem, req->tex_width,
                    req->tex_height, fmt, 0u, 1u, VP_GXM_TEXTURE_DESC_PRIMARY,
                    req->sampler, texture)) {
                rc = sceGxmTextureInitLinear(texture, tex_mem,
                        fmt, req->tex_width, req->tex_height, 1);
                if (rc < 0) {
                    frame_release_fs(frame_fs, frame_fs_owned);
                    return rc;
                }
                rc = gxm_apply_primary_sampler(texture, req);
                if (rc < 0) {
                    frame_release_fs(frame_fs, frame_fs_owned);
                    return rc;
                }
                gxm_texture_desc_store(tex_mem, req->tex_width, req->tex_height,
                        fmt, 0u, 1u, VP_GXM_TEXTURE_DESC_PRIMARY,
                        req->sampler, texture);
            }
            batch_entry->primary_cpu_texture = 1;
        } else {
            uint64_t tex_t0 = gxm_prof_now();
            if (gxm_game_texture_mem == NULL || gxm_game_texture_format!=req->tex_format || gxm_game_texture_width != req->tex_width ||
                gxm_game_texture_height != req->tex_height) {
                /* Vita3K caches addresses from previously bound GXM descriptors.
                 * Keep replaced backings mapped to protect later state syncs. */
                if (gxm_game_texture_mem == NULL || gxm_game_texture_capacity<texture_bytes) {
                    void *new_mem=gpu_alloc((unsigned)texture_bytes,0,NULL);
                    if (!new_mem) {
                        frame_release_fs(frame_fs, frame_fs_owned);
                        return -1;
                    }
                    gxm_game_texture_mem=new_mem;
                    gxm_game_texture_capacity=texture_bytes;
                }
                rc = sceGxmTextureInitLinear(&gxm_game_texture, gxm_game_texture_mem,
                        fmt, req->tex_width, req->tex_height, 1);
                if (rc < 0) {
                    frame_release_fs(frame_fs, frame_fs_owned);
                    return rc;
                }
                gxm_game_texture_width = req->tex_width;
                gxm_game_texture_height = req->tex_height;
                gxm_game_texture_format=req->tex_format;
            }
            memcpy(gxm_game_texture_mem, req->texels,texture_bytes);
            gxm_tex_us += gxm_prof_now() - tex_t0;
            gxm_tex_bytes += texture_bytes;
            texture = &gxm_game_texture;
        }
        if (!req->quiet)
            glog("VP_GXM_FRAME_TEX idx=%u source=cpu va=0x%llx %ux%u\n",
                 req->draw_index, (unsigned long long)req->texture_va,
                 req->tex_width, req->tex_height);
    }
    if (texture == NULL) {
        unsigned samplers = static_mode ?
            static_residents[static_pair->fs_resident].refl.sampler_count :
            program_sampler_count(pair->fs_program);
        if (!primary_sampler_used) {
            if (!req->quiet)
                glog("VP_GXM_FRAME_TEX_UNUSED idx=%u hash=0x%x va=0x%llx "
                     "fragment declares no sampler depth_only=%d\n",
                     req->draw_index, req->hash,
                     (unsigned long long)req->texture_va, depth_only);
        } else {
            /* No black/white stand-in: a declared sampler must have a real CPU
             * payload or this frame's GPU producer. Depth-only production uses
             * exactly the same rule. */
            glog("VP_GXM_FRAME_TEX_REJECT idx=%u reason=missing-real-resource "
                 "stage=fragment slot=tex0 cbuf_off=32 va=0x%llx hash=0x%x "
                 "samplers=%u depth_only=%d\n", req->draw_index,
                 (unsigned long long)req->texture_va, req->hash, samplers,
                 depth_only);
            gxm_stage9_note(VP_GXM_S9_TEX_MISSING, req->draw_index);
            frame_release_fs(frame_fs, frame_fs_owned);
            return -2;
        }
    }
    }

    if (static_mode ?
            (static_set_guest_uniforms(NULL,
                &static_residents[static_pair->vs_resident].refl, 0u,
                req->stage_cbuf[0]) < 0 ||
             static_set_guest_uniforms(NULL,
                &static_residents[static_pair->fs_resident].refl, 1u,
                req->stage_cbuf[1]) < 0) :
            (set_guest_uniforms(NULL, pair->vs_program, "vs", req->stage_cbuf[0]) < 0 ||
             set_guest_uniforms(NULL, pair->fs_program, "fs", req->stage_cbuf[1]) < 0)) {
        gxm_stage9_note(VP_GXM_S9_PRIMARY_UNIFORMS, req->draw_index);
        frame_release_fs(frame_fs, frame_fs_owned);
        return -2;
    }

    for (unsigned e=0;e<VP_GXM_FRAGMENT_SAMPLER_LIMIT-1u;e++) {
        if (static_mode) {
            parameter =
                static_residents[static_pair->fs_resident].refl.tex[e+1u];
            static_stats.reflect_name_formats_avoided++;
        } else {
            char name[8];
            snprintf(name,sizeof(name),"tex%u",e+1u);
            parameter = sceGxmProgramFindParameterByName(pair->fs_program,name);
        }
        if (!parameter) continue;
        const VPGxmExtraTexture *extra=&req->extra_textures[e];
        {
            VPGxmShadowParams shadow_params;
            const char *shadow_reason = NULL;
            int shadow_mode;
            if (static_mode)
                static_shadow_params(
                    &static_residents[static_pair->fs_resident].refl, e + 1u,
                    &shadow_params);
            else
                legacy_shadow_params(pair->fs_program, e + 1u, 0,
                                     &shadow_params);
            shadow_mode = shadow_slot_mode(
                &shadow_params, extra,
                extra->source == VP_GXM_EXTRA_SOURCE_DEPTH, &shadow_reason);
            if (shadow_mode < 0) {
                gxm_shadow_rejects++;
                glog("VP_GXM_SHADOW_REJECT idx=%u stage=fragment slot=%u "
                     "reason=%s source=%u tic_depth=%u desc_depth=%u "
                     "ticfmt=0x%x tic_compare=%u contract=%u va=0x%llx\n",
                     req->draw_index, e + 1u, shadow_reason, extra->source,
                     extra->depth, extra->descriptor_depth, extra->tic_format,
                     (extra->tsc_words[0] >> 9) & 1u, shadow_params.present,
                     (unsigned long long)extra->va);
                rc = gxm_extra_reject(req->draw_index, e + 1u,
                                      "shadow-contract-mismatch", extra,
                                      req->target_va);
                frame_release_fs(frame_fs, frame_fs_owned);
                return rc;
            }
            frame_extra_shadow[e] = shadow_mode;
        }
        if (extra->source == VP_GXM_EXTRA_SOURCE_DEPTH) {
            SceGxmTexture *depth_bind = NULL;
            if (depth_binding.enabled &&
                depth_binding.desc.va == extra->va) {
                rc=gxm_extra_reject(req->draw_index,e+1u,
                                    "feedback-depth-attachment",extra,
                                    req->target_va);
                frame_release_fs(frame_fs, frame_fs_owned);
                return rc;
            }
            rc=depth_texture_view(req->draw_index,e+1u,extra,
                                  (unsigned)frame_extra_shadow[e],
                                  &frame_extra_depth_plan[e],
                                  batch_entry != NULL
                                      ? &batch_entry->extra_depth_storage[e]
                                      : &frame_extra_depth_view[e],
                                  &depth_bind);
            if (rc != 0) {
                char reason[48];
                snprintf(reason,sizeof(reason),"depth-view-%d",rc);
                rc=gxm_extra_reject(req->draw_index,e+1u,reason,extra,
                                    req->target_va);
                frame_release_fs(frame_fs, frame_fs_owned);
                return rc;
            }
            frame_extra_bind[e]=depth_bind;
            frame_extra_producer[e]=NULL;
            frame_extra_depth[e]=1;
            if (batch_entry != NULL) {
                batch_entry->extra_depth_view[e] = 1;
                batch_entry->extra_bound[e] = 1;
            }
            continue;
        }
        if (extra->source == VP_GXM_EXTRA_SOURCE_GPU) {
            /* The producer owns the live A8B8G8R8 surface. Guest memory is
             * not consulted here: a known GPU producer never falls back to a
             * CPU snapshot. Compatibility is decided from the raw TIC fields
             * of the consumer descriptor, not from the upload enum. */
            VPGxmSurface *producer=surface_find(extra->va);
            VPTextureSurfaceView view;
            const char *view_reason;
            if (extra->va == 0u || producer == NULL || !producer->has_color ||
                producer->color_mem == NULL) {
                rc=gxm_extra_reject(req->draw_index,e+1u,
                                    "missing texture producer",extra,
                                    req->target_va);
                frame_release_fs(frame_fs, frame_fs_owned);
                return rc;
            }
            /* The surface table always allocates A8B8G8R8 internally, but the
             * guest target format is a Maxwell RT enum (0xD5 for
             * A8B8G8R8_UNORM), not the TIC format number. Only that reviewed
             * colour target is an accepted producer. */
            if (producer->format != 0xd5u) {
                char reason[48];
                snprintf(reason,sizeof(reason),"producer-format-%u-unsupported",
                         producer->format);
                rc=gxm_extra_reject(req->draw_index,e+1u,reason,extra,
                                    req->target_va);
                frame_release_fs(frame_fs, frame_fs_owned);
                return rc;
            }
            /* Actual prior contents only: a surface allocation alone has no
             * defined pixels. A full colour clear or a successful colour draw
             * proves the producer content. */
            if (!producer->color_drawn &&
                producer->color_clear_mask != 0x3cu) {
                rc=gxm_extra_reject(req->draw_index,e+1u,
                                    "missing texture uninitialized-producer",
                                    extra,req->target_va);
                frame_release_fs(frame_fs, frame_fs_owned);
                return rc;
            }
            memset(&view,0,sizeof(view));
            view.tic_format=extra->tic_format;
            view.tic_word0=extra->swizzle;
            view.header=extra->tic_header;
            view.texture_type=extra->texture_type;
            view.depth=extra->depth;
            view.block_depth=extra->block_depth;
            view.max_mip_level=extra->max_mip_level;
            view.view_width=extra->width;
            view.view_height=extra->height;
            view.surface_width=producer->width;
            view.surface_height=producer->height;
            view_reason=vp_texture_surface_view_check(&view);
            if (view_reason != NULL) {
                char reason[40];
                snprintf(reason,sizeof(reason),"view-%s",view_reason);
                rc=gxm_extra_reject(req->draw_index,e+1u,reason,extra,
                                    req->target_va);
                frame_release_fs(frame_fs, frame_fs_owned);
                return rc;
            }
            /* Sampling must not mutate the producer or another consumer's
             * descriptor: apply this draw's TSC to a local copy. */
            {
                SceGxmTexture *surface_tex = batch_entry != NULL
                    ? &batch_entry->extra_surface_texture[e]
                    : &frame_extra_surface[e].texture;
                *surface_tex = producer->texture;
                rc=frame_apply_extra_sampler(surface_tex,
                                             extra,req->draw_index,e+1u);
                if (rc<0) {
                    rc=gxm_extra_reject(req->draw_index,e+1u,"sampler-unsupported",
                                        extra,req->target_va);
                    frame_release_fs(frame_fs, frame_fs_owned);
                    return rc;
                }
                frame_extra_bind[e]=surface_tex;
            }
            if (batch_entry != NULL) {
                batch_entry->extra_producer[e] = producer;
                batch_entry->extra_bound[e] = 1;
            } else {
                frame_extra_producer[e]=producer;
            }
            continue;
        }
        if (extra->source != VP_GXM_EXTRA_SOURCE_CPU) {
            rc=gxm_extra_reject(req->draw_index,e+1u,"unresolved-descriptor",
                                extra,req->target_va);
            frame_release_fs(frame_fs, frame_fs_owned);
            return rc;
        }
        /* Existing CPU payload path, including cube and upload layouts. */
        if (!extra->texels || !extra->width || !extra->height) {
            rc=gxm_extra_reject(req->draw_index,e+1u,"missing texture cpu-payload",
                                extra,req->target_va);
            frame_release_fs(frame_fs, frame_fs_owned);
            return rc;
        }
        unsigned mip_count=extra->cube ? extra->mip_count : 1u;
        size_t texture_bytes=extra->cube ?
            vp_texture_cube_upload_bytes_mips(extra->width,extra->height,mip_count) :
            vp_texture_upload_bytes(extra->width,extra->height,extra->tex_format);
        if (!texture_bytes || (extra->cube &&
            (!mip_count || mip_count>vp_texture_cube_mip_count(extra->width,extra->height)))) {
            rc=gxm_extra_reject(req->draw_index,e+1u,"layout-invalid",extra,
                                req->target_va);
            frame_release_fs(frame_fs, frame_fs_owned);
            return rc;
        }
        {
            SceGxmTextureFormat fmt;
            if (frame_gxm_texture_format(extra->tex_format,&fmt)<0) {
                rc=gxm_extra_reject(req->draw_index,e+1u,"format-unsupported",extra,
                                    req->target_va);
                frame_release_fs(frame_fs, frame_fs_owned);
                return rc;
            }
            if (batch_entry != NULL) {
                VPTextureSnapshotKey key = gxm_snapshot_key(extra->va,
                    extra->width, extra->height, extra->tex_format, extra->cube,
                    mip_count);
                int direct_eligible = gxm_direct_candidate(
                    extra->texels, extra->texels_owned, extra->width,
                    extra->height, extra->tex_format, extra->cube, mip_count,
                    0, NULL, NULL);
                void *tex_mem = gxm_batch_texture_bind(batch_entry, &key,
                    extra->texels, texture_bytes,
                    extra->texels_owned ? extra->texels : NULL,
                    direct_eligible);
                SceGxmTexture *tex = &batch_entry->extra_texture[e];
                if (tex_mem == NULL) {
                    frame_release_fs(frame_fs, frame_fs_owned);
                    return -1;
                }
                if (!gxm_texture_desc_lookup(tex_mem, extra->width,
                        extra->height, fmt, extra->cube, mip_count,
                        VP_GXM_TEXTURE_DESC_EXTRA, extra->sampler, tex)) {
                    rc=extra->cube ?
                        sceGxmTextureInitCube(tex,tex_mem,fmt,
                            extra->width,extra->height,mip_count) :
                        sceGxmTextureInitLinear(tex,tex_mem,
                            fmt,extra->width,extra->height,1);
                    if (rc<0) {frame_release_fs(frame_fs, frame_fs_owned);return rc;}
                    rc=frame_apply_extra_sampler(tex,extra,
                                                 req->draw_index,e+1u);
                    if (rc<0) {
                        rc=gxm_extra_reject(req->draw_index,e+1u,"sampler-unsupported",
                                            extra,req->target_va);
                        frame_release_fs(frame_fs, frame_fs_owned);
                        return rc;
                    }
                    gxm_texture_desc_store(tex_mem, extra->width, extra->height,
                        fmt, extra->cube, mip_count, VP_GXM_TEXTURE_DESC_EXTRA,
                        extra->sampler, tex);
                }
                frame_extra_bind[e]=tex;
                batch_entry->extra_cpu_texture[e] = 1;
                batch_entry->extra_bound[e] = 1;
                continue;
            }
            if (!gxm_extra_mem[e] || gxm_extra_width[e]!=extra->width ||
                gxm_extra_height[e]!=extra->height || gxm_extra_format[e]!=extra->tex_format ||
                gxm_extra_cube[e]!=extra->cube || gxm_extra_mips[e]!=mip_count ||
                gxm_extra_bytes[e]!=texture_bytes) {
                /* Vita3K caches texture data addresses when a fragment texture is
                 * synchronized. Keep the old backing allocation alive if a
                 * sampler changes shape, and reuse it when it is large enough. */
                if (!gxm_extra_mem[e] || gxm_extra_capacity[e]<texture_bytes) {
                    void *new_mem=gpu_alloc((unsigned)texture_bytes,0,NULL);
                    if (!new_mem) {frame_release_fs(frame_fs, frame_fs_owned);return -1;}
                    gxm_extra_mem[e]=new_mem;
                    gxm_extra_capacity[e]=texture_bytes;
                }
                if (!gxm_extra_mem[e]) {frame_release_fs(frame_fs, frame_fs_owned);return -1;}
                rc=extra->cube ?
                    sceGxmTextureInitCube(&gxm_extra_texture[e],gxm_extra_mem[e],fmt,
                        extra->width,extra->height,mip_count) :
                    sceGxmTextureInitLinear(&gxm_extra_texture[e],gxm_extra_mem[e],
                        fmt,extra->width,extra->height,1);
                if (rc<0) {frame_release_fs(frame_fs, frame_fs_owned);return rc;}
                gxm_extra_width[e]=extra->width;gxm_extra_height[e]=extra->height;
                gxm_extra_format[e]=extra->tex_format;
                gxm_extra_cube[e]=extra->cube;gxm_extra_mips[e]=mip_count;
                gxm_extra_bytes[e]=texture_bytes;
            }
        }
        /* Decoded cube payload already contains every real face and mip in
         * GXM order. The cache decoder validates its full backing range. */
        {
            uint64_t tex_t0 = gxm_prof_now();
            memcpy(gxm_extra_mem[e],extra->texels,texture_bytes);
            gxm_tex_us += gxm_prof_now() - tex_t0;
            gxm_tex_bytes += texture_bytes;
        }
        rc=frame_apply_extra_sampler(&gxm_extra_texture[e],extra,
                                     req->draw_index,e+1u);
        if (rc<0) {
            rc=gxm_extra_reject(req->draw_index,e+1u,"sampler-unsupported",
                                extra,req->target_va);
            frame_release_fs(frame_fs, frame_fs_owned);
            return rc;
        }
        frame_extra_bind[e]=&gxm_extra_texture[e];
    }
    {
        const VPGxmExtraTexture *extra=&req->vertex_texture;
        if (static_mode) {
            parameter =
                static_residents[static_pair->vs_resident].refl.tex[0];
            static_stats.reflect_lookups_avoided++;
        } else {
            parameter=sceGxmProgramFindParameterByName(pair->vs_program,"tex0");
        }
        if (parameter!=NULL) {
            if (extra->source == VP_GXM_EXTRA_SOURCE_DEPTH) {
                if (depth_binding.enabled &&
                    depth_binding.desc.va == extra->va) {
                    rc=gxm_extra_reject(req->draw_index,0u,
                                        "feedback-depth-attachment",extra,
                                        req->target_va);
                    frame_release_fs(frame_fs, frame_fs_owned);
                    return rc;
                }
                rc=depth_texture_view(req->draw_index,0u,extra,
                                      (unsigned)frame_vertex_shadow,
                                      &frame_vertex_depth_plan,
                                      batch_entry != NULL
                                          ? &batch_entry->vertex_depth_storage
                                          : &frame_vertex_depth_view,
                                      &frame_vertex_depth_bind);
                if (rc != 0) {
                    char reason[48];
                    snprintf(reason,sizeof(reason),"depth-view-%d",rc);
                    rc=gxm_extra_reject(req->draw_index,0u,reason,extra,
                                        req->target_va);
                    frame_release_fs(frame_fs, frame_fs_owned);
                    return rc;
                }
                if (batch_entry != NULL) batch_entry->vertex_depth_view = 1;
            } else {
            unsigned mip_count=extra->cube ? extra->mip_count : 1u;
            size_t texture_bytes=extra->cube ?
                vp_texture_cube_upload_bytes_mips(extra->width,extra->height,mip_count) :
                vp_texture_upload_bytes(extra->width,extra->height,extra->tex_format);
            if (!extra->texels || !extra->width || !extra->height || !texture_bytes ||
                (extra->cube && (!mip_count || mip_count>
                    vp_texture_cube_mip_count(extra->width,extra->height)))) {
                glog("VP_GXM_FRAME_DRAW idx=%u missing vertex tex0 hash=0x%x "
                     "va=0x%llx %ux%u fmt=%u cube=%u mips=%u\n",
                     req->draw_index,req->hash,(unsigned long long)extra->va,
                     extra->width,extra->height,extra->tex_format,
                     extra->cube,extra->mip_count);
                gxm_stage9_note(VP_GXM_S9_VERTEX_TEX, req->draw_index);
                frame_release_fs(frame_fs, frame_fs_owned);
                return -2;
            }
            SceGxmTextureFormat fmt;
            if (frame_gxm_texture_format(extra->tex_format,&fmt)<0) {
                glog("VP_GXM_FRAME_DRAW idx=%u unsupported vertex tex0 format "
                     "upload=0x%x va=0x%llx %ux%u\n",req->draw_index,
                     extra->tex_format,(unsigned long long)extra->va,
                     extra->width,extra->height);
                gxm_stage9_note(VP_GXM_S9_VERTEX_TEX, req->draw_index);
                frame_release_fs(frame_fs, frame_fs_owned);
                return -2;
            }
            SceGxmTexture *vtex = batch_entry != NULL
                ? &batch_entry->vertex_texture : &gxm_vertex_texture;
            int vertex_desc_hit = 0;
            const void *vertex_desc_backing = NULL;
            /* The logical extent stays in request/uniform space; GXM sees the
             * physical 2x width of the lossless RG32F split. */
            unsigned desc_width = extra->tex_format==VP_TEXTURE_UPLOAD_RGBA32F_RG32F
                ? extra->width*2u : extra->width;
            if (batch_entry != NULL) {
                VPTextureSnapshotKey key = gxm_snapshot_key(extra->va,
                    extra->width, extra->height, extra->tex_format, extra->cube,
                    mip_count);
                int direct_eligible = gxm_direct_candidate(
                    extra->texels, extra->texels_owned, extra->width,
                    extra->height, extra->tex_format, extra->cube, mip_count,
                    0, NULL, NULL);
                void *tex_mem = gxm_batch_texture_bind(batch_entry, &key,
                    extra->texels, texture_bytes,
                    extra->texels_owned ? extra->texels : NULL,
                    direct_eligible);
                if (tex_mem == NULL) {
                    frame_release_fs(frame_fs, frame_fs_owned);
                    return -1;
                }
                vertex_desc_backing = tex_mem;
                if (!gxm_texture_desc_lookup(tex_mem, desc_width,
                        extra->height, fmt, extra->cube, mip_count,
                        VP_GXM_TEXTURE_DESC_EXTRA, extra->sampler, vtex)) {
                    rc=extra->cube ?
                        sceGxmTextureInitCube(vtex,tex_mem,
                            fmt,extra->width,extra->height,mip_count) :
                        sceGxmTextureInitLinear(vtex,tex_mem,
                            fmt,desc_width,extra->height,1);
                    if (rc<0) {
                        frame_release_fs(frame_fs, frame_fs_owned);
                        return rc;
                    }
                } else vertex_desc_hit = 1;
                batch_entry->vertex_cpu_texture = 1;
            } else {
                if (!gxm_vertex_texture_mem || gxm_vertex_texture_width!=desc_width ||
                    gxm_vertex_texture_height!=extra->height ||
                    gxm_vertex_texture_format!=extra->tex_format ||
                    gxm_vertex_texture_cube!=extra->cube ||
                    gxm_vertex_texture_mips!=mip_count ||
                    gxm_vertex_texture_bytes!=texture_bytes) {
                    if (!gxm_vertex_texture_mem || gxm_vertex_texture_capacity<texture_bytes) {
                        void *new_mem=gpu_alloc((unsigned)texture_bytes,0,NULL);
                        if (!new_mem) {
                            frame_release_fs(frame_fs, frame_fs_owned);
                            return -1;
                        }
                        gxm_vertex_texture_mem=new_mem;
                        gxm_vertex_texture_capacity=texture_bytes;
                    }
                    rc=extra->cube ?
                        sceGxmTextureInitCube(&gxm_vertex_texture,gxm_vertex_texture_mem,
                            fmt,extra->width,extra->height,mip_count) :
                        sceGxmTextureInitLinear(&gxm_vertex_texture,gxm_vertex_texture_mem,
                            fmt,desc_width,extra->height,1);
                    if (rc<0) {
                        frame_release_fs(frame_fs, frame_fs_owned);
                        return rc;
                    }
                    gxm_vertex_texture_width=desc_width;
                    gxm_vertex_texture_height=extra->height;
                    gxm_vertex_texture_format=extra->tex_format;
                    gxm_vertex_texture_cube=extra->cube;
                    gxm_vertex_texture_mips=mip_count;
                    gxm_vertex_texture_bytes=texture_bytes;
                }
                {
                    uint64_t tex_t0 = gxm_prof_now();
                    memcpy(gxm_vertex_texture_mem,extra->texels,texture_bytes);
                    gxm_tex_us += gxm_prof_now() - tex_t0;
                    gxm_tex_bytes += texture_bytes;
                }
            }
            if (!vertex_desc_hit) {
                SceGxmTextureAddrMode u_mode, v_mode;
                if (gxm_shared_prepare_active()) {
                    const VPGxmSamplerPlan *plan = gxm_sampler_plan_for(
                        VP_GXM_SAMPLER_PLAN_MIP, extra->sampler);
                    if (plan == NULL || plan->rc < 0) {
                        glog("VP_GXM_FRAME_DRAW idx=%u unsupported vertex tex0 "
                             "sampler=%08x,%08x\n", req->draw_index,
                             extra->sampler[0], extra->sampler[1]);
                        gxm_stage9_note(VP_GXM_S9_VERTEX_TEX, req->draw_index);
                        frame_release_fs(frame_fs, frame_fs_owned);
                        return -2;
                    }
                    gxm_note_addr_border(plan->raw_u, plan->raw_v,
                                         extra->tsc_words, req->draw_index);
                    sceGxmTextureSetMinFilter(vtex, plan->min_filter
                        ? SCE_GXM_TEXTURE_FILTER_LINEAR
                        : SCE_GXM_TEXTURE_FILTER_POINT);
                    sceGxmTextureSetMagFilter(vtex, plan->mag_filter
                        ? SCE_GXM_TEXTURE_FILTER_LINEAR
                        : SCE_GXM_TEXTURE_FILTER_POINT);
                    /* The lossless RG32F vertex layout cannot carry a mip
                     * chain; that format condition still overrides the guest
                     * mip code exactly as in the legacy path. */
                    sceGxmTextureSetMipFilter(vtex,
                        extra->tex_format != VP_TEXTURE_UPLOAD_RGBA32F_RG32F &&
                        plan->mip_filter ?
                            SCE_GXM_TEXTURE_MIP_FILTER_ENABLED :
                            SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
                    sceGxmTextureSetUAddrMode(vtex,
                        (SceGxmTextureAddrMode)plan->addr_u);
                    sceGxmTextureSetVAddrMode(vtex,
                        (SceGxmTextureAddrMode)plan->addr_v);
                } else {
                    if ((extra->sampler[1]&3u)<1u || (extra->sampler[1]&3u)>2u ||
                        ((extra->sampler[1]>>4)&3u)<1u || ((extra->sampler[1]>>4)&3u)>2u ||
                        gxm_texture_addr_mode(extra->sampler[0],&u_mode)<0 ||
                        gxm_texture_addr_mode(extra->sampler[0]>>3,&v_mode)<0) {
                        glog("VP_GXM_FRAME_DRAW idx=%u unsupported vertex tex0 sampler=%08x,%08x\n",
                             req->draw_index,extra->sampler[0],extra->sampler[1]);
                        gxm_stage9_note(VP_GXM_S9_VERTEX_TEX, req->draw_index);
                        frame_release_fs(frame_fs, frame_fs_owned);
                        return -2;
                    }
                    gxm_note_addr_border(extra->sampler[0]&7u, (extra->sampler[0]>>3)&7u,
                                         extra->tsc_words, req->draw_index);
                    sceGxmTextureSetMinFilter(vtex,
                        ((extra->sampler[1]>>4)&3u)==2u?SCE_GXM_TEXTURE_FILTER_LINEAR:
                                                          SCE_GXM_TEXTURE_FILTER_POINT);
                    sceGxmTextureSetMagFilter(vtex,
                        (extra->sampler[1]&3u)==2u?SCE_GXM_TEXTURE_FILTER_LINEAR:
                                                    SCE_GXM_TEXTURE_FILTER_POINT);
                    sceGxmTextureSetMipFilter(vtex,
                        extra->tex_format!=VP_TEXTURE_UPLOAD_RGBA32F_RG32F &&
                        ((extra->sampler[1]>>6)&3u)!=0u ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED :
                                                           SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
                    sceGxmTextureSetUAddrMode(vtex,u_mode);
                    sceGxmTextureSetVAddrMode(vtex,v_mode);
                }
                if (batch_entry != NULL)
                    gxm_texture_desc_store(vertex_desc_backing, desc_width,
                        extra->height, fmt, extra->cube, mip_count,
                        VP_GXM_TEXTURE_DESC_EXTRA, extra->sampler, vtex);
            }
            vertex_bind_texture = vtex;
            }
        }
    }
    gxm_draw_stage = 10u;
    t_prepared=gxm_prof_now();
    if (batch_entry != NULL && gxm_batch.scene_open &&
        gxm_batch.policy.open) {
        /* The compatible scene and its pinned attachments are already open. */
        t_begin = gxm_prof_now();
        scene_state = 1;
    } else {
        if (depth_binding.enabled) {
            depth_surface = &depth_binding.entry->surface;
            sceGxmDepthStencilSurfaceSetForceLoadMode(depth_surface,
                depth_binding.entry->depth_valid ?
                    SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED :
                    SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
        } else {
            /* Guest zeta disabled: GXM models this with an initialized-disabled
             * surface. No private colour-surface depth is substituted. */
            depth_surface = &gxm_depth_disabled;
        }
        rc = sceGxmBeginScene(gxm_context, 0, render_target, NULL, NULL, NULL,
                              depth_only ? NULL : &target->color, depth_surface);
        if (rc < 0) {
            glog("VP_GXM_FRAME_DRAW idx=%u begin rc=%d depth_only=%d\n",
                 req->draw_index, rc, depth_only);
            frame_release_fs(frame_fs, frame_fs_owned);
            return rc;
        }
        memset(&gxm_draw_state, 0, sizeof(gxm_draw_state));
        if (gxm_clear_fusion_enabled() && !depth_only &&
            target->clear_pending && req->color_mask != 0u) {
            rc = frame_clear_in_scene(target);
            if (rc >= 0) {
                surface_clear_consumed_in_scene(target);
                if (req->color_mask == 0x100u) gxm_clear_fusion_blue++;
                /* The clear changed programs, streams and viewport; the game
                 * draw must re-apply every cached piece of state. */
                memset(&gxm_draw_state, 0, sizeof(gxm_draw_state));
            } else {
                int end_rc = frame_end_scene(gxm_context);
                glog("VP_GXM_CLEAR_FUSION_FAIL idx=%u rc=%d end=%d\n",
                     req->draw_index, rc, end_rc);
                if (end_rc < 0) frame_poison_context(end_rc);
                frame_release_fs(frame_fs, frame_fs_owned);
                return rc;
            }
        }
        t_begin = gxm_prof_now();
        scene_state = 1;
        if (batch_entry != NULL) {
            gxm_batch.scene_open = 1;
            gxm_batch.depth_only = depth_only;
            gxm_batch.target = target;
            gxm_batch.render_target = render_target;
            gxm_batch.depth_entry = depth_binding.enabled ? depth_binding.entry : NULL;
            gxm_batch.depth_desc = depth_binding.desc;
            gxm_batch.depth_enabled = depth_binding.enabled;
            gxm_batch.render_width = render_width;
            gxm_batch.render_height = render_height;
            gxm_batch.scope = req->batch_scope;
            gxm_batch.opened_us = t_begin;
        }
    }
    {
        unsigned vw = req->viewport_width != 0u ? req->viewport_width : render_width;
        unsigned vh = req->viewport_height != 0u ? req->viewport_height : render_height;
        VPRenderExtent native = vp_render_resolution_extent(
            render_width, render_height, gxm_render_resolution_height);
        /* Each axis uses its own native/logical ratio: ceil rounding can make
         * the two ratios differ (for example 160x90 -> 120x68). */
        float x_ratio = (float)native.width / (float)render_width;
        float y_ratio = (float)native.height / (float)render_height;
        /* GXM's viewport y scale is negative for a top-down window, as its own
         * default (0.5*(1+min+max), -0.5*(1+max-min)) and the guest's Maxwell
         * viewport state both are. A positive scale makes the GXM backend put
         * the window one row high and drop the last row of a full-surface draw;
         * the captured reference covers that row. */
        int mode = viewport_mode();
        float y_offset = (float)req->viewport_y + (float)vh * 0.5f;
        float y_scale = -(float)vh * 0.5f;
        /* Guest Maxwell viewport_transform z: window = translate + scale*z.
         * The converted shaders write the guest clip z, so a hardcoded
         * 0.5/0.5 depth range compresses stored depth to [0.5,1] and breaks
         * every shadow-map compare (ref is the guest window depth). */
        float z_offset = 0.5f, z_scale = 0.5f;
        if (req->viewport_z_valid) {
            memcpy(&z_scale, &req->viewport_scale_z, sizeof(z_scale));
            memcpy(&z_offset, &req->viewport_translate_z, sizeof(z_offset));
        }
        if (mode == 0) y_scale = (float)vh * 0.5f;
        else if (mode == 2) { y_offset += 1.0f; y_scale = (float)vh * 0.5f; }
        float viewport[6] = {
            ((float)req->viewport_x + (float)vw * 0.5f) * x_ratio,
            (float)vw * 0.5f * x_ratio,
            y_offset * y_ratio, y_scale * y_ratio, z_offset, z_scale
        };
        if (!gxm_draw_state.valid ||
            memcmp(gxm_draw_state.viewport, viewport, sizeof(viewport)) != 0) {
            sceGxmSetViewport(gxm_context, viewport[0], viewport[1],
                              viewport[2], viewport[3], viewport[4], viewport[5]);
            memcpy(gxm_draw_state.viewport, viewport, sizeof(viewport));
            gxm_state_sets++;
        } else gxm_state_skips++;
    }
    {
        VPRenderExtent native = vp_render_resolution_extent(
            render_width, render_height, gxm_render_resolution_height);
        unsigned clip[5] = {SCE_GXM_REGION_CLIP_NONE, 0, 0,
                            native.width, native.height};
        if (req->scissor_enable != 0u) {
            /* Maxwell upper bounds are exclusive; the helper scales the
             * half-open rectangle before clamping and returns GXM's inclusive
             * endpoints. OUTSIDE maps to glScissor (keep the rectangle);
             * INSIDE disables scissoring and logs an unimplemented warning.
             * An empty guest rectangle stays CLIP_ALL rather than being
             * rounded into coverage. */
            VPRenderScissor scissor = vp_render_resolution_scissor(
                render_width, render_height, native.width, native.height,
                req->scissor_min_x, req->scissor_min_y,
                req->scissor_max_x, req->scissor_max_y);
            clip[0] = scissor.clip_all ? SCE_GXM_REGION_CLIP_ALL
                                       : SCE_GXM_REGION_CLIP_OUTSIDE;
            clip[1] = scissor.min_x;
            clip[2] = scissor.min_y;
            clip[3] = scissor.max_x;
            clip[4] = scissor.max_y;
        }
        if (!gxm_draw_state.valid || memcmp(gxm_draw_state.clip, clip, sizeof(clip)) != 0) {
            sceGxmSetRegionClip(gxm_context, clip[0], clip[1], clip[2], clip[3], clip[4]);
            memcpy(gxm_draw_state.clip, clip, sizeof(clip));
            gxm_state_sets++;
        } else gxm_state_skips++;
    }
    if (!gxm_draw_state.valid || gxm_draw_state.depth_func != depth_func) {
        sceGxmSetFrontDepthFunc(gxm_context, depth_func);
        sceGxmSetBackDepthFunc(gxm_context, depth_func);
        gxm_draw_state.depth_func = depth_func;
        gxm_state_sets += 2u;
    } else gxm_state_skips += 2u;
    if (!gxm_draw_state.valid || gxm_draw_state.bias_factor != req->depth_bias_factor ||
        gxm_draw_state.bias_units != req->depth_bias_units) {
        sceGxmSetFrontDepthBias(gxm_context, req->depth_bias_factor, req->depth_bias_units);
        sceGxmSetBackDepthBias(gxm_context, req->depth_bias_factor, req->depth_bias_units);
        gxm_draw_state.bias_factor = req->depth_bias_factor;
        gxm_draw_state.bias_units = req->depth_bias_units;
        gxm_state_sets += 2u;
    } else gxm_state_skips += 2u;
    if (!gxm_draw_state.valid || gxm_draw_state.depth_write != depth_write) {
        sceGxmSetFrontDepthWriteEnable(gxm_context,
            depth_write != 0u ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
        sceGxmSetBackDepthWriteEnable(gxm_context,
            depth_write != 0u ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
        gxm_draw_state.depth_write = depth_write;
        gxm_state_sets += 2u;
    } else gxm_state_skips += 2u;
    if (!gxm_draw_state.valid) {
        sceGxmSetFrontStencilFunc(gxm_context, SCE_GXM_STENCIL_FUNC_ALWAYS,
                              SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                              SCE_GXM_STENCIL_OP_KEEP, 255, 255);
        sceGxmSetBackStencilFunc(gxm_context, SCE_GXM_STENCIL_FUNC_ALWAYS,
                             SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                             SCE_GXM_STENCIL_OP_KEEP, 255, 255);
    /* Fully masked colour writes never reach here: they are skipped above,
     * because this backend zeroes the surface even with the fragment program
     * disabled. Everything from here on writes colour. */
        sceGxmSetFrontFragmentProgramEnable(gxm_context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
        sceGxmSetBackFragmentProgramEnable(gxm_context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
        gxm_state_sets += 4u;
    } else gxm_state_skips += 4u;
    frame_set_cull(cull);
    if (!gxm_draw_state.valid || gxm_draw_state.vertex_program != pair->vs) {
        sceGxmSetVertexProgram(gxm_context, pair->vs);
        gxm_draw_state.vertex_program = pair->vs;
        gxm_state_sets++;
    } else gxm_state_skips++;
    if (frame_fs_owned) {
        sceGxmSetFragmentProgram(gxm_context, frame_fs);
        gxm_draw_state.fragment_program = NULL;
        gxm_state_sets++;
    } else if (!gxm_draw_state.valid ||
               gxm_draw_state.fragment_program != frame_fs) {
        sceGxmSetFragmentProgram(gxm_context, frame_fs);
        gxm_draw_state.fragment_program = frame_fs;
        gxm_state_sets++;
    } else gxm_state_skips++;
    gxm_draw_state.valid = 1;
    parameter = static_mode ? static_residents[static_pair->vs_resident].refl.tex[0]
                            : sceGxmProgramFindParameterByName(pair->vs_program,"tex0");
    if (parameter!=NULL) {
        SceGxmTexture *vertex_bind = frame_vertex_depth_bind != NULL
                                         ? frame_vertex_depth_bind
                                         : vertex_bind_texture != NULL
                                             ? vertex_bind_texture
                                             : &gxm_vertex_texture;
        rc=sceGxmSetVertexTexture(gxm_context,
            sceGxmProgramParameterGetResourceIndex(parameter),vertex_bind);
        if (rc<0) goto frame_fail;
        if (model_trace)
            glog("VP_GXM_MODEL_BIND idx=%u stage=vertex slot=0 rc=%d resource=%u "
                 "va=0x%llx %ux%u fmt=%u\n",req->draw_index,rc,
                 (unsigned)sceGxmProgramParameterGetResourceIndex(parameter),
                 (unsigned long long)req->vertex_texture.va,
                 req->vertex_texture.width,req->vertex_texture.height,
                 req->vertex_texture.tex_format);
    }

    rc = sceGxmReserveVertexDefaultUniformBuffer(gxm_context, &uniform_buffer);
    if (rc < 0) goto frame_fail;
    rc = static_mode ?
        static_init_vertex_uniforms(uniform_buffer,
                                    &static_residents[static_pair->vs_resident].refl) :
        init_vertex_uniforms(uniform_buffer, pair->vs_program);
    if (rc < 0) goto frame_fail;
    rc = static_mode ?
        static_set_guest_uniforms(uniform_buffer,
            &static_residents[static_pair->vs_resident].refl, 0u, req->stage_cbuf[0]) :
        set_guest_uniforms(uniform_buffer, pair->vs_program, "vs", req->stage_cbuf[0]);
    if (rc < 0) goto frame_fail;
    rc = static_mode ?
        static_set_texture_extent(uniform_buffer,
            &static_residents[static_pair->vs_resident].refl, req, 1) :
        set_texture_extent_uniforms(uniform_buffer,pair->vs_program,req,1);
    if (rc < 0) goto frame_fail;
    parameter = static_mode ? static_residents[static_pair->vs_resident].refl.render_area
                            : sceGxmProgramFindParameterByName(pair->vs_program, "render_area");
    if (parameter != NULL) {
        area[0] = (float)render_width;
        area[1] = (float)render_height;
        area[2] = 0.0f;
        area[3] = 0.0f;
        rc = sceGxmSetUniformDataF(uniform_buffer, parameter, 0, 4, area);
        if (rc < 0) goto frame_fail;
        if (!req->quiet)
            glog("VP_GXM_RENDER_AREA hash=0x%x w=%u h=%u depth_only=%d rc=%d\n",
                 req->hash, render_width, render_height, depth_only, rc);
    } else {
        static unsigned missing_area_logs;
        if (missing_area_logs < 8u) {
            glog("VP_GXM_RENDER_AREA_MISSING hash=0x%x\n", req->hash);
            missing_area_logs++;
        }
    }
    rc = sceGxmReserveFragmentDefaultUniformBuffer(gxm_context, &uniform_buffer);
    if (rc < 0) goto frame_fail;
    memset(uniform_buffer, 0, static_mode ?
        static_residents[static_pair->fs_resident].refl.default_uniform_bytes :
        sceGxmProgramGetDefaultUniformBufferSize(pair->fs_program));
    parameter = static_mode ?
        static_residents[static_pair->fs_resident].refl.fragcoord_scale :
        sceGxmProgramFindParameterByName(pair->fs_program, "vp_fragcoord_scale");
    if (parameter != NULL) {
        VPRenderExtent native = vp_render_resolution_extent(
            render_width, render_height, gxm_render_resolution_height);
        const float scale[4] = {
            (float)render_width / (float)native.width,
            (float)render_height / (float)native.height,
            1.0f, 1.0f
        };
        rc = sceGxmSetUniformDataF(uniform_buffer, parameter, 0, 4, scale);
        if (rc < 0) goto frame_fail;
    }
    rc = static_mode ?
        static_set_guest_uniforms(uniform_buffer,
            &static_residents[static_pair->fs_resident].refl, 1u, req->stage_cbuf[1]) :
        set_guest_uniforms(uniform_buffer, pair->fs_program, "fs", req->stage_cbuf[1]);
    if (rc < 0) goto frame_fail;
    rc = static_mode ?
        static_set_texture_extent(uniform_buffer,
            &static_residents[static_pair->fs_resident].refl, req, 0) :
        set_texture_extent_uniforms(uniform_buffer,pair->fs_program,req,0);
    if (rc < 0) goto frame_fail;
    if (frame_primary_shadow) {
        rc = frame_set_shadow_uniforms(uniform_buffer, pair->fs_program,
                static_mode ? &static_residents[static_pair->fs_resident].refl
                            : NULL,
                0u, &frame_primary_depth_plan, req->draw_index);
        if (rc < 0) {
            glog("VP_GXM_SHADOW_UNIFORM_FAIL idx=%u slot=0 rc=%d\n",
                 req->draw_index, rc);
            goto frame_fail;
        }
    }
    for (unsigned e = 0; e < VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u; ++e) {
        if (!frame_extra_shadow[e]) continue;
        rc = frame_set_shadow_uniforms(uniform_buffer, pair->fs_program,
                static_mode ? &static_residents[static_pair->fs_resident].refl
                            : NULL,
                e + 1u, &frame_extra_depth_plan[e], req->draw_index);
        if (rc < 0) {
            glog("VP_GXM_SHADOW_UNIFORM_FAIL idx=%u slot=%u rc=%d\n",
                 req->draw_index, e + 1u, rc);
            goto frame_fail;
        }
    }
    parameter = static_mode ? static_residents[static_pair->fs_resident].refl.alpha_test
                            : sceGxmProgramFindParameterByName(pair->fs_program, "vp_alpha_test");
    const SceGxmProgramParameter *face_parameter = static_mode ?
        static_residents[static_pair->fs_resident].refl.face_state :
        sceGxmProgramFindParameterByName(pair->fs_program, "vp_face_state");
    if (face_parameter != NULL) {
        if (!req->cull_enable &&
            (req->cull_face == 0x404u || req->cull_face == 0x405u ||
             req->cull_face == 0x408u)) {
            /* The translated shader turns the guest FACE semantic into a
             * uniform predicate: the constant is exact only when culling left
             * one known face. With culling disabled both faces survive, so the
             * draw is split into a back-face pass and a front-face pass, each
             * with the predicate that pass proves. */
            SceGxmCullMode cull_back =
                req->front_face == 0x901u ? SCE_GXM_CULL_CW : SCE_GXM_CULL_CCW;
            face_two_pass = 1;
            face_pass_cull[0] = cull_back == SCE_GXM_CULL_CW
                                    ? SCE_GXM_CULL_CCW : SCE_GXM_CULL_CW;
            face_pass_cull[1] = cull_back;
            static unsigned two_pass_logs;
            if (two_pass_logs < 16u || (two_pass_logs % 256u) == 0u)
                glog("VP_GXM_FACE two-pass hash=0x%x front_face=%u n=%u\n",
                     req->hash, req->front_face, two_pass_logs);
            two_pass_logs++;
        } else if (!req->cull_enable ||
                   (req->cull_face != 0x404u && req->cull_face != 0x405u)) {
            glog("VP_GXM_FACE unsupported hash=0x%x cull=%u face=%u\n",
                 req->hash,req->cull_enable,req->cull_face);
            rc=-2;
            goto frame_fail;
        } else {
            float face_state[4]={req->cull_face==0x405u?1.0f:0.0f,0.0f,0.0f,0.0f};
            rc=sceGxmSetUniformDataF(uniform_buffer,face_parameter,0,4,face_state);
            if (rc<0) goto frame_fail;
        }
    }
    uint32_t alpha_func = req->alpha_test_func;
    if (alpha_func >= 0x200u && alpha_func <= 0x207u) alpha_func -= 0x1ffu;
    if (req->alpha_test_enable &&
        (parameter == NULL || alpha_func < 1u || alpha_func > 8u)) {
        glog("VP_GXM_ALPHA_TEST unsupported hash=0x%x func=%u parameter=%u\n",
             req->hash, req->alpha_test_func, parameter != NULL);
        rc = -2;
        goto frame_fail;
    }
    if (parameter != NULL) {
        float alpha_test[4] = {(float)req->alpha_test_enable,
                              (float)alpha_func, req->alpha_test_ref, 0.0f};
        rc = sceGxmSetUniformDataF(uniform_buffer, parameter, 0, 4, alpha_test);
        if (rc < 0) goto frame_fail;
        if (req->alpha_test_enable) {
            static unsigned alpha_logs;
            if (alpha_logs++ < 8u)
                glog("VP_GXM_ALPHA_TEST_BIND hash=0x%x func=%u ref=%f rc=%d\n",
                     req->hash, alpha_func, req->alpha_test_ref, rc);
        }
    }
    parameter = static_mode ? static_residents[static_pair->fs_resident].refl.tex[0]
                            : sceGxmProgramFindParameterByName(pair->fs_program, "tex0");
    if (parameter != NULL) {
        if (frame_primary_depth_bind != NULL) {
            /* The depth planner already applied the only state this view may
             * have.  Re-applying the raw guest sampler would allow a second
             * filter/wrap (and for software shadow would fight the helper's
             * clamp rule).  The planned native state is point/clamp for shadow
             * and the TSC-derived state for ordinary depth. */
            if (frame_primary_shadow &&
                (frame_primary_depth_plan.min_filter !=
                     VP_GXM_DEPTH_SAMPLER_FILTER_POINT ||
                 frame_primary_depth_plan.mag_filter !=
                     VP_GXM_DEPTH_SAMPLER_FILTER_POINT ||
                 frame_primary_depth_plan.addr_u !=
                     VP_GXM_DEPTH_SAMPLER_ADDR_CLAMP ||
                 frame_primary_depth_plan.addr_v !=
                     VP_GXM_DEPTH_SAMPLER_ADDR_CLAMP)) {
                glog("VP_GXM_SHADOW_NATIVE_STATE idx=%u slot=0 invalid "
                     "filters=%u/%u addr=%u/%u\n", req->draw_index,
                     frame_primary_depth_plan.min_filter,
                     frame_primary_depth_plan.mag_filter,
                     frame_primary_depth_plan.addr_u,
                     frame_primary_depth_plan.addr_v);
                rc = -2;
                goto frame_fail;
            }
        } else if (batch_entry == NULL || !batch_entry->primary_cpu_texture) {
            rc = gxm_apply_primary_sampler(texture, req);
            if (rc < 0) goto frame_fail;
        }
        rc = sceGxmSetFragmentTexture(gxm_context,
                sceGxmProgramParameterGetResourceIndex(parameter), texture);
        if (rc < 0) goto frame_fail;
        if (movie_trace)
            glog("VP_GXM_MOVIE_BIND n=%u slot=0 rc=%d resource=%u\n",
                 (unsigned)(req->draw_index), rc,
                 (unsigned)sceGxmProgramParameterGetResourceIndex(parameter));
        if (model_trace)
            glog("VP_GXM_MODEL_BIND idx=%u slot=0 rc=%d resource=%u va=0x%llx %ux%u fmt=%u\n",
                 req->draw_index, rc,
                 (unsigned)sceGxmProgramParameterGetResourceIndex(parameter),
                 (unsigned long long)req->texture_va, req->tex_width,
                 req->tex_height, req->tex_format);
    }
    if (static_mode) {
        parameter =
            static_residents[static_pair->fs_resident].refl.f_mask;
        static_stats.reflect_lookups_avoided++;
    } else {
        parameter =
            sceGxmProgramFindParameterByName(pair->fs_program, "f_mask");
    }
    for (unsigned e=0;e<VP_GXM_FRAGMENT_SAMPLER_LIMIT-1u;e++) {
        const SceGxmProgramParameter *extra;
        if (static_mode) {
            extra =
                static_residents[static_pair->fs_resident].refl.tex[e+1u];
            static_stats.reflect_name_formats_avoided++;
        } else {
            char name[8];
            snprintf(name,sizeof(name),"tex%u",e+1u);
            extra = sceGxmProgramFindParameterByName(pair->fs_program,name);
        }
        if (extra) {
            SceGxmTexture *chosen=frame_extra_bind[e];
            if (chosen == NULL) {
                glog("VP_GXM_FRAME_DRAW idx=%u tex%u missing prepared source\n",
                     req->draw_index,e+1u);
                rc=-2;
                goto frame_fail;
            }
            rc=sceGxmSetFragmentTexture(gxm_context,sceGxmProgramParameterGetResourceIndex(extra),chosen);
            if (rc<0) goto frame_fail;
            /* Binding is an API event, not proof of consumption. The source
             * counters and success log are emitted after the draw and the
             * EndScene/notification/Finish succeed. */
            if (movie_trace)
                glog("VP_GXM_MOVIE_BIND n=%u slot=%u rc=%d resource=%u "
                     "va=0x%llx %ux%u fmt=%u cube=%u mips=%u source=%s\n",
                     req->draw_index, e + 1u, rc,
                     (unsigned)sceGxmProgramParameterGetResourceIndex(extra),
                     (unsigned long long)req->extra_textures[e].va,
                     req->extra_textures[e].width,
                     req->extra_textures[e].height,
                     req->extra_textures[e].tex_format,
                     req->extra_textures[e].cube,
                     req->extra_textures[e].mip_count,
                     frame_extra_producer[e] != NULL ? "gpu" : "cpu");
            if (model_trace)
                glog("VP_GXM_MODEL_BIND idx=%u slot=%u rc=%d resource=%u "
                     "va=0x%llx %ux%u fmt=%u cube=%u mips=%u backing=%p bytes=%llu "
                     "source=%s\n",
                     req->draw_index, e + 1u, rc,
                     (unsigned)sceGxmProgramParameterGetResourceIndex(extra),
                     (unsigned long long)req->extra_textures[e].va,
                     req->extra_textures[e].width,
                     req->extra_textures[e].height,
                     req->extra_textures[e].tex_format,
                     req->extra_textures[e].cube,
                     req->extra_textures[e].mip_count,
                     frame_extra_producer[e] != NULL ?
                         frame_extra_producer[e]->color_mem : gxm_extra_mem[e],
                     (unsigned long long)(frame_extra_producer[e] != NULL ?
                         (size_t)frame_extra_producer[e]->width *
                         frame_extra_producer[e]->height * 4u : gxm_extra_bytes[e]),
                     frame_extra_producer[e] != NULL ? "gpu" : "cpu");
        }
    }
    if (parameter != NULL) {
        if (gxm_mask_mem == NULL) {
            gxm_mask_mem = gpu_alloc(4u, 0, NULL);
            if (gxm_mask_mem != NULL) {
                *(uint32_t *)gxm_mask_mem = 0xffffffffu;
                sceGxmTextureInitLinear(&gxm_mask_texture, gxm_mask_mem,
                        SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, 1, 1, 1);
            }
        }
        if (gxm_mask_mem != NULL)
            sceGxmSetFragmentTexture(gxm_context,
                    sceGxmProgramParameterGetResourceIndex(parameter), &gxm_mask_texture);
    }
    rc = sceGxmSetVertexStream(gxm_context, 0,
                               vertex_stream != NULL ? vertex_stream
                                                     : gxm_game_vertices);
    if (rc < 0) goto frame_fail;
    if (packed_stream_stride != 0u) {
        rc = sceGxmSetVertexStream(gxm_context, 1,
                                   packed_stream != NULL ? packed_stream
                                                         : gxm_game_vertices_packed);
        if (rc < 0) goto frame_fail;
    }
    gxm_draw_stage = 11u;
    t_encoded=gxm_prof_now();
    for (unsigned face_pass = 0;
         face_pass < (face_two_pass ? 2u : 1u); ++face_pass) {
        if (face_two_pass) {
            if (face_pass != 0u) {
                void *next_uniform_buffer;
                rc = sceGxmReserveFragmentDefaultUniformBuffer(gxm_context,
                                                               &next_uniform_buffer);
                if (rc < 0) goto frame_fail;
                /* Submitted draws still reference the first pass's buffer. */
                memcpy(next_uniform_buffer, uniform_buffer,
                       static_mode ?
                       static_residents[static_pair->fs_resident].refl.default_uniform_bytes :
                       sceGxmProgramGetDefaultUniformBufferSize(pair->fs_program));
                uniform_buffer = next_uniform_buffer;
            }
            float state[4] = {face_pass == 0u ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f};
            rc = sceGxmSetUniformDataF(uniform_buffer, face_parameter, 0, 4,
                                       state);
            if (rc < 0) goto frame_fail;
            frame_set_cull(face_pass_cull[face_pass]);
        }
        /* A restart draw can issue several commands before a later segment
         * fails. Retain its payload until completion even on that failure. */
        if (batch_entry != NULL) batch_entry->direct_consumed = 1u;
        if (strip_joined) {
            rc = index_count ? sceGxmDraw(gxm_context, prim, index_format, index_source, index_count) : 0;
            if (rc >= 0 && index_count) { gxm_strip_draws++; gxm_strip_segments += joined_segments; }
        } else if (req->indexed && effective_restart) {
            unsigned bytes=index_format==SCE_GXM_INDEX_FORMAT_U32?4u:2u;
            uint32_t marker=bytes==4u?0xffffffffu:0xffffu;
            unsigned first=0,segments=0;
            rc=0;
            for (unsigned at=0;at<=index_count;at++) {
                uint32_t value=at==index_count?marker:
                    bytes==4u?((const uint32_t *)index_source)[at]:((const uint16_t *)index_source)[at];
                if (at!=index_count && value!=marker) continue;
                if (at-first>=3u) {
                    rc=sceGxmDraw(gxm_context,prim,index_format,
                        (const uint8_t *)index_source+(size_t)first*bytes,at-first);
                    if (rc<0) break;
                    segments++;
                }
                first=at+1u;
            }
            if (rc>=0 && segments==0u)
                glog("VP_GXM_FRAME_DRAW idx=%u restart_segments=0 action=no-primitive\n",
                     req->draw_index);
        } else rc = sceGxmDraw(gxm_context, prim, index_format, index_source, index_count);
        if (rc < 0) {
            glog("VP_GXM_FRAME_DRAW idx=%u draw rc=%d face_pass=%u\n",
                 req->draw_index, rc, face_pass);
            goto frame_fail;
        }
        /* At least one draw command was issued into this pass. Even a later
         * pass failure therefore makes the direct leases GPU-visible. */
        if (batch_entry != NULL) batch_entry->direct_consumed = 1u;
    }
    gxm_draw_stage = 12u;
    t_drawn=gxm_prof_now();
    if (batch_entry != NULL) {
        unsigned e;
        unsigned decision;
        unsigned planned_attachment = depth_binding.enabled ? 1u : 0u;
        batch_entry->req = *req;
        batch_entry->req.vertices = NULL;
        batch_entry->req.indices = NULL;
        batch_entry->req.texels = NULL;
        for (e = 0; e < VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u; ++e)
            batch_entry->req.extra_textures[e].texels = NULL;
        batch_entry->req.vertex_texture.texels = NULL;
        for (unsigned stage = 0; stage < 2u; ++stage)
            for (unsigned bank = 0; bank < 18u; ++bank) {
                batch_entry->req.stage_cbuf[stage][bank].data = NULL;
                batch_entry->req.stage_cbuf[stage][bank].size = 0u;
            }
        batch_entry->target = target;
        batch_entry->depth_entry = depth_binding.enabled ? depth_binding.entry : NULL;
        batch_entry->depth_enabled = depth_binding.enabled;
        batch_entry->depth_only = depth_only;
        batch_entry->depth_written = depth_binding.enabled && depth_write != 0u;
        batch_entry->commit_mask = vp_gxm_draw_commit_mask(
            depth_only ? 0u : 1u, req->color_mask,
            depth_binding.enabled ? 1u : 0u);
        batch_entry->t_encoded = t_drawn;
        /* Every sceGxmDraw call above succeeded; the direct payloads are now
         * potentially read by the GPU until the scene notification. */
        batch_entry->direct_consumed = 1u;
        decision = vp_batch_policy_admit(
            &gxm_batch.policy, req->batch_scope, depth_only,
            req->target_va, req->target_width, req->target_height,
            req->target_format, planned_attachment,
            depth_binding.desc.va, depth_binding.desc.format,
            depth_binding.desc.width, depth_binding.desc.height,
            batch_reserve, VP_BATCH_MAX_DRAWS, VP_BATCH_STAGING_BUDGET);
        if (decision != VP_BATCH_ACCEPT ||
            vp_batch_policy_push(&gxm_batch.policy, &gxm_batch.stats,
                req->batch_scope, depth_only,
                req->target_va, req->target_width, req->target_height,
                req->target_format, planned_attachment,
                depth_binding.desc.va, depth_binding.desc.format,
                depth_binding.desc.width, depth_binding.desc.height,
                batch_reserve, VP_BATCH_MAX_DRAWS,
                VP_BATCH_STAGING_BUDGET) < 0) {
            glog("VP_GXM_BATCH_PUSH_FAIL idx=%u decision=%u draws=%u "
                 "reserve=%u action=poison\n", req->draw_index, decision,
                 gxm_batch.policy.draws, (unsigned)batch_reserve);
            gxm_scene_poisoned = 1;
            /* The aborted worker releases this draw's leases only when its
             * EndScene/wait proved completion; otherwise it quarantines them
             * through reset_scene_state. */
            (void)gxm_batch_finish_aborted_worker("push-fail", -1);
            return -1;
        }
        gxm_batch.count = gxm_batch.policy.draws;
        gxm_batch_note_pushed_entry(batch_entry);
        gxm_batch_current_entry = NULL;
        static_stats.batch_encoded++;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].draws++;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].prep_us += t_prepared - t_start;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].begin_us += t_begin - t_prepared;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].encode_us += t_encoded - t_begin;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].draw_us += t_drawn - t_encoded;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].setup_us += t_vertex - t_start;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].validate_us += t_validate - t_start;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].pair_us += t_pair - t_validate;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].feedback_us += t_feedback - t_pair;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].admit_us += t_admit - t_feedback;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].resolve_us += t_resolve - t_admit;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].pair_flush_us += pair_flush_us;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].admit_flush_us += admit_flush_us;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].vertex_us += t_indices - t_vertex;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].index_us += t_textures - t_indices;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].texture_us += t_prepared - t_textures;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].nested_flush_us +=
            gxm_batch_perf.total_sum - nested_flush_before;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].nested_pending_us +=
            gxm_pending_wait_us - nested_pending_before;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].nested_notify_us +=
            gxm_notify_wait_us - nested_notify_before;
        gxm_batch_encode_perf[depth_only ? 1u : 0u].nested_finish_us +=
            gxm_finish_us - nested_finish_before;
        req->batch_encoded = 1u;
        if (gxm_batch.flush_logs < 48u || (gxm_batch.policy.draws % 16u) == 0u)
            glog("VP_GXM_BATCH_ENCODE idx=%u hash=0x%x slot=%u draws=%u "
                 "reserve=%u depth_only=%d scope=%llu\n",
                 req->draw_index, req->hash, gxm_batch.count - 1u,
                 gxm_batch.count, (unsigned)batch_reserve, depth_only,
                 (unsigned long long)req->batch_scope);
        return 0;
    }
    {
        gxm_draw_stage = 13u;
        rc = frame_end_scene_timed(gxm_context, "sync-scene",
                                   &t_end, &t_notify, &t_finished);
        if (rc < 0) {
            /* The scene is no longer safely owned: never retry EndScene. */
            scene_state = 2;
            frame_poison_context(rc);
            goto frame_fail;
        }
        scene_state = 0;
    }
    gxm_draw_stage = 14u;
    /* GPU completion of this draw is established here, so a diagnostic
     * readback observes exactly the bytes this draw produced. */
    if (!depth_only) b34_dump_surface("draw", target, req, NULL);
    /* The draw and the scene's EndScene/notification/Finish all succeeded:
     * this is the point where the bound extra sampler content was consumed. */
    for (unsigned e=0;e<VP_GXM_FRAGMENT_SAMPLER_LIMIT-1u;e++) {
        if (frame_extra_bind[e] == NULL) continue;
        if (frame_extra_depth[e]) {
            gxm_depth_view_consumes++;
            if (gxm_depth_view_consumes <= 16u ||
                (gxm_depth_view_consumes % 256u) == 0u)
                glog("VP_GXM_DEPTH_VIEW_CONSUME idx=%u slot=%u source=depth "
                     "va=0x%llx view=%ux%u consumes=%u draws=%u\n",
                     req->draw_index, e + 1u,
                     (unsigned long long)req->extra_textures[e].va,
                     req->extra_textures[e].width,
                     req->extra_textures[e].height,
                     gxm_depth_view_consumes, gxm_depth_view_binds);
            continue;
        }
        if (frame_extra_producer[e] != NULL) {
            frame_extra_producer[e]->sampled_count++;
            gxm_extra_consume_log(req->draw_index,e+1u,&req->extra_textures[e],
                                  frame_extra_producer[e],req->target_va,
                                  &gxm_extra_gpu_binds);
        } else {
            gxm_extra_consume_log(req->draw_index,e+1u,&req->extra_textures[e],
                                  NULL,req->target_va,&gxm_extra_cpu_binds);
        }
    }
    if (frame_primary_depth_bind != NULL &&
        req->texture_source == VP_GXM_EXTRA_SOURCE_DEPTH) {
        gxm_depth_view_consumes++;
        if (gxm_depth_view_consumes <= 16u ||
            (gxm_depth_view_consumes % 256u) == 0u)
            glog("VP_GXM_DEPTH_VIEW_CONSUME idx=%u slot=0 source=depth "
                 "va=0x%llx view=%ux%u consumes=%u draws=%u\n",
                 req->draw_index, (unsigned long long)req->texture_va,
                 req->tex_width, req->tex_height, gxm_depth_view_consumes,
                 gxm_depth_view_binds);
    }
    if (frame_vertex_depth_bind != NULL &&
        req->vertex_texture.source == VP_GXM_EXTRA_SOURCE_DEPTH) {
        gxm_depth_view_consumes++;
        if (gxm_depth_view_consumes <= 16u ||
            (gxm_depth_view_consumes % 256u) == 0u)
            glog("VP_GXM_DEPTH_VIEW_CONSUME idx=%u stage=vertex slot=0 "
                 "source=depth va=0x%llx view=%ux%u consumes=%u draws=%u\n",
                 req->draw_index,
                 (unsigned long long)req->vertex_texture.va,
                 req->vertex_texture.width, req->vertex_texture.height,
                 gxm_depth_view_consumes, gxm_depth_view_binds);
    }
    if (frame_fs != NULL) frame_release_fs(frame_fs, frame_fs_owned);
    commit_mask = vp_gxm_draw_commit_mask(depth_only ? 0u : 1u,
                                          req->color_mask,
                                          depth_binding.enabled ? 1u : 0u);
    if (depth_binding.enabled) {
        /* Scene Begin applied the background (or loaded previous depth) and
         * EndScene/notification/Finish completed synchronously: this identity
         * now has defined depth/stencil content for later ForceLoad passes.
         * Only this post-sync point commits depth_valid/draw_count. */
        if ((commit_mask & VP_GXM_DRAW_COMMIT_DEPTH_VALID) != 0u) {
            /* The BeginScene load action also materialized any pending clear
             * when ForceLoad was disabled; this post-sync point publishes it
             * together with the draw. */
            depth_entry_commit_content(depth_binding.entry, req->draw_index,
                                       "draw-commit");
        }
        if ((commit_mask & VP_GXM_DRAW_COMMIT_DEPTH_DRAW_COUNT) != 0u)
            depth_binding.entry->draw_count++;
        if (depth_binding.desc.format_class == VP_GXM_DEPTH_CLASS_S8D24 &&
            req->depth_state.stencil_enable != 0u) {
            /* Stencil test operations are not implemented (native stencil is
             * fixed to ALWAYS/KEEP). The guest nevertheless believes this
             * draw may have changed stencil, so mark the identity live; a
             * later depth-only clear must then be refused rather than
             * silently reloading stencil from the background. */
            depth_binding.entry->stencil_live = 1u;
            gxm_depth_stats.stencil_unsupported++;
            if (gxm_stencil_unsupported_logs < 16u ||
                (gxm_stencil_unsupported_logs % 512u) == 0u)
                glog("VP_GXM_STENCIL_UNSUPPORTED idx=%u zva=0x%llx "
                     "format=0x%x action=guest-state-not-emulated "
                     "stencil_live=1 n=%u\n", req->draw_index,
                     (unsigned long long)req->depth_state.va,
                     req->depth_state.format, gxm_stencil_unsupported_logs);
            gxm_stencil_unsupported_logs++;
        }
    }
    {
        unsigned kind = depth_only ? 1u : 0u;
        VPGxmPerfKind *perf = &gxm_perf_kinds[kind];
        if (perf->draws == 0u) perf->window_start = t_start;
        perf->draws++;
        gxm_perf_add(&perf->prep, t_prepared - t_start);
        gxm_perf_add(&perf->begin, t_begin - t_prepared);
        gxm_perf_add(&perf->encode, t_encoded - t_begin);
        gxm_perf_add(&perf->draw, t_drawn - t_encoded);
        gxm_perf_add(&perf->end_scene, t_end - t_drawn);
        gxm_perf_add(&perf->notify, t_notify - t_end);
        gxm_perf_add(&perf->finish, t_finished - t_notify);
        gxm_perf_add(&perf->total, t_finished - t_start);
    }
    if (depth_only) {
        static unsigned produce_logs;
        static uint64_t atlas_logged_va;
        char sha_hex[65];
        unsigned n = produce_logs++;
        unsigned vs_stage = static_mode ? static_pair->vs_stage :
                            VP_STATIC_INDEX_STAGE_NONE;
        unsigned fs_stage = static_mode ? static_pair->fs_stage :
                            VP_STATIC_INDEX_STAGE_NONE;
        gxm_stage_sha_hex(fs_stage, sha_hex, sizeof(sha_hex));
        if (n < 16u || (n % 1024u) == 0u)
            glog("VP_GXM_DEPTH_PRODUCE n=%u idx=%u hash=0x%x vs_stage=%u "
                 "fs_stage=%u fs_sha=%s zva=0x%llx fmt=0x%x render=%ux%u "
                 "stale_color=0x%llx %ux%u slot=%d depth_bytes=%llu "
                 "count=%u indexed=%u icount=%u depth_test=%u depth_write=%u "
                 "total_us=%llu end_us=%llu notify_us=%llu finish_us=%llu n=%u\n",
                 n, req->draw_index, req->hash, vs_stage, fs_stage, sha_hex,
                 (unsigned long long)req->depth_state.va,
                 req->depth_state.format, render_width, render_height,
                 (unsigned long long)req->target_va, req->target_width,
                 req->target_height, depth_binding.slot,
                 (unsigned long long)depth_binding.desc.total_alloc_bytes,
                 req->count, req->indexed,
                 req->indexed ? req->index_count : req->count, req->depth_test,
                 depth_write, (unsigned long long)(t_finished - t_start),
                 (unsigned long long)(t_end - t_drawn),
                 (unsigned long long)(t_notify - t_end),
                 (unsigned long long)(t_finished - t_notify), n);
        if (atlas_logged_va != req->depth_state.va &&
            (render_width > 2048u || render_height > 2048u)) {
            atlas_logged_va = req->depth_state.va;
            glog("VP_GXM_DEPTH_ATLAS va=0x%llx fmt=0x%x render=%ux%u "
                 "stride=%u storage_h=%u depth_bytes=%llu alloc_bytes=%llu "
                 "slot=%d action=runtime-bound-draw-issued "
                 "content_proof=none\n",
                 (unsigned long long)req->depth_state.va,
                 req->depth_state.format, render_width, render_height,
                 depth_binding.desc.stride_samples,
                 depth_binding.desc.storage_height,
                 (unsigned long long)depth_binding.desc.depth_bytes,
                 (unsigned long long)depth_binding.desc.total_alloc_bytes,
                 depth_binding.slot);
        }
    }
    if ((commit_mask & VP_GXM_DRAW_COMMIT_COLOR_DRAWN) != 0u)
        target->color_drawn = 1u;
    if ((commit_mask & VP_GXM_DRAW_COMMIT_COLOR_DRAW_COUNT) != 0u)
        target->draw_count++;
    if ((commit_mask & VP_GXM_DRAW_COMMIT_DISPLAY_RECT) != 0u)
        display_rect_add(target, req);
    if ((commit_mask & VP_GXM_DRAW_COMMIT_LAST_TARGET) != 0u) {
        gxm_last_target_va = target->va;
        gxm_last_target_w = target->width;
        gxm_last_target_h = target->height;
    }
    if (target != NULL) {
        after_rgb = req->quiet && !movie_trace && !model_trace ? 0u : surface_nonzero_rgb(target);
        after_alpha = req->quiet && !movie_trace && !model_trace ? 0u : surface_nonzero_alpha(target);
    }
    if (target != NULL && gxm_creation_dump >= 0) {
        creation_dump_bytes((unsigned)gxm_creation_dump,"target",target->color_mem,
                           (size_t)target->render_width*target->render_height*4u);
    }
    if (target != NULL && draw_dump_enabled() && gxm_draw_dumps < 48u && target->color_mem != NULL) {
        char path[96];
        char vhex[97];
        unsigned vbytes, vn, vi;
        FILE *df;
        vbytes = req->count * req->stride;
        vn = vbytes < 48u ? vbytes : 48u;
        for (vi = 0; vi < vn; vi++)
            snprintf(vhex + vi * 2u, 3, "%02x",
                     ((const uint8_t *)req->vertices)[vi]);
        vhex[vn * 2u] = 0;
        snprintf(path, sizeof(path),
                 "ux0:data/mhgu-vita-port/m3-draw-%u.rgba", req->draw_index);
        df = fopen(path, "wb");
        if (df != NULL) {
            fwrite(target->color_mem, 4u, (size_t)target->render_width * target->render_height, df);
            fclose(df);
            if (gxm_draw_dump_manifest != NULL) {
                fprintf(gxm_draw_dump_manifest,
                        "{\"idx\":%u,\"hash\":\"0x%x\",\"target\":\"0x%llx\","
                        "\"width\":%u,\"height\":%u,\"file\":\"m3-draw-%u.rgba\","
                        "\"stride\":%u,\"count\":%u,\"vertex_prefix\":\"%s\","
                        "\"rgb\":%u,\"alpha\":%u}\n",
                        req->draw_index, req->hash, (unsigned long long)target->va,
                        target->render_width, target->render_height, req->draw_index,
                        req->stride, req->count, vhex,
                        after_rgb, after_alpha);
                fflush(gxm_draw_dump_manifest);
            }
            glog("VP_GXM_DRAWDUMP idx=%u hash=0x%x file=m3-draw-%u.rgba %ux%u\n",
                 req->draw_index, req->hash, req->draw_index,
                 target->width, target->height);
            gxm_draw_dumps++;
        }
    }
    if (!req->quiet || movie_trace || model_trace) {
        if (depth_only)
            glog("VP_GXM_FRAME_DRAW rc=0 idx=%u hash=0x%x target=none "
                 "render=%ux%u stale_color=0x%llx %ux%u count=%u "
                 "window_first=%u base=%d indexed=%u icount=%u tex=0x%llx mask=0x%x "
                 "viewport=%u,%u %ux%u depth=%s:0x%llx slot=%d valid=%u "
                 "total_us=%llu\n",
                 req->draw_index, req->hash, render_width, render_height,
                 (unsigned long long)req->target_va, req->target_width,
                 req->target_height, req->count, req->index_window_first,
                 req->base_vertex, req->indexed,
                 req->indexed ? req->index_count : req->count,
                 (unsigned long long)req->texture_va, req->color_mask,
                 req->viewport_x, req->viewport_y, req->viewport_width,
                 req->viewport_height, depth_binding.enabled ? "on" : "off",
                 (unsigned long long)req->depth_state.va, depth_binding.slot,
                 depth_binding.enabled ? depth_binding.entry->depth_valid : 0u,
                 (unsigned long long)(t_finished - t_start));
        else
            glog("VP_GXM_FRAME_DRAW rc=0 idx=%u hash=0x%x target=0x%llx %ux%u count=%u "
                 "window_first=%u base=%d indexed=%u icount=%u tex=0x%llx mask=0x%x "
                 "viewport=%u,%u %ux%u depth=%s:0x%llx slot=%d valid=%u "
                 "rgb=%u->%u alpha=%u->%u\n",
                 req->draw_index, req->hash, (unsigned long long)target->va,
                 target->width, target->height, req->count, req->index_window_first,
                 req->base_vertex, req->indexed,
                 req->indexed ? req->index_count : req->count,
                 (unsigned long long)req->texture_va, req->color_mask,
                 req->viewport_x, req->viewport_y, req->viewport_width, req->viewport_height,
                 depth_binding.enabled ? "on" : "off",
                 (unsigned long long)req->depth_state.va, depth_binding.slot,
                 depth_binding.enabled ? depth_binding.entry->depth_valid : 0u,
                 before_rgb, after_rgb, before_alpha, after_alpha);
    }
    return 0;

frame_fail:
    glog("VP_GXM_FRAME_DRAW idx=%u fail rc=%d\n", req->draw_index, rc);
    {
        int draw_rc = rc;
        int fs_deferred = 0;
        if (batch_entry != NULL) {
            int batch_rc = 0;
            if (gxm_batch.scene_open)
                batch_rc = gxm_batch_finish_aborted_worker("frame-fail", draw_rc);
            /* The aborted worker owns the current entry's rollback: release
             * after a proven completion, quarantine otherwise. */
            if (batch_rc < 0 && draw_rc >= 0) draw_rc = batch_rc;
            frame_release_fs(frame_fs, frame_fs_owned);
            return draw_rc;
        }
        if (scene_state == 1) {
            int cleanup_rc = frame_end_scene(gxm_context);
            if (cleanup_rc < 0) {
                frame_poison_context(cleanup_rc);
                fs_deferred = 1;
                glog("VP_GXM_FRAME_DRAW idx=%u scene_unwind rc=%d draw_rc=%d "
                     "context_failed=1\n",
                     req->draw_index, cleanup_rc, draw_rc);
            } else {
                glog("VP_GXM_FRAME_DRAW idx=%u scene_unwind rc=0 draw_rc=%d\n",
                     req->draw_index, draw_rc);
            }
        } else if (scene_state == 2) {
            fs_deferred = 1;
            glog("VP_GXM_FRAME_DRAW idx=%u scene_unwind skipped=end_or_sync_failed "
                 "draw_rc=%d context_failed=1\n", req->draw_index, draw_rc);
        }
        if (fs_deferred && frame_fs_owned && frame_fs != NULL)
            glog("VP_GXM_FRAME_DRAW idx=%u fs_release=deferred "
                 "reason=scene_sync_failed\n", req->draw_index);
        else
            frame_release_fs(frame_fs, frame_fs_owned);
        return draw_rc;
    }
}

static int frame_draw_internal(VPGxmDrawRequest *req) {
    int rc;
    uint64_t fc = vp_gxm_frame_cost_category_begin(VP_FRAME_COST_GXM_DRAW);
    /* Owned-source associations persist across draws; the decoder retires them
     * at the actual rewrite/free event (vp_gxm_texture_owned_retire). */
    gxm_draw_trace(req);
    gxm_vtx_dump(req);
    rc = frame_draw_internal_body(req);
    if (rc != 0) {
        unsigned stage = gxm_draw_stage < VP_GXM_DRAW_STAGES
                             ? gxm_draw_stage : VP_GXM_DRAW_STAGES - 1u;
        gxm_draw_refuse_stage[stage]++;
        gxm_draw_refuse_total++;
        if (gxm_draw_refuse_logs < 64u || (gxm_draw_refuse_logs % 128u) == 0u)
            glog("VP_GXM_DRAW_REFUSE idx=%u hash=0x%x vs=0x%x fs=0x%x "
                 "stage=%u rc=%d n=%u\n", req->draw_index, req->hash,
                 req->vs_token, req->fs_token, stage, rc,
                 gxm_draw_refuse_logs);
        gxm_draw_refuse_logs++;
        gxm_refuse_hash_note(req->hash, stage, req->draw_index, rc);
        /* Some validation/resource refusals return directly from deep inside
         * the preparation path. Roll back only this draw's reserved staging;
         * earlier pending draws stay in the scene and are committed later. */
        if (gxm_batch_current_entry != NULL) {
            gxm_batch_discard_current_decision(
                gxm_batch_current_entry->direct_consumed, 0);
            req->batch_fallback = 1u;
            static_stats.batch_sync_fallbacks++;
        }
        if (gxm_batch.scene_open && !gxm_batch.policy.open) {
            /* The failed draw opened an otherwise empty scene. Close it now so
             * the next draw cannot accidentally reuse an unpinned attachment. */
            (void)gxm_batch_flush_worker(GXM_BATCH_REASON_FAILURE);
        }
    }
    vp_gxm_frame_cost_category_end(VP_FRAME_COST_GXM_DRAW, fc);
    return rc;
}

/* Pure mode-6 admission predicate. All inputs are explicit so the host tests
 * exercise the same decision the worker makes. Every false return keeps the
 * original synchronous clear path. A surface that does not already exist with
 * the same VA, size and format, or whose colour backing is missing, is never
 * admitted. */
/* VP_HOST_TEST_EXTRACT:clear_defer_admit_begin */
static int clear_defer_admit(const VPGxmClearRequest *req,
                             unsigned surface_width, unsigned surface_height,
                             uint32_t surface_format, int color_mem_present,
                             int surface_pending, int defer_enabled,
                             int worker_ready, int scene_poisoned,
                             int test_active, int capture_active,
                             int pipeline_active, int fusion_enabled,
                             int dump_due) {
    VPClearFusionPlan plan;
    if (!defer_enabled || !worker_ready || scene_poisoned || test_active ||
        capture_active || !pipeline_active)
        return 0;
    /* Full RGBA in the low six mask bits, no depth/stencil bit and no
     * unexpected caller bits. Combined depth clears are out of scope. */
    if ((req->mask & ~VP_CLEAR_FUSION_COLOR_MASK) != 0u) return 0;
    if (!vp_clear_fusion_full_colour(req->mask)) return 0;
    if (!color_mem_present) return 0;
    if (surface_width != req->width || surface_height != req->height ||
        surface_format != req->format)
        return 0;
    plan = vp_clear_fusion_plan(1, fusion_enabled, color_mem_present,
                                surface_pending, req->mask, dump_due, 0u);
    return plan.defer_colour;
}
/* VP_HOST_TEST_EXTRACT:clear_defer_admit_end */

/* Shared record step for both deferred-clear paths. The latest full colour
 * wins; `async` records whether a scene was submitted without a completion
 * proof at this boundary. An older async tag is never dropped by a later
 * record: only a proven materialization or an in-scene GPU clear consumes it,
 * so a mode switch or a coalescing clear cannot weaken the drain requirement. */
/* VP_HOST_TEST_EXTRACT:clear_defer_record_begin */
static void surface_clear_defer_record(VPGxmSurface *s, uint32_t color,
                                       unsigned async) {
    s->clear_pending = 1u;
    s->clear_pending_color = color;
    s->clear_pending_async = s->clear_pending_async || async;
    gxm_clear_fusion_deferred++;
}
/* VP_HOST_TEST_EXTRACT:clear_defer_record_end */

/* Worker lookup for the fast path. The surface metadata is read before any
 * scene is submitted; a missing/mismatched surface or any diagnostic gate
 * keeps the original path. */
static int clear_defer_candidate(const VPGxmClearRequest *req,
                                 VPGxmSurface **out) {
    VPGxmSurface *s = NULL;
    int enabled = gxm_clear_defer_enabled();
    *out = NULL;
    if (!enabled) return 0;
    gxm_clear_defer_tries++;
    s = surface_find(req->target_va);
    if (!clear_defer_admit(req,
                           s != NULL ? s->width : 0u,
                           s != NULL ? s->height : 0u,
                           s != NULL ? s->format : 0u,
                           s != NULL && s->color_mem != NULL,
                           s != NULL && s->clear_pending != 0u,
                           enabled, gxm_ready, gxm_scene_poisoned,
                           gxm_batch_test_active(),
                           b34_dump_surface_capture_active(),
                           gxm_scene_pipeline_active(),
                           gxm_clear_fusion_enabled(), b34_dump_due())) {
        gxm_clear_defer_fallbacks++;
        return 0;
    }
    *out = s;
    return 1;
}

/* Submit the open scene through the dedicated non-synchronous boundary, then
 * record the admitted clear. The flush may fail; the request then keeps the
 * original error and no clear state changes. */
/* VP_HOST_TEST_EXTRACT:clear_defer_submit_begin */
static int clear_defer_submit_and_record(VPGxmClearRequest *req,
                                         VPGxmSurface *s, int scene_open) {
    int rc;
    if (scene_open) {
        rc = gxm_batch_flush_worker(GXM_BATCH_REASON_CLEAR_DEFER);
        if (rc < 0) {
            gxm_clear_defer_submit_failures++;
            return rc;
        }
        gxm_clear_defer_scene_submits++;
    }
    if (s->clear_pending) gxm_clear_defer_coalesced++;
    surface_clear_defer_record(s, req->color, 1u);
    gxm_clear_defer_accepts++;
    return 0;
}
/* VP_HOST_TEST_EXTRACT:clear_defer_submit_end */

static int frame_clear_internal_impl(VPGxmClearRequest *req, int allow_defer) {
    static unsigned clear_disabled_logs;
    VPGxmSurface *s;
    VPGxmSurface *defer_surface = NULL;
    int wait_rc;

    /* Mode 6 keeps the whole original clear machinery but, for a strictly
     * admitted full-colour clear of an already matching surface, submits the
     * open scene through the non-synchronous CLEAR_DEFER boundary and records
     * only the pending colour. Every other request keeps the synchronous
     * flush + pending drain below. */
    if (allow_defer && clear_defer_candidate(req, &defer_surface))
        return clear_defer_submit_and_record(req, defer_surface,
                                             gxm_batch.scene_open);
    /* All surface mutation runs on this worker. Admission above found an
     * existing, matching colour-only surface; no allocation or depth work is
     * needed on that branch. Every other clear keeps the original boundary. */
    if (gxm_batch.scene_open) {
        wait_rc = gxm_batch_flush_worker(GXM_BATCH_REASON_CLEAR);
        if (wait_rc < 0) return wait_rc;
    }
    wait_rc = gxm_pending_scene_complete("clear");
    if (wait_rc < 0) return wait_rc;
    VPGxmDepthBinding depth_binding;
    uint32_t depth_mask;
    unsigned depth_apply_mask = 0u;
    unsigned stencil_coupled = 0u;
    unsigned stencil_live = 0u;
    unsigned depth_fit_width = 0u, depth_fit_height = 0u;
    int depth_status, rc;
    VPClearFusionPlan clear_plan;

    memset(&depth_binding, 0, sizeof(depth_binding));
    depth_binding.slot = -1;
    if (gxm_scene_poisoned) return frame_context_reject("frame-clear");
    if (!gxm_ready) return -1;
    depth_mask = req->mask & 3u;
    if ((depth_mask & 2u) != 0u && (depth_mask & 1u) == 0u) {
        /* Refuse before metadata validation, pool mutation, or colour effects.
         * Native ForceLoad couples depth and stencil, so no independent
         * stencil-only clear exists even when no zeta surface is bound. */
        gxm_depth_stats.clear_refuse_stencil++;
        depth_log_reject("clear", VP_GXM_DEPTH_CLEAR_ERR_STANDALONE, 0u,
                         req->target_va, req->width, req->height, 0u, 0u,
                         &req->depth_state);
        return -2;
    }
    if (depth_mask != 0u) {
        depth_status = vp_gxm_depth_desc_build(&depth_binding.desc,
                req->depth_state.enable, req->depth_state.va,
                req->depth_state.width, req->depth_state.height,
                req->depth_state.depth, req->depth_state.dim_control,
                req->depth_state.format, req->depth_state.tile_mode,
                req->depth_state.array_pitch);
        if (depth_status == VP_GXM_DEPTH_DISABLED) {
            /* Guest depth is disabled; the depth/stencil clear has no bound
             * target and therefore no hardware effect. The colour clear still
             * proceeds. No private colour-surface depth is touched. */
            if (depth_pool_ensure() < 0) return -1;
            gxm_depth_stats.disabled++;
            if (clear_disabled_logs < 16u ||
                (clear_disabled_logs % 1024u) == 0u)
                glog("VP_GXM_DEPTH_CLEAR_DISABLED va=0x%llx %ux%u mask=0x%x "
                     "zen=%u zva=0x%llx action=no-effect n=%u\n",
                     (unsigned long long)req->target_va, req->width,
                     req->height, req->mask, req->depth_state.enable,
                     (unsigned long long)req->depth_state.va,
                     clear_disabled_logs);
            clear_disabled_logs++;
        } else if (depth_status != VP_GXM_DEPTH_OK) {
            gxm_depth_stats.reject_meta++;
            depth_log_reject("clear", depth_status, 0u, req->target_va,
                             req->width, req->height, 0u, 0u,
                             &req->depth_state);
            return -2;
        } else {
            /* Decide the representable clear before any pool mutation or
             * colour effect. A stencil-only clear has no independent native
             * load control, and a depth-only clear cannot preserve live
             * stencil because ForceLoad couples both attachments. */
            if (depth_mask == 1u)
                stencil_live = depth_live_stencil_for(&depth_binding.desc);
            rc = vp_gxm_depth_clear_target_plan(&depth_binding.desc,
                    req->width, req->height, depth_mask, stencil_live,
                    &depth_fit_width, &depth_fit_height,
                    &depth_apply_mask, &stencil_coupled);
            if (rc != VP_GXM_DEPTH_OK) {
                if (rc == VP_GXM_DEPTH_CLEAR_ERR_STANDALONE ||
                    rc == VP_GXM_DEPTH_CLEAR_ERR_PRESERVE ||
                    rc == VP_GXM_DEPTH_ERR_STENCIL)
                    gxm_depth_stats.clear_refuse_stencil++;
                else
                    gxm_depth_stats.reject_meta++;
                depth_log_reject("clear", rc, 0u, req->target_va, req->width,
                                 req->height, 0u, 0u, &req->depth_state);
                return -2;
            }
            rc = depth_binding_acquire(&depth_binding, &req->depth_state,
                    req->width, req->height,
                    depth_fit_width, depth_fit_height,
                    0u, req->target_va, 0u, 0u, 0u, "clear");
            if (rc < 0) return rc;
        }
    }
    s = surface_get(req->target_va, req->width, req->height, req->format, "clear");
    if (s == NULL) return -1;
    /* Every depth/stencil refusal above returned before this point; only a
     * representable clear may now mutate colour. The plan keeps the original
     * ordering: an older pending full clear is materialized before any later
     * colour write that cannot coalesce with it, while a repeated full clear
     * replaces its value. The depth/stencil bookkeeping runs once in both the
     * deferred and the standalone case. */
    clear_plan = vp_clear_fusion_plan(1, gxm_clear_fusion_enabled(),
            s->color_mem != NULL, s->clear_pending != 0u, req->mask,
            b34_dump_due(), depth_apply_mask);
    if (clear_plan.materialize_pending) {
        rc = surface_clear_materialize(s, "clear-before");
        if (rc < 0) return rc;
    }
    if (clear_plan.defer_colour) {
        /* The next colour-writing draw of this target encodes the full clear as
         * its first in-scene draw. Sampling, present, readback and finish
         * materialize it. A depth/stencil clear in the same request is not
         * deferred and is recorded below. */
        surface_clear_defer_record(s, req->color, 0u);
        if (clear_plan.apply_depth) gxm_clear_fusion_combined++;
        if (gxm_clear_fusion_deferred <= 16u ||
            (gxm_clear_fusion_deferred % 256u) == 0u)
            glog("VP_GXM_CLEAR_DEFER va=0x%llx color=0x%08x depth=0x%x "
                 "n=%llu\n", (unsigned long long)s->va, req->color, depth_mask,
                 (unsigned long long)gxm_clear_fusion_deferred);
        rc = 0;
    } else {
        rc = surface_clear(s, req->mask, req->color);
        if (rc < 0) return rc;
    }
    if (depth_binding.enabled && clear_plan.apply_depth) {
        depth_entry_clear(depth_binding.entry, depth_apply_mask,
                          req->depth_clear, req->stencil_clear);
        if (stencil_coupled) {
            gxm_depth_stats.clear_coupled_stencil++;
            if (gxm_depth_stats.clear_coupled_stencil < 16u ||
                (gxm_depth_stats.clear_coupled_stencil % 1024u) == 0u)
                glog("VP_GXM_DEPTH_CLEAR_COUPLED va=0x%llx zva=0x%llx "
                     "slot=%d mask=0x%x stencil_live=0 "
                     "action=stencil-reloaded-from-unchanged-background n=%u\n",
                     (unsigned long long)req->target_va,
                     (unsigned long long)req->depth_state.va,
                     depth_binding.slot, depth_mask,
                     gxm_depth_stats.clear_coupled_stencil);
        }
    }
    if (!clear_plan.defer_colour) {
        if (!req->quiet)
            glog("VP_GXM_FRAME_CLEAR va=0x%llx %ux%u mask=0x%x color=0x%08x "
                 "depth=%s:0x%llx slot=%d apply=0x%x stencil_live=%u "
                 "stencil_coupled=%u fit=%ux%u rc=%d\n",
                 (unsigned long long)req->target_va, req->width, req->height,
                 req->mask, req->color, depth_binding.enabled ? "on" : "off",
                 (unsigned long long)req->depth_state.va, depth_binding.slot,
                 depth_apply_mask, stencil_live, stencil_coupled,
                 depth_fit_width, depth_fit_height, rc);
        if (rc == 0) b34_dump_surface("clear", s, NULL, req);
    }
    return rc;
}

/* Original clear entry. The self test and any non-worker caller never admit
 * the mode-6 asynchronous record; only the worker dispatch below passes
 * allow_defer. */
static int frame_clear_internal(VPGxmClearRequest *req) {
    uint64_t fc = vp_gxm_frame_cost_category_begin(VP_FRAME_COST_GXM_CLEAR);
    int rc = frame_clear_internal_impl(req, 0);
    vp_gxm_frame_cost_category_end(VP_FRAME_COST_GXM_CLEAR, fc);
    return rc;
}

static int frame_finish_internal(void) {
    VPGxmSurface *s;
    SceDisplayFrameBuf display;
    int rc;

    rc = gxm_pending_scene_complete("frame-finish");
    if (rc < 0) return rc;

    if (gxm_scene_poisoned) return frame_context_reject("frame-finish");
    if (!gxm_ready || gxm_last_target_va == 0u) return -1;
    s = surface_find(gxm_last_target_va);
    if (s == NULL || s->color_mem == NULL) return -1;
    if (gxm_clear_fusion_enabled() && s->clear_pending) {
        rc = surface_clear_materialize(s, "finish");
        if (rc < 0) return rc;
    }
    if (gxm_clear_fusion_enabled() && gxm_frame_dumps == 0u) {
        for (unsigned i = 0u; i < VP_GXM_SURFACE_LIMIT; ++i) {
            if (!gxm_surfaces[i].used || !gxm_surfaces[i].clear_pending) continue;
            rc = surface_clear_materialize(&gxm_surfaces[i], "finish-dump");
            if (rc < 0) return rc;
        }
    }
    if (gxm_present == NULL) {
        gxm_present = gpu_alloc_purpose(VP_GXM_BYTES, 0, NULL, 1);
        if (gxm_present == NULL) return -1;
    }
    if (gxm_frame_dumps == 0u) {
        FILE *f;
        unsigned pass = 0;
        FILE *manifest = fopen("ux0:data/mhgu-vita-port/m2-passes.json", "wb");
        if (manifest != NULL) fprintf(manifest, "{\"surfaces\":[\n");
        for (unsigned i = 0; i < VP_GXM_SURFACE_LIMIT; i++) {
            VPGxmSurface *p = &gxm_surfaces[i];
            char path[96];
            FILE *pass_file;
            if (!p->used || p->color_mem == NULL) continue;
            snprintf(path, sizeof(path),
                     "ux0:data/mhgu-vita-port/m2-pass%u.rgba", pass);
            pass_file = fopen(path, "wb");
            if (pass_file != NULL) {
                fwrite(p->color_mem, 4u, (size_t)p->render_width * p->render_height, pass_file);
                fclose(pass_file);
            }
            if (manifest != NULL) {
                fprintf(manifest,
                        "%s{\"file\":\"m2-pass%u.rgba\",\"va\":\"0x%llx\","
                        "\"width\":%u,\"height\":%u,\"draws\":%u,\"samples\":%u,"
                        "\"clears\":%u}",
                        pass == 0u ? "" : ",\n", pass,
                        (unsigned long long)p->va, p->render_width, p->render_height,
                        p->draw_count, p->sampled_count, p->cleared_count);
            }
            glog("VP_GXM_FRAME_PASS file=m2-pass%u.rgba va=0x%llx %ux%u draws=%u "
                 "samples=%u clears=%u\n", pass, (unsigned long long)p->va,
                 p->width, p->height, p->draw_count, p->sampled_count,
                 p->cleared_count);
            pass++;
        }
        if (manifest != NULL) { fprintf(manifest, "\n]}\n"); fclose(manifest); }
        gxm_frame_passes = pass;
        f = fopen("ux0:data/mhgu-vita-port/m2-frame-rt.rgba", "wb");
        if (f != NULL) {
            fwrite(s->color_mem, 4u, (size_t)s->render_width * s->render_height, f);
            fclose(f);
        }
        f = fopen("ux0:data/mhgu-vita-port/m2-frame-meta.json", "wb");
        if (f != NULL) {
            fprintf(f, "{\"width\":%u,\"height\":%u,\"va\":\"0x%llx\","
                       "\"passes\":%u,\"surface_limit\":%u}\n",
                    s->render_width, s->render_height, (unsigned long long)s->va,
                    gxm_frame_passes, VP_GXM_SURFACE_LIMIT);
            fclose(f);
        }
        gxm_frame_dumps++;
    }
    for (unsigned y = 0; y < VP_GXM_HEIGHT; y++) {
        unsigned sy = y * s->render_height / VP_GXM_HEIGHT;
        for (unsigned x = 0; x < VP_GXM_WIDTH; x++) {
            unsigned sx = x * s->render_width / VP_GXM_WIDTH;
            ((uint32_t *)gxm_present)[y * VP_GXM_WIDTH + x] =
                ((const uint32_t *)s->color_mem)[sy * s->render_width + sx];
        }
    }
    memset(&display, 0, sizeof(display));
    display.size = sizeof(display);
    display.base = gxm_present;
    display.pitch = VP_GXM_PITCH;
    display.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    display.width = VP_GXM_WIDTH;
    display.height = VP_GXM_HEIGHT;
    rc = sceDisplaySetFrameBuf(&display, SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (rc < 0) return rc;
    glog("VP_GXM_FRAME_FINISH va=0x%llx %ux%u draws=%u\n",
         (unsigned long long)s->va, s->width, s->height, s->draw_count);
    if (gxm_extra_summary_logs++ < 16u)
        glog("VP_GXM_EXTRA_TEX_SUMMARY consumed_gpu=%u consumed_cpu=%u "
             "depth_views=%u depth_consumes=%u depth_reject=%u reject=%u "
             "feedback=%u missing=%u uninitialized=%u view=%u format=%u "
             "sampler=%u layout=%u\n", gxm_extra_gpu_binds, gxm_extra_cpu_binds,
             gxm_depth_view_binds, gxm_depth_view_consumes,
             gxm_depth_view_rejects, gxm_extra_rejects,
             gxm_extra_reject_feedback, gxm_extra_reject_missing,
             gxm_extra_reject_uninitialized, gxm_extra_reject_view,
             gxm_extra_reject_format, gxm_extra_reject_sampler,
             gxm_extra_reject_layout);
    return 0;
}

int vp_gxm_frame_clear(VPGxmClearRequest *request);
int vp_gxm_frame_finish(void);

/* Display bridge: two host buffers are alternated so the buffer being scanned
 * out is never overwritten. The source is the linear GXM colour surface, not
 * the guest's block-linear render target. Only the rectangle written by
 * displayed draws since the previous present is copied and scaled to the Vita
 * display. A surface with no new displayed draw is a miss (-3), never a
 * repeated frame. */
static void *gxm_display_buf[2];
static unsigned gxm_display_index;
static unsigned gxm_present_count;
static unsigned gxm_present_dumps;
static int gxm_gpu_present=-1;
static int gxm_gpu_present_bottom_origin;
/* Standalone blit payload. frame_blit_gpu waits its own scene to completion
 * before returning, so these bytes are never live across a submitted scene.
 * In-scene fused clears use separate per-arena buffers below. */
static void *gxm_blit_vertices,*gxm_blit_indices;

/* Present through GXM, preserving the already-rendered surface on the GPU.
 * The reviewed f9d pair is a float4 xy/uv passthrough + texture sample with
 * two unity colour gains. This scales the actual VI rectangle; no guest draw
 * or resource is substituted. Double-buffered scanout remains GPU-owned. */
static int frame_blit_gpu(VPGxmSurface *s,void *dst,unsigned x0,unsigned y0,
                          unsigned w,unsigned h,unsigned dst_width,
                          unsigned dst_height,unsigned dst_pitch,int bottom_origin) {
    SceGxmColorSurface color;
    SceGxmRenderTarget *target=target_for(dst_width,dst_height);
    const SceGxmProgramParameter *p;
    const float area[4]={1,1,0,0}, gain[4]={1,1,0,0};
    float u0=(float)x0/s->width,v0=(float)y0/s->height;
    float u1=(float)(x0+w)/s->width,v1=(float)(y0+h)/s->height;
    /* V orientation is an explicit host-view policy. The Vulkan backend
     * samples a rendered colour surface with the same V origin as the guest
     * image, so the canonical mapping is unswapped; the swap is only the
     * opt-in bottom-origin display mode. Native Vita stays unverified. */
    if (bottom_origin) { float swap=v0; v0=v1; v1=swap; }
    const float vertices[16]={0,1,u0,v0,1,1,u1,v0,0,0,u0,v1,1,0,u1,v1};
    const uint16_t indices[4]={0,1,2,3};
    void *ub=NULL;
    if (gxm_scene_poisoned) return frame_context_reject("present-gpu");
    if (!target || !vertex_program || !fragment_program) return -1;
    if (!gxm_blit_vertices) gxm_blit_vertices=gpu_alloc(4096,0,NULL);
    if (!gxm_blit_indices) gxm_blit_indices=gpu_alloc(4096,0,NULL);
    if (!gxm_blit_vertices || !gxm_blit_indices) return -1;
    memcpy(gxm_blit_vertices,vertices,sizeof(vertices));
    memcpy(gxm_blit_indices,indices,sizeof(indices));
    int rc=sceGxmColorSurfaceInit(&color,SCE_GXM_COLOR_FORMAT_A8B8G8R8,
        SCE_GXM_COLOR_SURFACE_LINEAR,SCE_GXM_COLOR_SURFACE_SCALE_NONE,
        SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,dst_width,dst_height,dst_pitch,dst);
    if (rc<0) return rc;
    rc=sceGxmBeginScene(gxm_context,0,target,NULL,NULL,NULL,&color,NULL);
    if (rc<0) return rc;
    /* Negate in float: -dst_height on an unsigned value wraps to ~2^32 and
     * turns the viewport into a 2^32-high (sub-pixel quad) region. */
    sceGxmSetViewport(gxm_context,(float)dst_width*0.5f,(float)dst_width*0.5f,
                      (float)dst_height*0.5f,-(float)dst_height*0.5f,0.5f,0.5f);
    sceGxmSetRegionClip(gxm_context,SCE_GXM_REGION_CLIP_NONE,0,0,
                        dst_width-1u,dst_height-1u);
    sceGxmSetCullMode(gxm_context,SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(gxm_context,SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(gxm_context,SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthBias(gxm_context,0,0);
    sceGxmSetBackDepthBias(gxm_context,0,0);
    sceGxmSetFrontDepthWriteEnable(gxm_context,SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(gxm_context,SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetFrontFragmentProgramEnable(gxm_context,SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
    sceGxmSetBackFragmentProgramEnable(gxm_context,SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
    sceGxmSetVertexProgram(gxm_context,vertex_program);
    sceGxmSetFragmentProgram(gxm_context,fragment_program);
    rc=sceGxmReserveVertexDefaultUniformBuffer(gxm_context,&ub);
    if (rc>=0) rc=init_vertex_uniforms(ub,gxm_vs_program);
    p=sceGxmProgramFindParameterByName(gxm_vs_program,"render_area");
    if (rc>=0 && p) rc=sceGxmSetUniformDataF(ub,p,0,4,area);
    if (rc>=0) rc=sceGxmReserveFragmentDefaultUniformBuffer(gxm_context,&ub);
    if (rc>=0) memset(ub,0,sceGxmProgramGetDefaultUniformBufferSize(gxm_fs_program));
    p=sceGxmProgramFindParameterByName(gxm_fs_program,"fs_cbuf3");
    if (rc>=0 && p) rc=sceGxmSetUniformDataF(ub,p,0,4,gain);
    p=sceGxmProgramFindParameterByName(gxm_fs_program,"tex0");
    if (rc>=0 && p) rc=sceGxmSetFragmentTexture(gxm_context,
        sceGxmProgramParameterGetResourceIndex(p),&s->texture);
    if (rc>=0) rc=sceGxmSetVertexStream(gxm_context,0,gxm_blit_vertices);
    if (rc>=0) rc=sceGxmDraw(gxm_context,SCE_GXM_PRIMITIVE_TRIANGLE_STRIP,
        SCE_GXM_INDEX_FORMAT_U16,gxm_blit_indices,4);
    {
        int end = frame_end_scene_timed(gxm_context, "sync-blit", NULL, NULL, NULL);
        if (end < 0) {
            frame_poison_context(end);
            glog("VP_GXM_PRESENT scene_unwind rc=%d draw_rc=%d context_failed=1\n",
                 end, rc);
            return rc < 0 ? rc : end;
        }
    }
    return rc;
}

static int surface_clear_gpu(VPGxmSurface *s, uint32_t color) {
    static void *pixels;
    VPGxmSurface source;
    int rc;
    if (pixels == NULL) pixels = gpu_alloc(4096u, 0, NULL);
    if (pixels == NULL) return -1;
    for (unsigned i = 0u; i < 64u; ++i) ((uint32_t *)pixels)[i] = color;
    memset(&source, 0, sizeof(source));
    source.width = source.height = 8u;
    rc = sceGxmTextureInitLinear(&source.texture, pixels,
            SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, 8u, 8u, 1u);
    if (rc < 0) return rc;
    return frame_blit_gpu(&source, s->color_mem, 0u, 0u, 8u, 8u,
                          s->render_width, s->render_height, s->render_width, 0);
}

/* Encode the deferred solid clear into the scene the caller already opened for
 * this render target. Uses the display-blit programs and a solid 8x8 source;
 * every binding the next game draw caches is invalidated afterwards.
 *
 * The scene pipeline can leave the previous arena's scene executing while this
 * one is encoded, and the GPU may read the solid texture, vertices and indices
 * until that scene completes. Each arena therefore owns an immutable payload;
 * Each in-flight arena has its own GPU-mapped payload (3 x 4096 bytes),
 * retained until completion, outside the staging/uniform budget. */
static void *gxm_clear_pixels[VP_GXM_BATCH_ARENAS];
static void *gxm_clear_vertices[VP_GXM_BATCH_ARENAS];
static void *gxm_clear_indices[VP_GXM_BATCH_ARENAS];

/* The in-scene GPU clear is encoded after every earlier submitted scene of
 * this context, so it consumes both the pending colour and its async tag
 * without a CPU wait. The metadata update is shared by all in-scene consumers
 * so the tag can never be cleared in one place but missed in another. */
/* VP_HOST_TEST_EXTRACT:clear_in_scene_consume_begin */
static void surface_clear_consumed_in_scene(VPGxmSurface *s) {
    if (s->clear_pending_async) gxm_clear_defer_in_scene++;
    s->clear_pending = 0u;
    s->clear_pending_async = 0u;
    s->color_clear_mask |= VP_CLEAR_FUSION_COLOR_MASK;
    s->cleared_count++;
    gxm_clear_fusion_in_scene++;
}
/* VP_HOST_TEST_EXTRACT:clear_in_scene_consume_end */

static int frame_clear_in_scene(VPGxmSurface *dst) {
    VPGxmSurface source;
    const SceGxmProgramParameter *p;
    const float area[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    const float gain[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    const float vertices[16] = {0,1,0,0, 1,1,1,0, 0,0,0,1, 1,0,1,1};
    const uint16_t indices[4] = {0, 1, 2, 3};
    unsigned w, h, i, arena;
    void *pixels, *verts, *inds;
    void *ub = NULL;
    int rc;

    if (dst == NULL || dst->color_mem == NULL || vertex_program == NULL ||
        fragment_program == NULL) return -1;
    gxm_batch_arena_init();
    arena = gxm_batch_arena;
    if (gxm_clear_pixels[arena] == NULL) {
        gxm_clear_pixels[arena] = gpu_alloc(4096u, 0, NULL);
        gxm_clear_vertices[arena] = gpu_alloc(4096u, 0, NULL);
        gxm_clear_indices[arena] = gpu_alloc(4096u, 0, NULL);
        if (gxm_clear_pixels[arena] != NULL &&
            gxm_clear_vertices[arena] != NULL &&
            gxm_clear_indices[arena] != NULL)
            glog("VP_GXM_CLEAR_PAYLOAD arena=%u bytes=%u\n", arena, 4096u * 3u);
    }
    pixels = gxm_clear_pixels[arena];
    verts = gxm_clear_vertices[arena];
    inds = gxm_clear_indices[arena];
    if (pixels == NULL || verts == NULL || inds == NULL) return -1;
    w = dst->render_width;
    h = dst->render_height;
    for (i = 0u; i < 64u; ++i) ((uint32_t *)pixels)[i] = dst->clear_pending_color;
    memset(&source, 0, sizeof(source));
    source.width = source.height = 8u;
    rc = sceGxmTextureInitLinear(&source.texture, pixels,
            SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, 8u, 8u, 1u);
    if (rc < 0) return rc;
    memcpy(verts, vertices, sizeof(vertices));
    memcpy(inds, indices, sizeof(indices));
    sceGxmSetViewport(gxm_context, (float)w * 0.5f, (float)w * 0.5f,
                      (float)h * 0.5f, -(float)h * 0.5f, 0.5f, 0.5f);
    sceGxmSetRegionClip(gxm_context, SCE_GXM_REGION_CLIP_NONE, 0, 0, w - 1u, h - 1u);
    sceGxmSetCullMode(gxm_context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(gxm_context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(gxm_context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthBias(gxm_context, 0, 0);
    sceGxmSetBackDepthBias(gxm_context, 0, 0);
    sceGxmSetFrontDepthWriteEnable(gxm_context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(gxm_context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetFrontStencilFunc(gxm_context, SCE_GXM_STENCIL_FUNC_ALWAYS,
        SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
        SCE_GXM_STENCIL_OP_KEEP, 255, 255);
    sceGxmSetBackStencilFunc(gxm_context, SCE_GXM_STENCIL_FUNC_ALWAYS,
        SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
        SCE_GXM_STENCIL_OP_KEEP, 255, 255);
    sceGxmSetFrontFragmentProgramEnable(gxm_context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
    sceGxmSetBackFragmentProgramEnable(gxm_context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
    sceGxmSetVertexProgram(gxm_context, vertex_program);
    sceGxmSetFragmentProgram(gxm_context, fragment_program);
    rc = sceGxmReserveVertexDefaultUniformBuffer(gxm_context, &ub);
    if (rc >= 0) rc = init_vertex_uniforms(ub, gxm_vs_program);
    p = sceGxmProgramFindParameterByName(gxm_vs_program, "render_area");
    if (rc >= 0 && p != NULL) rc = sceGxmSetUniformDataF(ub, p, 0, 4, area);
    if (rc >= 0) rc = sceGxmReserveFragmentDefaultUniformBuffer(gxm_context, &ub);
    if (rc >= 0)
        memset(ub, 0, sceGxmProgramGetDefaultUniformBufferSize(gxm_fs_program));
    p = sceGxmProgramFindParameterByName(gxm_fs_program, "fs_cbuf3");
    if (rc >= 0 && p != NULL) rc = sceGxmSetUniformDataF(ub, p, 0, 4, gain);
    p = sceGxmProgramFindParameterByName(gxm_fs_program, "tex0");
    if (rc >= 0 && p != NULL)
        rc = sceGxmSetFragmentTexture(gxm_context,
            sceGxmProgramParameterGetResourceIndex(p), &source.texture);
    if (rc >= 0)
        rc = sceGxmSetVertexStream(gxm_context, 0, verts);
    if (rc >= 0)
        rc = sceGxmDraw(gxm_context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP,
                        SCE_GXM_INDEX_FORMAT_U16, inds, 4);
    return rc;
}

static int frame_present_internal(uint64_t va) {
    VPGxmSurface *s;
    SceDisplayFrameBuf display;
    void *dst;
    unsigned idx, x0, y0, w, h, x, y;
    uint64_t present_start = sceKernelGetProcessTimeWide();
    int rc;

    if (gxm_scene_poisoned) return frame_context_reject("present");
    /* Present may sample the frame with a GPU blit and, with surface readback
     * enabled, copy it on the CPU. Both require the deferred scene to finish. */
    rc = gxm_pending_scene_complete("present");
    if (rc < 0) return rc;
    if (!gxm_ready) return -1;
    /* The native movie owns the display while it is presenting. Retire the
     * guest's submitted scene (above) but do not copy or queue its stale movie
     * surface; the guest display binder still receives a successful present. */
    if (vp_native_movie_presenting()) {
        s = surface_find(va);
        if (s != NULL) {
            s->disp_valid = 0u;
            s->present_draws = 0u;
        }
        return 0;
    }
    s = surface_find(va);
    if (s == NULL || s->color_mem == NULL) return -2;
    if (gxm_clear_fusion_enabled() && s->clear_pending) {
        rc = surface_clear_materialize(s, "present");
        if (rc < 0) return rc;
    }
    if (s->present_draws == 0u) return -3;
    x0 = s->disp_valid ? s->disp_x0 : 0u;
    y0 = s->disp_valid ? s->disp_y0 : 0u;
    w = s->disp_valid ? s->disp_x1 - s->disp_x0 : s->width;
    h = s->disp_valid ? s->disp_y1 - s->disp_y0 : s->height;
    if (w == 0u || h == 0u) return -3;
    idx = gxm_display_index ^ 1u;
    if (gxm_display_buf[idx] == NULL)
        gxm_display_buf[idx] = gpu_alloc(VP_GXM_BYTES, 0, NULL);
    if (gxm_display_buf[idx] == NULL) return -1;
    dst = gxm_display_buf[idx];
    if (gxm_gpu_present<0) {
        FILE *f=fopen("ux0:data/mhgu-vita-port/gpu-present.enable","rb");
        gxm_gpu_present=f!=NULL;
        if (f) fclose(f);
        glog("VP_GPU_PRESENT_MODE enabled=%d\n",gxm_gpu_present);
        f=fopen("ux0:data/mhgu-vita-port/gpu-present-bottom-origin.enable","rb");
        gxm_gpu_present_bottom_origin=f!=NULL;
        if (f) fclose(f);
        glog("VP_GPU_PRESENT_ORIGIN bottom=%d\n",gxm_gpu_present_bottom_origin);
    }
    if (gxm_gpu_present) {
        rc=frame_blit_gpu(s,dst,x0,y0,w,h,VP_GXM_WIDTH,VP_GXM_HEIGHT,VP_GXM_PITCH,
                          gxm_gpu_present_bottom_origin);
        if (rc<0) return rc;
    } else {
    for (y = 0; y < VP_GXM_HEIGHT; y++) {
        unsigned sy = (y0 + (y * h) / VP_GXM_HEIGHT) * s->render_height / s->height;
        const uint32_t *src = (const uint32_t *)s->color_mem + (size_t)sy * s->render_width;
        uint32_t *row = (uint32_t *)dst + (size_t)y * VP_GXM_PITCH;
        for (x = 0; x < VP_GXM_WIDTH; x++) {
            unsigned sx = (x0 + (x * w) / VP_GXM_WIDTH) * s->render_width / s->width;
            row[x] = src[sx];
        }
    }
    }
    memset(&display, 0, sizeof(display));
    display.size = sizeof(display);
    display.base = dst;
    display.pitch = VP_GXM_PITCH;
    display.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    display.width = VP_GXM_WIDTH;
    display.height = VP_GXM_HEIGHT;
    rc = sceDisplaySetFrameBuf(&display, SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (rc < 0) return rc;
    gxm_display_index = idx;
    ++gxm_present_count;
    b34_dump_present_pair(s, dst, VP_GXM_WIDTH, VP_GXM_HEIGHT, VP_GXM_PITCH,
                          va, x0, y0, w, h);
    {
        uint64_t present_now = sceKernelGetProcessTimeWide();
        gxm_perf_present(present_now - present_start, present_now);
    }
    if (gxm_gpu_present) {
        if (gxm_present_count<=8u || (gxm_present_count%128u)==0u)
            glog("VP_GPU_PRESENT count=%u va=0x%llx rect=%u,%u %ux%u rc=%d\n",
                 gxm_present_count,(unsigned long long)va,x0,y0,w,h,rc);
        s->disp_valid=0u;s->present_draws=0u;
        return 0;
    }
    if (gxm_present_dumps < 32u &&
        (gxm_present_dumps < 8u || (gxm_present_count % 512u) == 0u)) {
        char path[96];
        FILE *file;
        unsigned src_nz = 0u, src_max = 0u, src_total = 0u;
        unsigned sx_step = w / 64u ? w / 64u : 1u;
        unsigned sy_step = h / 64u ? h / 64u : 1u;
        unsigned sx, sy;
        for (sy = y0; sy < y0 + h; sy += sy_step) {
            const uint32_t *src = (const uint32_t *)s->color_mem +
                (size_t)(sy * s->render_height / s->height) * s->render_width;
            for (sx = x0; sx < x0 + w; sx += sx_step) {
                uint32_t p = src[sx * s->render_width / s->width];
                unsigned m = (p & 0xffu);
                if (((p >> 8) & 0xffu) > m) m = (p >> 8) & 0xffu;
                if (((p >> 16) & 0xffu) > m) m = (p >> 16) & 0xffu;
                if (m > 8u) ++src_nz;
                if (m > src_max) src_max = m;
                ++src_total;
            }
        }
        snprintf(path, sizeof(path),
                 "ux0:data/mhgu-vita-port/display-present-%03u.rgba",
                 gxm_present_dumps);
        file = fopen(path, "wb");
        if (file != NULL) {
            fwrite(dst, 4u, VP_GXM_WIDTH * VP_GXM_HEIGHT, file);
            fclose(file);
        }
        glog("VP_DISPLAY_PRESENT va=0x%llx rect=%u,%u %ux%u draws=%u src_nz=%u/%u src_max=%u rc=%d dump=%u count=%u\n",
             (unsigned long long)va, x0, y0, w, h, s->present_draws,
             src_nz, src_total, src_max, rc, gxm_present_dumps,
             gxm_present_count);
        ++gxm_present_dumps;
    }
    s->disp_valid = 0u;
    s->present_draws = 0u;
    return 0;
}

/* Re-latch the retained GXM frame when an applet closes without a new guest
 * draw. This preserves the last real game pixels and gives the display path a
 * fresh base frame after the indirect applet layer is removed. */
static int frame_refresh_internal(void) {
    SceDisplayFrameBuf display;
    void *buffer = gxm_display_buf[gxm_display_index];
    int rc;
    if (gxm_scene_poisoned) return frame_context_reject("display-refresh");
    rc = gxm_pending_scene_complete("refresh");
    if (rc < 0) return rc;
    if (!gxm_ready) return -1;
    if (buffer == NULL) return -2;
    memset(&display, 0, sizeof(display));
    display.size = sizeof(display);
    display.base = buffer;
    display.pitch = VP_GXM_PITCH;
    display.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    display.width = VP_GXM_WIDTH;
    display.height = VP_GXM_HEIGHT;
    rc = sceDisplaySetFrameBuf(&display, SCE_DISPLAY_SETBUF_NEXTFRAME);
    glog("VP_DISPLAY_REFRESH buffer=%p %ux%u pitch=%u rc=%d\n",
         buffer, VP_GXM_WIDTH, VP_GXM_HEIGHT, VP_GXM_PITCH, rc);
    return rc;
}


/* Select the package explicitly by file presence. A malformed static package
 * fails registration; only an absent package selects the legacy bundle. */
static int shader_register_default(unsigned *files, unsigned *registered,
                                   int *first_error) {
    char path[VP_GXM_STATIC_PATH];
    FILE *probe;
    snprintf(path, sizeof(path), "%s/shader-index.bin", VP_GXM_STATIC_DIR);
    probe = fopen(path, "rb");
    if (probe != NULL) {
        fclose(probe);
        glog("VP_GXM_SHADER mode=static dir=%s\n", VP_GXM_STATIC_DIR);
        return vp_gxm_shader_register_all(VP_GXM_STATIC_DIR, files, registered,
                                          first_error);
    }
    glog("VP_GXM_SHADER mode=legacy dir=%s static_package=absent\n",
         VP_GXM_LEGACY_DIR);
    return vp_gxm_shader_register_all(VP_GXM_LEGACY_DIR, files, registered,
                                      first_error);
}

int vp_gxm_runtime_test(uint32_t color, vp_gxm_test_result *out) {
    SceGxmInitializeParams init;
    SceDisplayFrameBuf display;
    void *pixels;
    unsigned shader_files = 0;
    unsigned shader_registered = 0;
    int shader_rc;
    int first_error = 0;

    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    /* The selection must be fixed before the context, surfaces or depth
     * storage exist; an invalid file stops this direct initialization path. */
    if (gxm_render_resolution_ensure() < 0) return -1;

    memset(&init, 0, sizeof(init));
    init.parameterBufferSize = 1024u * 1024u;
    init.displayQueueMaxPendingCount = 2;
    init.displayQueueCallback = display_callback;
    init.displayQueueCallbackDataSize = sizeof(void *);
    out->init_rc = sceGxmInitialize(&init);
    if (out->init_rc < 0) {
        glog("VP_GXM_TEST init=%d\n", out->init_rc);
        return -1;
    }

    pixels = gpu_alloc(VP_GXM_BYTES, 0, NULL);
    if (pixels == NULL) {
        out->alloc_rc = -1;
        glog("VP_GXM_TEST init=%d alloc=%d\n", out->init_rc, out->alloc_rc);
        return -1;
    }
    out->map_rc = 0;
    out->fill_rc = sceGxmTransferFill(color,
            SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, pixels,
            0, 0, VP_GXM_WIDTH, VP_GXM_HEIGHT, VP_GXM_PITCH * 4u, NULL, 0,
            NULL);
    out->finish_rc = sceGxmTransferFinish();
    out->pixel = ((const uint32_t *)pixels)[0];

    memset(&display, 0, sizeof(display));
    display.size = sizeof(display);
    display.base = pixels;
    display.pitch = VP_GXM_PITCH;
    display.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    display.width = VP_GXM_WIDTH;
    display.height = VP_GXM_HEIGHT;
    out->display_rc = gxm_show_bringup_frame ?
        sceDisplaySetFrameBuf(&display, SCE_DISPLAY_SETBUF_NEXTFRAME) : 0;
    glog("VP_GXM_BRINGUP_DISPLAY synthetic_present=%d\n",gxm_show_bringup_frame);

    shader_rc = shader_register_default(&shader_files, &shader_registered,
                                        &first_error);
    out->shader_files = shader_files;
    out->shader_registered = shader_registered;
    out->shader_rc = shader_rc;
    out->program_rc = shader_rc < 0 ? -1 : create_first_programs();
    out->draw_rc = -1;
    if (out->program_rc == 0) {
        const SceGxmProgram *vs = NULL;
        const SceGxmProgram *fs = NULL;
        unsigned i;
        for (i = 0; i < shader_count; i++) {
            if (shaders[i].hash != 0xf9dul || shaders[i].viewport_transform) continue;
            if (shaders[i].stage == 1u) vs = shaders[i].program;
            if (shaders[i].stage == 5u) fs = shaders[i].program;
        }
        out->draw_rc = run_first_draw(vs, fs, &out->draw_pixel);
    }

    glog("VP_GXM_TEST init=%d map=%d fill=%d finish=%d display=%d color=0x%08x pixel=0x%08x "
         "shader_files=%u shader_registered=%u shader_rc=%d first_error=%d program_rc=%d "
         "draw_rc=%d draw_pixel=0x%08x\n",
         out->init_rc, out->map_rc, out->fill_rc, out->finish_rc,
         out->display_rc, color, out->pixel, shader_files, shader_registered,
         shader_rc, first_error, out->program_rc, out->draw_rc, out->draw_pixel);
    return (out->fill_rc < 0 || out->finish_rc < 0 || out->display_rc < 0 ||
            shader_rc < 0 || out->program_rc < 0 || out->draw_rc < 0) ? -1 : 0;
}

static SceUID gxm_work_event = -1;
static SceUID gxm_done_event = -1;
static SceUID gxm_worker_thread = -1;
static SceUID gxm_api_mutex = -1;
static volatile int gxm_worker_ready;
static volatile int gxm_worker_ok;
static void (*gxm_pending_execute)(void *);
static void *gxm_execute_user;
static VPGxmDrawRequest *volatile gxm_pending;
static VPGxmClearRequest *volatile gxm_pending_clear;
static volatile int gxm_pending_finish;
static volatile int gxm_finish_rc;
static volatile int gxm_pending_present;
static volatile uint64_t gxm_pending_present_va;
static volatile int gxm_present_rc;
static volatile int gxm_pending_refresh;
static volatile int gxm_refresh_rc;
static volatile int gxm_pending_batch_flush;
static volatile int gxm_batch_flush_rc;
#include "gxm_batch_selftest.inc"

static int gxm_api_enter(const char *who) {
    int rc;
    if (gxm_api_mutex < 0) {
        glog("VP_GXM_API_FAIL who=%s op=lock reason=no-mutex\n", who);
        return -1;
    }
    rc = sceKernelLockMutex(gxm_api_mutex, 1, NULL);
    if (rc < 0) {
        glog("VP_GXM_API_FAIL who=%s op=lock rc=%d\n", who, rc);
        return -1;
    }
    /* Readiness may have changed while this caller waited for the mutex. */
    if (!gxm_worker_ok) {
        sceKernelUnlockMutex(gxm_api_mutex, 1);
        return -1;
    }
    return 0;
}

static int gxm_api_leave(void) {
    int rc;
    if (gxm_api_mutex < 0) return -1;
    rc = sceKernelUnlockMutex(gxm_api_mutex, 1);
    if (rc < 0) {
        glog("VP_GXM_API_FAIL op=unlock rc=%d\n", rc);
        gxm_worker_ok = 0;
    }
    return rc;
}

/* Signal the worker. A failed signal must not leave the pending slot armed:
 * the caller owns cleanup of its own request and reports failure upward. */
static int gxm_worker_notify(const char *who) {
    int rc = sceKernelSetEventFlag(gxm_work_event, 1);
    if (rc < 0) {
        glog("VP_GXM_API_FAIL who=%s op=set-event rc=%d\n", who, rc);
        return -1;
    }
    return 0;
}

/* Wait for the worker receipt. A failed wait must never return control to a
 * caller whose request object may still be owned by the worker: the worker
 * writes into that request before signalling. Fail closed instead: stop
 * accepting new requests and keep retrying until the receipt arrives, so the
 * caller only regains ownership after the worker released it. */
static int gxm_worker_wait(const char *who) {
    unsigned failures = 0u;
    for (;;) {
        int rc = sceKernelWaitEventFlag(gxm_done_event, 1,
                SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, NULL, NULL);
        if (rc >= 0) return failures == 0u && gxm_worker_ok ? 0 : -1;
        if (failures++ == 0u || (failures % 256u) == 0u)
            glog("VP_GXM_API_FAIL who=%s op=wait-done rc=%d retry=%u "
                 "action=fail-closed\n", who, rc, failures);
        /* A broken receipt means this worker can no longer be trusted with a
         * new request; do not publish success to later callers. */
        gxm_worker_ok = 0;
        sceKernelDelayThread(10000);
    }
    return 0;
}

/* The worker's own admission wait. A failed wait must not degrade into a
 * silent busy loop or a made-up "no work" receipt: keep the failure visible,
 * stop trusting this worker for new requests, and retry with a delay. Pending
 * requests stay armed and are processed as soon as the event works again. */
static void gxm_worker_wait_work(void) {
    unsigned failures = 0u;
    for (;;) {
        int rc = sceKernelWaitEventFlag(gxm_work_event, 1,
                SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, NULL, NULL);
        if (rc >= 0) return;
        if (failures++ == 0u || (failures % 256u) == 0u)
            glog("VP_GXM_API_FAIL who=worker op=wait-work rc=%d retry=%u "
                 "action=fail-closed\n", rc, failures);
        gxm_worker_ok = 0;
        sceKernelDelayThread(10000);
    }
}

static unsigned gxm_worker_process(void) {
    uint64_t work_started = gxm_prof_now();
    unsigned work_kind = 0, work_hash = 0;
    if (gxm_pending_execute != NULL) {
        void (*execute)(void *) = gxm_pending_execute;
        gxm_pending_execute = NULL;
        work_kind = 7;
        gxm_api_perf.stream_entries++;
        execute(gxm_execute_user);
    } else if (gxm_pending_clear != NULL) {
        work_kind = 1;
        VPGxmClearRequest *clear = gxm_pending_clear;
        /* All clear requests (external worker dispatch and the worker-local
         * route) share one admission decision. A refused request keeps the
         * original synchronous clear boundary inside frame_clear_internal. */
        {
            uint64_t fc = vp_gxm_frame_cost_category_begin(
                VP_FRAME_COST_GXM_CLEAR);
            clear->rc = frame_clear_internal_impl(clear, 1);
            vp_gxm_frame_cost_category_end(VP_FRAME_COST_GXM_CLEAR, fc);
        }
        gxm_pending_clear = NULL;
    } else if (gxm_pending_finish) {
        int flush_rc = 0;
        work_kind = 2;
        gxm_pending_finish = 0;
        if (gxm_batch.scene_open)
            flush_rc = gxm_batch_flush_worker(GXM_BATCH_REASON_SUBMIT);
        if (flush_rc < 0)
            glog("VP_GXM_WORKER_FINISH flush_rc=%d action=skip\n", flush_rc);
        gxm_finish_rc = flush_rc < 0 ? flush_rc : frame_finish_internal();
    } else if (gxm_pending_present) {
        int flush_rc = 0;
        work_kind = 3;
        uint64_t va = gxm_pending_present_va;
        gxm_pending_present = 0;
        if (gxm_batch.scene_open)
            flush_rc = gxm_batch_flush_worker(GXM_BATCH_REASON_PRESENT);
        gxm_present_rc = flush_rc < 0
            ? flush_rc : frame_present_internal(va);
    } else if (gxm_pending_refresh) {
        int flush_rc = 0;
        work_kind = 5;
        gxm_pending_refresh = 0;
        if (gxm_batch.scene_open)
            flush_rc = gxm_batch_flush_worker(GXM_BATCH_REASON_READBACK);
        gxm_refresh_rc = flush_rc < 0
            ? flush_rc : frame_refresh_internal();
    } else if (gxm_pending_batch_flush) {
        work_kind = 6;
        gxm_pending_batch_flush = 0;
        gxm_batch_flush_rc = gxm_batch_flush_worker(GXM_BATCH_REASON_SUBMIT);
        if (gxm_batch_flush_rc == 0 && gxm_scene_poisoned)
            gxm_batch_flush_rc = frame_context_reject("batch-flush");
    } else if (gxm_pending != NULL) {
        VPGxmDrawRequest *req = gxm_pending;
        work_kind = 4; work_hash = req->hash;
        if (req->frame_draw) {
            uint64_t draw_started = gxm_prof_now();
            uint64_t draw_finished, draw_elapsed;
            unsigned perf_kind;
            req->rc = frame_draw_internal(req);
            uint64_t tail_fc = vp_gxm_frame_cost_category_begin(VP_FRAME_COST_WORKER_TAIL);
            draw_finished = gxm_prof_now();
            draw_elapsed = draw_finished - draw_started;
            if (req->batch_encoded) {
                /* Encode-only: the draw is not complete until the bounded
                 * scene flush. Do not increment completed/global success or
                 * add its wall time to a per-draw completed window. */
                VPGxmPerfKind *perf = &gxm_perf_kinds[
                    gxm_last_draw_class < 2u ? gxm_last_draw_class : 0u];
                if (perf->window_start == 0u)
                    perf->window_start = draw_started;
                gxm_perf_add(&perf->staged, draw_elapsed);
                gxm_perf_window_flush(gxm_last_draw_class == 1u ? 1u : 0u,
                                      draw_finished, 0);
                if (static_mode &&
                    static_stats.batch_encoded % 256u == 0u)
                    static_log_stats();
            } else {
            perf_kind = gxm_last_draw_class;
            if (perf_kind < 2u) {
                VPGxmPerfKind *perf = &gxm_perf_kinds[perf_kind];
                if (perf->window_start == 0u)
                    perf->window_start = draw_started;
                if (req->rc == 0) {
                    /* Worker-side processing only; this excludes guest
                     * submission/queue wait and is not an API roundtrip. */
                    gxm_perf_add(&perf->worker, draw_elapsed);
                    gxm_perf_global_ok++;
                } else {
                    perf->failures++;
                    gxm_perf_add(&perf->fail, draw_elapsed);
                    gxm_perf_global_fail++;
                }
                gxm_perf_window_flush(perf_kind, draw_finished, 0);
            } else if (perf_kind == 2u) {
                VPGxmPerfKind *perf = &gxm_perf_kinds[1];
                if (perf->window_start == 0u)
                    perf->window_start = draw_started;
                perf->skips++;
                gxm_perf_global_skip++;
            }
            if (static_mode) {
                if (req->rc == 0) static_stats.draw_ok++;
                else static_stats.draw_fail++;
                if ((static_stats.draw_ok + static_stats.draw_fail) % 256u == 0u)
                    static_log_stats();
            }
            }
            vp_gxm_frame_cost_category_end(VP_FRAME_COST_WORKER_TAIL, tail_fc);
            gxm_pending = NULL;
        } else {
            int flush_rc = 0;
            if (gxm_batch.scene_open)
                flush_rc = gxm_batch_flush_worker(GXM_BATCH_REASON_LEGACY);
            if (flush_rc < 0) {
                req->rc = flush_rc;
            } else {
                req->rc = vp_gxm_game_draw(req->vertices, req->count, req->stride, req->hash,
                    req->viewport_transform,
                    req->attributes,
                    req->screen_width, req->screen_height,
                    req->has_uniforms ? req->uniforms : NULL,
                    req->texels, req->tex_width, req->tex_height,
                    req->sampler,
                    req->stage_cbuf,
                    &req->center, &req->nonzero);
            }
        }
        gxm_pending = NULL;
    }
    {
        static unsigned timing_count;
        if (timing_count < 96u) {
            glog("VP_GXM_TIMING n=%u kind=%u hash=0x%x start=%llu elapsed=%llu\n",
                 timing_count++,work_kind,work_hash,(unsigned long long)work_started,
                 (unsigned long long)(gxm_prof_now()-work_started));
        }
    }
    return work_kind;
}

static int gxm_worker_main(SceSize args, void *argp) {
    vp_gxm_test_result result;
    int rc;
    (void)args;
    (void)argp;
    memset(&result, 0, sizeof(result));
    {
        FILE *play = fopen("ux0:data/mhgu-vita-port/play.enable", "rb");
        gxm_show_bringup_frame = play == NULL;
        if (play != NULL) fclose(play);
    }
    rc = vp_gxm_runtime_test(0xff30c020u, &result);
    if (rc == 0) rc = gxm_batch_selftest();
    glog("VP_GXM_WORKER bringup_rc=%d draw_pixel=0x%08x program_rc=%d\n",
         rc, result.draw_pixel, result.program_rc);
    gxm_worker_ok = rc == 0;
    gxm_worker_ready = 1;
    for (;;) {
        gxm_worker_wait_work();
        unsigned work_kind = gxm_worker_process();
        {
            unsigned failures = 0u;
            int done_rc;
            while ((done_rc = sceKernelSetEventFlag(gxm_done_event, 1)) < 0) {
                if (failures++ == 0u || (failures % 256u) == 0u)
                    glog("VP_GXM_API_FAIL who=worker op=set-done rc=%d "
                         "kind=%u retry=%u action=fail-closed\n",
                         done_rc, work_kind, failures);
                gxm_worker_ok = 0;
                sceKernelDelayThread(10000);
            }
        }
    }
    return 0;
}

int vp_gxm_worker_start(void) {
    unsigned spin = 0;
    if (gxm_worker_thread >= 0) return gxm_worker_ok ? 0 : -1;
    /* Deterministic one-time init for the normal worker path; direct runtime
     * entries also initialize on their first glog. Logging is diagnostic only
     * and a failed open never prevents the worker from starting. There is no
     * runtime teardown, so the successful handle lives until process exit and
     * worker-start failures do not close it out from under later callers. */
    (void)gxm_log_ensure();
    /* Fix the render resolution before creating any worker/GXM resource. The
     * worker's bring-up calls the same once-only loader. */
    if (gxm_render_resolution_ensure() < 0) return -1;
    gxm_work_event = sceKernelCreateEventFlag("vp-gxm-work", 0, 0, NULL);
    gxm_done_event = sceKernelCreateEventFlag("vp-gxm-done", 0, 0, NULL);
    if (gxm_work_event < 0 || gxm_done_event < 0) {
        glog("VP_GXM_WORKER_START_FAIL stage=events work=%d done=%d\n",
             gxm_work_event, gxm_done_event);
        return -1;
    }
    gxm_api_mutex = sceKernelCreateMutex("vp-gxm-api", 0, 0, NULL);
    if (gxm_api_mutex < 0) {
        glog("VP_GXM_WORKER_START_FAIL stage=mutex rc=%d\n", gxm_api_mutex);
        return -1;
    }
    gxm_worker_thread = sceKernelCreateThread("vp-gxm", gxm_worker_main,
            0x10000100, 0x40000, 0, 0, NULL);
    if (gxm_worker_thread < 0) {
        glog("VP_GXM_WORKER_START_FAIL stage=thread rc=%d\n", gxm_worker_thread);
        return -1;
    }
    if (sceKernelStartThread(gxm_worker_thread, 0, NULL) < 0) {
        glog("VP_GXM_WORKER_START_FAIL stage=start thread=%d\n",
             gxm_worker_thread);
        return -1;
    }
    while (!gxm_worker_ready && spin++ < 20000u) sceKernelDelayThread(1000);
    return gxm_worker_ready && gxm_worker_ok ? 0 : -1;
}

int vp_gxm_worker_is_current(void) {
    return gxm_worker_thread >= 0 && sceKernelGetThreadId() == gxm_worker_thread;
}

int vp_gxm_worker_execute(void (*execute)(void *), void *user) {
    if (execute == NULL || gxm_worker_thread < 0 || !gxm_worker_ok) return -1;
    if (gxm_api_enter("execute") < 0) return -1;
    gxm_execute_user = user;
    gxm_pending_execute = execute;
    if (gxm_worker_notify("execute") < 0) {
        gxm_pending_execute = NULL;
        gxm_api_leave();
        return -1;
    }
    int rc = gxm_worker_wait("execute");
    return gxm_api_leave() < 0 ? -1 : rc;
}

int vp_gxm_worker_submit(VPGxmDrawRequest *request) {
    int rc;
    uint64_t t0, t1, t2;
    if (request == NULL || gxm_worker_thread < 0 || !gxm_worker_ok) return -1;
    if (vp_gxm_worker_is_current()) {
        gxm_api_perf.direct_draws++;
        gxm_pending = request;
        gxm_worker_process();
        return request->rc;
    }
    if (gxm_api_enter("draw") < 0) return -1;
    request->rc = -1;
    gxm_pending = request;
    t0 = gxm_prof_now();
    if (gxm_worker_notify("draw") < 0) {
        gxm_pending = NULL;
        gxm_api_leave();
        return -1;
    }
    t1 = gxm_prof_now();
    if (gxm_worker_wait("draw") < 0) {
        /* The request may still be owned by the worker; do not reuse it. */
        gxm_api_leave();
        return -1;
    }
    t2 = gxm_prof_now();
    gxm_api_perf.submits++;
    if (t1 != 0u && t2 != 0u) {
        gxm_api_perf.notify_sum += t1 - t0;
        gxm_api_perf.wait_sum += t2 - t1;
        if (t1 - t0 > gxm_api_perf.notify_max) gxm_api_perf.notify_max = t1 - t0;
        if (t2 - t1 > gxm_api_perf.wait_max) gxm_api_perf.wait_max = t2 - t1;
    }
    rc = request->rc;
    return gxm_api_leave() < 0 ? -1 : rc;
}

int vp_gxm_batch_flush(const char *reason) {
    int rc;
    (void)reason;
    if (gxm_worker_thread < 0) {
        /* No worker context means no scene can be open. A submission that
         * arrives before GXM bring-up (or after a failed bring-up) must not
         * wait on an event that no thread can signal. */
        return 0;
    }
    if (!gxm_worker_ok) return -1;
    /* Batch state belongs to the worker. Serialize even an empty flush so
     * the worker checks both its state invariants and a poisoned context. */
    if (vp_gxm_worker_is_current()) {
        gxm_pending_batch_flush = 1;
        gxm_worker_process();
        return gxm_batch_flush_rc;
    }
    if (gxm_api_enter("batch-flush") < 0) return -1;
    gxm_batch_flush_rc = -1;
    gxm_pending_batch_flush = 1;
    if (gxm_worker_notify("batch-flush") < 0) {
        gxm_pending_batch_flush = 0;
        gxm_api_leave();
        return -1;
    }
    if (gxm_worker_wait("batch-flush") < 0) {
        gxm_api_leave();
        return -1;
    }
    rc = gxm_batch_flush_rc;
    return gxm_api_leave() < 0 ? -1 : rc;
}

int vp_gxm_frame_clear(VPGxmClearRequest *request) {
    int rc;
    if (request == NULL || gxm_worker_thread < 0 || !gxm_worker_ok) return -1;
    if (vp_gxm_worker_is_current()) {
        gxm_pending_clear = request;
        gxm_worker_process();
        return request->rc;
    }
    if (gxm_api_enter("clear") < 0) return -1;
    request->rc = -1;
    gxm_pending_clear = request;
    if (gxm_worker_notify("clear") < 0) {
        gxm_pending_clear = NULL;
        gxm_api_leave();
        return -1;
    }
    if (gxm_worker_wait("clear") < 0) {
        gxm_api_leave();
        return -1;
    }
    rc = request->rc;
    return gxm_api_leave() < 0 ? -1 : rc;
}

int vp_gxm_frame_finish(void) {
    int rc;
    if (gxm_worker_thread < 0 || !gxm_worker_ok) return -1;
    if (vp_gxm_worker_is_current()) {
        gxm_pending_finish = 1;
        gxm_worker_process();
        return gxm_finish_rc;
    }
    if (gxm_api_enter("finish") < 0) return -1;
    gxm_finish_rc = -1;
    gxm_pending_finish = 1;
    if (gxm_worker_notify("finish") < 0) {
        gxm_pending_finish = 0;
        gxm_api_leave();
        return -1;
    }
    if (gxm_worker_wait("finish") < 0) {
        gxm_api_leave();
        return -1;
    }
    rc = gxm_finish_rc;
    return gxm_api_leave() < 0 ? -1 : rc;
}

int vp_gxm_display_present(uint64_t surface_va) {
    int rc;
    if (gxm_worker_thread < 0 || !gxm_worker_ok) return -1;
    if (surface_va == 0u) return -2;
    if (vp_gxm_worker_is_current()) {
        gxm_pending_present_va = surface_va; gxm_pending_present = 1;
        gxm_worker_process();
        return gxm_present_rc;
    }
    if (gxm_api_enter("present") < 0) return -1;
    gxm_pending_present_va = surface_va;
    gxm_present_rc = -1;
    gxm_pending_present = 1;
    if (gxm_worker_notify("present") < 0) {
        gxm_pending_present = 0;
        gxm_api_leave();
        return -1;
    }
    if (gxm_worker_wait("present") < 0) {
        gxm_api_leave();
        return -1;
    }
    rc = gxm_present_rc;
    return gxm_api_leave() < 0 ? -1 : rc;
}

int vp_gxm_display_refresh(void) {
    int rc;
    if (gxm_worker_thread < 0 || !gxm_worker_ok) return -1;
    if (vp_gxm_worker_is_current()) {
        gxm_pending_refresh = 1;
        gxm_worker_process();
        return gxm_refresh_rc;
    }
    if (gxm_api_enter("refresh") < 0) return -1;
    gxm_refresh_rc = -1;
    gxm_pending_refresh = 1;
    if (gxm_worker_notify("refresh") < 0) {
        gxm_pending_refresh = 0;
        gxm_api_leave();
        return -1;
    }
    if (gxm_worker_wait("refresh") < 0) {
        gxm_api_leave();
        return -1;
    }
    rc = gxm_refresh_rc;
    return gxm_api_leave() < 0 ? -1 : rc;
}

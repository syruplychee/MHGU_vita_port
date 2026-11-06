/* Minimal GXM/display bring-up used to prove the first-frame path.
 *
 * This module fills a 960x544 A8B8G8R8 surface with the GXM transfer engine,
 * reads the first pixel back, presents the surface, and (when a shader bundle
 * is staged under ux0:data/mhgu-vita-port/shaders) registers every GXP with a
 * shader patcher. It does not use the game's draws yet; it exists so GXM init,
 * GPU mapping, display present and GXP registration are exercised before the
 * draw translator is wired in.
 */
#ifndef VP_GXM_RUNTIME_H
#define VP_GXM_RUNTIME_H

#include "log_sink.h"
#include "work_log.h"
#include "gxm_sampler_slots.h"
#include "gxm_resource_layout.h"
#include "gxm_shader_plan.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const void *data;
    unsigned size;
} VPGxmUniform;

/* Where an extra fragment sampler's content comes from. Descriptor identity is
 * preserved even when no CPU backing can be decoded, so a resolved GPU
 * producer is never replaced by a stale CPU snapshot. */
enum {
    VP_GXM_EXTRA_SOURCE_NONE = 0,
    VP_GXM_EXTRA_SOURCE_CPU = 1,
    VP_GXM_EXTRA_SOURCE_GPU = 2,
    /* A real owned depth-pool surface.  The worker creates a tiled texture
     * view over that allocation; guest depth is never decoded on the CPU. */
    VP_GXM_EXTRA_SOURCE_DEPTH = 3
};

typedef struct {
    const void *texels;
    /* Nonzero only when texels is a live borrowed pointer returned by the
     * VPTextureCache decoder (guest_svc). A caller that decoded into its own
     * malloc/GPU/depth buffer must leave this zero: a pointer alone is not
     * proof of a decoder-owned immutable source. */
    unsigned texels_owned;
    unsigned width,height;
    unsigned tex_format;
    unsigned cube;
    unsigned mip_count;
    uint32_t sampler[2];
    uint64_t va;
    /* Content source and the guest TIC identity behind it. These fields stay
     * valid when texels is NULL (for example a GPU-produced render target with
     * no supported CPU layout). */
    unsigned source;
    unsigned tic_format;
    unsigned tic_header;
    unsigned texture_type;
    unsigned depth;
    unsigned block_depth;
    unsigned max_mip_level;
    unsigned block_height;
    uint32_t swizzle;
    /* Complete raw descriptors.  The depth-view planner parses these itself;
     * the partial sampler[2] view is not sufficient to prove a view. */
    uint32_t tic_words[8];
    uint32_t tsc_words[8];
    /* Sidecar descriptor type (VP_RESOURCE_TEX_*), 0 when no layout was used. */
    unsigned descriptor_type;
    /* Sidecar shader descriptor depth flag (0/1).  Distinct from the TIC
     * depth/layer field above: an ordinary 2D texture has TIC depth 1 and
     * descriptor depth 0.  Only 1 selects the software-shadow contract. */
    unsigned descriptor_depth;
    /* Sidecar shader-usage flags (VP_RESOURCE_TEXTURE_FLAG_*). */
    unsigned usage_flags;
    size_t storage;
    int resolve_rc;
} VPGxmExtraTexture;

/* Guest depth/stencil binding taken verbatim from the Maxwell methods:
 * 0x3f8/0x3f9 address, 0x3fa format, 0x3fb tile mode, 0x3fc array pitch,
 * 0x48a/0x48b size, 0x48c depth low16/dim_control bit16, 0x54e enable.
 * The runtime validates this through the host-testable gxm_depth planner and
 * never substitutes a private colour-surface depth when a zeta VA is present. */
typedef struct {
    uint32_t enable;
    uint64_t va;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t dim_control;
    uint32_t format;
    uint32_t tile_mode;
    uint32_t array_pitch;
    /* Maxwell stencil_enable (method 0x4e0). Carried only so a fully masked
     * draw with potential stencil effects is refused instead of reported. */
    uint32_t stencil_enable;
} VPGxmGuestDepth;

typedef struct {
    int init_rc;
    int alloc_rc;
    int map_rc;
    int fill_rc;
    int finish_rc;
    int display_rc;
    unsigned shader_files;
    unsigned shader_registered;
    int shader_rc;
    int program_rc;
    int draw_rc;
    uint32_t draw_pixel;
    uint32_t pixel;
} vp_gxm_test_result;

/* Cumulative guest-side counters used by the present-time frame budget. They
 * are read cross-thread for diagnostics only; a torn 64-bit sample can perturb
 * one log line, never a decision. */
typedef struct {
    uint64_t entries, draws, decode_us, draw_prepare_us;
    /* Inclusive wall time of guest GXM requests that executed inline on the
     * command worker. This contains worker-side preparation, encoding and any
     * synchronous completion wait; it is not a queue/submit wait. Since B104
     * the worker keeps the session lock across the decoder entry, so this is
     * the actual vp_gxm_* call time; device_release_us/device_reacquire_us no
     * longer advance on this path. segment_acquire_us/segment_release_us are
     * the single worker-entry/exit session lock handoffs, outside decode_us. */
    uint64_t gxm_call_us;
    uint64_t device_release_us, gxm_request_core_us, device_reacquire_us;
    uint64_t segment_acquire_us, segment_release_us;
    /* B112 outer wall around a worker-backed decoder entry. Each is a nested
     * observation outside decode_us, not a mutually exclusive residual term:
     *   gate_wait_us          actual gpu_execution_gate mutex block
     *   worker_queue_us       caller enqueue to worker callback entry
     *   worker_postwake_us    worker callback end to submitting-thread return
     *   caller_reacquire_us   submitting thread's post-worker Session lock
     * external_* cover only the !on_worker path (present/flush) and are kept
     * separate from the worker path's zero device_release/reacquire fields. */
    uint64_t gate_wait_us;
    uint64_t worker_queue_us, worker_postwake_us, caller_reacquire_us;
    uint64_t external_release_us, external_core_us, external_reacquire_us;
    /* B112 coarse decoder phases, timed once per entry and nested in
     * decode_us. command_dispatch_us contains draw/clear/GXM work;
     * submission_finish_us is frame_finish_submission. command_words counts
     * copied entry words; decoded_writes counts only successful collects. */
    uint64_t entry_setup_us, command_copy_us, command_collect_us;
    uint64_t command_dispatch_us, submission_finish_us;
    uint64_t command_words, decoded_writes;
    uint64_t tex_requests, tex_hits, tex_misses;
    uint64_t tex_decode_us, tex_lookup_us;
    /* Miss-only post-decode swizzle + raw cache copy, play-perf gated. Measured
     * after tex_decode_us and not folded into it; both are nested in
     * draw_prepare_us. */
    uint64_t tex_update_tail_us;
    uint64_t tex_family_us[5], tex_family_bytes[5];
    uint64_t tex_miss_changed, tex_miss_absent, tex_miss_gpu_only;
    uint64_t tex_compare_bytes, tex_compare_calls;
} VPGuestFramePerf;
void vp_guest_frame_perf(VPGuestFramePerf *out);
/* The selected work logs and this snapshot are owned by the GXM worker. */
void vp_guest_work_log_stats(VPWorkLogStats *out);

typedef struct {
    int applied;
    unsigned memory_mode;      /* 0 = 64 MiB unified, 1 = 128 MiB unified, 2 = split */
    unsigned raw_class;
    size_t budget_bytes, used_bytes, peak_used_bytes;
    unsigned live_slots, peak_live_slots, gpu_refs, cpu_pins;
    uint64_t evictions, pressure_events, nomem_events;
    uint64_t alloc_map_calls, free_map_calls;
} VPGuestTextureMemoryPerf;
/* Snapshot of the applied deep texture memory configuration. Reads only
 * published atomics and counter fields; it never switches mode or allocates. */
void vp_guest_texture_memory_perf(VPGuestTextureMemoryPerf *out);

/* Bounded nested frame-cost profile. All entry points must run on the GXM
 * worker; other threads do not record spans and must not read the control
 * file. The boundary poll applies a mode change only while no span is open.
 * begin/end return the timestamp they captured (0 when profiling is off), so
 * an inactive run performs no clock call. */
void vp_gxm_frame_cost_worker_boundary(void);
int vp_gxm_frame_cost_active(void);
uint64_t vp_gxm_frame_cost_now(void);
uint64_t vp_gxm_frame_cost_category_begin(unsigned category);
void vp_gxm_frame_cost_category_end(unsigned category, uint64_t started_us);
uint64_t vp_gxm_frame_cost_log_begin(void);
int vp_gxm_frame_cost_resource_audit(uint64_t *frame);

/* Register every GXP listed in <dir>/index.txt. Returns 0 when all registered. */
int vp_gxm_shader_register_all(const char *dir, unsigned *files,
                               unsigned *registered, int *first_error);
int vp_gxm_shader_has_fragment_sampler(unsigned hash, unsigned viewport_transform,
                                       unsigned sampler_index);
int vp_gxm_shader_has_vertex_sampler(unsigned hash, unsigned viewport_transform,
                                     unsigned sampler_index);
/* Static package state: 1 = index + validated resource sidecar, 0 = legacy
 * hash bundle, -1 = a static index is active but its sidecar failed. */
int vp_gxm_static_resources_active(void);
/* Resolve one declared sampler's compiler descriptor through the exact stage
 * SHA of token + gpu_stage (0 = vertex, 4 = fragment). Returns 0 and fills out;
 * 1 when the registered GXP does not declare that sampler (unused); negative
 * when the package has no validated metadata for the stage/slot. */
int vp_gxm_shader_resource(unsigned token, unsigned gpu_stage,
                           unsigned viewport_transform, unsigned slot,
                           VPResourceTexture *out);
/* One bounded plan for a whole stage: all tex0..tex4 declarations, the current
 * resources-active state and each declared slot's original shader_resource
 * return. Returns 0 on success; -1 for bad arguments; -3 when no static index
 * is active (the caller must use the legacy per-slot path). The plan contains
 * value copies only and is invalidated by resident/index generation changes. */
int vp_gxm_shader_resource_plan(unsigned token, unsigned gpu_stage,
                                unsigned viewport_transform,
                                VPGxmShaderResourcePlan *out);

typedef struct {
    const void *vertices;
    unsigned count;
    unsigned stride;
    /* Legacy single-token diagnostic path. Static mode resolves vs_token and
     * fs_token independently through the offline program index; hash remains
     * only for logging and the pre-index compatibility path. */
    unsigned hash;
    unsigned vs_token;
    unsigned fs_token;
    unsigned viewport_transform;
    uint32_t attributes[16];
    float uniforms[4];
    int has_uniforms;
    VPGxmUniform stage_cbuf[2][18];
    const void *texels;
    /* See VPGxmExtraTexture.texels_owned. Only the primary texture borrowed
     * from the VPTextureCache decoder sets this; a malloc decode does not. */
    unsigned texels_owned;
    unsigned tex_width;
    unsigned tex_height;
    unsigned tex_format;
    uint32_t sampler[2];
    /* Fragment tex1..tex3 handles. tex0 has its own fields above; the array is
     * sized from the single named sampler bound in gxm_sampler_slots.h. */
    VPGxmExtraTexture extra_textures[VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u];
    /* Vertex-stage resources are distinct from the fragment tex0..tex3 set.
     * The captured character shader samples tex0 while fetching vertex data,
     * so its descriptor must survive through the synchronous draw. */
    VPGxmExtraTexture vertex_texture;
    unsigned screen_width;
    unsigned screen_height;
    int rc;
    uint32_t center;
    unsigned nonzero;
    /* Full-frame extension (M2 G2/G3/G4). */
    int frame_draw;
    uint64_t target_va;
    unsigned target_width;
    unsigned target_height;
    uint32_t target_format;
    VPGxmGuestDepth depth_state;
    int indexed;
    uint32_t topology;
    const void *indices;
    unsigned index_count;
    uint32_t index_format;
    /* Raw minimum index removed from every staged index. The vertex stream
     * already points at the effective source window (base vertex applied), so
     * subtracting this raw minimum preserves the original fetched vertices. */
    uint32_t index_window_first;
    /* Maxwell global_base_vertex_index (method 0x50d), applied once by the
     * submitter when it computes the source window. Diagnostic only here. */
    int32_t base_vertex;
    /* Method 0x446 (vertex id base) and 0x587 (VertexIdCopy). The static
     * catalogue rejects every gl_VertexID shader, so accepted programs cannot
     * observe index identity; a nonzero copy enable is still gated in the
     * window planner and logged rather than silently rebased. */
    uint32_t vertex_id_base;
    uint32_t vertex_id_copy;
    uint32_t vertex_id_copy_slot;
    uint32_t output_vertex_id;
    uint32_t restart_enabled;
    uint32_t restart_index;
    /* Proof from the submitter's one index scan while planning the vertex
     * window. index_plan_valid is set only when that scan covered this exact
     * request's index range; index_plan_restart_count is then its marker count.
     * A valid zero lets the encoder skip strip joining, marker splitting and
     * the 2x index reservation. Unplanned requests keep the raw
     * restart_enabled value, and a plan with markers keeps it too. */
    uint32_t index_plan_restart_count;
    uint32_t index_plan_valid;
    uint64_t texture_va;
    /* Primary fragment sampler identity.  tex_format is the CPU-upload format;
     * the raw TIC/TSC words and the parsed fields are the guest descriptor.
     * texture_source is one of VP_GXM_EXTRA_SOURCE_* and decides whether the
     * worker uses a CPU payload, a colour producer or a depth-pool view. */
    unsigned texture_source;
    unsigned tex_descriptor_type;
    uint32_t tex_tic[8];
    uint32_t tex_tsc[8];
    unsigned tex_tic_format;
    unsigned tex_usage_flags;
    unsigned tex_header;
    unsigned tex_texture_type;
    unsigned tex_depth;
    /* Sidecar shader descriptor depth flag; 0 when no layout was used. */
    unsigned tex_descriptor_depth;
    unsigned tex_block_depth;
    unsigned tex_max_mip_level;
    unsigned tex_block_height;
    uint32_t tex_swizzle;
    size_t tex_storage;
    uint32_t viewport_x;
    uint32_t viewport_y;
    uint32_t viewport_width;
    uint32_t viewport_height;
    /* Guest Maxwell viewport_transform[0] z state (methods 0x282/0x285).
     * The converted vertex shaders write the guest clip z unchanged, so GXM
     * must apply the guest's own window-depth transform instead of the
     * OpenGL-default 0.5/0.5 depth range. viewport_z_valid is zero when the
     * guest never wrote the transform (or viewport_scale_offset is off);
     * the executor then keeps the previous 0.5/0.5 fallback. */
    uint32_t viewport_scale_z;
    uint32_t viewport_translate_z;
    uint32_t viewport_z_valid;
    uint32_t depth_test;
    int depth_bias_factor;
    int depth_bias_units;
    uint32_t depth_write;
    uint32_t depth_func;
    uint32_t alpha_test_enable;
    uint32_t alpha_test_func;
    float alpha_test_ref;
    uint32_t cull_enable;
    uint32_t front_face;
    uint32_t cull_face;
    uint32_t color_mask;
    /* Effective-attachment evidence from RT_CONTROL (0x487) and the raw mask
     * methods. color_mask above is the effective mask: zero when no colour
     * attachment exists, so the depth planner can classify the draw correctly
     * without consulting stale RT0 address/dimensions. */
    uint32_t color_mask_raw;
    uint32_t color_mask_common;
    uint32_t rt_control;
    uint32_t active_rt_index;
    uint32_t color_attachment;
    uint32_t color_mrt_unsupported;
    uint32_t color_mapping_unsupported;
    uint32_t blend_color_op;
    uint32_t blend_color_src;
    uint32_t blend_color_dst;
    uint32_t blend_alpha_op;
    uint32_t blend_alpha_src;
    uint32_t blend_alpha_dst;
    uint32_t scissor_enable;
    uint32_t scissor_min_x;
    uint32_t scissor_max_x;
    uint32_t scissor_min_y;
    uint32_t scissor_max_y;
    uint32_t draw_index;
    /* Bounded intra-submission batching. batch_scope is owned by the guest GPU
     * submission (fd/submission); it prevents one channel's open scene from
     * absorbing a draw from another submission. batch_encoded is set by the
     * worker only after the draw was encoded into the open scene; completion is
     * published later, at the flush. */
    uint64_t batch_scope;
    uint32_t batch_encoded;
    uint32_t batch_fallback;
    /* Display bridge: this draw targets a preallocated VI slot, so its
     * viewport contributes to the rectangle copied to the screen. quiet
     * suppresses per-draw success logging/surface scans in continuous play;
     * failures are still reported. */
    unsigned display_target;
    unsigned quiet;
} VPGxmDrawRequest;

typedef struct {
    uint64_t target_va;
    unsigned width;
    unsigned height;
    uint32_t format;
    uint32_t mask;
    uint32_t color;
    uint32_t depth_clear;
    uint32_t stencil_clear;
    VPGxmGuestDepth depth_state;
    unsigned quiet;
    int rc;
} VPGxmClearRequest;

/* Start the GXM worker thread and run the bring-up on it. Returns 0 when the
 * bring-up succeeded. All GXM calls happen on this one thread. */
int vp_gxm_worker_start(void);
float vp_gxm_native_font_scale(void);
/* Execute one command-stream entry synchronously on the context-owning worker.
 * Nested GXM requests execute directly there, without another thread handoff. */
int vp_gxm_worker_execute(void (*execute)(void *), void *user);
int vp_gxm_worker_is_current(void);

/* Snapshot the diagnostic log sink counters. The sink is initialized on first
 * use (worker start or any direct runtime entry); a failed open or write is
 * also signalled once through sceClibPrintf as VP_GXM_LOG_FAIL. Returns 0 on
 * success, -1 for a null argument or an in-progress first initialization. */
int vp_gxm_log_get_stats(VPLogSinkStats *out);

/* Queue one draw on the worker and wait for its result. Returns -1 when the
 * worker is not running. */
int vp_gxm_worker_submit(VPGxmDrawRequest *request);

/* Full-frame clear: create/lookup the surface named by target_va and apply the
 * Maxwell clear mask. Synchronous on the GXM worker. The A/B mode-6
 * experiment may instead record an admitted full-colour clear and materialize
 * it at a later consumption point; depth/stencil and every unadmitted request
 * keep the original synchronous path. */
int vp_gxm_frame_clear(VPGxmClearRequest *request);

/* Present and dump the surface written by the last full-frame draw. Called when
 * the submission's event group ends. */
int vp_gxm_frame_finish(void);

/* Flush the current bounded draw batch on the GXM worker. The guest GPU
 * submission calls this before retiring its fence, so encoded draws are never
 * acknowledged as complete early. */
int vp_gxm_batch_flush(const char *reason);

/* Decoder->GXM retirement event. The VPTextureCache decoder must call this on
 * the worker BEFORE it rewrites or frees a decoded RGBA allocation that it
 * previously returned as a borrowed texture. It clears only the owned-source
 * associations in the upload cache, both scene-snapshot arenas and the
 * draw-local memo; snapshot/upload bytes and GPU backing lifetime are not
 * changed, so an earlier scene keeps its immutable copies until completion. */
void vp_gxm_texture_owned_retire(const void *source);

/* Deep texture-pipeline support. Reusable regions back the pool. Decoded
 * regions use class 0 (uncached, GXM-mapped for read); split raw snapshots use
 * class 1 (existing cached C heap, never GXM-mapped). Reclaim
 * flushes the open scene and waits for every deferred notification so all
 * direct-texture leases are provably released before the pool is reused,
 * torn down or retried. */
int vp_gxm_texture_regions_init(size_t budget_bytes);
void *vp_gxm_texture_region_create(void *user, size_t bytes,
                                   unsigned allocation_class);
int vp_gxm_texture_region_release(void *user, void *base);
int vp_gxm_texture_regions_reset(void);
typedef struct {
    size_t budget_bytes, physical_bytes, peak_bytes;
    size_t mapped_physical_bytes, cached_physical_bytes;
    size_t mapped_allocated_bytes, cached_allocated_bytes;
    uint64_t kernel_allocs, kernel_frees, suballocs, subfrees;
    unsigned live, failed;
} VPGxmTextureRegionStats;
void vp_gxm_texture_region_stats(VPGxmTextureRegionStats *out);
int vp_gxm_texture_copy_cache_reset(void);
int vp_gxm_texture_pipeline_reclaim(void);
/* Invalidates CPU descriptors that may name a torn-down pool address. */
void vp_gxm_texture_descriptor_cache_reset(void);

/* Experiment switch for the texture-identity question. When
 * ux0:data/mhgu-vita-port/texture-identity-skip-bytes.enable exists, cache
 * lookups trust the full layout key + byte size and skip the exact byte
 * comparison. Default off; the marker and the decision are logged once.
 * This is not an accepted identity: use only for bounded experiments. */
int vp_gxm_texture_skip_bytes_enabled(void);

/* Display bridge: synchronously copy the pixels of the GXM surface named by
 * surface_va into the front display buffer and latch it with
 * sceDisplaySetFrameBuf. The copied rectangle is the union of the viewports of
 * draws that targeted this surface since the previous present. Returns 0 on a
 * completed present, -3 when the surface has no new displayed content, and a
 * negative value for any lookup/present failure. Never substitutes a test
 * pattern or a previous frame. */
int vp_gxm_display_present(uint64_t surface_va);

/* Re-latch the last GXM frame after a system applet removes an indirect layer.
 * This does not redraw or substitute pixels; it presents the retained display
 * buffer produced by the last successful frame_present_internal call. */
int vp_gxm_display_refresh(void);

/* Draw one real vertex buffer through the already-created 0xf9d programs, using
 * the given screen size for render_area. Returns 0 on success. */
int vp_gxm_game_draw(const void *vertices, unsigned count, unsigned stride,
                     unsigned hash, unsigned viewport_transform,
                     const uint32_t attributes[16],
                     unsigned screen_width, unsigned screen_height,
                     const float *fragment_uniforms,
                     const void *texels, unsigned tex_width, unsigned tex_height,
                     const uint32_t sampler[2],
                     const VPGxmUniform stage_cbuf[2][18],
                     uint32_t *center_pixel, unsigned *nonzero);

/* Returns 0 when initialize/map/fill/finish/display all return non-negative and
 * shader registration succeeds. */
int vp_gxm_runtime_test(uint32_t color, vp_gxm_test_result *out);

#endif

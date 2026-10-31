/* Guest depth-surface identity and bounded pool metadata.
 *
 * This helper is deliberately free of GXM/SDK dependencies so the identity,
 * validation, byte planning and eviction policy can be exercised on the host.
 * The running backend owns the actual memblocks and SceGxmDepthStencilSurface
 * objects; this file only decides whether a guest zeta binding is representable
 * and which pool slot it belongs to.
 *
 * Guest identity comes from the real Maxwell methods:
 *   zeta.address_high/low  methods 0x3f8/0x3f9
 *   zeta.format            method  0x3fa   (Tegra::DepthFormat, not a TIC enum)
 *   zeta.tile_mode         method  0x3fb
 *   zeta.array_pitch       method  0x3fc
 *   zeta_size.width        method  0x48a
 *   zeta_size.height       method  0x48b
 *   zeta_size.depth/dim    method  0x48c
 *   zeta_enable            method  0x54e
 */
#ifndef VP_GXM_DEPTH_H
#define VP_GXM_DEPTH_H

#include <stddef.h>
#include <stdint.h>

/* Eight identities is enough for a colour pass, a reused cross-colour depth
 * and several shadow-style producers, while keeping the fixed table small. */
#define VP_GXM_DEPTH_POOL_SLOTS 8u
/* A 2048x2048 S8D24 surface needs about 20 MiB including tile padding and
 * page-aligned native buffers. The cap holds one largest surface plus a
 * 1080p DF32 surface, and is far below the colour table's worst case
 * (32 * 2048 * 2048 * 4 = 512 MiB). A live depth identity is never evicted:
 * returning to the same zeta VA later must preserve its content. If all slots
 * or the byte cap are exhausted the request is refused explicitly. */
#define VP_GXM_DEPTH_BYTE_CAP (32u * 1024u * 1024u)
/* The verified shadow-atlas shape is 1024x3072 (DF32, 12 MiB). A 4096 ceiling
 * admits that shape and keeps the checked byte planner as the real budget;
 * it is not a general asset-size increase. */
#define VP_GXM_DEPTH_MAX_DIM 4096u
/* Shared GXM render targets may cover the same verified extent. The colour
 * surface table keeps its own smaller guard. */
#define VP_GXM_RENDER_TARGET_MAX_DIM 4096u
#define VP_GXM_DEPTH_STRIDE_ALIGN 64u
#define VP_GXM_DEPTH_TILE_ALIGN 32u
#define VP_GXM_DEPTH_ALLOC_ALIGN 4096u

enum {
    VP_GXM_DEPTH_CLASS_NONE = 0,
    VP_GXM_DEPTH_CLASS_DF32 = 1,
    VP_GXM_DEPTH_CLASS_S8D24 = 2
};

enum {
    VP_GXM_DEPTH_OK = 0,
    VP_GXM_DEPTH_DISABLED = 1,
    VP_GXM_DEPTH_ERR_ARG = -1,
    VP_GXM_DEPTH_ERR_VA = -2,
    VP_GXM_DEPTH_ERR_DIM = -3,
    VP_GXM_DEPTH_ERR_LAYERS = -4,
    VP_GXM_DEPTH_ERR_DIM_CONTROL = -5,
    VP_GXM_DEPTH_ERR_FORMAT = -6,
    VP_GXM_DEPTH_ERR_TILE_MODE = -7,
    VP_GXM_DEPTH_ERR_OVERFLOW = -8,
    VP_GXM_DEPTH_ERR_BUDGET = -9,
    VP_GXM_DEPTH_ERR_TARGET = -10,
    VP_GXM_DEPTH_ERR_POOL = -11,
    VP_GXM_DEPTH_ERR_VOLUME = -12,
    VP_GXM_DEPTH_ERR_STENCIL = -13,
    /* Clear-policy refusals (see vp_gxm_depth_clear_plan). */
    VP_GXM_DEPTH_CLEAR_ERR_STANDALONE = -14,
    VP_GXM_DEPTH_CLEAR_ERR_PRESERVE = -15
};

enum {
    VP_GXM_DEPTH_ACTION_REUSE = 0,
    VP_GXM_DEPTH_ACTION_CREATE = 1,
    VP_GXM_DEPTH_ACTION_RECREATE = 2
};

/* Maxwell DepthFormat values that this backend can represent faithfully.
 * Z16_UNORM is intentionally absent: neither DF32 nor S8D24 preserves its
 * precision. X8Z24/V8Z24/S8_UINT/Z32_FLOAT_X24S8_UINT are also rejected until
 * their native semantics are proven. */
#define VP_GXM_DEPTH_FORMAT_Z32_FLOAT 0xau
#define VP_GXM_DEPTH_FORMAT_Z24_UNORM_S8_UINT 0x14u
#define VP_GXM_DEPTH_FORMAT_S8Z24_UNORM 0x16u

typedef struct {
    uint64_t va;
    unsigned width;
    unsigned height;
    unsigned raw_depth;   /* zeta_size.depth low 16 bits, carried verbatim */
    unsigned dim_control; /* zeta_size bit 16: 0=define array, 1=one */
    unsigned layers;      /* effective array size; only 1 is supported */
    unsigned format;      /* Maxwell DepthFormat */
    unsigned tile_mode;   /* packed Maxwell TileMode bitfield, raw */
    unsigned tile_block_width;
    unsigned tile_block_height;
    unsigned tile_block_depth;
    unsigned tile_pitch_linear;
    unsigned tile_dim_control; /* TileMode bit 16: 0=array, 1=depth(3D) */
    unsigned array_pitch;      /* raw zeta array pitch; unused for one layer */
    unsigned format_class; /* VP_GXM_DEPTH_CLASS_* */
    unsigned stride_samples;
    unsigned storage_height; /* tile-aligned height used for native storage */
    size_t depth_bytes;      /* logical payload after tile padding */
    size_t stencil_bytes;
    size_t depth_alloc_bytes;   /* gpu_alloc page-aligned depth buffer */
    size_t stencil_alloc_bytes; /* gpu_alloc page-aligned stencil buffer */
    size_t total_alloc_bytes;   /* enforced against VP_GXM_DEPTH_BYTE_CAP */
} VPGxmDepthDesc;

typedef struct {
    unsigned used;
    VPGxmDepthDesc desc;
    unsigned long long last_use;
} VPGxmDepthPoolEntry;

typedef struct {
    VPGxmDepthPoolEntry entries[VP_GXM_DEPTH_POOL_SLOTS];
    unsigned slot_limit;
    size_t byte_cap;
    size_t bytes;
    unsigned long long clock;
} VPGxmDepthPool;

typedef struct {
    int slot; /* chosen slot, -1 on refusal */
    int action;
    unsigned evict_mask; /* slots whose runtime resources must be released */
    unsigned evicted_count;
    size_t evicted_bytes;
} VPGxmDepthPoolPlan;

/* Validate one guest zeta binding and compute the native allocation plan.
 * Returns VP_GXM_DEPTH_OK, VP_GXM_DEPTH_DISABLED, or a negative
 * VP_GXM_DEPTH_ERR_* status. A disabled binding leaves *out zeroed. */
int vp_gxm_depth_desc_build(VPGxmDepthDesc *out, unsigned enabled, uint64_t va,
                            unsigned width, unsigned height, unsigned depth,
                            unsigned dim_control, unsigned format,
                            unsigned tile_mode, unsigned array_pitch);

/* Checked byte planner. Returns VP_GXM_DEPTH_OK or ERR_ARG/ERR_OVERFLOW. */
int vp_gxm_depth_plan_bytes(unsigned stride_samples, unsigned height,
                            unsigned bytes_per_sample, size_t *out_bytes);

/* A native depth surface may be larger than the colour target it serves, but
 * never smaller: GXM addresses it through the render target's dimensions. */
int vp_gxm_depth_desc_fits(const VPGxmDepthDesc *desc, unsigned target_width,
                           unsigned target_height);

int vp_gxm_depth_pool_init(VPGxmDepthPool *pool, unsigned slot_limit,
                           size_t byte_cap);

/* Acquire/update metadata for an enabled descriptor. On success plan->slot is
 * the slot to populate; any slot in plan->evict_mask must be released before
 * creating (only the same-VA recreate case sets it). On refusal the pool is
 * unchanged, including its clock, and plan->slot is -1.
 * Reusing the same VA with the same storage identity (shape/format) is REUSE;
 * raw tile/pitch hints may be refreshed. The same VA with a different
 * shape/format is RECREATE: old contents cannot be reinterpreted and are
 * explicitly undefined. Distinct live identities are never evicted; a full
 * slot table or byte cap is an explicit refusal. All calls run on the single
 * GXM worker between synchronous submissions, after Finish. */
int vp_gxm_depth_pool_acquire(VPGxmDepthPool *pool,
                              const VPGxmDepthDesc *desc,
                              VPGxmDepthPoolPlan *plan);

/* Remove metadata for one slot (used by tests and to abort a failed native
 * allocation after acquire). */
void vp_gxm_depth_pool_release(VPGxmDepthPool *pool, unsigned slot);

/* Maxwell/GL depth semantics: a disabled depth test disables depth writes as
 * well, even when the raw depth_write_enable bit is set. The runtime applies
 * this effective value to native GXM so a fully masked draw is only classified
 * as side-effect-free after this normalization. */
unsigned vp_gxm_depth_effective_write(unsigned depth_test,
                                      unsigned depth_write);

/* One draw's colour/depth classification, independent of GXM. The runtime
 * calls this after building the guest zeta descriptor and before touching a
 * colour surface, scene, texture unit or the depth pool.
 *
 * A fully masked colour draw (color_mask == 0) with a real depth write is a
 * depth-only render: it must use the validated zeta extent and no colour
 * surface, even when the stale Maxwell RT0 binding names a different colour
 * size. A disabled test, a NEVER test or a disabled zeta has no write side
 * effect. Stencil test/write is not implemented, so any masked stencil state
 * on S8D24 is an explicit refusal rather than a silent success.
 *
 * For the colour path this function only reports the colour extent; the
 * existing colour fit guard remains in charge. */
typedef struct {
    unsigned color_mask;
    unsigned depth_test;
    unsigned depth_write_raw;
    unsigned depth_func;          /* Maxwell depth func, 1/0x200 = NEVER */
    unsigned stencil_enable;
    int depth_status;             /* vp_gxm_depth_desc_build result */
    unsigned depth_format_class;  /* VP_GXM_DEPTH_CLASS_* */
    unsigned color_width;
    unsigned color_height;
    unsigned depth_width;
    unsigned depth_height;
} VPGxmDepthDrawPlanInput;

typedef struct {
    unsigned depth_only;      /* 1 = execute a real colour-less depth draw */
    unsigned noop;            /* 1 = no side effect, return success (0) */
    unsigned refuse;          /* 1 = explicit negative return */
    unsigned effective_write; /* depth_test && raw write */
    unsigned color_surface;   /* 1 = keep the existing colour path */
    unsigned render_width;    /* selected real render extent */
    unsigned render_height;
    const char *reason;       /* static diagnostic string */
} VPGxmDepthDrawPlan;

int vp_gxm_depth_draw_plan(const VPGxmDepthDrawPlanInput *in,
                           VPGxmDepthDrawPlan *out);

/* Which colour/depth content markers one successfully synchronized draw may
 * update. A depth-only draw must never publish a colour producer, contribute
 * a display rectangle or become the last colour target. Callers apply this
 * only after EndScene/notification/Finish succeeded. */
enum {
    VP_GXM_DRAW_COMMIT_COLOR_DRAWN = 1u << 0,
    VP_GXM_DRAW_COMMIT_COLOR_DRAW_COUNT = 1u << 1,
    VP_GXM_DRAW_COMMIT_DISPLAY_RECT = 1u << 2,
    VP_GXM_DRAW_COMMIT_LAST_TARGET = 1u << 3,
    VP_GXM_DRAW_COMMIT_DEPTH_VALID = 1u << 4,
    VP_GXM_DRAW_COMMIT_DEPTH_DRAW_COUNT = 1u << 5
};

unsigned vp_gxm_draw_commit_mask(unsigned color_surface, unsigned color_mask,
                                 unsigned depth_bound);

/* Whether a draw may be presented to the display bridge. A depth-only draw
 * (colour mask zero) or a request with no colour target is never a display
 * producer, independent of binder bookkeeping. Callers must pass the guest
 * draw's colour mask directly so request-initialization ordering cannot make
 * every draw look depth-only. */
unsigned vp_gxm_display_target(unsigned bridge_quiet, unsigned color_mask,
                               uint64_t target_va, unsigned binder_display);

/* Decide how a guest clear mask (bit 0 depth, bit 1 stencil; colour bits are
 * ignored here) can be represented by native GXM.
 *
 * ForceStore is always enabled, but ForceLoad is a single flag covering depth
 * and stencil. With ForceLoad disabled, the locked GL backend clears BOTH
 * attachments to their backgrounds. There is therefore no independent
 * stencil-only clear:
 *   - stencil bit without depth bit  -> CLEAR_ERR_STANDALONE;
 *   - stencil bit with a DF32 format -> ERR_STENCIL;
 *   - depth-only on S8D24 while live stencil content exists
 *                                    -> CLEAR_ERR_PRESERVE (a depth-only clear
 *                                       would also reload stencil from the
 *                                       background);
 *   - both bits on S8D24             -> represented exactly; both backgrounds
 *                                       are applied and both contents reset.
 * A depth-only clear on S8D24 with no live stencil is permitted: the coupled
 * stencil load rewrites stencil to its unchanged background, which is exactly
 * the content this backend has (stencil test operations are not implemented),
 * and *out_stencil_coupled is set so the caller logs the coupling.
 * *out_apply_mask is the depth/stencil mask actually applied (0, 1 or 3). */
int vp_gxm_depth_clear_plan(unsigned mask, unsigned format_class,
                            unsigned stencil_live,
                            unsigned *out_apply_mask,
                            unsigned *out_stencil_coupled);

/* Clear-side target policy. A depth/stencil clear is executed against the
 * bound zeta's own dimensions, never the colour target's dimensions. The
 * colour dimensions are diagnostics only and must not invalidate the clear.
 * Returns the validated descriptor dimensions in *out_fit_width/height for the
 * caller's shape check, plus the mask/stencil decision from
 * vp_gxm_depth_clear_plan. A draw still uses vp_gxm_depth_desc_fits against the
 * colour target; this function does not relax that guard. */
int vp_gxm_depth_clear_target_plan(const VPGxmDepthDesc *desc,
                                   unsigned colour_width,
                                   unsigned colour_height, unsigned mask,
                                   unsigned stencil_live,
                                   unsigned *out_fit_width,
                                   unsigned *out_fit_height,
                                   unsigned *out_apply_mask,
                                   unsigned *out_stencil_coupled);

const char *vp_gxm_depth_status_name(int status);
const char *vp_gxm_depth_action_name(int action);

#endif

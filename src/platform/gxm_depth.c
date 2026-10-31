#include "gxm_depth.h"

#include <string.h>

static unsigned align_up(unsigned value, unsigned alignment) {
    return (value + alignment - 1u) / alignment * alignment;
}

static int align_up_bytes(size_t value, size_t alignment, size_t *out) {
    size_t remainder = value % alignment;
    size_t padding = remainder == 0u ? 0u : alignment - remainder;
    if (value > SIZE_MAX - padding) return VP_GXM_DEPTH_ERR_OVERFLOW;
    *out = value + padding;
    return VP_GXM_DEPTH_OK;
}

/* A depth identity changes native storage only for these fields. Tile-mode
 * hints, raw array-width fields and array pitch describe guest memory that
 * this backend does not mirror, so a hint-only change keeps the contents. */
static int same_storage_identity(const VPGxmDepthDesc *a,
                                 const VPGxmDepthDesc *b) {
    return a->va == b->va && a->width == b->width && a->height == b->height &&
           a->layers == b->layers && a->format == b->format &&
           a->stride_samples == b->stride_samples &&
           a->storage_height == b->storage_height;
}

int vp_gxm_depth_plan_bytes(unsigned stride_samples, unsigned height,
                            unsigned bytes_per_sample, size_t *out_bytes) {
    uint64_t bytes;
    if (out_bytes == NULL || stride_samples == 0u || height == 0u ||
        bytes_per_sample == 0u)
        return VP_GXM_DEPTH_ERR_ARG;
    /* Division checks catch multiplication overflow before the modular uint64
     * product, including when SIZE_MAX itself is UINT64_MAX. */
    if ((uint64_t)stride_samples > (uint64_t)SIZE_MAX / height)
        return VP_GXM_DEPTH_ERR_OVERFLOW;
    bytes = (uint64_t)stride_samples * (uint64_t)height;
    if ((uint64_t)bytes_per_sample > (uint64_t)SIZE_MAX / bytes)
        return VP_GXM_DEPTH_ERR_OVERFLOW;
    bytes *= (uint64_t)bytes_per_sample;
    *out_bytes = (size_t)bytes;
    return VP_GXM_DEPTH_OK;
}

int vp_gxm_depth_desc_build(VPGxmDepthDesc *out, unsigned enabled, uint64_t va,
                            unsigned width, unsigned height, unsigned depth,
                            unsigned dim_control, unsigned format,
                            unsigned tile_mode, unsigned array_pitch) {
    /* Maxwell Regs::TileMode is a packed 32-bit bitfield:
     *   0..3 block_width, 4..7 block_height, 8..11 block_depth,
     *   12 is_pitch_linear, 16 dim_control (0=array, 1=depth/3D).
     * Bits 13..15 and 17..31 are not defined by the primary structure. */
    const unsigned tile_known_mask = 0x00011fffu; /* bits 0..12 and 16 */
    unsigned depth_bpp = 0u, stencil_bpp = 0u;
    unsigned tile_dim_control;
    int rc;

    if (out == NULL) return VP_GXM_DEPTH_ERR_ARG;
    memset(out, 0, sizeof(*out));
    if (!enabled) return VP_GXM_DEPTH_DISABLED;
    if (va == 0u) return VP_GXM_DEPTH_ERR_VA;
    if (width == 0u || height == 0u || width > VP_GXM_DEPTH_MAX_DIM ||
        height > VP_GXM_DEPTH_MAX_DIM)
        return VP_GXM_DEPTH_ERR_DIM;
    if (dim_control > 1u) return VP_GXM_DEPTH_ERR_DIM_CONTROL;
    if ((tile_mode & ~tile_known_mask) != 0u)
        return VP_GXM_DEPTH_ERR_TILE_MODE;
    tile_dim_control = (tile_mode >> 16) & 1u;
    /* TileMode::DefineDepthSize makes the zeta a 3D/volume depth surface.
     * Layer offsets and volume slices are not representable here. */
    if (tile_dim_control != 0u) return VP_GXM_DEPTH_ERR_VOLUME;
    if (dim_control == 1u) {
        /* ZetaSize::ArraySizeIsOne: the depth field is not a layer count. */
        out->layers = 1u;
    } else {
        /* ZetaSize::DefineArraySize: the depth field is the array size. */
        if (depth != 1u) return VP_GXM_DEPTH_ERR_LAYERS;
        out->layers = 1u;
    }
    switch (format) {
    case VP_GXM_DEPTH_FORMAT_Z32_FLOAT:
        out->format_class = VP_GXM_DEPTH_CLASS_DF32;
        depth_bpp = 4u;
        stencil_bpp = 0u;
        break;
    case VP_GXM_DEPTH_FORMAT_Z24_UNORM_S8_UINT:
    case VP_GXM_DEPTH_FORMAT_S8Z24_UNORM:
        /* Both Maxwell encodings describe 24-bit UNORM depth plus an 8-bit
         * stencil. The backend owns separate depth/stencil allocations and
         * never reinterprets the guest's packed zeta memory, so the guest
         * packing order is not a sampling conversion here. */
        out->format_class = VP_GXM_DEPTH_CLASS_S8D24;
        depth_bpp = 4u;
        stencil_bpp = 1u;
        break;
    default:
        return VP_GXM_DEPTH_ERR_FORMAT;
    }
    out->va = va;
    out->width = width;
    out->height = height;
    out->raw_depth = depth;
    out->dim_control = dim_control;
    out->format = format;
    out->tile_mode = tile_mode;
    out->tile_block_width = tile_mode & 0xfu;
    out->tile_block_height = (tile_mode >> 4) & 0xfu;
    out->tile_block_depth = (tile_mode >> 8) & 0xfu;
    out->tile_pitch_linear = (tile_mode >> 12) & 1u;
    out->tile_dim_control = tile_dim_control;
    out->array_pitch = array_pitch;
    out->stride_samples = align_up(width, VP_GXM_DEPTH_STRIDE_ALIGN);
    /* Tiled native storage occupies whole 32x32 tiles; a 1080-row surface
     * therefore has 1088 native rows. Vita3K's tiled texture path uses the
     * same 32x32 tile geometry (SCE_GXM_TILE_SIZEY == 32). */
    out->storage_height = align_up(height, VP_GXM_DEPTH_TILE_ALIGN);
    rc = vp_gxm_depth_plan_bytes(out->stride_samples, out->storage_height,
                                 depth_bpp, &out->depth_bytes);
    if (rc != VP_GXM_DEPTH_OK) return rc;
    if (stencil_bpp != 0u) {
        rc = vp_gxm_depth_plan_bytes(out->stride_samples, out->storage_height,
                                     stencil_bpp, &out->stencil_bytes);
        if (rc != VP_GXM_DEPTH_OK) return rc;
    }
    rc = align_up_bytes(out->depth_bytes, VP_GXM_DEPTH_ALLOC_ALIGN,
                        &out->depth_alloc_bytes);
    if (rc != VP_GXM_DEPTH_OK) return rc;
    if (out->stencil_bytes != 0u) {
        rc = align_up_bytes(out->stencil_bytes, VP_GXM_DEPTH_ALLOC_ALIGN,
                            &out->stencil_alloc_bytes);
        if (rc != VP_GXM_DEPTH_OK) return rc;
    }
    if (out->stencil_alloc_bytes > SIZE_MAX - out->depth_alloc_bytes)
        return VP_GXM_DEPTH_ERR_OVERFLOW;
    out->total_alloc_bytes = out->depth_alloc_bytes + out->stencil_alloc_bytes;
    if (out->total_alloc_bytes == 0u) return VP_GXM_DEPTH_ERR_OVERFLOW;
    return VP_GXM_DEPTH_OK;
}

int vp_gxm_depth_desc_fits(const VPGxmDepthDesc *desc, unsigned target_width,
                           unsigned target_height) {
    if (desc == NULL || desc->format_class == VP_GXM_DEPTH_CLASS_NONE)
        return 0;
    if (target_width == 0u || target_height == 0u) return 0;
    return desc->width >= target_width && desc->height >= target_height;
}

int vp_gxm_depth_pool_init(VPGxmDepthPool *pool, unsigned slot_limit,
                           size_t byte_cap) {
    if (pool == NULL || slot_limit == 0u ||
        slot_limit > VP_GXM_DEPTH_POOL_SLOTS || byte_cap == 0u)
        return VP_GXM_DEPTH_ERR_ARG;
    memset(pool, 0, sizeof(*pool));
    pool->slot_limit = slot_limit;
    pool->byte_cap = byte_cap;
    return VP_GXM_DEPTH_OK;
}

int vp_gxm_depth_pool_acquire(VPGxmDepthPool *pool,
                              const VPGxmDepthDesc *desc,
                              VPGxmDepthPoolPlan *plan) {
    int same = -1;
    int free_slot = -1;
    unsigned i;

    if (pool == NULL || desc == NULL || plan == NULL) return VP_GXM_DEPTH_ERR_ARG;
    memset(plan, 0, sizeof(*plan));
    plan->slot = -1;
    plan->action = VP_GXM_DEPTH_ACTION_REUSE;
    if (pool->slot_limit == 0u || pool->slot_limit > VP_GXM_DEPTH_POOL_SLOTS ||
        pool->byte_cap == 0u || desc->format_class == VP_GXM_DEPTH_CLASS_NONE ||
        desc->total_alloc_bytes == 0u)
        return VP_GXM_DEPTH_ERR_ARG;
    if (desc->total_alloc_bytes > pool->byte_cap)
        return VP_GXM_DEPTH_ERR_BUDGET;

    for (i = 0u; i < pool->slot_limit; i++) {
        if (!pool->entries[i].used) {
            if (free_slot < 0) free_slot = (int)i;
            continue;
        }
        if (pool->entries[i].desc.va == desc->va) same = (int)i;
    }
    if (same >= 0 &&
        same_storage_identity(&pool->entries[same].desc, desc)) {
        /* Same native storage identity: preserve content. Only raw guest
         * layout hints are refreshed for diagnostics. */
        pool->clock++;
        pool->entries[same].desc.tile_mode = desc->tile_mode;
        pool->entries[same].desc.tile_block_width = desc->tile_block_width;
        pool->entries[same].desc.tile_block_height = desc->tile_block_height;
        pool->entries[same].desc.tile_block_depth = desc->tile_block_depth;
        pool->entries[same].desc.tile_pitch_linear = desc->tile_pitch_linear;
        pool->entries[same].desc.tile_dim_control = desc->tile_dim_control;
        pool->entries[same].desc.array_pitch = desc->array_pitch;
        pool->entries[same].desc.raw_depth = desc->raw_depth;
        pool->entries[same].desc.dim_control = desc->dim_control;
        pool->entries[same].last_use = pool->clock;
        plan->slot = same;
        plan->action = VP_GXM_DEPTH_ACTION_REUSE;
        return VP_GXM_DEPTH_OK;
    }

    if (same >= 0) {
        /* Same VA, different native shape/format: the old contents cannot be
         * reinterpreted. Recreating is explicit; the caller logs that the old
         * depth content is undefined. The pool is unchanged on refusal. */
        size_t old_bytes = pool->entries[same].desc.total_alloc_bytes;
        if (desc->total_alloc_bytes > pool->byte_cap ||
            pool->bytes - old_bytes >
                pool->byte_cap - desc->total_alloc_bytes)
            return VP_GXM_DEPTH_ERR_BUDGET;
        pool->clock++;
        pool->bytes -= old_bytes;
        pool->entries[same].desc = *desc;
        pool->entries[same].used = 1u;
        pool->entries[same].last_use = pool->clock;
        pool->bytes += desc->total_alloc_bytes;
        plan->slot = same;
        plan->action = VP_GXM_DEPTH_ACTION_RECREATE;
        plan->evict_mask = 1u << (unsigned)same;
        plan->evicted_count = 1u;
        plan->evicted_bytes = old_bytes;
        return VP_GXM_DEPTH_OK;
    }

    if (free_slot < 0) return VP_GXM_DEPTH_ERR_POOL;
    if (pool->bytes > pool->byte_cap - desc->total_alloc_bytes)
        return VP_GXM_DEPTH_ERR_BUDGET;
    pool->clock++;
    pool->entries[free_slot].used = 1u;
    pool->entries[free_slot].desc = *desc;
    pool->entries[free_slot].last_use = pool->clock;
    pool->bytes += desc->total_alloc_bytes;
    plan->slot = free_slot;
    plan->action = VP_GXM_DEPTH_ACTION_CREATE;
    return VP_GXM_DEPTH_OK;
}

void vp_gxm_depth_pool_release(VPGxmDepthPool *pool, unsigned slot) {
    if (pool == NULL || slot >= pool->slot_limit) return;
    if (!pool->entries[slot].used) return;
    pool->bytes -= pool->entries[slot].desc.total_alloc_bytes;
    memset(&pool->entries[slot], 0, sizeof(pool->entries[slot]));
}

unsigned vp_gxm_depth_effective_write(unsigned depth_test,
                                      unsigned depth_write) {
    return depth_test != 0u && depth_write != 0u;
}

int vp_gxm_depth_draw_plan(const VPGxmDepthDrawPlanInput *in,
                           VPGxmDepthDrawPlan *out) {
    unsigned stencil_effect;
    unsigned depth_effect;

    if (in == NULL || out == NULL) return VP_GXM_DEPTH_ERR_ARG;
    memset(out, 0, sizeof(*out));
    out->effective_write = vp_gxm_depth_effective_write(in->depth_test,
                                                        in->depth_write_raw);
    out->reason = "colour-path";
    if (in->color_mask != 0u) {
        /* The existing colour render path is unchanged: it uses its own
         * colour target, fit guard and surface bookkeeping. */
        out->color_surface = 1u;
        out->render_width = in->color_width;
        out->render_height = in->color_height;
        return VP_GXM_DEPTH_OK;
    }
    if (in->depth_status != VP_GXM_DEPTH_OK &&
        in->depth_status != VP_GXM_DEPTH_DISABLED) {
        out->refuse = 1u;
        out->reason = "invalid-depth-metadata";
        return VP_GXM_DEPTH_OK;
    }
    if (in->depth_status == VP_GXM_DEPTH_DISABLED) {
        /* No depth surface is bound, so neither depth nor stencil state can
         * have a target side effect. */
        out->noop = 1u;
        out->reason = "side-effect-free";
        return VP_GXM_DEPTH_OK;
    }
    stencil_effect = in->stencil_enable != 0u &&
                     in->depth_format_class == VP_GXM_DEPTH_CLASS_S8D24;
    /* NEVER (Maxwell 1 / 0x200) can never pass a fragment, so it cannot write
     * depth. A disabled test already zeroed effective_write. */
    depth_effect = out->effective_write != 0u &&
                   in->depth_func != 1u && in->depth_func != 0x200u;
    if (stencil_effect) {
        out->refuse = 1u;
        out->reason = depth_effect ? "masked-depth-stencil" :
                                     "masked-stencil-state";
        return VP_GXM_DEPTH_OK;
    }
    if (!depth_effect) {
        out->noop = 1u;
        out->reason = "side-effect-free";
        return VP_GXM_DEPTH_OK;
    }
    /* Real depth-only production: the validated zeta owns the render extent.
     * The stale Maxwell RT0 colour dimensions are not consulted. */
    out->depth_only = 1u;
    out->color_surface = 0u;
    out->render_width = in->depth_width;
    out->render_height = in->depth_height;
    out->reason = "depth-only";
    return VP_GXM_DEPTH_OK;
}

unsigned vp_gxm_draw_commit_mask(unsigned color_surface, unsigned color_mask,
                                 unsigned depth_bound) {
    unsigned mask = 0u;
    if (color_surface != 0u && color_mask != 0u) {
        mask |= VP_GXM_DRAW_COMMIT_COLOR_DRAWN;
        mask |= VP_GXM_DRAW_COMMIT_COLOR_DRAW_COUNT;
        mask |= VP_GXM_DRAW_COMMIT_DISPLAY_RECT;
        mask |= VP_GXM_DRAW_COMMIT_LAST_TARGET;
    }
    if (depth_bound != 0u) {
        mask |= VP_GXM_DRAW_COMMIT_DEPTH_VALID;
        mask |= VP_GXM_DRAW_COMMIT_DEPTH_DRAW_COUNT;
    }
    return mask;
}

unsigned vp_gxm_display_target(unsigned bridge_quiet, unsigned color_mask,
                               uint64_t target_va, unsigned binder_display) {
    return bridge_quiet != 0u && color_mask != 0u && target_va != 0u &&
           binder_display != 0u;
}

int vp_gxm_depth_clear_plan(unsigned mask, unsigned format_class,
                            unsigned stencil_live,
                            unsigned *out_apply_mask,
                            unsigned *out_stencil_coupled) {
    unsigned bits = mask & 3u;

    if (out_apply_mask == NULL || out_stencil_coupled == NULL)
        return VP_GXM_DEPTH_ERR_ARG;
    *out_apply_mask = 0u;
    *out_stencil_coupled = 0u;
    if (bits == 0u) return VP_GXM_DEPTH_OK;
    if (format_class != VP_GXM_DEPTH_CLASS_DF32 &&
        format_class != VP_GXM_DEPTH_CLASS_S8D24)
        return VP_GXM_DEPTH_ERR_ARG;
    if ((bits & 2u) != 0u) {
        if (format_class != VP_GXM_DEPTH_CLASS_S8D24)
            return VP_GXM_DEPTH_ERR_STENCIL;
        if ((bits & 1u) == 0u)
            return VP_GXM_DEPTH_CLEAR_ERR_STANDALONE;
        *out_apply_mask = 3u;
        return VP_GXM_DEPTH_OK;
    }
    /* Depth-only clear. */
    if (format_class == VP_GXM_DEPTH_CLASS_S8D24) {
        if (stencil_live != 0u) return VP_GXM_DEPTH_CLEAR_ERR_PRESERVE;
        *out_stencil_coupled = 1u;
    }
    *out_apply_mask = 1u;
    return VP_GXM_DEPTH_OK;
}

int vp_gxm_depth_clear_target_plan(const VPGxmDepthDesc *desc,
                                   unsigned colour_width,
                                   unsigned colour_height, unsigned mask,
                                   unsigned stencil_live,
                                   unsigned *out_fit_width,
                                   unsigned *out_fit_height,
                                   unsigned *out_apply_mask,
                                   unsigned *out_stencil_coupled) {
    /* Colour dimensions are deliberately not compared: a 1920x1080 colour
     * clear may clear a 1280x720 bound zeta and vice versa. Only the zeta's
     * own validated dimensions feed the native depth-surface shape check. */
    (void)colour_width;
    (void)colour_height;
    if (out_fit_width == NULL || out_fit_height == NULL ||
        out_apply_mask == NULL || out_stencil_coupled == NULL)
        return VP_GXM_DEPTH_ERR_ARG;
    *out_fit_width = 0u;
    *out_fit_height = 0u;
    *out_apply_mask = 0u;
    *out_stencil_coupled = 0u;
    if (desc == NULL || desc->format_class == VP_GXM_DEPTH_CLASS_NONE ||
        desc->width == 0u || desc->height == 0u)
        return VP_GXM_DEPTH_ERR_ARG;
    *out_fit_width = desc->width;
    *out_fit_height = desc->height;
    return vp_gxm_depth_clear_plan(mask, desc->format_class, stencil_live,
                                   out_apply_mask, out_stencil_coupled);
}

const char *vp_gxm_depth_status_name(int status) {
    switch (status) {
    case VP_GXM_DEPTH_OK: return "ok";
    case VP_GXM_DEPTH_DISABLED: return "disabled";
    case VP_GXM_DEPTH_ERR_ARG: return "bad-argument";
    case VP_GXM_DEPTH_ERR_VA: return "zero-va";
    case VP_GXM_DEPTH_ERR_DIM: return "dimensions";
    case VP_GXM_DEPTH_ERR_LAYERS: return "layers";
    case VP_GXM_DEPTH_ERR_DIM_CONTROL: return "dim-control";
    case VP_GXM_DEPTH_ERR_FORMAT: return "format";
    case VP_GXM_DEPTH_ERR_TILE_MODE: return "tile-mode";
    case VP_GXM_DEPTH_ERR_OVERFLOW: return "overflow";
    case VP_GXM_DEPTH_ERR_BUDGET: return "byte-budget";
    case VP_GXM_DEPTH_ERR_TARGET: return "target-shape";
    case VP_GXM_DEPTH_ERR_POOL: return "pool-full";
    case VP_GXM_DEPTH_ERR_VOLUME: return "volume-depth";
    case VP_GXM_DEPTH_ERR_STENCIL: return "stencil-format";
    case VP_GXM_DEPTH_CLEAR_ERR_STANDALONE: return "stencil-standalone";
    case VP_GXM_DEPTH_CLEAR_ERR_PRESERVE: return "stencil-preserve";
    default: return "unknown";
    }
}

const char *vp_gxm_depth_action_name(int action) {
    switch (action) {
    case VP_GXM_DEPTH_ACTION_REUSE: return "reuse";
    case VP_GXM_DEPTH_ACTION_CREATE: return "create";
    case VP_GXM_DEPTH_ACTION_RECREATE: return "recreate";
    default: return "unknown";
    }
}

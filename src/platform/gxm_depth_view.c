#include "gxm_depth_view.h"
#include "gxm_resource_layout.h"
#include "gxm_shadow_sampler.h"

#include <string.h>

/* Maxwell TIC word0 component types and channel selectors. */
enum {
    VP_TIC_COMPONENT_SNORM = 1,
    VP_TIC_COMPONENT_UNORM = 2,
    VP_TIC_COMPONENT_SINT = 3,
    VP_TIC_COMPONENT_UINT = 4,
    VP_TIC_COMPONENT_FLOAT = 7
};

enum {
    VP_TIC_SWIZZLE_ZERO = 0,
    VP_TIC_SWIZZLE_R = 2,
    VP_TIC_SWIZZLE_G = 3,
    VP_TIC_SWIZZLE_ONE_FLOAT = 7
};

int vp_gxm_tic_depth_class(unsigned format) {
    switch (format) {
    case VP_GXM_TIC_FORMAT_Z24S8:
    case VP_GXM_TIC_FORMAT_S8Z24:
        return VP_GXM_DEPTH_CLASS_S8D24;
    case VP_GXM_TIC_FORMAT_Z32:
        return VP_GXM_DEPTH_CLASS_DF32;
    case VP_GXM_TIC_FORMAT_X8Z24:
        /* Depth-domain format without a proven native view: X8Z24 has no
         * stencil and is not one of the zeta formats this backend creates. */
        return VP_GXM_DEPTH_ERR_FORMAT;
    default:
        break;
    }
    /* 0x2c..0x3e are depth/coverage variants in the Maxwell PixelFormat enum.
     * None has a reviewed GXM mapping here. */
    if (format >= 0x2cu && format <= 0x3eu) return VP_GXM_DEPTH_ERR_FORMAT;
    return VP_GXM_DEPTH_CLASS_NONE;
}

int vp_gxm_tic_is_depth(unsigned format) {
    return (format >= VP_GXM_TIC_FORMAT_Z24S8 && format <= 0x3eu) ||
           format == VP_GXM_TIC_FORMAT_Z32;
}

int vp_gxm_depth_tsc_plan(const uint32_t tsc[8], unsigned max_mip_level,
                          unsigned *min_filter, unsigned *mag_filter,
                          unsigned *addr_u, unsigned *addr_v) {
    unsigned word0, word1;
    unsigned wrap_u, wrap_v, compare, anisotropy;
    unsigned mag, min, mip, reduction, lod_bias, float_coord, trilin;

    if (tsc == NULL || min_filter == NULL || mag_filter == NULL ||
        addr_u == NULL || addr_v == NULL)
        return VP_GXM_DEPTH_VIEW_ERR_ARG;
    word0 = tsc[0];
    word1 = tsc[1];
    wrap_u = word0 & 7u;
    wrap_v = (word0 >> 3) & 7u;
    compare = (word0 >> 9) & 1u;
    anisotropy = (word0 >> 20) & 7u;
    mag = word1 & 3u;
    min = (word1 >> 4) & 3u;
    mip = (word1 >> 6) & 3u;
    reduction = (word1 >> 10) & 3u;
    lod_bias = (word1 >> 12) & 0x1fffu;
    float_coord = (word1 >> 25) & 1u;
    trilin = (word1 >> 26) & 0x1fu;

    /* Maxwell DepthCompareFunc is only meaningful when comparison is enabled.
     * This milestone does not lower comparison sampling; the raw TSC words
     * stay in the request/log for the next round. */
    if (compare != 0u) return VP_GXM_DEPTH_VIEW_ERR_COMPARE;
    /* The compare function field is only meaningful when comparison is
     * enabled.  It is intentionally not rejected here: the live descriptor
     * carries bits in fields whose meaning is not proven, and with compare off
     * hardware does not use the function. */
    /* The observed depth sampler sets TSC sRGB (word0 bit13). Depth formats
     * have no sRGB encoding; the flag is ignored for a depth TIC rather than
     * changing pixel values.  Bits 14..19 are not treated as reserved: the
     * live descriptor sets them and their meaning is not proven. */
    /* No GXM anisotropic state is set by this path; anisotropy 0 means one
     * sample (no anisotropic filtering). */
    if (anisotropy != 0u) return VP_GXM_DEPTH_VIEW_ERR_ANISOTROPY;
    /* 0 is reserved in all three 2-bit filter fields.  Nearest/Linear are the
     * only defined mag/min filters.  A mip filter of Nearest/Linear is exact
     * only when the view owns a single level; otherwise it would request
     * levels this backend does not have. */
    if (mag == 0u || mag > 2u || min == 0u || min > 2u)
        return VP_GXM_DEPTH_VIEW_ERR_FILTER;
    if (mip == 0u) return VP_GXM_DEPTH_VIEW_ERR_FILTER;
    if (mip != 1u && max_mip_level != 0u)
        return VP_GXM_DEPTH_VIEW_ERR_FILTER;
    /* The observed depth sampler leaves the cubemap-only bits 8/9 set while
     * the TIC is a 2D depth texture; they do not apply.  Reduction, LOD bias,
     * float-coordinate normalization and trilinear optimization would change
     * sampling and are not lowered, so a nonzero value is refused. */
    if (reduction != 0u || lod_bias != 0u || float_coord != 0u || trilin != 0u)
        return VP_GXM_DEPTH_VIEW_ERR_TSC;
    /* Maxwell WrapMode Repeat=0, Mirror=1, ClampToEdge=2.  Border=3, Clamp=4
     * and MirrorOnce=5..7 have no reviewed GXM/GL-equivalent semantics here;
     * in particular a raw 3 must never be coerced to clamp. */
    if (wrap_u > 2u || wrap_v > 2u) return VP_GXM_DEPTH_VIEW_ERR_WRAP;

    *min_filter = min == 1u ? VP_GXM_DEPTH_SAMPLER_FILTER_POINT
                            : VP_GXM_DEPTH_SAMPLER_FILTER_LINEAR;
    *mag_filter = mag == 1u ? VP_GXM_DEPTH_SAMPLER_FILTER_POINT
                            : VP_GXM_DEPTH_SAMPLER_FILTER_LINEAR;
    *addr_u = wrap_u == 0u ? VP_GXM_DEPTH_SAMPLER_ADDR_REPEAT :
              wrap_u == 1u ? VP_GXM_DEPTH_SAMPLER_ADDR_MIRROR :
                             VP_GXM_DEPTH_SAMPLER_ADDR_CLAMP;
    *addr_v = wrap_v == 0u ? VP_GXM_DEPTH_SAMPLER_ADDR_REPEAT :
              wrap_v == 1u ? VP_GXM_DEPTH_SAMPLER_ADDR_MIRROR :
                             VP_GXM_DEPTH_SAMPLER_ADDR_CLAMP;
    return VP_GXM_DEPTH_VIEW_OK;
}

/* Reviewed TIC component/channel mapping per depth format.  The component
 * types come from the primary Maxwell enum (and match yuzu's
 * PixelFormatFromTextureInfo table for Z24S8/S8Z24/Z32).  The depth value is
 * the channel selected by x_source: G for Z24S8 (observed live 0x76da4a29),
 * R for S8Z24/Z32.  y/z may replicate the depth channel or be zero; w must be
 * the floating-point one.  These are explicit rules, not numeric guesses. */
static int tic_depth_mapping(unsigned format, unsigned comp[4],
                             unsigned *depth_component) {
    switch (format) {
    case VP_GXM_TIC_FORMAT_Z24S8:
        comp[0] = VP_TIC_COMPONENT_UINT;
        comp[1] = VP_TIC_COMPONENT_UNORM;
        comp[2] = VP_TIC_COMPONENT_UNORM;
        comp[3] = VP_TIC_COMPONENT_UNORM;
        *depth_component = VP_TIC_SWIZZLE_G;
        return VP_GXM_DEPTH_VIEW_OK;
    case VP_GXM_TIC_FORMAT_S8Z24:
        comp[0] = VP_TIC_COMPONENT_UNORM;
        comp[1] = VP_TIC_COMPONENT_UINT;
        comp[2] = VP_TIC_COMPONENT_UINT;
        comp[3] = VP_TIC_COMPONENT_UINT;
        *depth_component = VP_TIC_SWIZZLE_R;
        return VP_GXM_DEPTH_VIEW_OK;
    case VP_GXM_TIC_FORMAT_Z32:
        comp[0] = VP_TIC_COMPONENT_FLOAT;
        comp[1] = VP_TIC_COMPONENT_FLOAT;
        comp[2] = VP_TIC_COMPONENT_FLOAT;
        comp[3] = VP_TIC_COMPONENT_FLOAT;
        *depth_component = VP_TIC_SWIZZLE_R;
        return VP_GXM_DEPTH_VIEW_OK;
    default:
        return VP_GXM_DEPTH_VIEW_ERR_TIC_FORMAT;
    }
}

int vp_gxm_depth_view_plan(const VPGxmDepthViewInput *in,
                           VPGxmDepthViewPlan *out) {
    const uint32_t *tic;
    unsigned tic_format, header, texture_type, depth_layers, block_depth;
    unsigned max_mip_level, msaa, normalized, sparse, srgb;
    unsigned width, height, comp[4], expected[4], depth_component;
    unsigned x_source, y_source, z_source, w_source;
    unsigned min_filter, mag_filter, addr_u, addr_v;
    VPGxmShadowSamplerPlan shadow;
    int tic_class, rc;

    if (in == NULL || out == NULL) return VP_GXM_DEPTH_VIEW_ERR_ARG;
    memset(out, 0, sizeof(*out));
    out->shadow_status = VP_GXM_SHADOW_OK;
    if (in->tic == NULL || in->tsc == NULL) return VP_GXM_DEPTH_VIEW_ERR_ARG;
    if (in->va == 0u || in->entry_va == 0u) return VP_GXM_DEPTH_VIEW_ERR_VA;
    if (in->expected_descriptor_type != 2u)  /* Color2D */
        return VP_GXM_DEPTH_VIEW_ERR_TYPE;
    if (in->sampler_mode != VP_GXM_DEPTH_SAMPLER_MODE_ORDINARY &&
        in->sampler_mode != VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW)
        return VP_GXM_DEPTH_VIEW_ERR_ARG;
    if (in->sampler_mode == VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW) {
        if (in->shadow_contract_complete != 1u)
            return VP_GXM_DEPTH_VIEW_ERR_SHADOW_CONTRACT;
        if (in->descriptor_depth != 1u)
            return VP_GXM_DEPTH_VIEW_ERR_SHADOW_DEPTH;
    } else if (in->descriptor_depth == 1u) {
        /* A sidecar shadow descriptor without the complete helper contract is
         * never silently sampled as an ordinary depth texture. */
        return VP_GXM_DEPTH_VIEW_ERR_SHADOW_CONTRACT;
    }

    tic = in->tic;
    tic_format = tic[0] & 0x7fu;
    header = (tic[2] >> 21) & 7u;
    texture_type = (tic[4] >> 23) & 0xfu;
    depth_layers = ((tic[5] >> 16) & 0x3fffu) + 1u;
    block_depth = (tic[3] >> 6) & 7u;
    max_mip_level = (tic[3] >> 28) & 0xfu;
    msaa = (tic[7] >> 8) & 0xfu;
    normalized = (tic[5] >> 31) & 1u;
    sparse = (tic[5] >> 30) & 1u;
    srgb = (tic[4] >> 22) & 1u;
    width = (tic[4] & 0xffffu) + 1u;
    height = (tic[5] & 0xffffu) + 1u;
    comp[0] = (tic[0] >> 7) & 7u;
    comp[1] = (tic[0] >> 10) & 7u;
    comp[2] = (tic[0] >> 13) & 7u;
    comp[3] = (tic[0] >> 16) & 7u;
    x_source = (tic[0] >> 19) & 7u;
    y_source = (tic[0] >> 22) & 7u;
    z_source = (tic[0] >> 25) & 7u;
    w_source = (tic[0] >> 28) & 7u;

    tic_class = vp_gxm_tic_depth_class(tic_format);
    if (tic_class == VP_GXM_DEPTH_CLASS_NONE)
        return VP_GXM_DEPTH_VIEW_ERR_NOT_DEPTH;
    if (tic_class < 0) return VP_GXM_DEPTH_VIEW_ERR_TIC_FORMAT;
    rc = tic_depth_mapping(tic_format, expected, &depth_component);
    if (rc != VP_GXM_DEPTH_VIEW_OK) return rc;
    if (memcmp(comp, expected, sizeof(expected)) != 0)
        return VP_GXM_DEPTH_VIEW_ERR_COMPONENT;
    if (x_source != depth_component ||
        (y_source != depth_component && y_source != VP_TIC_SWIZZLE_ZERO) ||
        (z_source != depth_component && z_source != VP_TIC_SWIZZLE_ZERO) ||
        w_source != VP_TIC_SWIZZLE_ONE_FLOAT)
        return VP_GXM_DEPTH_VIEW_ERR_SWIZZLE;
    /* Native F32_R/X8U24_SD have a proven .r readback only.  A TIC vector that
     * replicates depth into .g/.b or supplies one in .a cannot be mapped to
     * GXM's one-channel depth formats, so the offline sidecar must prove the
     * exact compiled shader reads no channel besides .r. */
    if ((in->usage_flags & VP_RESOURCE_TEXTURE_USAGE_ANALYZED) == 0u ||
        (in->usage_flags & VP_RESOURCE_TEXTURE_USAGE_MASK &
         ~VP_RESOURCE_TEXTURE_USAGE_R) != 0u)
        return VP_GXM_DEPTH_VIEW_ERR_USAGE;
    /* The owned depth allocation is created as a tiled GXM surface, so only a
     * block-linear TIC describes the same bytes.  Pitch-linear guest zeta
     * layouts are not mirrored. */
    if (header != 3u) return VP_GXM_DEPTH_VIEW_ERR_HEADER;
    /* Guest TIC texture_type 1 is a 2D texture in this ABI. */
    if (texture_type != 1u) return VP_GXM_DEPTH_VIEW_ERR_TYPE;
    if (depth_layers != 1u || block_depth != 0u)
        return VP_GXM_DEPTH_VIEW_ERR_LAYERS;
    if (max_mip_level != 0u) return VP_GXM_DEPTH_VIEW_ERR_MIPS;
    if (msaa != 0u) return VP_GXM_DEPTH_VIEW_ERR_MSAA;
    if (normalized == 0u) return VP_GXM_DEPTH_VIEW_ERR_NORMALIZED;
    if (sparse != 0u) return VP_GXM_DEPTH_VIEW_ERR_SPARSE;
    if (srgb != 0u) return VP_GXM_DEPTH_VIEW_ERR_SRGB;
    if (in->entry_width == 0u || in->entry_height == 0u ||
        in->entry_width > VP_GXM_DEPTH_MAX_DIM ||
        in->entry_height > VP_GXM_DEPTH_MAX_DIM)
        return VP_GXM_DEPTH_VIEW_ERR_SHAPE;
    if (width != in->entry_width || height != in->entry_height)
        return VP_GXM_DEPTH_VIEW_ERR_SHAPE;
    /* sceGxmTextureInitTiled has no explicit pitch.  The tiled depth surface
     * was created with stride align_up(width,64); a native view is only exact
     * when that stride equals the texture width (width is 64-aligned). */
    if ((width & 63u) != 0u || in->entry_stride_samples != width)
        return VP_GXM_DEPTH_VIEW_ERR_STRIDE;
    if (in->entry_va != in->va) return VP_GXM_DEPTH_VIEW_ERR_VA;
    if (in->entry_format_class != (unsigned)tic_class)
        return VP_GXM_DEPTH_VIEW_ERR_CLASS;
    if (in->content_state != VP_GXM_DEPTH_CONTENT_VALID ||
        in->content_generation == 0u)
        return in->content_state == VP_GXM_DEPTH_CONTENT_CLEARED
                   ? VP_GXM_DEPTH_VIEW_ERR_CLEARED
                   : VP_GXM_DEPTH_VIEW_ERR_UNPRODUCED;
    if (in->entry_storage_generation == 0u)
        return VP_GXM_DEPTH_VIEW_ERR_GENERATION;
    if (in->sampler_mode == VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW) {
        int shadow_rc = vp_gxm_shadow_sampler_plan(
            in->tsc, in->expected_descriptor_type, in->descriptor_depth,
            (int)max_mip_level, width, height, &shadow);
        out->shadow_status = shadow_rc;
        if (shadow_rc != VP_GXM_SHADOW_OK)
            return VP_GXM_DEPTH_VIEW_ERR_SHADOW_POLICY;
        min_filter = shadow.native_min_filter;
        mag_filter = shadow.native_mag_filter;
        addr_u = shadow.native_addr_u;
        addr_v = shadow.native_addr_v;
    } else {
        rc = vp_gxm_depth_tsc_plan(in->tsc, max_mip_level, &min_filter,
                                   &mag_filter, &addr_u, &addr_v);
        if (rc != VP_GXM_DEPTH_VIEW_OK) return rc;
    }
    out->format = tic_class == VP_GXM_DEPTH_CLASS_DF32
                      ? VP_GXM_DEPTH_VIEW_FORMAT_F32_R
                      : VP_GXM_DEPTH_VIEW_FORMAT_X8U24_SD;
    out->width = width;
    out->height = height;
    out->depth_class = (unsigned)tic_class;
    out->tic_format = tic_format;
    out->source_component = x_source;
    out->content_generation = in->content_generation;
    out->storage_generation = in->entry_storage_generation;
    out->min_filter = min_filter;
    out->mag_filter = mag_filter;
    out->addr_u = addr_u;
    out->addr_v = addr_v;
    out->sampler_mode = in->sampler_mode;
    if (in->sampler_mode == VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW) {
        out->shadow_compare_func = shadow.compare_func;
        out->shadow_maxwell_filter = shadow.maxwell_filter;
        out->shadow_filter_code = shadow.filter_code;
        out->shadow_border_used = shadow.border_used;
        memcpy(out->shadow_extent, shadow.extent, sizeof(out->shadow_extent));
        memcpy(out->shadow_state, shadow.state, sizeof(out->shadow_state));
        memcpy(out->shadow_border, shadow.border, sizeof(out->shadow_border));
    }
    return VP_GXM_DEPTH_VIEW_OK;
}

const char *vp_gxm_depth_view_status_name(int status) {
    switch (status) {
    case VP_GXM_DEPTH_VIEW_OK: return "ok";
    case VP_GXM_DEPTH_VIEW_ERR_ARG: return "bad-argument";
    case VP_GXM_DEPTH_VIEW_ERR_NOT_DEPTH: return "not-depth-format";
    case VP_GXM_DEPTH_VIEW_ERR_TIC_FORMAT: return "unsupported-depth-format";
    case VP_GXM_DEPTH_VIEW_ERR_HEADER: return "not-block-linear";
    case VP_GXM_DEPTH_VIEW_ERR_TYPE: return "not-2d";
    case VP_GXM_DEPTH_VIEW_ERR_LAYERS: return "layers-or-volume";
    case VP_GXM_DEPTH_VIEW_ERR_MIPS: return "mips";
    case VP_GXM_DEPTH_VIEW_ERR_MSAA: return "msaa";
    case VP_GXM_DEPTH_VIEW_ERR_NORMALIZED: return "unnormalized";
    case VP_GXM_DEPTH_VIEW_ERR_SPARSE: return "sparse";
    case VP_GXM_DEPTH_VIEW_ERR_SRGB: return "srgb";
    case VP_GXM_DEPTH_VIEW_ERR_SHAPE: return "shape-mismatch";
    case VP_GXM_DEPTH_VIEW_ERR_STRIDE: return "non64-or-stride";
    case VP_GXM_DEPTH_VIEW_ERR_VA: return "va-mismatch";
    case VP_GXM_DEPTH_VIEW_ERR_CLASS: return "format-class-mismatch";
    case VP_GXM_DEPTH_VIEW_ERR_UNPRODUCED: return "depth-unproduced";
    case VP_GXM_DEPTH_VIEW_ERR_CLEARED: return "depth-cleared-unmaterialized";
    case VP_GXM_DEPTH_VIEW_ERR_GENERATION: return "content-generation";
    case VP_GXM_DEPTH_VIEW_ERR_COMPARE: return "depth-compare-unsupported";
    case VP_GXM_DEPTH_VIEW_ERR_FILTER: return "filter-or-mip-unsupported";
    case VP_GXM_DEPTH_VIEW_ERR_WRAP: return "wrap-unsupported";
    case VP_GXM_DEPTH_VIEW_ERR_ANISOTROPY: return "anisotropy-unsupported";
    case VP_GXM_DEPTH_VIEW_ERR_TSC: return "tsc-unsupported";
    case VP_GXM_DEPTH_VIEW_ERR_COMPONENT: return "component-type-unsupported";
    case VP_GXM_DEPTH_VIEW_ERR_SWIZZLE: return "swizzle-unsupported";
    case VP_GXM_DEPTH_VIEW_ERR_USAGE: return "shader-usage-not-r-only";
    case VP_GXM_DEPTH_VIEW_ERR_SHADOW_CONTRACT:
        return "shadow-contract-incomplete";
    case VP_GXM_DEPTH_VIEW_ERR_SHADOW_DEPTH:
        return "shadow-descriptor-not-depth";
    case VP_GXM_DEPTH_VIEW_ERR_SHADOW_POLICY:
        return "shadow-policy-refused";
    default: return "unknown";
    }
}

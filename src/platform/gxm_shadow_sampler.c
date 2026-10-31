#include "gxm_shadow_sampler.h"

#include "gxm_depth_view.h"

#include <math.h>
#include <string.h>

_Static_assert(VP_GXM_SHADOW_NATIVE_FILTER_POINT ==
                   VP_GXM_DEPTH_SAMPLER_FILTER_POINT,
               "shadow native point filter must match the depth-view enum");
_Static_assert(VP_GXM_SHADOW_NATIVE_ADDR_CLAMP ==
                   VP_GXM_DEPTH_SAMPLER_ADDR_CLAMP,
               "shadow native clamp must match the depth-view enum");

static float f32_from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int sign_extend(unsigned value, unsigned width) {
    unsigned sign = 1u << (width - 1u);
    return (int)((value ^ sign) - sign);
}

static int finite_float(float value) {
    return isfinite(value) ? 1 : 0;
}

int vp_gxm_shadow_source_mode(unsigned descriptor_depth,
                              unsigned contract_present,
                              unsigned source_is_depth,
                              const char **reason) {
    if (reason != NULL) *reason = "ordinary";
    if (contract_present != 0u && contract_present != 3u) {
        if (reason != NULL) *reason = "incomplete-contract";
        return -1;
    }
    if (descriptor_depth > 1u) {
        if (reason != NULL) *reason = "descriptor-depth-not-boolean";
        return -1;
    }
    if (contract_present == 3u && descriptor_depth != 1u) {
        if (reason != NULL) *reason = "shadow-params-without-depth-flag";
        return -1;
    }
    if (contract_present == 0u && descriptor_depth == 1u) {
        if (reason != NULL) *reason = "shadow-depth-without-contract";
        return -1;
    }
    if (contract_present == 3u && !source_is_depth) {
        if (reason != NULL) *reason = "shadow-requires-depth-source";
        return -1;
    }
    if (contract_present == 3u) {
        if (reason != NULL) *reason = "software-shadow";
        return VP_GXM_SHADOW_SOURCE_SOFTWARE;
    }
    return VP_GXM_SHADOW_SOURCE_ORDINARY;
}

int vp_gxm_shadow_sampler_plan(const uint32_t tsc[8], unsigned texture_type,
                               unsigned depth_flag, int max_mip_level,
                               unsigned width, unsigned height,
                               VPGxmShadowSamplerPlan *out) {
    unsigned w0, w1, w2, w3;
    unsigned wrap_u, wrap_v, compare_enabled, compare_func, srgb;
    unsigned anisotropy, mag, min, mip, cubemap_aniso, cubemap_interface;
    unsigned reduction, lod_bias_bits, float_coord, trilin, min_lod;
    int lod_bias;
    float border_depth;
    unsigned border_used;

    if (out == NULL) return VP_GXM_SHADOW_ERR_ARG;
    memset(out, 0, sizeof(*out));
    if (tsc == NULL) return VP_GXM_SHADOW_ERR_ARG;
    if (width == 0u || height == 0u || width > VP_GXM_DEPTH_MAX_DIM ||
        height > VP_GXM_DEPTH_MAX_DIM)
        return VP_GXM_SHADOW_ERR_EXTENT;

    w0 = tsc[0];
    w1 = tsc[1];
    w2 = tsc[2];
    w3 = tsc[3];
    wrap_u = w0 & 7u;
    wrap_v = (w0 >> 3) & 7u;
    compare_enabled = (w0 >> 9) & 1u;
    compare_func = (w0 >> 10) & 7u;
    srgb = (w0 >> 13) & 1u;
    anisotropy = (w0 >> 20) & 7u;
    mag = w1 & 3u;
    min = (w1 >> 4) & 3u;
    mip = (w1 >> 6) & 3u;
    cubemap_aniso = (w1 >> 8) & 1u;
    cubemap_interface = (w1 >> 9) & 1u;
    reduction = (w1 >> 10) & 3u;
    lod_bias_bits = (w1 >> 12) & 0x1fffu;
    lod_bias = sign_extend(lod_bias_bits, 13u);
    float_coord = (w1 >> 25) & 1u;
    trilin = (w1 >> 26) & 0x1fu;
    min_lod = w2 & 0xfffu;
    (void)w3; /* R sRGB border byte is not the float border value. */

    if (texture_type != 2u) return VP_GXM_SHADOW_ERR_TEXTURE_TYPE;
    if (depth_flag != 1u) return VP_GXM_SHADOW_ERR_NOT_DEPTH;
    if (compare_enabled != 1u) return VP_GXM_SHADOW_ERR_COMPARE_DISABLED;
    if (wrap_u > 3u) return VP_GXM_SHADOW_ERR_WRAP_U;
    if (wrap_v > 3u) return VP_GXM_SHADOW_ERR_WRAP_V;
    if (mag < 1u || mag > 2u) return VP_GXM_SHADOW_ERR_FILTER_MAG;
    if (min < 1u || min > 2u) return VP_GXM_SHADOW_ERR_FILTER_MIN;
    if (mag != min) return VP_GXM_SHADOW_ERR_FILTER_MISMATCH;
    if (mip < 1u || mip > 3u) return VP_GXM_SHADOW_ERR_MIPMAP_FILTER;
    if (max_mip_level < 0) return VP_GXM_SHADOW_ERR_MIP_LEVEL_UNKNOWN;
    if (max_mip_level != 0) return VP_GXM_SHADOW_ERR_MIP_LEVEL_PRESENT;
    if (reduction != 0u) return VP_GXM_SHADOW_ERR_REDUCTION;
    if (anisotropy != 0u) return VP_GXM_SHADOW_ERR_ANISOTROPY;
    /* policy-2: the cubemap-only filtering bits and the sRGB conversion bit
     * are inert for the accepted 2D depth sampler.  Real village shadow TSCs
     * (captured B36: w0=0x00027292, w1=0x000003a2, TIC Z32 1024x3072) carry
     * both cubemap bits and bit13 while sampling a depth surface, so the
     * policy-1 refusal rejected every shadow receiver.  Neither bit changes
     * how the helper reads or compares .r depth; they stay decoded below for
     * the audit trail, not silently dropped from the parser. */
    (void)cubemap_aniso;
    (void)cubemap_interface;
    (void)srgb;
    if (lod_bias != 0) return VP_GXM_SHADOW_ERR_LOD_BIAS;
    if (float_coord != 0u) return VP_GXM_SHADOW_ERR_FLOAT_COORD;
    if (trilin != 0u) return VP_GXM_SHADOW_ERR_TRILINEAR;
    if (min_lod != 0u) return VP_GXM_SHADOW_ERR_MIN_LOD;

    border_used = (wrap_u == 3u || wrap_v == 3u) ? 1u : 0u;
    border_depth = f32_from_bits(tsc[4]);
    if (border_used) {
        if (!finite_float(border_depth))
            return VP_GXM_SHADOW_ERR_BORDER_NONFINITE;
        if (!(border_depth >= 0.0f && border_depth <= 1.0f))
            return VP_GXM_SHADOW_ERR_BORDER_RANGE;
    }

    out->compare_func = compare_func;
    out->maxwell_filter = min; /* Nearest=1, Linear=2; min == mag. */
    out->filter_code = min == 1u ? 0u : 1u;
    out->wrap_u = wrap_u;
    out->wrap_v = wrap_v;
    out->border_used = border_used;
    out->border_depth = border_used ? border_depth : 0.0f;
    out->native_min_filter = VP_GXM_SHADOW_NATIVE_FILTER_POINT;
    out->native_mag_filter = VP_GXM_SHADOW_NATIVE_FILTER_POINT;
    out->native_mip_filter_disabled = 1u;
    out->native_compare_disabled = 1u;
    out->native_addr_u = VP_GXM_SHADOW_NATIVE_ADDR_CLAMP;
    out->native_addr_v = VP_GXM_SHADOW_NATIVE_ADDR_CLAMP;
    out->extent[0] = (float)width;
    out->extent[1] = (float)height;
    out->extent[2] = 1.0f / (float)width;
    out->extent[3] = 1.0f / (float)height;
    out->state[0] = (float)compare_func;
    out->state[1] = (float)out->filter_code;
    out->state[2] = (float)wrap_u;
    out->state[3] = (float)wrap_v;
    out->border[0] = out->border_depth;
    out->border[1] = 0.0f;
    out->border[2] = 0.0f;
    out->border[3] = 0.0f;
    return VP_GXM_SHADOW_OK;
}

const char *vp_gxm_shadow_status_name(int status) {
    switch (status) {
    case VP_GXM_SHADOW_OK: return "ok";
    case VP_GXM_SHADOW_ERR_ARG: return "bad-argument";
    case VP_GXM_SHADOW_ERR_TEXTURE_TYPE: return "texture-type-not-2d";
    case VP_GXM_SHADOW_ERR_NOT_DEPTH: return "texture-not-depth";
    case VP_GXM_SHADOW_ERR_COMPARE_DISABLED: return "compare-disabled";
    case VP_GXM_SHADOW_ERR_WRAP_U: return "wrap-u-unsupported";
    case VP_GXM_SHADOW_ERR_WRAP_V: return "wrap-v-unsupported";
    case VP_GXM_SHADOW_ERR_FILTER_MAG: return "mag-filter-unsupported";
    case VP_GXM_SHADOW_ERR_FILTER_MIN: return "min-filter-unsupported";
    case VP_GXM_SHADOW_ERR_FILTER_MISMATCH: return "filter-mismatch";
    case VP_GXM_SHADOW_ERR_MIPMAP_FILTER: return "mipmap-filter-unsupported";
    case VP_GXM_SHADOW_ERR_MIP_LEVEL_UNKNOWN: return "mip-level-unknown";
    case VP_GXM_SHADOW_ERR_MIP_LEVEL_PRESENT: return "mip-level-present";
    case VP_GXM_SHADOW_ERR_REDUCTION: return "reduction-unsupported";
    case VP_GXM_SHADOW_ERR_ANISOTROPY: return "anisotropy-unsupported";
    case VP_GXM_SHADOW_ERR_CUBEMAP_ANISOTROPY:
        return "cubemap-anisotropy-unsupported";
    case VP_GXM_SHADOW_ERR_CUBEMAP_INTERFACE:
        return "cubemap-interface-filtering-unsupported";
    case VP_GXM_SHADOW_ERR_LOD_BIAS: return "lod-bias-unsupported";
    case VP_GXM_SHADOW_ERR_FLOAT_COORD:
        return "float-coord-normalization-unsupported";
    case VP_GXM_SHADOW_ERR_TRILINEAR:
        return "trilinear-optimization-unsupported";
    case VP_GXM_SHADOW_ERR_MIN_LOD: return "min-lod-clamp-unsupported";
    case VP_GXM_SHADOW_ERR_SRGB: return "srgb-unsupported";
    case VP_GXM_SHADOW_ERR_BORDER_NONFINITE: return "border-depth-nonfinite";
    case VP_GXM_SHADOW_ERR_BORDER_RANGE: return "border-depth-out-of-range";
    case VP_GXM_SHADOW_ERR_EXTENT: return "extent-invalid";
    default: return "unknown";
    }
}

/* Host-pure Maxwell TSC planner for the observed sampler2DShadow contract.
 *
 * The offline policy in tools/shader/shadow_helpers.py is the reference.  This
 * module mirrors that policy in C so the runtime and the GPU content probe use
 * one decoder instead of parsing a different subset.  It does not call GXM and
 * does not look at the shader: the caller supplies the TIC extent, the shader
 * descriptor type/depth flags and the TIC max-mip field.
 *
 * A plan carries three float4 helper uniforms and a fixed native sampler
 * state.  The native sampler is always point, clamp-to-edge, no-mip and
 * no-compare; the emitted Cg helper performs wrap, point/linear PCF and the
 * comparison.  The Cg filter code (0 point, 1 linear) is deliberately kept
 * separate from the Maxwell filter enum (Nearest=1, Linear=2).
 */
#ifndef VP_GXM_SHADOW_SAMPLER_H
#define VP_GXM_SHADOW_SAMPLER_H

#include <stdint.h>

enum {
    VP_GXM_SHADOW_OK = 0,
    VP_GXM_SHADOW_ERR_ARG = -1,
    VP_GXM_SHADOW_ERR_TEXTURE_TYPE = -2,
    VP_GXM_SHADOW_ERR_NOT_DEPTH = -3,
    VP_GXM_SHADOW_ERR_COMPARE_DISABLED = -4,
    VP_GXM_SHADOW_ERR_WRAP_U = -5,
    VP_GXM_SHADOW_ERR_WRAP_V = -6,
    VP_GXM_SHADOW_ERR_FILTER_MAG = -7,
    VP_GXM_SHADOW_ERR_FILTER_MIN = -8,
    VP_GXM_SHADOW_ERR_FILTER_MISMATCH = -9,
    VP_GXM_SHADOW_ERR_MIPMAP_FILTER = -10,
    VP_GXM_SHADOW_ERR_MIP_LEVEL_UNKNOWN = -11,
    VP_GXM_SHADOW_ERR_MIP_LEVEL_PRESENT = -12,
    VP_GXM_SHADOW_ERR_REDUCTION = -13,
    VP_GXM_SHADOW_ERR_ANISOTROPY = -14,
    VP_GXM_SHADOW_ERR_CUBEMAP_ANISOTROPY = -15,
    VP_GXM_SHADOW_ERR_CUBEMAP_INTERFACE = -16,
    VP_GXM_SHADOW_ERR_LOD_BIAS = -17,
    VP_GXM_SHADOW_ERR_FLOAT_COORD = -18,
    VP_GXM_SHADOW_ERR_TRILINEAR = -19,
    VP_GXM_SHADOW_ERR_MIN_LOD = -20,
    VP_GXM_SHADOW_ERR_SRGB = -21,
    VP_GXM_SHADOW_ERR_BORDER_NONFINITE = -22,
    VP_GXM_SHADOW_ERR_BORDER_RANGE = -23,
    VP_GXM_SHADOW_ERR_EXTENT = -24
};

/* Native semantic values, kept in the same domain as gxm_depth_view.h without
 * making this header include the depth-view contract. */
#define VP_GXM_SHADOW_NATIVE_FILTER_POINT 1u
#define VP_GXM_SHADOW_NATIVE_ADDR_CLAMP 2u

/* Explicit source mode for one declared sampler slot.  descriptor_depth is the
 * offline shader descriptor flag from the stage sidecar, NOT the TIC depth/layer
 * field; a normal 2D texture has TIC depth 1 and descriptor depth 0.  The mode
 * is software shadow only when the descriptor flag is 1, the complete three
 * uniform parameters are present and the resolved source is the depth pool. */
#define VP_GXM_SHADOW_SOURCE_ORDINARY 0
#define VP_GXM_SHADOW_SOURCE_SOFTWARE 1
int vp_gxm_shadow_source_mode(unsigned descriptor_depth,
                              unsigned contract_present,
                              unsigned source_is_depth,
                              const char **reason);

typedef struct {
    unsigned compare_func;    /* Maxwell DepthCompareFunc 0..7 */
    unsigned maxwell_filter;  /* Maxwell TSC enum: Nearest=1, Linear=2 */
    unsigned filter_code;     /* Cg helper state.y: point=0, linear=1 */
    unsigned wrap_u;          /* Maxwell WrapMode 0..3 */
    unsigned wrap_v;
    unsigned border_used;     /* wrap_u == Border || wrap_v == Border */
    float border_depth;       /* TSC word4 R when border_used */
    /* The native sampler state is fixed; it never carries compare/filter/wrap
     * from the guest TSC. */
    unsigned native_min_filter;
    unsigned native_mag_filter;
    unsigned native_mip_filter_disabled;
    unsigned native_compare_disabled;
    unsigned native_addr_u;
    unsigned native_addr_v;
    /* Helper uniforms: width,height,1/w,1/h; compare,filter,wrap_u,wrap_v;
     * border R,0,0,0. */
    float extent[4];
    float state[4];
    float border[4];
} VPGxmShadowSamplerPlan;

/* Decode one complete eight-word TSC.  max_mip_level < 0 means the caller could
 * not supply the TIC field and is refused as unknown.  Returns
 * VP_GXM_SHADOW_OK and fills *out, or a negative named reason. */
int vp_gxm_shadow_sampler_plan(const uint32_t tsc[8], unsigned texture_type,
                               unsigned depth_flag, int max_mip_level,
                               unsigned width, unsigned height,
                               VPGxmShadowSamplerPlan *out);

const char *vp_gxm_shadow_status_name(int status);

#endif

/* Host-testable depth-texture view and TSC sampler planner.
 *
 * The runtime owns native depth/stencil surfaces (gxm_depth.c plans them) and
 * may expose an owned surface as a tiled sampler2D view.  This module decides
 * whether a guest TIC/TSC descriptor may be matched to one owned entry; it
 * does not allocate memory, does not call GXM and does not decode guest depth.
 *
 * Shared depth-view formats are the two B15-proven native mappings:
 *   DF32  -> SCE_GXM_TEXTURE_FORMAT_F32_R    (read .r)
 *   S8D24 -> SCE_GXM_TEXTURE_FORMAT_X8U24_SD (read .r)
 * The guest TIC format and the Maxwell zeta format are different domains; the
 * compatibility check is explicit, never numeric equality.
 */
#ifndef VP_GXM_DEPTH_VIEW_H
#define VP_GXM_DEPTH_VIEW_H

#include "gxm_depth.h"

#include <stddef.h>
#include <stdint.h>

/* Guest TIC texture formats (Maxwell PixelFormat) that may describe a depth
 * buffer.  Z24S8/S8Z24 and Z32 are the formats this backend understands;
 * X8Z24 and the coverage/compare variants are depth-domain but unsupported. */
#define VP_GXM_TIC_FORMAT_Z24S8 0x29u
#define VP_GXM_TIC_FORMAT_X8Z24 0x2au
#define VP_GXM_TIC_FORMAT_S8Z24 0x2bu
#define VP_GXM_TIC_FORMAT_Z32 0x2fu

enum {
    VP_GXM_DEPTH_VIEW_FORMAT_NONE = 0,
    VP_GXM_DEPTH_VIEW_FORMAT_F32_R = 1,
    VP_GXM_DEPTH_VIEW_FORMAT_X8U24_SD = 2
};

enum {
    VP_GXM_DEPTH_CONTENT_NONE = 0,  /* no clear, no committed depth write */
    VP_GXM_DEPTH_CONTENT_CLEARED = 1, /* clear recorded, materialization unproven */
    VP_GXM_DEPTH_CONTENT_VALID = 2    /* committed depth-writing draw */
};

enum {
    VP_GXM_DEPTH_SAMPLER_FILTER_POINT = 1,
    VP_GXM_DEPTH_SAMPLER_FILTER_LINEAR = 2
};

/* Explicit sampler contract.  Ordinary uses the TSC filters/wrap directly and
 * refuses compare sampling.  Software-shadow is selected only when the stage
 * reflection has the complete three-uniform helper contract and the resource
 * sidecar marks the descriptor as a shadow depth sampler (depth=1); it fixes
 * the native sampler to point/clamp/no-mip/no-compare and returns the helper
 * uniforms. */
enum {
    VP_GXM_DEPTH_SAMPLER_MODE_ORDINARY = 0,
    VP_GXM_DEPTH_SAMPLER_MODE_SOFTWARE_SHADOW = 1
};

enum {
    VP_GXM_DEPTH_SAMPLER_ADDR_REPEAT = 0,
    VP_GXM_DEPTH_SAMPLER_ADDR_MIRROR = 1,
    VP_GXM_DEPTH_SAMPLER_ADDR_CLAMP = 2
};

enum {
    VP_GXM_DEPTH_VIEW_OK = 0,
    VP_GXM_DEPTH_VIEW_ERR_ARG = -1,
    VP_GXM_DEPTH_VIEW_ERR_NOT_DEPTH = -2,
    VP_GXM_DEPTH_VIEW_ERR_TIC_FORMAT = -3,
    VP_GXM_DEPTH_VIEW_ERR_HEADER = -4,
    VP_GXM_DEPTH_VIEW_ERR_TYPE = -5,
    VP_GXM_DEPTH_VIEW_ERR_LAYERS = -6,
    VP_GXM_DEPTH_VIEW_ERR_MIPS = -7,
    VP_GXM_DEPTH_VIEW_ERR_MSAA = -8,
    VP_GXM_DEPTH_VIEW_ERR_NORMALIZED = -9,
    VP_GXM_DEPTH_VIEW_ERR_SPARSE = -10,
    VP_GXM_DEPTH_VIEW_ERR_SRGB = -11,
    VP_GXM_DEPTH_VIEW_ERR_SHAPE = -12,
    VP_GXM_DEPTH_VIEW_ERR_STRIDE = -13,
    VP_GXM_DEPTH_VIEW_ERR_VA = -14,
    VP_GXM_DEPTH_VIEW_ERR_CLASS = -15,
    VP_GXM_DEPTH_VIEW_ERR_UNPRODUCED = -16,
    VP_GXM_DEPTH_VIEW_ERR_CLEARED = -17,
    VP_GXM_DEPTH_VIEW_ERR_GENERATION = -18,
    VP_GXM_DEPTH_VIEW_ERR_COMPARE = -19,
    VP_GXM_DEPTH_VIEW_ERR_FILTER = -20,
    VP_GXM_DEPTH_VIEW_ERR_WRAP = -21,
    VP_GXM_DEPTH_VIEW_ERR_ANISOTROPY = -22,
    VP_GXM_DEPTH_VIEW_ERR_TSC = -23,
    VP_GXM_DEPTH_VIEW_ERR_COMPONENT = -24,
    VP_GXM_DEPTH_VIEW_ERR_SWIZZLE = -25,
    VP_GXM_DEPTH_VIEW_ERR_USAGE = -26,
    VP_GXM_DEPTH_VIEW_ERR_SHADOW_CONTRACT = -27,
    VP_GXM_DEPTH_VIEW_ERR_SHADOW_DEPTH = -28,
    VP_GXM_DEPTH_VIEW_ERR_SHADOW_POLICY = -29
};

typedef struct {
    /* Guest identity and the complete raw descriptors.  TIC/TSC fields are
     * decoded inside the planner from these eight-word images so the runtime
     * and the probe cannot diverge by parsing a different subset. */
    uint64_t va;
    const uint32_t *tic;         /* 8 raw TIC words */
    const uint32_t *tsc;         /* 8 raw TSC words */
    /* Shader descriptor type from the offline resource sidecar
     * (VP_RESOURCE_TEX_2D = 2).  It is a different domain from the TIC type. */
    unsigned expected_descriptor_type;
    /* Shader descriptor depth flag from the exact stage's sidecar record.
     * Software-shadow mode requires 1; an ordinary depth view is 0. */
    unsigned descriptor_depth;
    /* VP_GXM_DEPTH_SAMPLER_MODE_*. */
    unsigned sampler_mode;
    /* 1 only when the fragment reflection declares texN_shadow_extent,
     * texN_shadow_state and texN_shadow_border and the matching texN sampler.
     * Software-shadow mode refuses an incomplete contract; it never fills
     * missing uniforms with zero. */
    unsigned shadow_contract_complete;
    /* VP_RESOURCE_TEXTURE_FLAG_* from the exact stage's sidecar record.  The
     * native depth formats have only the proven .r channel mapping, so the
     * shader must have been analyzed and must not read .g/.b/.a. */
    unsigned usage_flags;

    /* Owned native depth entry. */
    uint64_t entry_va;
    unsigned entry_width;
    unsigned entry_height;
    unsigned entry_format_class; /* VP_GXM_DEPTH_CLASS_* */
    unsigned entry_stride_samples;
    unsigned content_state;      /* VP_GXM_DEPTH_CONTENT_* */
    uint32_t content_generation;
    /* Monotonic epoch of the owned allocation itself.  Re-creating a depth
     * identity at the same VA must never look like a newer content epoch of
     * the old storage, so this is separate from content_generation and is
     * never reset on a clear or draw commit.  0 is invalid. */
    uint32_t entry_storage_generation;
} VPGxmDepthViewInput;

typedef struct {
    unsigned format;      /* VP_GXM_DEPTH_VIEW_FORMAT_* */
    unsigned width;
    unsigned height;
    unsigned depth_class; /* VP_GXM_DEPTH_CLASS_* */
    unsigned tic_format;
    unsigned source_component; /* TIC swizzle selector feeding shader .r */
    unsigned content_generation;
    unsigned storage_generation;
    unsigned min_filter;  /* VP_GXM_DEPTH_SAMPLER_FILTER_* */
    unsigned mag_filter;
    unsigned addr_u;      /* VP_GXM_DEPTH_SAMPLER_ADDR_* */
    unsigned addr_v;
    /* Only one level is owned, so the native mip filter is always disabled. */
    unsigned sampler_mode; /* VP_GXM_DEPTH_SAMPLER_MODE_* */
    /* Set even when the overall plan fails so the caller can log the exact
     * shadow policy refusal that caused a SHADOW_POLICY error. */
    int shadow_status;
    unsigned shadow_compare_func;
    unsigned shadow_maxwell_filter; /* 1 Nearest, 2 Linear */
    unsigned shadow_filter_code;    /* Cg helper 0 point, 1 linear */
    unsigned shadow_border_used;
    float shadow_extent[4]; /* width,height,1/w,1/h */
    float shadow_state[4];  /* compare,filter,wrap_u,wrap_v */
    float shadow_border[4]; /* border R,0,0,0 */
} VPGxmDepthViewPlan;

/* Classify one TIC format.  Returns VP_GXM_DEPTH_CLASS_DF32/S8D24 for the
 * reviewed mappings, VP_GXM_DEPTH_CLASS_NONE for a non-depth format and
 * VP_GXM_DEPTH_ERR_FORMAT for a depth-domain format with no proven view. */
int vp_gxm_tic_depth_class(unsigned format);
/* 1 when the format is in the Maxwell depth/depth-stencil domain (supported or
 * explicitly refused), 0 for a colour format.  Guest resolution uses this to
 * route a declared sampler to the depth path and never to CPU decode. */
int vp_gxm_tic_is_depth(unsigned format);

/* Validate one typed descriptor against the owned entry and the TSC.  Returns
 * VP_GXM_DEPTH_VIEW_OK and fills *out, or a negative reason.  Only committed
 * depth content (VALID) is sampleable; a clear alone is refused because this
 * backend has not proven that a native clear was materialized. */
int vp_gxm_depth_view_plan(const VPGxmDepthViewInput *in,
                           VPGxmDepthViewPlan *out);

/* Decode the reviewed subset of a TSC.  Comparison sampling is refused (next
 * milestone), as are reserved mip/filter values, unsupported wrap modes and
 * anisotropy.  The returned filters/addressing are Maxwell semantic values;
 * the runtime maps them explicitly to the distinct GXM enum domain. */
int vp_gxm_depth_tsc_plan(const uint32_t tsc[8], unsigned max_mip_level,
                          unsigned *min_filter, unsigned *mag_filter,
                          unsigned *addr_u, unsigned *addr_v);

const char *vp_gxm_depth_view_status_name(int status);

#endif

#include "gxm_depth_view_native.h"

#include <stddef.h>
#include <string.h>

static SceGxmTextureFilter native_filter(unsigned filter) {
    return filter == VP_GXM_DEPTH_SAMPLER_FILTER_LINEAR
               ? SCE_GXM_TEXTURE_FILTER_LINEAR
               : SCE_GXM_TEXTURE_FILTER_POINT;
}

static SceGxmTextureAddrMode native_addr(unsigned addr) {
    switch (addr) {
    case VP_GXM_DEPTH_SAMPLER_ADDR_REPEAT: return SCE_GXM_TEXTURE_ADDR_REPEAT;
    case VP_GXM_DEPTH_SAMPLER_ADDR_MIRROR: return SCE_GXM_TEXTURE_ADDR_MIRROR;
    default: return SCE_GXM_TEXTURE_ADDR_CLAMP;
    }
}

int vp_gxm_depth_native_init(SceGxmTexture *texture, void *depth_data,
                             const VPGxmDepthViewPlan *plan) {
    SceGxmTextureFormat format;
    int rc;

    if (texture == NULL || depth_data == NULL || plan == NULL)
        return -1;
    if (plan->width == 0u || plan->height == 0u)
        return -1;
    format = plan->format == VP_GXM_DEPTH_VIEW_FORMAT_F32_R
                 ? SCE_GXM_TEXTURE_FORMAT_F32_R
                 : plan->format == VP_GXM_DEPTH_VIEW_FORMAT_X8U24_SD
                       ? SCE_GXM_TEXTURE_FORMAT_X8U24_SD
                       : (SceGxmTextureFormat)0;
    if (format == (SceGxmTextureFormat)0)
        return -1;
    memset(texture, 0, sizeof(*texture));
    rc = sceGxmTextureInitTiled(texture, depth_data, format, plan->width,
                                plan->height, 1);
    if (rc < 0) return rc;
    rc = sceGxmTextureSetMinFilter(texture, native_filter(plan->min_filter));
    if (rc < 0) return rc;
    rc = sceGxmTextureSetMagFilter(texture, native_filter(plan->mag_filter));
    if (rc < 0) return rc;
    rc = sceGxmTextureSetMipFilter(texture, SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
    if (rc < 0) return rc;
    rc = sceGxmTextureSetUAddrMode(texture, native_addr(plan->addr_u));
    if (rc < 0) return rc;
    return sceGxmTextureSetVAddrMode(texture, native_addr(plan->addr_v));
}

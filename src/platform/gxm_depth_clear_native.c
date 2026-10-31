#include "gxm_depth_clear_native.h"

#include <string.h>

int vp_gxm_depth_native_clear(SceGxmContext *context,
                              SceGxmRenderTarget *target,
                              SceGxmDepthStencilSurface *depth,
                              VPGxmDepthNativeClearResult *out) {
    SceGxmNotification notification;
    volatile unsigned *region;
    int rc;

    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    if (context == NULL || target == NULL || depth == NULL) {
        out->begin_rc = -1;
        return -1;
    }

    /* The load action, not a draw, performs the clear.  ForceStore keeps the
     * result in the depth allocation; ForceLoad must be disabled so the
     * backend clears from the surface's background value instead of loading
     * undefined/old contents. */
    sceGxmDepthStencilSurfaceSetForceLoadMode(
        depth, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
    sceGxmDepthStencilSurfaceSetForceStoreMode(
        depth, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
    rc = sceGxmBeginScene(context, 0, target, NULL, NULL, NULL, NULL, depth);
    out->begin_rc = rc;
    if (rc < 0) return rc;
    out->scene_started = 1;

    region = sceGxmGetNotificationRegion();
    notification.address = region;
    notification.value = 1;
    *region = 0;
    rc = sceGxmEndScene(context, NULL, &notification);
    out->end_rc = rc;
    if (rc < 0) {
        /* The scene state is unknown.  Do not call EndScene again, do not
         * wait and do not Finish; the caller poisons its context. */
        return rc;
    }
    rc = sceGxmNotificationWait(&notification);
    out->notify_rc = rc;
    if (rc < 0) {
        /* Not finished: the caller must not release or reuse resources. */
        return rc;
    }
    sceGxmFinish(context);
    out->synchronized = 1;
    return 0;
}

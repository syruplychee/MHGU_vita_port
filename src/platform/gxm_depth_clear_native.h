/* Shared GXM native-depth clear materialization.
 *
 * The production draw path and the target-side depth probe use this one
 * function to turn a recorded guest depth/stencil clear into actual GPU depth
 * content.  It opens a depth-only scene (no colour attachment, no draw) with
 * ForceLoad disabled so the render target's load action writes the surface's
 * background depth/stencil over the whole attachment, then closes the scene
 * synchronously.  It never fills depth from the CPU and never substitutes a
 * colour draw.
 */
#ifndef VP_GXM_DEPTH_CLEAR_NATIVE_H
#define VP_GXM_DEPTH_CLEAR_NATIVE_H

#include <psp2/gxm.h>

typedef struct {
    int begin_rc;
    int end_rc;
    int notify_rc;
    int scene_started;
    int synchronized; /* EndScene and notification wait both succeeded */
} VPGxmDepthNativeClearResult;

/* Execute one full-extent native depth/stencil clear.  The caller must have
 * set the background depth/stencil on `depth` and must own the context (no
 * other scene may be active).  On BeginScene failure no scene is active and
 * the caller may continue; on EndScene/notification failure the context state
 * is unknown and the caller must not reuse or retry it. */
int vp_gxm_depth_native_clear(SceGxmContext *context,
                              SceGxmRenderTarget *target,
                              SceGxmDepthStencilSurface *depth,
                              VPGxmDepthNativeClearResult *out);

#endif

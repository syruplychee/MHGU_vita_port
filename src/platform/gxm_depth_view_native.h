/* Shared GXM native-depth view initialization.
 *
 * Both the production draw path and the target-side depth content probe use
 * this one function after their own independent planner decisions.  It only
 * creates a 1-level tiled view over an already-owned native depth allocation
 * and applies the planned sampler state; it never allocates, fills, decodes or
 * substitutes depth bytes.
 */
#ifndef VP_GXM_DEPTH_VIEW_NATIVE_H
#define VP_GXM_DEPTH_VIEW_NATIVE_H

#include <psp2/gxm.h>

#include "gxm_depth_view.h"

/* Initialize `texture` over `depth_data` using the format/filter/addressing in
 * `plan`.  Returns 0 on success or the first negative GXM return code. */
int vp_gxm_depth_native_init(SceGxmTexture *texture, void *depth_data,
                             const VPGxmDepthViewPlan *plan);

#endif

#ifndef VP_ENGINE_STATE_H
#define VP_ENGINE_STATE_H

#include "engine_nvn.h"

/* Native reconstructions of the two state helpers called by the parent queue:
 *   main+0xBC0E98 (1328 bytes) target/viewport/scissor
 *   main+0xBC13E0 ( 856 bytes) blend/channel/depth/stencil/polygon
 * Both preserve the original caches and NVN call order.  All NVN traffic goes
 * through VPEngineNvnEnv.emit with the original call-site offsets. */

int vp_engine_state_targets(const VPEngineNvnEnv *env, uint32_t ctx,
                            uint32_t state, uint32_t target);
int vp_engine_state_pipeline(const VPEngineNvnEnv *env, uint32_t ctx,
                             uint32_t state, uint32_t pipeline);

#endif

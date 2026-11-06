#ifndef VP_ENGINE_QUEUE_NATIVE_H
#define VP_ENGINE_QUEUE_NATIVE_H

#include "engine_material.h"
#include "engine_nvn.h"

/* Native execution of the locked parent queue at main+0xBBE9C8
 * (0xBBE9C8..0xBC0524, 7004 bytes, SHA-256
 *  32a31fb8ec825cfab9c184d0864fab8411c12b41c93767fd26e9a8f5399b3b67).
 *
 * The function is translated branch-for-branch from the locked disassembly:
 * prologue state reset, root/nested range stack at state+0x568, all record
 * dispatch types 0..15 (type 8..13 and 15 advance without a handler), the
 * original adjacent indexed-draw coalescing at 0xBC0298..0xBC03BC and the
 * per-node range skip/pop at 0xBC0198..0xBC0204.  Records, meshes, matrices
 * and material objects are read from guest memory through env->resolve.
 *
 * NVN calls use the original call-site offsets and slots.  Query calls return
 * real 64-bit results through VPEngineNvnEnv.emit; a sink that does not fill
 * result->has_value stops the run with VP_ENGINE_NATIVE_ERR_CALLBACK.
 *
 * The three locked takeover functions are called only in their native form:
 *   vp_engine_state_targets  (main+0xBC0E98)
 *   vp_engine_state_pipeline (main+0xBC13E0)
 *   vp_engine_material_submit(main+0xBC1764, through material_env)
 * The original AArch32 functions are never invoked.
 *
 * At 0xBBF438 STM sp,{r0,r1,r3,r5} stores four consecutive stack words;
 * DrawElementsInstanced stack[3] comes from record+0x24. */

#define VP_ENGINE_QUEUE_NATIVE_CTX_CMD             0x094u
#define VP_ENGINE_QUEUE_NATIVE_CTX_PENDING_SYNC    0x340f0u
#define VP_ENGINE_QUEUE_NATIVE_CTX_PENDING_UPDATE  0x340f4u
#define VP_ENGINE_QUEUE_NATIVE_CTX_LRU             0x34070u
#define VP_ENGINE_QUEUE_NATIVE_CTX_CURRENT_A       0x389c0u
#define VP_ENGINE_QUEUE_NATIVE_CTX_CURRENT_B       0x389ccu
#define VP_ENGINE_QUEUE_NATIVE_CTX_SELECTOR        0x33f4cu
#define VP_ENGINE_QUEUE_NATIVE_CTX_SELECTOR_XOR    0x01fu
#define VP_ENGINE_QUEUE_NATIVE_CTX_ROOT_STATE      0x43bb60u
#define VP_ENGINE_QUEUE_NATIVE_CTX_ROOT_CURSORS    0x43bb10u
#define VP_ENGINE_QUEUE_NATIVE_CTX_ROOT_COUNTS     0x43bb18u

#define VP_ENGINE_QUEUE_NATIVE_STATE_CMD           0x004u
#define VP_ENGINE_QUEUE_NATIVE_STATE_LAST_TARGET   0x008u
#define VP_ENGINE_QUEUE_NATIVE_STATE_LAST_STATE    0x00cu
#define VP_ENGINE_QUEUE_NATIVE_STATE_LAST_MATERIAL 0x010u
#define VP_ENGINE_QUEUE_NATIVE_STATE_VERTEX_CACHE  0x014u
#define VP_ENGINE_QUEUE_NATIVE_STATE_VBUF_CACHE    0x044u
#define VP_ENGINE_QUEUE_NATIVE_STATE_STRIDE_CACHE  0x054u
#define VP_ENGINE_QUEUE_NATIVE_STATE_OFFSET_CACHE  0x064u
#define VP_ENGINE_QUEUE_NATIVE_STATE_VERTEX_COUNT  0x116cu
#define VP_ENGINE_QUEUE_NATIVE_STATE_RANGE_COUNT   0x1168u
#define VP_ENGINE_QUEUE_NATIVE_STATE_MSAA_CACHE    0x117cu
#define VP_ENGINE_QUEUE_NATIVE_STATE_RANGES        0x568u

/* Run the whole parent queue once.  ctx and state are guest addresses.
 * material_env may share opaque/resolve with env; it supplies the typed sinks
 * of the native material submit.  The run stops at the first memory, callback,
 * query or material failure. */
int vp_engine_queue_native_run(const VPEngineNvnEnv *env,
                               const VPEngineMaterialEnv *material_env,
                               uint32_t ctx, uint32_t state);

#endif

/* Decoder-owned direct texture interface.
 *
 * The deep pipeline's 128-slot pool (texture_pool.h) is the only authority
 * that can turn a decoded RGBA pointer into a GPU lease. GXM never treats an
 * address range as identity: it calls this interface, which resolves the
 * pointer through a live slot and returns that slot's generation. */
#ifndef VP_TEXTURE_PIPELINE_H
#define VP_TEXTURE_PIPELINE_H

#include "texture_pool.h"

/* True only after the deep path was applied at a worker boundary and its pool
 * exists. False keeps every GXM caller on the existing exact-copy path. */
int vp_texture_direct_active(void);
int vp_texture_direct_compatible(const void *rgba, size_t bytes);
/* Resolve a live decoder-owned payload and retain it for the GPU. Returns 1
 * and fills token on success, 0 when the pointer is not a live pool payload. */
int vp_texture_direct_retain_gpu(const void *rgba, VPTexturePoolToken *token);
/* Pair of a successful retain. A stale generation is counted and ignored. */
void vp_texture_direct_release_gpu(VPTexturePoolToken token);
/* Live GPU leases across all slots; diagnostics and teardown precondition. */
unsigned vp_texture_direct_live_refs(void);
/* Fail the current GPU submission; its guest fence must not be published. */
void vp_texture_direct_fail_submission(void);

#endif

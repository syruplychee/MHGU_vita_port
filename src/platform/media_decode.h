#ifndef VP_MEDIA_DECODE_H
#define VP_MEDIA_DECODE_H

#include <stdint.h>

#include "guest_svc.h"

/* P0 observation and command-state decode of nvhost-nvdec / nvhost-vic
 * command buffers.
 *
 * The media engines consume host1x CDMA command buffers, not plain register
 * writes. This walks the same ChCommandHeader state machine the GPU channel
 * uses (SetClass / Incrementing / NonIncrementing / Mask / Immediate), records
 * the NVDEC and VIC method arguments, and on execute calls the video engine
 * hooks. The method-level trace and the NVDEC picture-info / AU / VIC config
 * dumps are emitted only when ux0:data/mhgu-vita-port/media-instrument.enable
 * exists; the state walk itself always runs so the hooks see the same register
 * values the guest wrote.
 */
void vp_media_parse_command_buffer(VPGuestSvc *svc, VPGuestSession *state,
                                   VPGuestGpuChannel *channel,
                                   const uint32_t *words, uint32_t count,
                                   int is_nvdec, int is_vic);

/* Resolve a media buffer address. The media nodes do not get a GMMU mapping:
 * nvhost MapBuffer pins the nvmap allocation and returns its guest address, so
 * try the direct guest range first, then the MapBufferEx table used by the 3D
 * channel. Returns 1 and a readable CPU address on success. */
int vp_media_resolve(VPGuestSvc *svc, VPGuestSession *state, uint64_t va,
                     uint64_t size, uint32_t *cpu);

#endif

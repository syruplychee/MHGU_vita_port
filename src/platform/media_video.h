#ifndef VP_MEDIA_VIDEO_H
#define VP_MEDIA_VIDEO_H

#include <stdint.h>

#include "guest_svc.h"

/* P2/P3 video engine for the nvhost-nvdec / nvhost-vic transport.
 *
 * Decode: on an NVDEC execute the loader reads the picture-info record and the
 * Annex B access unit from guest memory, synthesizes SPS/PPS from the
 * picture-info fields (the title's streams omit them), and submits the result
 * to the Vita hardware AVC decoder (SceAvcdec). Decoded frames are queued.
 *
 * Composite: on a VIC execute the oldest queued frame is written to the
 * guest's output luma/chroma surfaces using the pitch-linear YUV420 layout the
 * VIC config asks for (aligned width, planar Y plus interleaved UV).
 *
 * Both hooks are no-ops unless ux0:data/mhgu-vita-port/media-video.enable
 * exists. Decode and composite failures are logged and never fabricate a
 * frame or change the submit result.
 */
int vp_media_video_decode(VPGuestSvc *svc, VPGuestSession *state, uint32_t fd,
                          uint64_t picture_info, uint64_t bitstream);

int vp_media_video_composite(VPGuestSvc *svc, VPGuestSession *state, uint32_t fd,
                             uint64_t config, uint64_t luma, uint64_t chroma);

/* Reports whether media-video.enable was seen, so the nvhost submit log can
 * name the model that actually retired the fence. */
int vp_media_video_enabled(void);

#endif

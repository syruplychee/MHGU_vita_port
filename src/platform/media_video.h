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

/* Native movie path: submit one Annex B access unit to the same decoder that
 * the guest NVDEC bridge created, without touching the guest frame queue.
 * Returns 1 when a picture was produced, 0 when the decoder needs more input,
 * and a negative value on error/busy. The returned frame is valid until the
 * next call. `coded_height` is the pitch-plane height used for the UV offset;
 * `visible_height` is the display height reported by the decoder. */
int vp_media_video_native_decode(const uint8_t *es, uint32_t size,
                                 uint32_t *width, uint32_t *coded_height,
                                 uint32_t *visible_height, uint32_t *pitch,
                                 const uint8_t **frame);

/* Reports whether media-video.enable was seen, so the nvhost submit log can
 * name the model that actually retired the fence. */
int vp_media_video_enabled(void);

/* Copy only CPU-produced VIC surfaces whose nvmap allocation is still live.
 * Returns zero when the original SDK cache/copy path must handle the source. */
int vp_media_video_readback(VPGuestSvc *svc, uint32_t source,
    uint32_t source_stride, uint32_t destination, uint32_t destination_stride,
    uint32_t row_bytes, uint32_t rows);
void vp_media_video_forget_session(const VPGuestSession *state);

#endif

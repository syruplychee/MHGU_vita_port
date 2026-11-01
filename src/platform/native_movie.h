#pragma once
#include <stdint.h>

#include "guest_svc.h"

/* Framework-level movie presentation. The loader records the ROM-tree path
 * of the title's movie reads, then runs a native MP4/H.264 presentation over
 * the same SceAvcdec decoder used by the original NVDEC bridge. Disabled
 * unless native-movie.enable exists; every failure returns to the original
 * guest path instead of claiming playback succeeded. */
int vp_native_movie_enabled(void);
void vp_native_movie_note_path(const char *relative_path);
int vp_native_movie_start(VPGuestSvc *svc);
void vp_native_movie_request_stop(void);
int vp_native_movie_active(void);
int vp_native_movie_presenting(void);

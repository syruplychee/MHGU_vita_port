/* B108: configurable native resolution for screen-sized GXM passes.
 *
 * Guest image identities, uniforms and texture coordinates stay in logical
 * pixels. Only 16:9 passes at least 160x90 are rescaled: the selected height
 * (360 or 540) is the numerator over the 720-line logical reference, and the
 * native height never exceeds 540. Both axes are rounded up independently, so
 * callers must use the returned width/height for each axis's ratio rather than
 * assuming a single scale. Square, non-16:9 and small images are unchanged.
 *
 * These functions are pure and host-testable; they do not read files or touch
 * GXM state. The runtime parses and caches the selection once at startup.
 */
#ifndef VP_RENDER_RESOLUTION_H
#define VP_RENDER_RESOLUTION_H

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VP_RENDER_RESOLUTION_BASE_HEIGHT 720u
#define VP_RENDER_RESOLUTION_MAX_HEIGHT 540u
#define VP_RENDER_RESOLUTION_MIN_WIDTH 160u
#define VP_RENDER_RESOLUTION_MIN_HEIGHT 90u

typedef struct {
    unsigned width, height;
} VPRenderExtent;

/* Half-open native scissor bounds (max is exclusive). clip_all is set for an
 * empty logical rectangle and for a nonempty rectangle that collapses to
 * empty after physical clamping; callers map it to SCE_GXM_REGION_CLIP_ALL.
 * Otherwise max_x/max_y are already converted to GXM's inclusive endpoints. */
typedef struct {
    unsigned min_x, min_y, max_x, max_y;
    int clip_all;
} VPRenderScissor;

/* Accept exactly 360p or 540p, ignoring trailing whitespace only. The length
 * is explicit so a NUL inside the file is rejected by the token comparison
 * instead of silently truncating the input. Returns 0 and the chosen height,
 * or -1 for an empty/unsupported selection. */
static inline int vp_render_resolution_parse(const char *text, size_t length,
                                             unsigned *chosen_height) {
    if (text == NULL || chosen_height == NULL) return -1;
    while (length > 0u && isspace((unsigned char)text[length - 1u])) length--;
    if (length == 4u && memcmp(text, "360p", 4u) == 0) {
        *chosen_height = 360u;
        return 0;
    }
    if (length == 4u && memcmp(text, "540p", 4u) == 0) {
        *chosen_height = 540u;
        return 0;
    }
    return -1;
}

/* Native extent for one logical image. 1280x720 is 960x540 at 540p and
 * 640x360 at 360p; 1920x1080 stays 960x540 in both modes.
 */
static inline VPRenderExtent vp_render_resolution_extent(
        unsigned width, unsigned height, unsigned chosen_height) {
    VPRenderExtent out;
    uint64_t numerator, denominator;

    out.width = width;
    out.height = height;
    if (width < VP_RENDER_RESOLUTION_MIN_WIDTH ||
        height < VP_RENDER_RESOLUTION_MIN_HEIGHT ||
        (uint64_t)width * 9u != (uint64_t)height * 16u)
        return out;
    if (chosen_height != 360u && chosen_height != 540u) return out;
    numerator = chosen_height;
    denominator = VP_RENDER_RESOLUTION_BASE_HEIGHT;
    if ((uint64_t)height * numerator >
        (uint64_t)VP_RENDER_RESOLUTION_MAX_HEIGHT * denominator) {
        numerator = VP_RENDER_RESOLUTION_MAX_HEIGHT;
        denominator = height;
    }
    out.width = (unsigned)(((uint64_t)width * numerator + denominator - 1u) /
                           denominator);
    out.height = (unsigned)(((uint64_t)height * numerator + denominator - 1u) /
                            denominator);
    return out;
}

/* Convert a half-open guest scissor to native half-open bounds and then to the
 * GXM inclusive upper endpoints. The empty test is made on the guest bounds
 * before scaling so a one-pixel or empty rectangle is never rounded into
 * coverage; clamping happens after scaling. */
static inline VPRenderScissor vp_render_resolution_scissor(
        unsigned logical_width, unsigned logical_height,
        unsigned native_width, unsigned native_height,
        unsigned min_x, unsigned min_y, unsigned max_x, unsigned max_y) {
    VPRenderScissor out;
    uint64_t x0, y0, x1, y1;

    out.min_x = out.min_y = out.max_x = out.max_y = 0u;
    out.clip_all = 1;
    if (logical_width == 0u || logical_height == 0u ||
        native_width == 0u || native_height == 0u)
        return out;
    if (min_x >= max_x || min_y >= max_y) return out;
    x0 = (uint64_t)min_x * native_width / logical_width;
    y0 = (uint64_t)min_y * native_height / logical_height;
    x1 = ((uint64_t)max_x * native_width + logical_width - 1u) / logical_width;
    y1 = ((uint64_t)max_y * native_height + logical_height - 1u) / logical_height;
    if (x0 > native_width) x0 = native_width;
    if (y0 > native_height) y0 = native_height;
    if (x1 > native_width) x1 = native_width;
    if (y1 > native_height) y1 = native_height;
    if (x0 >= x1 || y0 >= y1) return out;
    out.min_x = (unsigned)x0;
    out.min_y = (unsigned)y0;
    out.max_x = (unsigned)(x1 - 1u);
    out.max_y = (unsigned)(y1 - 1u);
    out.clip_all = 0;
    return out;
}

#endif

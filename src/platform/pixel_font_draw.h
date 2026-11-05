#ifndef VP_PIXEL_FONT_DRAW_H
#define VP_PIXEL_FONT_DRAW_H
#include <math.h>
#include <stdint.h>
#include <string.h>

#define VP_NATIVE_FONT_MAGIC 0x31465056u

static inline int vp_font_catalog_required(const char *prefix) {
    return !strcmp(prefix,"GUI\\02_common\\font\\font_festa") ||
           !strcmp(prefix,"GUI\\02_common\\font\\boss_demo");
}

/* Select a source strike and an integer magnification. Prefer the more
 * detailed strike when two choices cover the same screen height. */
static inline unsigned vp_font_choose(const uint32_t *records, unsigned count,
                                      float requested, unsigned *magnification) {
    float best=INFINITY;
    unsigned chosen=count, best_height=0u;
    for (unsigned i=0;i<count;++i) {
        const uint32_t *r=records+i*5u;
        unsigned height=(r[3]>>12)&4095u;
        if (!r[2] || !height) continue;
        unsigned scale=(unsigned)fmaxf(1.0f,floorf(requested/(float)height+0.5f));
        float error=fabsf((float)(height*scale)-requested);
        if (error<best || (error==best && height>best_height)) {
            chosen=i;best=error;best_height=height;*magnification=scale;
        }
    }
    return chosen;
}

/* For an axis-aligned parent transform, move the local anchor so its final
 * screen coordinate is an integer. Signed scale preserves mirrored axes. */
static inline float vp_font_snap(float local, float scale, float translate) {
    return (floorf(local*scale+translate+0.5f)-translate)/scale;
}
#endif

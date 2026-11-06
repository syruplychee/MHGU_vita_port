#ifndef VP_PIXEL_FONT_H
#define VP_PIXEL_FONT_H
#include <stdint.h>
#include <string.h>

#define VP_PIXEL_FONT_SLOTS 256u
typedef struct {
    uint32_t object, width, height;
    uint64_t va;
} VPPixelFontResource;

/* Resource names come from the original rTexture, not a content heuristic. */
static inline int vp_pixel_font_name(const char *name) {
    if (!strcmp(name,"system\\texture\\sysfont_AM_NOMIP") ||
        !strcmp(name,"system\\texture\\detail_sysfont_AM_NOMIP")) return 2;
    const char *prefix="GUI\\02_common\\font\\";
    size_t length=strlen(prefix);
    if (strncmp(name,prefix,length)) return 0;
    name+=length;
    if (!strncmp(name,"font_festa_",11u)) name+=11u;
    else if (!strncmp(name,"boss_demo_",10u)) name+=10u;
    else return 0;
    return name[0]>='0' && name[0]<='9' && name[1]>='0' && name[1]<='9' &&
           !strcmp(name+2u,"_HQ");
}

static inline void vp_pixel_font_retire(VPPixelFontResource *resources,
                                        uint32_t object) {
    for (unsigned i=0;i<VP_PIXEL_FONT_SLOTS;++i)
        if (resources[i].object==object) memset(&resources[i],0,sizeof(resources[i]));
}

static inline int vp_pixel_font_register(VPPixelFontResource *resources,
    uint32_t object, uint64_t va, unsigned width, unsigned height) {
    vp_pixel_font_retire(resources,object);
    if (!object || !va || !width || !height) return -1;
    for (unsigned i=0;i<VP_PIXEL_FONT_SLOTS;++i) {
        if (!resources[i].object) {
            resources[i]=(VPPixelFontResource){object,width,height,va};
            return 0;
        }
    }
    return -1;
}

static inline int vp_pixel_font_lookup(const VPPixelFontResource *resources,
    uint64_t va, unsigned width, unsigned height) {
    for (unsigned i=0;i<VP_PIXEL_FONT_SLOTS;++i)
        if (resources[i].object && resources[i].va==va &&
            resources[i].width==width && resources[i].height==height) return 1;
    return 0;
}
#endif

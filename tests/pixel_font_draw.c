#include <assert.h>
#include "pixel_font_draw.h"

int main(void) {
    assert(vp_font_catalog_required("GUI\\02_common\\font\\font_festa"));
    assert(vp_font_catalog_required("GUI\\02_common\\font\\boss_demo"));
    assert(!vp_font_catalog_required("GUI\\02_common\\font\\button_festa"));
    uint32_t variants[20]={0};
    unsigned heights[4]={8u,10u,12u,16u},scale=0;
    for (unsigned i=0;i<4u;++i) {
        variants[i*5u+2u]=heights[i]|heights[i]<<12;
        variants[i*5u+3u]=heights[i]<<12;
    }
    assert(vp_font_choose(variants,4u,16.5f,&scale)==3u && scale==1u);
    assert(vp_font_choose(variants,4u,21.0f,&scale)==1u && scale==2u);
    assert(vp_font_choose(variants,4u,12.0f,&scale)==2u && scale==1u);
    assert(vp_font_choose(variants,4u,32.0f,&scale)==3u && scale==2u);
    variants[2]=variants[7]=variants[12]=0u;
    assert(vp_font_choose(variants,4u,12.0f,&scale)==3u && scale==1u);
    for (int i=-100;i<100;++i) {
        float p=(float)i*0.13f;
        float snapped=vp_font_snap(p,0.75f,23.25f);
        float screen=snapped*0.75f+23.25f;
        assert(fabsf(screen-roundf(screen))<0.00001f);
        snapped=vp_font_snap(p,-0.75f,23.25f);
        screen=snapped*-0.75f+23.25f;
        assert(fabsf(screen-roundf(screen))<0.00001f);
    }
    return 0;
}

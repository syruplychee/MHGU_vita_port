#include <assert.h>
#include "pixel_font.h"

int main(void) {
    VPPixelFontResource resources[VP_PIXEL_FONT_SLOTS]={0};
    assert(vp_pixel_font_name("GUI\\02_common\\font\\font_festa_00_HQ"));
    assert(vp_pixel_font_name("GUI\\02_common\\font\\boss_demo_10_HQ"));
    assert(!vp_pixel_font_name("GUI\\02_common\\font\\button_festa_00_ID"));
    assert(!vp_pixel_font_name("GUI\\02_common\\font\\font_festa_00_HQ_more"));
    assert(!vp_pixel_font_name("GUI\\02_common\\font\\font_festa_0_HQ"));
    assert(vp_pixel_font_name("system\\texture\\sysfont_AM_NOMIP")==2);
    assert(vp_pixel_font_name("system\\texture\\detail_sysfont_AM_NOMIP")==2);
    assert(vp_pixel_font_register(resources,1u,0x100000001ull,1024u,1024u)==0);
    assert(vp_pixel_font_lookup(resources,0x100000001ull,1024u,1024u));
    assert(!vp_pixel_font_lookup(resources,1u,1024u,1024u));
    assert(!vp_pixel_font_lookup(resources,0x100000001ull,512u,1024u));
    assert(vp_pixel_font_register(resources,1u,2u,1024u,1024u)==0);
    assert(!vp_pixel_font_lookup(resources,0x100000001ull,1024u,1024u));
    vp_pixel_font_retire(resources,1u);
    assert(!vp_pixel_font_lookup(resources,2u,1024u,1024u));
    for (unsigned i=0;i<VP_PIXEL_FONT_SLOTS;++i)
        assert(vp_pixel_font_register(resources,i+1u,i+1u,1024u,1024u)==0);
    assert(vp_pixel_font_register(resources,999u,999u,1024u,1024u)==-1);
    vp_pixel_font_retire(resources,1u);
    assert(vp_pixel_font_register(resources,999u,999u,1024u,1024u)==0);
    return 0;
}

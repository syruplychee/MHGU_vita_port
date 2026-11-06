/* Original A32 emitters use synchronous per-thread scratch until returning.
 * Their only callees are the page getter and the GUI batch lookup; neither
 * recursively emits glyphs. No primary GFD or cached layout record is changed. */
typedef struct {
    int thread;
    uint32_t character[11];
} VPNativeFontScratch;
static VPNativeFontScratch native_font_scratch[16];

int vp_guest_pixel_font_native_enabled(void) {
    static int enabled=-1;
    if (enabled<0) {
        FILE *file=fopen("ux0:data/mhgu-vita-port/pixel-font-native.enable","rb");
        enabled=file!=NULL;
        if (file) fclose(file);
    }
    return enabled;
}

static float font_float(const uint8_t *p) {
    float value;memcpy(&value,p,4u);return value;
}
static uint32_t font_word(const uint8_t *p) {
    uint32_t value;memcpy(&value,p,4u);return value;
}

static int font_missing_catalog(VPGuestSvc *svc, const uint8_t *font) {
    uint32_t prefix=font_word(font+0xb4u);
    char name[64];
    for (unsigned i=0;i<sizeof(name);++i) {
        if (!vp_guest_svc_readable(svc,prefix+i,1u)) return -1;
        name[i]=*(const char *)(uintptr_t)(prefix+i);
        if (!name[i]) {
            if (!vp_font_catalog_required(name)) return 0;
            if (svc->log) fprintf(svc->log,"NATIVE_FONT_REJECT reason=missing-catalog prefix=%s\n",name);
            return -1;
        }
    }
    return -1;
}

int vp_guest_pixel_font_quad(VPGuestSvc *svc, uint32_t *regs, unsigned kind) {
    if (!vp_guest_pixel_font_native_enabled()) return 0;
    uint32_t owner=regs[0],ctx=regs[1],ch=regs[4];
    if (!vp_guest_svc_readable(svc,ctx,0x40u) ||
        !vp_guest_svc_readable(svc,owner,0x58u) ||
        !vp_guest_svc_readable(svc,ch,44u)) return -1;
    const uint8_t *c=(const uint8_t *)(uintptr_t)ctx;
    const uint8_t *o=(const uint8_t *)(uintptr_t)owner;
    const uint8_t *p=(const uint8_t *)(uintptr_t)ch;
    uint32_t font=font_word(c),glyph=font_word(p+12u);
    if (!glyph) return 0;
    if (!vp_guest_svc_readable(svc,font,0xd8u)) return -1;
    const uint8_t *f=(const uint8_t *)(uintptr_t)font;
    uint32_t secondary=font_word(f+0xb0u), total=font_word(f+0xa0u);
    if (!total || !secondary) return font_missing_catalog(svc,f);
    if (!vp_guest_svc_readable(svc,secondary,20u)) return -1;
    const uint32_t *catalog=(const uint32_t *)(uintptr_t)secondary;
    if (catalog[0]!=VP_NATIVE_FONT_MAGIC) return font_missing_catalog(svc,f);
    uint32_t primary=font_word(f+0xacu), count=font_word(f+0x9cu), strikes=catalog[3];
    if (catalog[1]!=1u || catalog[2]!=count || !strikes || strikes>16u ||
        total!=1u+count*strikes || glyph<primary || (glyph-primary)%20u ||
        (glyph-primary)/20u>=count || !vp_guest_svc_readable(svc,glyph,20u)) return -1;
    unsigned index=(glyph-primary)/20u;
    uint32_t variants=secondary+20u+index*strikes*20u;
    if (!vp_guest_svc_readable(svc,variants,strikes*20u)) return -1;
    const uint32_t *available=(const uint32_t *)(uintptr_t)variants;
    const uint8_t *g=(const uint8_t *)(uintptr_t)glyph;
    if (!(font_word(g+8u)&4095u) || !((font_word(g+8u)>>12)&4095u)) return 0;
    float screen_scale=vp_gxm_native_font_scale();
    float m0=font_float(o+0x10u),m1=font_float(o+0x14u);
    float m4=font_float(o+0x20u),m5=font_float(o+0x24u);
    float sign=(font_word(o+0x54u)&(kind?0x400000u:0x200000u))?-1.0f:1.0f;
    float sx=hypotf(m0,m1)*screen_scale,sy=hypotf(m4,m5)*screen_scale;
    float size_x=font_float(c+0x18u),size_y=font_float(c+0x1cu);
    if (!isfinite(sx) || !isfinite(sy) || !isfinite(size_x) || !isfinite(size_y) ||
        !isfinite(screen_scale)) return -1;
    if (sx==0.0f || sy==0.0f) return 0; /* Collapsed, invisible animation. */
    float base=(float)((font_word(f+0xc0u)>>8)&65535u);
    if (!base) return -1;
    if (size_x==0.0f && size_y==0.0f) size_x=size_y=base;
    if (size_x==0.0f || size_y==0.0f) return 0;
    unsigned scale=1u;
    unsigned selected=vp_font_choose(available,strikes,fabsf(size_y)*sy,&scale);
    if (selected==strikes) return -1;
    const uint32_t *r=available+selected*5u;
    if (r[0]!=font_word(g) || (r[1]&255u)>=font_word(f+0x98u)) return -1;
    int thread=sceKernelGetThreadId();
    VPNativeFontScratch *scratch=NULL;
    /* Bridge dispatch holds the session lock while assigning the slot. */
    for (unsigned i=0;i<16u;++i) if (native_font_scratch[i].thread==thread) {scratch=&native_font_scratch[i];break;}
    if (!scratch) for (unsigned i=0;i<16u;++i) if (!native_font_scratch[i].thread) {
        scratch=&native_font_scratch[i];scratch->thread=thread;break;
    }
    if (!scratch) return -1;
    memcpy(scratch->character,p,44u);
    scratch->character[3]=(uint32_t)(uintptr_t)r;
    float x=font_float(p+16u)-(float)(int8_t)g[16]*size_x/base;
    float y=font_float(p+20u)-(float)(int8_t)g[17]*size_y/base;
    unsigned width=r[2]&4095u,height=(r[2]>>12)&4095u;
    float advance=(float)(font_word(g+12u)&4095u)*size_x/base;
    x+=(advance-(float)(r[3]&4095u)*scale/sx)*0.5f+(float)(int8_t)(r[4]&255u)*scale/sx;
    y+=font_float(f+0xd0u)*size_y/base-
        ((float)(r[4]>>16)-(float)(int8_t)((r[4]>>8)&255u))*scale/sy;
    int aligned=fabsf(m1)<0.000001f && fabsf(m4)<0.000001f;
    if (aligned) {
        uint32_t render=font_word(c+0x34u);
        if (!vp_guest_svc_readable(svc,render,0x68u)) return -1;
        float offset=font_float((const uint8_t *)(uintptr_t)render+0x64u);
        x=vp_font_snap(x,m0*screen_scale,font_float(o+0x40u)*screen_scale);
        y=vp_font_snap(y,m5*sign*screen_scale,(font_float(o+0x44u)+offset)*screen_scale);
    }
    float values[4]={x,y,(float)width*scale/sx,(float)height*scale/sy};
    if (scratch->character[10]&0x400u) { values[2]=(float)height*scale/sx;values[3]=(float)width*scale/sy; }
    memcpy(scratch->character+4u,values,sizeof(values));
    regs[4]=(uint32_t)(uintptr_t)scratch->character;
    static unsigned notes;
    if (notes++<48u && svc->log) fprintf(svc->log,
        "NATIVE_FONT_QUAD cp=%x requested=%.6g strike=%u scale=%u grid=%d xy=%.6g,%.6g wh=%.6g,%.6g\n",
        r[0],(double)(fabsf(size_y)*sy),(r[3]>>12)&4095u,scale,aligned,
        (double)x,(double)y,(double)values[2],(double)values[3]);
    return 0;
}

int vp_guest_pixel_font_quad_uv(VPGuestSvc *svc, uint32_t *regs) {
    if (!vp_guest_pixel_font_native_enabled()) return 0;
    VPNativeFontScratch *scratch=NULL;
    for (unsigned i=0;i<16u;++i)
        if (regs[4]==(uint32_t)(uintptr_t)native_font_scratch[i].character) {scratch=&native_font_scratch[i];break;}
    if (!scratch) return 0;
    /* Both UV branches join at VSTR/LDM. LDM has replaced r1/r2 with the
     * batch counters before entering the bridge, so reconstruct the vertex
     * pointer from the still-live context (r5) and batch header (r0-12). */
    if (regs[0]<12u || !vp_guest_svc_readable(svc,regs[5],64u) ||
        !vp_guest_svc_readable(svc,regs[0]-12u,24u)) return -1;
    const uint8_t *batch=(const uint8_t *)(uintptr_t)(regs[0]-12u);
    const uint8_t *context=(const uint8_t *)(uintptr_t)regs[5];
    uint16_t first;memcpy(&first,batch+18u,2u);
    uint32_t address=font_word(context+60u)+(font_word(batch+20u)+first)*24u;
    if (!vp_guest_svc_readable(svc,address,96u)) return -1;
    float *v=(float *)(uintptr_t)address;
    const uint32_t *g=(const uint32_t *)(uintptr_t)scratch->character[3];
    float x=(float)((g[1]>>8)&4095u),y=(float)(g[1]>>20);
    float w=(float)(g[2]&4095u),h=(float)((g[2]>>12)&4095u);
    if (scratch->character[10]&0x400u) {
        v[4]=x;v[5]=y+h;v[10]=x;v[11]=y;
        v[16]=x+w;v[17]=y+h;v[22]=x+w;v[23]=y;
    } else {
        v[4]=x;v[5]=y;v[10]=x+w;v[11]=y;
        v[16]=x;v[17]=y+h;v[22]=x+w;v[23]=y+h;
    }
    return 0;
}

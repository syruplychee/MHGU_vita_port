#include "gpu_texture.h"
#include <stdlib.h>
#include <string.h>

void vp_texture_swizzle(uint8_t *rgba,size_t pixels,uint32_t word) {
    unsigned sel[4];
    for (unsigned i=0;i<4u;i++) sel[i]=(word>>(19u+3u*i))&7u;
    if (sel[0]==2u && sel[1]==3u && sel[2]==4u && sel[3]==5u) return;
    for (size_t p=0;p<pixels;p++) {
        uint8_t in[4];memcpy(in,rgba+p*4u,4u);
        for (unsigned i=0;i<4u;i++) rgba[p*4u+i]=sel[i]>=2u && sel[i]<=5u?
            in[sel[i]-2u]:sel[i]>=6u?255u:0u;
    }
}

static size_t tiled_offset(unsigned x, unsigned y, unsigned width,
                            unsigned bpp, unsigned bh) {
    unsigned xb = x * bpp;
    size_t gobs = ((size_t)width * bpp + 63u) / 64u;
    return (y / (8u << bh)) * gobs * (512u << bh) +
           (xb / 64u) * (512u << bh) + ((y / 8u) & ((1u << bh) - 1u)) * 512u +
           ((xb & 63u) / 32u) * 256u + ((y & 7u) / 2u) * 64u +
           ((xb & 31u) / 16u) * 32u + (y & 1u) * 16u + (xb & 15u);
}

/* Map an exact Maxwell channel-selector tuple to the GXM swizzle enum whose
 * output vector is bit-identical. Selector values are Maxwell SwizzleSource:
 * 0 = zero, 2 = R, 3 = G, 7 = float one. Only combinations with a reviewed GXM
 * enum are accepted; arbitrary shuffles and OneInt(6) are not guessed. */
static unsigned vp_texture_bc4_swizzle_code(const unsigned sel[4]) {
    if (sel[0]==2u && sel[1]==0u && sel[2]==0u && sel[3]==7u)
        return VP_TEXTURE_BC4_SWIZZLE_R;          /* R,0,0,1 */
    if (sel[0]==2u && sel[1]==0u && sel[2]==0u && sel[3]==0u)
        return VP_TEXTURE_BC4_SWIZZLE_000R;       /* R,0,0,0 */
    if (sel[0]==2u && sel[1]==7u && sel[2]==7u && sel[3]==7u)
        return VP_TEXTURE_BC4_SWIZZLE_111R;       /* R,1,1,1 */
    if (sel[0]==2u && sel[1]==2u && sel[2]==2u && sel[3]==2u)
        return VP_TEXTURE_BC4_SWIZZLE_RRRR;       /* R,R,R,R */
    if (sel[0]==2u && sel[1]==2u && sel[2]==2u && sel[3]==0u)
        return VP_TEXTURE_BC4_SWIZZLE_0RRR;       /* R,R,R,0 */
    if (sel[0]==2u && sel[1]==2u && sel[2]==2u && sel[3]==7u)
        return VP_TEXTURE_BC4_SWIZZLE_1RRR;       /* R,R,R,1 */
    if (sel[0]==0u && sel[1]==0u && sel[2]==0u && sel[3]==2u)
        return VP_TEXTURE_BC4_SWIZZLE_R000;       /* 0,0,0,R */
    if (sel[0]==7u && sel[1]==7u && sel[2]==7u && sel[3]==2u)
        return VP_TEXTURE_BC4_SWIZZLE_R111;       /* 1,1,1,R */
    return ~0u;
}
static unsigned vp_texture_bc5_swizzle_code(const unsigned sel[4]) {
    if (sel[0]==2u && sel[1]==3u && sel[2]==0u && sel[3]==7u)
        return VP_TEXTURE_BC5_SWIZZLE_GR;         /* R,G,0,1 */
    if (sel[0]==2u && sel[1]==3u && sel[2]==0u && sel[3]==0u)
        return VP_TEXTURE_BC5_SWIZZLE_00GR;       /* R,G,0,0 */
    if (sel[0]==2u && sel[1]==2u && sel[2]==2u && sel[3]==3u)
        return VP_TEXTURE_BC5_SWIZZLE_GRRR;       /* R,R,R,G */
    if (sel[0]==3u && sel[1]==3u && sel[2]==3u && sel[3]==2u)
        return VP_TEXTURE_BC5_SWIZZLE_RGGG;       /* G,G,G,R */
    if (sel[0]==2u && sel[1]==3u && sel[2]==2u && sel[3]==3u)
        return VP_TEXTURE_BC5_SWIZZLE_GRGR;       /* R,G,R,G */
    if (sel[0]==3u && sel[1]==2u && sel[2]==0u && sel[3]==0u)
        return VP_TEXTURE_BC5_SWIZZLE_00RG;       /* G,R,0,0 */
    return ~0u;
}

unsigned vp_texture_upload_format(unsigned fmt,uint32_t word) {
    unsigned swizzle=(word>>19)&0xfffu;
    if (fmt==0x01u) return VP_TEXTURE_UPLOAD_RGBA16F;
    if ((fmt==0x24u || fmt==0x26u) && (swizzle==0xb1au || swizzle==0xf1au))
        return fmt|(swizzle==0xf1au?0x100u:0u);
    if (fmt==0x27u || fmt==0x28u) {
        unsigned types=(word>>7)&0xfffu,signed_flag=0,code;
        if (types==0x492u) signed_flag=0u;             /* four UNORM fields */
        else if (types==0x249u) signed_flag=VP_TEXTURE_UPLOAD_SIGNED; /* four SNORM */
        else return 0u; /* SINT/UINT/float/mixed/unknown: no numeric guess */
        unsigned sel[4];
        for (unsigned i=0;i<4u;i++) sel[i]=(word>>(19u+3u*i))&7u;
        if (fmt==0x27u) code=vp_texture_bc4_swizzle_code(sel);
        else code=vp_texture_bc5_swizzle_code(sel);
        if (code>0xfu) return 0u;
        return fmt|signed_flag|(code<<VP_TEXTURE_UPLOAD_SWIZZLE_SHIFT);
    }
    return 0u;
}
size_t vp_texture_upload_bytes(unsigned w,unsigned h,unsigned fmt) {
    unsigned bpp,base;
    if (!w || !h || w>4096u || h>4096u) return 0;
    if (!fmt) return (size_t)w*h*4u;
    if (fmt==VP_TEXTURE_UPLOAD_RGBA16F) return (size_t)w*h*8u;
    base=fmt&0xffu;
    if (base==0x24u || base==0x27u) bpp=8u;
    else if (base==0x26u || base==0x28u) bpp=16u;
    else return 0u;
    return (size_t)((w+3u)/4u)*((h+3u)/4u)*bpp;
}

static uint16_t float_bits_to_half(uint32_t bits) {
    const uint32_t sign=(bits>>16)&0x8000u;
    const uint32_t exponent=(bits>>23)&0xffu;
    const uint32_t mantissa=bits&0x7fffffu;
    int half_exponent=(int)exponent-127+15;
    if (exponent==0xffu) {
        if (!mantissa) return (uint16_t)(sign|0x7c00u);
        uint32_t payload=mantissa>>13;
        if (!payload) payload=1u;
        return (uint16_t)(sign|0x7c00u|payload);
    }
    if (exponent==0u) return (uint16_t)sign;
    if (half_exponent>=31) return (uint16_t)(sign|0x7c00u);
    if (half_exponent<=0) {
        if (half_exponent < -10) return (uint16_t)sign;
        uint32_t significand=mantissa|0x800000u;
        unsigned shift=(unsigned)(14-half_exponent);
        uint32_t rounded=significand>>shift;
        uint32_t remainder=significand&((1u<<shift)-1u);
        uint32_t halfway=1u<<(shift-1u);
        if (remainder>halfway || (remainder==halfway && (rounded&1u))) rounded++;
        return (uint16_t)(sign|rounded);
    }
    uint32_t rounded=mantissa>>13;
    uint32_t remainder=mantissa&0x1fffu;
    if (remainder>0x1000u || (remainder==0x1000u && (rounded&1u))) {
        rounded++;
        if (rounded==0x400u) {
            rounded=0;
            half_exponent++;
            if (half_exponent>=31) return (uint16_t)(sign|0x7c00u);
        }
    }
    return (uint16_t)(sign|((uint32_t)half_exponent<<10)|rounded);
}

int vp_texture_rgba32f_pitch_to_rgba16f(const uint8_t *src,size_t src_bytes,
    uint8_t *dst,unsigned width,unsigned height,size_t pitch,uint32_t word) {
    size_t required;
    unsigned selector[4];
    if (!src || !dst || !width || !height || width>4096u || height>4096u ||
        pitch<(size_t)width*16u ||
        (size_t)height>SIZE_MAX/pitch) return -1;
    required=pitch*height;
    if (src_bytes<required || (size_t)width*height>SIZE_MAX/8u) return -1;
    for (unsigned c=0;c<4u;c++) selector[c]=(word>>(19u+3u*c))&7u;
    for (unsigned y=0;y<height;y++) for (unsigned x=0;x<width;x++) {
        uint32_t input[4];
        uint16_t output[4];
        const uint8_t *pixel=src+(size_t)y*pitch+(size_t)x*16u;
        for (unsigned c=0;c<4u;c++) memcpy(&input[c],pixel+c*4u,4u);
        for (unsigned c=0;c<4u;c++) {
            unsigned s=selector[c];
            uint32_t bits=s>=2u && s<=5u ? input[s-2u] : s>=6u ? 0x3f800000u : 0u;
            output[c]=float_bits_to_half(bits);
        }
        memcpy(dst+((size_t)y*width+x)*8u,output,sizeof(output));
    }
    return 0;
}

static size_t cube_mip_bytes(unsigned w,unsigned h,unsigned mip_count) {
    size_t bytes=0;
    unsigned mip=0;
    if (!mip_count || mip_count>vp_texture_cube_mip_count(w,h)) return 0;
    while (mip<mip_count && w && h) {
        if ((size_t)w*h > (SIZE_MAX-bytes)/4u) return 0;
        bytes+=(size_t)w*h*4u;
        ++mip;
        if (mip==mip_count) break;
        w=w>1u?w/2u:1u;h=h>1u?h/2u:1u;
    }
    return bytes;
}

unsigned vp_texture_cube_mip_count(unsigned w,unsigned h) {
    unsigned count=0;
    if (!w || w!=h || w>1024u || (w&(w-1u))) return 0;
    do { ++count; if (w==1u) break; w>>=1; } while (1);
    return count;
}

size_t vp_texture_cube_upload_bytes(unsigned w,unsigned h) {
    return vp_texture_cube_upload_bytes_mips(w,h,vp_texture_cube_mip_count(w,h));
}

size_t vp_texture_cube_upload_bytes_mips(unsigned w,unsigned h,unsigned mips) {
    size_t face=cube_mip_bytes(w,h,mips);
    if (!mips || !face) return 0;
    /* Vita3K's GXM cube upload advances to the next face on 2 KiB boundaries
     * for RGBA8 faces at least 16x16. Reserve that gap for every face. */
    if (w>=16u && h>=16u) face=(face+2047u)&~(size_t)2047u;
    else face=(face+3u)&~(size_t)3u;
    return face<=SIZE_MAX/6u?face*6u:0;
}

static size_t cube_morton(unsigned x,unsigned y,unsigned width) {
    size_t index=0;
    for (unsigned bit=0;(1u<<bit)<width;bit++) {
        index|=(size_t)((y>>bit)&1u)<<(2u*bit);
        index|=(size_t)((x>>bit)&1u)<<(2u*bit+1u);
    }
    return index;
}

static unsigned cube_level_bh(unsigned h,unsigned fmt,unsigned bh,unsigned mips) {
    unsigned rows=fmt==8u?h:(h+3u)/4u;
    if (mips>1u) while (bh && rows<=(8u<<(bh-1u))) --bh;
    return bh;
}

size_t vp_texture_cube_storage(unsigned w,unsigned h,unsigned fmt,unsigned bh,unsigned mips) {
    size_t face=0;
    if (!mips || mips>vp_texture_cube_mip_count(w,h) || bh>5u ||
        (fmt!=8u && fmt!=0x24u && fmt!=0x26u)) return 0;
    for (unsigned mip=0;mip<mips;mip++) {
        unsigned mw=w>>mip,mh=h>>mip;
        if (!mw) mw=1u;
        if (!mh) mh=1u;
        size_t bytes=vp_texture_storage(mw,mh,fmt,cube_level_bh(mh,fmt,bh,mips));
        if (!bytes || face>SIZE_MAX-bytes) return 0;
        face+=bytes;
    }
    /* Array-layer stride is aligned to a complete base-level tile. */
    unsigned layer_bh=bh;
    while (layer_bh && h<=(8u<<(layer_bh-1u))) --layer_bh;
    size_t alignment=512u<<layer_bh;
    face=(face+alignment-1u)&~(alignment-1u);
    return face<=SIZE_MAX/6u?face*6u:0;
}

int vp_texture_cube_rgba(const uint8_t *src,size_t src_bytes,uint8_t *dst,
    size_t dst_bytes,unsigned w,unsigned h,unsigned fmt,unsigned bh,
    unsigned mips,uint32_t word) {
    size_t storage=vp_texture_cube_storage(w,h,fmt,bh,mips);
    size_t upload=vp_texture_cube_upload_bytes_mips(w,h,mips);
    if (!src || !dst || !storage || !upload || src_bytes<storage || dst_bytes<upload)
        return -1;
    uint8_t *linear=malloc((size_t)w*h*4u);
    if (!linear) return -1;
    memset(dst,0,upload);
    for (unsigned face=0;face<6u;face++) {
        size_t src_offset=0,dst_offset=0;
        for (unsigned mip=0;mip<mips;mip++) {
            unsigned mw=w>>mip,mh=h>>mip;
            if (!mw) mw=1u;
            if (!mh) mh=1u;
            unsigned level_bh=cube_level_bh(mh,fmt,bh,mips);
            size_t bytes=vp_texture_storage(mw,mh,fmt,level_bh);
            if (vp_texture_rgba(src+face*(storage/6u)+src_offset,bytes,
                                linear,mw,mh,fmt,level_bh)<0) {
                free(linear); return -1;
            }
            vp_texture_swizzle(linear,(size_t)mw*mh,word);
            for (unsigned y=0;y<mh;y++) for (unsigned x=0;x<mw;x++)
                memcpy(dst+face*(upload/6u)+dst_offset+cube_morton(x,y,mw)*4u,
                       linear+((size_t)y*mw+x)*4u,4u);
            src_offset+=bytes;
            dst_offset+=(size_t)mw*mh*4u;
        }
    }
    free(linear);
    return 0;
}

int vp_texture_cube_pack_rgba(const uint8_t *rgba,unsigned w,unsigned h,
                              uint8_t *dst,size_t dst_bytes) {
    return vp_texture_cube_pack_rgba_mips(rgba,w,h,
        vp_texture_cube_mip_count(w,h),dst,dst_bytes);
}

int vp_texture_cube_pack_rgba_mips(const uint8_t *rgba,unsigned w,unsigned h,
                                   unsigned mip_count,uint8_t *dst,
                                   size_t dst_bytes) {
    const size_t required=vp_texture_cube_upload_bytes_mips(w,h,mip_count);
    const size_t face_bytes=cube_mip_bytes(w,h,mip_count);
    size_t face_stride;
    uint8_t *a,*b;
    if (!rgba || !dst || !required || dst_bytes<required || !face_bytes) return -1;
    face_stride=required/6u;
    a=(uint8_t *)malloc(face_bytes);
    b=(uint8_t *)malloc(face_bytes);
    if (!a || !b) { free(a);free(b);return -1; }
    memset(dst,0,required);
    for (unsigned face=0;face<6u;face++) {
        unsigned mw=w,mh=h;
        size_t mip_offset=0;
        uint8_t *level=a,*next=b;
        memcpy(level,rgba,(size_t)w*h*4u);
        for (unsigned mip=0;mip<mip_count;mip++) {
            uint8_t *out=dst+(size_t)face*face_stride+mip_offset;
            for (unsigned y=0;y<mh;y++) for (unsigned x=0;x<mw;x++)
                memcpy(out+cube_morton(x,y,mw)*4u,
                       level+((size_t)y*mw+x)*4u,4u);
            mip_offset+=(size_t)mw*mh*4u;
            if (mip+1u==mip_count) break;
            {
                unsigned nw=mw>1u?mw/2u:1u,nh=mh>1u?mh/2u:1u;
                for (unsigned y=0;y<nh;y++) for (unsigned x=0;x<nw;x++)
                    for (unsigned c=0;c<4u;c++) {
                        unsigned sx=x*2u,sy=y*2u;
                        unsigned sx1=sx+1u<mw?sx+1u:sx;
                        unsigned sy1=sy+1u<mh?sy+1u:sy;
                        unsigned sum=level[((size_t)sy*mw+sx)*4u+c]+
                                     level[((size_t)sy*mw+sx1)*4u+c]+
                                     level[((size_t)sy1*mw+sx)*4u+c]+
                                     level[((size_t)sy1*mw+sx1)*4u+c];
                        next[((size_t)y*nw+x)*4u+c]=(uint8_t)((sum+2u)/4u);
                    }
                { uint8_t *old=level;level=next;next=old; }
                mw=nw;mh=nh;
            }
        }
    }
    free(a);free(b);
    return 0;
}
int vp_texture_blocks_linear(const uint8_t *src,size_t bytes,uint8_t *dst,
    unsigned w,unsigned h,unsigned fmt,unsigned bh) {
    size_t required=vp_texture_storage(w,h,fmt,bh);
    unsigned bpp;
    if (!src || !dst || !required || bytes<required) return -1;
    if (fmt==0x24u || fmt==0x27u) bpp=8u;
    else if (fmt==0x26u || fmt==0x28u) bpp=16u;
    else return -1;
    unsigned bw=(w+3u)/4u,hh=(h+3u)/4u;
    /* B113: untile by GOB sector instead of computing tiled_offset per block.
     * A 64-byte horizontal GOB holds 8 8-byte or 4 16-byte blocks, and its
     * eight rows use fixed offsets 0,16,64,80,128,144,192,208. Every full GOB
     * row is four 16-byte spans at +0/+32/+256/+288, so a whole 64-byte
     * destination sector is copied without decompressing or reordering block
     * bytes. A partial GOB tail stays on the original per-block path. */
    const size_t gob_stride=(size_t)512u<<bh;
    const unsigned per_gob=64u/bpp;
    static const size_t row_offsets[8]={0u,16u,64u,80u,128u,144u,192u,208u};
    for (unsigned gob_y=0;gob_y<hh;gob_y+=8u) {
        const unsigned rows=hh-gob_y<8u?hh-gob_y:8u;
        const size_t base=tiled_offset(0,gob_y,bw,bpp,bh);
        unsigned gob_x=0;
        for (;gob_x+per_gob<=bw;gob_x+=per_gob)
            for (unsigned row=0;row<rows;row++) {
                uint8_t *out=dst+((size_t)(gob_y+row)*bw+gob_x)*bpp;
                const uint8_t *in=src+base+
                    (size_t)(gob_x/per_gob)*gob_stride+row_offsets[row];
                memcpy(out,in,16u);
                memcpy(out+16u,in+32u,16u);
                memcpy(out+32u,in+256u,16u);
                memcpy(out+48u,in+288u,16u);
            }
        for (;gob_x<bw;gob_x++)
            for (unsigned row=0;row<rows;row++)
                memcpy(dst+((size_t)(gob_y+row)*bw+gob_x)*bpp,
                       src+tiled_offset(gob_x,gob_y+row,bw,bpp,bh),bpp);
    }
    return 0;
}

size_t vp_texture_storage(unsigned w, unsigned h, unsigned fmt, unsigned bh) {
    unsigned bpp;
    if (!w || !h || w > 4096u || h > 4096u || bh > 5u) return 0;
    if (fmt == 8u) bpp = 4u;
    else if (fmt == 0x1du || fmt == 0x1cu) bpp = 1u;
    else if (fmt == 0x18u) bpp = 2u;
    else if (fmt == 0x24u || fmt == 0x26u || fmt == 0x27u || fmt == 0x28u) {
        bpp = (fmt == 0x24u || fmt == 0x27u) ? 8u : 16u;
        w = (w + 3u) / 4u; h = (h + 3u) / 4u;
    }
    else return 0;
    return (((size_t)w * bpp + 63u) / 64u) * (512u << bh) *
           ((h + (8u << bh) - 1u) / (8u << bh));
}

static void rgb565(unsigned c, uint8_t out[4]) {
    unsigned r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
    out[3] = 255u;
}

int vp_texture_rgba(const uint8_t *src, size_t bytes, uint8_t *dst,
                    unsigned w, unsigned h, unsigned fmt, unsigned bh) {
    size_t required = vp_texture_storage(w, h, fmt, bh);
    unsigned x, y;
    if (!src || !dst || !required || bytes < required) return -1;
    /* BC4/BC5 have native compressed upload paths only. Decoding them here as
     * if they were BC1/BC3 would reinterpret endpoint/index bytes. */
    if (fmt == 0x27u || fmt == 0x28u) return -1;
    if (fmt == 8u) {
        const size_t gob_stride = (size_t)512u << bh;
        if ((w & 15u) == 0u) {
            /* B110: GOB row-group order. The old row-major order read 64 bytes
             * of a GOB then jumped away, revisiting every 512<<bh block once
             * per row. Four horizontal GOBs (64 pixels) are now finished for
             * all eight rows before moving right, so both the source block and
             * the destination row segment stay hot. Host timing against the
             * old order on the large mixed textures showed this grouping
             * faster whenever a destination row starts line-aligned. Narrow
             * unaligned widths kept the old order: the same measurement showed
             * grouping can lose there (misaligned 64-byte row starts). */
            static const size_t row_offsets[8] = {0u,16u,64u,80u,
                                                  128u,144u,192u,208u};
            for (unsigned gob_y = 0; gob_y < h; gob_y += 8u) {
                const unsigned rows = h - gob_y < 8u ? h - gob_y : 8u;
                const size_t base = tiled_offset(0, gob_y, w, 4u, bh);
                unsigned gob_x = 0;
                for (; gob_x + 64u <= w; gob_x += 64u) {
                    for (unsigned row = 0; row < rows; row++) {
                        uint8_t *out = dst +
                            ((size_t)(gob_y + row) * w + gob_x) * 4u;
                        const uint8_t *in = src + base +
                            (size_t)(gob_x / 16u) * gob_stride +
                            row_offsets[row];
                        for (unsigned gob = 0; gob < 4u; gob++) {
                            const uint8_t *row_in = in + (size_t)gob * gob_stride;
                            uint8_t *gob_out = out + (size_t)gob * 64u;
                            memcpy(gob_out, row_in, 16u);
                            memcpy(gob_out + 16u, row_in + 32u, 16u);
                            memcpy(gob_out + 32u, row_in + 256u, 16u);
                            memcpy(gob_out + 48u, row_in + 288u, 16u);
                        }
                    }
                }
                for (; gob_x < w; gob_x += 16u) {
                    const uint8_t *in = src + base +
                                        (size_t)(gob_x / 16u) * gob_stride;
                    for (unsigned row = 0; row < rows; row++) {
                        uint8_t *out = dst +
                            ((size_t)(gob_y + row) * w + gob_x) * 4u;
                        const uint8_t *row_in = in + row_offsets[row];
                        memcpy(out, row_in, 16u);
                        memcpy(out + 16u, row_in + 32u, 16u);
                        memcpy(out + 32u, row_in + 256u, 16u);
                        memcpy(out + 48u, row_in + 288u, 16u);
                    }
                }
            }
        } else {
            for (y = 0; y < h; y++) {
                const size_t row_base = tiled_offset(0, y, w, 4u, bh);
                uint8_t *out = dst + (size_t)y * w * 4u;
                for (x = 0; x + 16u <= w; x += 16u) {
                    const uint8_t *in = src + row_base +
                                        (x / 16u) * gob_stride;
                    memcpy(out + (size_t)x * 4u, in, 16u);
                    memcpy(out + (size_t)x * 4u + 16u, in + 32u, 16u);
                    memcpy(out + (size_t)x * 4u + 32u, in + 256u, 16u);
                    memcpy(out + (size_t)x * 4u + 48u, in + 288u, 16u);
                }
                for (; x + 4u <= w; x += 4u)
                    memcpy(out + (size_t)x * 4u,
                           src + tiled_offset(x, y, w, 4u, bh), 16u);
                if (x < w)
                    memcpy(out + (size_t)x * 4u,
                           src + tiled_offset(x, y, w, 4u, bh), (w - x) * 4u);
            }
        }
    } else if (fmt==0x1du || fmt==0x1cu || fmt==0x18u) {
        unsigned bpp=fmt==0x18u?2u:1u;
        for (y=0;y<h;y++) for (x=0;x<w;x++) {
            const uint8_t *in=src+tiled_offset(x,y,w,bpp,bh);
            uint8_t *out=dst+((size_t)y*w+x)*4u;
            out[0]=in[0];out[1]=bpp==2u?in[1]:0u;out[2]=0u;out[3]=255u;
        }
    } else {
        unsigned bw = (w + 3u) / 4u, hh = (h + 3u) / 4u;
        for (y = 0; y < hh; y++) for (x = 0; x < bw; x++) {
            const uint8_t *block = src + tiled_offset(x, y, bw, fmt == 0x24u ? 8u : 16u, bh);
            const uint8_t *p = block + (fmt == 0x26u ? 8u : 0u);
            uint8_t alpha[8] = {0};
            uint64_t alpha_indices = 0;
            unsigned c0 = p[0] | ((unsigned)p[1] << 8), c1 = p[2] | ((unsigned)p[3] << 8);
            uint32_t indices = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                               ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
            uint8_t colors[4][4];
            int four_colors = c0 > c1 || fmt == 0x26u;
            if (fmt == 0x26u) {
                alpha[0] = block[0]; alpha[1] = block[1];
                if (alpha[0] > alpha[1]) {
                    for (unsigned i = 1; i <= 6u; i++)
                        alpha[i+1] = ((7u-i)*alpha[0] + i*alpha[1]) / 7u;
                } else {
                    for (unsigned i = 1; i <= 4u; i++)
                        alpha[i+1] = ((5u-i)*alpha[0] + i*alpha[1]) / 5u;
                    alpha[6] = 0; alpha[7] = 255;
                }
                for (unsigned i = 0; i < 6u; i++)
                    alpha_indices |= (uint64_t)block[i+2] << (8u*i);
            }
            rgb565(c0, colors[0]); rgb565(c1, colors[1]);
            for (unsigned c = 0; c < 3u; c++) {
                colors[2][c] = four_colors ? (2u * colors[0][c] + colors[1][c]) / 3u :
                                                  (colors[0][c] + colors[1][c]) / 2u;
                colors[3][c] = four_colors ? (colors[0][c] + 2u * colors[1][c]) / 3u : 0u;
            }
            colors[2][3] = 255u; colors[3][3] = four_colors ? 255u : 0u;
            for (unsigned py = 0; py < 4u; py++) for (unsigned px = 0; px < 4u; px++) {
                unsigned dx = x * 4u + px, dy = y * 4u + py;
                unsigned index = (indices >> (2u * (py * 4u + px))) & 3u;
                if (dx < w && dy < h) {
                    memcpy(dst + ((size_t)dy * w + dx) * 4u, colors[index], 4u);
                    if (fmt == 0x26u)
                        dst[((size_t)dy*w+dx)*4u+3u] = alpha[(alpha_indices >> (3u*(py*4u+px))) & 7u];
                }
            }
        }
    }
    return 0;
}

/* The GXM producer surface is linear A8B8G8R8 (R,G,B,A in increasing
 * addresses). A matching guest view therefore needs the raw TIC format 8 and
 * the identity channel swizzle (R,G,B,A selected from channels 2..5). The
 * upload enum is zero for RGBA8, so it cannot distinguish RGBA8 from every
 * other unhandled layout; the raw descriptor fields must decide. */
const char *vp_texture_surface_view_check(const VPTextureSurfaceView *view) {
    if (view == NULL) return "missing";
    if (view->view_width == 0u || view->view_height == 0u) return "zero-size";
    if (view->tic_format != 8u) return "format";
    if (view->header != 2u && view->header != 3u) return "header";
    if (view->texture_type != 1u) return "type";
    if (view->depth != 1u) return "depth";
    if (view->block_depth != 0u) return "block-depth";
    if (view->max_mip_level != 0u) return "mip";
    if (((view->tic_word0 >> 19) & 0xfffu) != 0xb1au) return "swizzle";
    /* Four 3-bit component types at bits 7..18. The GXM producer stores
     * A8B8G8R8 as UNORM; accepting SNORM/SINT/UINT/sRGB/FLOAT here would read
     * the same bits with a different numeric interpretation. Unknown encodings
     * are refusals, not guesses. */
    if (((view->tic_word0 >> 7) & 0xfffu) != 0x492u) return "component-type";
    if (view->view_width != view->surface_width ||
        view->view_height != view->surface_height) return "size";
    return NULL;
}

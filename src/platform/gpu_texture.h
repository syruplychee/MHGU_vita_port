#ifndef VP_GPU_TEXTURE_H
#define VP_GPU_TEXTURE_H
#include <stddef.h>
#include <stdint.h>

#define VP_TEXTURE_UPLOAD_RGBA16F 0x101u

/* Encoded upload identity returned by vp_texture_upload_format(). The low byte
 * is the Maxwell TIC format. The high bits distinguish descriptor variants that
 * would otherwise all collapse to the same low byte:
 *   bit 8       BC1/BC3 constant-alpha-one swizzle (legacy encoding)
 *   bit 9       BC4/BC5 SNORM (GXM SBC4/SBC5 instead of UBC4/UBC5)
 *   bits 12..15 swizzle variant code, validated against the exact GXM enum
 * See gpu_texture.c for the accepted BC4/BC5 selector combinations. */
#define VP_TEXTURE_UPLOAD_SIGNED 0x00000200u
#define VP_TEXTURE_UPLOAD_SWIZZLE_SHIFT 12u
#define VP_TEXTURE_UPLOAD_SWIZZLE_MASK 0x0000f000u

/* GXM-compatible swizzle variant codes. BC4 codes follow the SCE_GXM
 * SWIZZLE1_* order; BC5 codes follow the SCE_GXM SWIZZLE2_* order. */
enum {
    VP_TEXTURE_BC4_SWIZZLE_R = 0,
    VP_TEXTURE_BC4_SWIZZLE_000R,
    VP_TEXTURE_BC4_SWIZZLE_111R,
    VP_TEXTURE_BC4_SWIZZLE_RRRR,
    VP_TEXTURE_BC4_SWIZZLE_0RRR,
    VP_TEXTURE_BC4_SWIZZLE_1RRR,
    VP_TEXTURE_BC4_SWIZZLE_R000,
    VP_TEXTURE_BC4_SWIZZLE_R111
};
enum {
    VP_TEXTURE_BC5_SWIZZLE_GR = 0,
    VP_TEXTURE_BC5_SWIZZLE_00GR,
    VP_TEXTURE_BC5_SWIZZLE_GRRR,
    VP_TEXTURE_BC5_SWIZZLE_RGGG,
    VP_TEXTURE_BC5_SWIZZLE_GRGR,
    VP_TEXTURE_BC5_SWIZZLE_00RG
};

/* Base mip of a 2D Maxwell block-linear RGBA8, BC1, BC3, BC4 or BC5 texture. */
size_t vp_texture_storage(unsigned width, unsigned height, unsigned format, unsigned block_h);
int vp_texture_rgba(const uint8_t *src, size_t bytes, uint8_t *dst,
                    unsigned width, unsigned height, unsigned format, unsigned block_h);
void vp_texture_swizzle(uint8_t *rgba, size_t pixels, uint32_t tic_word0);
unsigned vp_texture_upload_format(unsigned format, uint32_t tic_word0);
size_t vp_texture_upload_bytes(unsigned w,unsigned h,unsigned format);
int vp_texture_blocks_linear(const uint8_t *src,size_t bytes,uint8_t *dst,
    unsigned w,unsigned h,unsigned format,unsigned block_h);
/* Convert a pitch-linear RGBA32F texture into GXM's RGBA16F format while
 * applying the TIC component remap. */
int vp_texture_rgba32f_pitch_to_rgba16f(const uint8_t *src,size_t src_bytes,
    uint8_t *dst,unsigned width,unsigned height,size_t pitch,uint32_t tic_word0);
/* Build a GXM cube-map payload from one decoded RGBA face. The face is
 * replicated six times and a complete Morton-swizzled mip chain is generated. */
unsigned vp_texture_cube_mip_count(unsigned width,unsigned height);
size_t vp_texture_cube_upload_bytes(unsigned width,unsigned height);
int vp_texture_cube_pack_rgba(const uint8_t *rgba,unsigned width,unsigned height,
    uint8_t *dst,size_t dst_bytes);
size_t vp_texture_cube_upload_bytes_mips(unsigned width,unsigned height,
    unsigned mip_count);
int vp_texture_cube_pack_rgba_mips(const uint8_t *rgba,unsigned width,
    unsigned height,unsigned mip_count,uint8_t *dst,size_t dst_bytes);
/* Real Maxwell cube faces/mips -> GXM RGBA8 Morton layout. No face or mip
 * synthesis. Only square 2D RGBA8/BC1/BC3 cubes, block depth zero. */
size_t vp_texture_cube_storage(unsigned width,unsigned height,unsigned format,
    unsigned block_h,unsigned mip_count);
int vp_texture_cube_rgba(const uint8_t *src,size_t src_bytes,uint8_t *dst,
    size_t dst_bytes,unsigned width,unsigned height,unsigned format,
    unsigned block_h,unsigned mip_count,uint32_t tic_word0);

/* Guest TIC view of an existing linear A8B8G8R8 colour surface. The upload
 * format enum maps RGBA8 to zero, so the raw TIC format, the raw word0
 * component-type fields (bits 7..18, UNORM only) and the raw channel swizzle
 * are the only trustworthy identity here: format 8 is not always plain RGBA.
 * Returns NULL when the view matches the producer, or a stable reason tag.
 * Depth/shadow, cube, layered and mip views stay explicit refusals until their
 * sampling semantics are implemented. */
typedef struct {
    unsigned tic_format;      /* TIC word0 bits 0..6 */
    uint32_t tic_word0;       /* raw word0, including the channel swizzle */
    unsigned header;          /* TIC header version */
    unsigned texture_type;    /* Maxwell view type: 1 = 2D, 3 = cube */
    unsigned depth;           /* TIC depth field, 1 for a 2D view */
    unsigned block_depth;
    unsigned max_mip_level;
    unsigned view_width, view_height;
    unsigned surface_width, surface_height;
} VPTextureSurfaceView;

const char *vp_texture_surface_view_check(const VPTextureSurfaceView *view);
#endif

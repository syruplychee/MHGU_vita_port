#ifndef VP_GPU_TEXTURE_H
#define VP_GPU_TEXTURE_H
#include <stddef.h>
#include <stdint.h>

#define VP_TEXTURE_UPLOAD_RGBA16F 0x101u
/* Lossless vertex RGBA32F representation: each logical pixel is stored as two
 * physical RG32F texels, so all four original 32-bit channel patterns survive
 * and the physical texture is twice as wide. The identity is distinct from
 * RGBA16F (0x101), signed BC4/BC5 (0x227/0x228) and their swizzle variants. */
#define VP_TEXTURE_UPLOAD_RGBA32F_RG32F 0x201u
#define VP_TEXTURE_RG32F_LOGICAL_MAX_WIDTH 2048u
#define VP_TEXTURE_RG32F_MAX_HEIGHT 4096u

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
/* Fused Maxwell block-linear RGBA8 tiled decode and TIC selector permutation.
 * The result is byte-identical to vp_texture_rgba(..., 8, block_h) followed by
 * vp_texture_swizzle(..., tic_word0): all selectors, GOB tails and the
 * non-multiple-of-16 row path keep their original semantics, but the decoded
 * bytes are written once instead of being permuted in a second pass. */
int vp_texture_rgba8_untile_swizzle(const uint8_t *src, size_t bytes,
    uint8_t *dst, unsigned width, unsigned height, unsigned block_h,
    uint32_t tic_word0);
/* Same fusion for the pitch-linear RGBA8 form (TIC header 2). */
int vp_texture_rgba8_linear_swizzle(const uint8_t *src, size_t bytes,
    uint8_t *dst, unsigned width, unsigned height, size_t pitch,
    uint32_t tic_word0);
unsigned vp_texture_upload_format(unsigned format, uint32_t tic_word0);
size_t vp_texture_upload_bytes(unsigned w,unsigned h,unsigned format);
int vp_texture_blocks_linear(const uint8_t *src,size_t bytes,uint8_t *dst,
    unsigned w,unsigned h,unsigned format,unsigned block_h);
/* Convert a pitch-linear RGBA32F texture into GXM's RGBA16F format while
 * applying the TIC component remap. */
int vp_texture_rgba32f_pitch_to_rgba16f(const uint8_t *src,size_t src_bytes,
    uint8_t *dst,unsigned width,unsigned height,size_t pitch,uint32_t tic_word0);
/* Lossless vertex upload: map the TIC channel selectors onto logical RGBA,
 * then store (R,G) and (B,A) as two RG32F texels per logical pixel. All four
 * 32-bit patterns are copied unchanged; no half conversion is performed.
 * The destination is tightly packed at 16 bytes per logical pixel. */
int vp_texture_rgba32f_pitch_to_rg32f(const uint8_t *src,size_t src_bytes,
    uint8_t *dst,size_t dst_bytes,unsigned width,unsigned height,size_t pitch,
    uint32_t tic_word0);
/* Measured vertex RGBA32F contract: TIC format/header/type, pitch-linear 2D,
 * no mips, point min/mag filters and no border address mode. */
typedef struct {
    unsigned width,height;
    uint32_t tic_word0;
    unsigned tic_format, tic_header, texture_type;
    unsigned depth, block_depth, max_mip_level;
    size_t pitch, storage;
    uint32_t sampler[2];
} VPTextureVertexRg32fView;

/* Returns NULL when the view is the measured layout, otherwise a stable
 * reason tag. Unknown selectors and SINT/UINT component fields are refused
 * rather than numerically reinterpreted. */
const char *vp_texture_vertex_rg32f_check(const VPTextureVertexRg32fView *view);
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

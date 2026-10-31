#include "gpu_texture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t input[4096], output[64 * 64 * 4];
static uint8_t float_source[48], float_half[16];
static uint8_t cube_rgba[4 * 4 * 4],cube_data[504];
static uint8_t cube_rgba64[64 * 64 * 4],cube_data64[135168];

int main(void) {
    size_t size = vp_texture_storage(32, 16, 8, 1);
    assert(size == 2048);
    input[0] = 1; input[32] = 2; input[256] = 3;
    input[16] = 4; input[64] = 5; input[1024] = 6; input[512] = 7;
    assert(vp_texture_rgba(input, size - 1, output, 32, 16, 8, 1) == -1);
    assert(vp_texture_rgba(input, size, output, 32, 16, 8, 1) == 0);
    assert(output[0] == 1 && output[4*4] == 2 && output[8*4] == 3);
    assert(output[32*4] == 4 && output[2*32*4] == 5);
    assert(output[16*4] == 6 && output[8*32*4] == 7);
    memset(input, 0, sizeof(input));
    input[0] = 0; input[1] = 0xf8; /* red RGB565 endpoint; all indices zero */
    assert(vp_texture_rgba(input, 512, output, 4, 4, 0x24, 0) == 0);
    for (unsigned i = 0; i < 16; i++) {
        assert(output[i*4] == 255 && output[i*4+1] == 0 &&
               output[i*4+2] == 0 && output[i*4+3] == 255);
    }
    memset(input, 0, sizeof(input));
    memset(input+4, 255, 4); /* c0 <= c1, index 3: transparent */
    assert(vp_texture_rgba(input, 512, output, 3, 2, 0x24, 0) == 0);
    for (unsigned i = 0; i < 6*4; i++) assert(output[i] == 0);
    memset(input, 0, sizeof(input));
    input[0] = 255; input[1] = 0;
    input[8] = 0; input[9] = 0xf8;
    assert(vp_texture_rgba(input, 512, output, 4, 4, 0x26, 0) == 0);
    for (unsigned i = 0; i < 16; i++) assert(output[i*4] == 255 && output[i*4+3] == 255);
    input[2] = 2; /* interpolated alpha index 2 = (6*255+0)/7 */
    assert(vp_texture_rgba(input, 512, output, 4, 4, 0x26, 0) == 0);
    assert(output[3] == 218);
    assert(vp_texture_storage(4, 4, 0x17, 0) == 0); /* unsupported is explicit */
    assert(vp_texture_storage(4, 4, 8, 6) == 0);
    memset(input,0,sizeof(input));input[0]=17;input[1]=29;
    assert(vp_texture_storage(64,8,0x1d,0)==512);
    assert(vp_texture_rgba(input,512,output,64,8,0x1d,0)==0);
    assert(output[0]==17 && output[1]==0 && output[3]==255);
    assert(output[4]==29);
    assert(vp_texture_rgba(input,512,output,32,8,0x18,0)==0);
    assert(output[0]==17 && output[1]==29 && output[2]==0 && output[3]==255);
    vp_texture_swizzle(output,32*8,(2u<<19)|(2u<<22)|(2u<<25)|(7u<<28));
    assert(output[0]==17 && output[1]==17 && output[2]==17 && output[3]==255);
    memset(input,0,sizeof(input));
    memset(input,11,8);memset(input+8,22,8);memset(input+32,33,8);
    assert(vp_texture_blocks_linear(input,512,output,12,4,0x24,0)==0);
    assert(output[0]==11 && output[8]==22 && output[16]==33);
    assert(vp_texture_blocks_linear(input,511,output,12,4,0x24,0)==-1);
    assert(vp_texture_upload_format(0x24,0xb1au<<19)==0x24);
    assert(vp_texture_upload_format(0x26,0xf1au<<19)==0x126);
    assert(vp_texture_upload_format(0x24,0x7012491d)==0);
    assert(vp_texture_upload_bytes(12,4,0x24)==24);
    assert(vp_texture_upload_format(1u,0x58d7ff81u)==VP_TEXTURE_UPLOAD_RGBA16F);
    assert(vp_texture_upload_bytes(2,1,VP_TEXTURE_UPLOAD_RGBA16F)==16);
    {
        const uint32_t pixel0[4]={0x3f800000u,0xc0000000u,0x3f000000u,0x41700000u};
        const uint32_t pixel1[4]={0x00000000u,0x3f800000u,0xbf800000u,0x7f800000u};
        const uint16_t expected[8]={0x3c00u,0xc000u,0x3800u,0x4b80u,
                                    0x0000u,0x3c00u,0xbc00u,0x7c00u};
        memset(float_source,0xa5,sizeof(float_source));
        memcpy(float_source,pixel0,sizeof(pixel0));
        memcpy(float_source+16,pixel1,sizeof(pixel1));
        assert(vp_texture_rgba32f_pitch_to_rgba16f(float_source,sizeof(float_source),
            float_half,2,1,32,0x58d7ff81u)==0);
        assert(memcmp(float_half,expected,sizeof(expected))==0);
        assert(vp_texture_rgba32f_pitch_to_rgba16f(float_source,31,
            float_half,2,1,32,0x58d7ff81u)==-1);
    }
    assert(vp_texture_cube_mip_count(4,4)==3);
    assert(vp_texture_cube_upload_bytes(4,4)==504);
    assert(vp_texture_cube_mip_count(6,6)==0);
    assert(vp_texture_cube_upload_bytes(64,64)==135168);
    assert(vp_texture_cube_upload_bytes_mips(64,64,6)==135168);
    assert(vp_texture_cube_upload_bytes_mips(64,64,0)==0);
    assert(vp_texture_cube_upload_bytes_mips(64,64,8)==0);
    for (unsigned i=0;i<16u;i++) {
        cube_rgba[i*4]=(uint8_t)i;cube_rgba[i*4+1]=9;
        cube_rgba[i*4+2]=7;cube_rgba[i*4+3]=255;
    }
    assert(vp_texture_cube_pack_rgba(cube_rgba,4,4,cube_data,sizeof(cube_data))==0);
    /* GXM cube maps store each face in Morton order, then its mip chain. */
    assert(cube_data[0]==0 && cube_data[8]==1 && cube_data[4]==4 && cube_data[12]==5);
    assert(cube_data[64]==3 && cube_data[72]==5 && cube_data[68]==11 && cube_data[76]==13);
    assert(cube_data[80]==8 && cube_data[84]==0 && cube_data[92]==1 && cube_data[88]==4);
    memset(cube_rgba64,0x5a,sizeof(cube_rgba64));
    assert(vp_texture_cube_pack_rgba_mips(cube_rgba64,64,64,6,
        cube_data64,sizeof(cube_data64))==0);
    assert(cube_data64[0]==0x5a && cube_data64[21824]==0x5a &&
           cube_data64[22528]==0x5a && cube_data64[112640]==0x5a);
    {
        /* Six genuinely different faces, with hand-authored mip colours.
         * Level offsets are 0/1024/1536 in Maxwell, 0/1024/1280 in GXM.
         * Replicating one face or generating mips cannot pass this fixture. */
        uint8_t source[12288],upload[12288];
        const unsigned offsets[3]={0,1024,1536},sizes[3]={1024,512,512};
        assert(vp_texture_cube_storage(16,16,8,1,3)==sizeof(source));
        assert(vp_texture_cube_storage(64,64,0x26,1,6)==43008);
        memset(source,0,sizeof(source));
        for (unsigned f=0;f<6u;f++) for (unsigned m=0;m<3u;m++)
            for (unsigned p=0;p<sizes[m];p+=4u) {
                uint8_t *v=source+f*2048u+offsets[m]+p;
                v[0]=(uint8_t)(10u+f);v[1]=(uint8_t)(30u+m);v[2]=70;v[3]=255;
            }
        assert(vp_texture_cube_rgba(source,sizeof(source),upload,sizeof(upload),
            16,16,8,1,3,0xb1au<<19)==0);
        for (unsigned f=0;f<6u;f++) {
            const unsigned gxm_offsets[3]={0,1024,1280};
            for (unsigned m=0;m<3u;m++) {
                const uint8_t *v=upload+f*2048u+gxm_offsets[m];
                assert(v[0]==10u+f && v[1]==30u+m && v[2]==70 && v[3]==255);
            }
        }
        assert(vp_texture_cube_rgba(source,sizeof(source)-1u,upload,sizeof(upload),
            16,16,8,1,3,0xb1au<<19)==-1);
        assert(vp_texture_cube_rgba(source,sizeof(source),upload,sizeof(upload)-1u,
            16,16,8,1,3,0xb1au<<19)==-1);
    }
    {
        /* Producer-surface view compatibility. The positive cases are the two
         * documented 2D RGBA8 layouts; every reinterpretation stays a refusal. */
        VPTextureSurfaceView view;
        memset(&view, 0, sizeof(view));
        view.tic_format = 8u;
        /* format 8 | four UNORM component types | identity RGBA swizzle */
        view.tic_word0 = (0xb1au << 19) | 0x24900u | 8u;
        view.header = 3u;
        view.texture_type = 1u;
        view.depth = 1u;
        view.view_width = 64u;
        view.view_height = 32u;
        view.surface_width = 64u;
        view.surface_height = 32u;
        assert(vp_texture_surface_view_check(&view) == NULL);
        view.header = 2u;
        assert(vp_texture_surface_view_check(&view) == NULL);
        view.tic_format = 0x24u;
        assert(strcmp(vp_texture_surface_view_check(&view), "format") == 0);
        view.tic_format = 8u;
        view.tic_word0 = 0x24900u | 8u; /* zero swizzle would sample channel 0 */
        assert(strcmp(vp_texture_surface_view_check(&view), "swizzle") == 0);
        view.tic_word0 = (0xf1au << 19) | 0x24900u | 8u; /* forces alpha to one */
        assert(strcmp(vp_texture_surface_view_check(&view), "swizzle") == 0);
        /* Same format and swizzle, but the component types are interpreted as
         * SNORM (3) / SINT (4) rather than UNORM. Numeric reinterpretation. */
        view.tic_word0 = (0xb1au << 19) |
            ((3u << 7) | (3u << 10) | (3u << 13) | (3u << 16)) | 8u;
        assert(strcmp(vp_texture_surface_view_check(&view), "component-type") == 0);
        view.tic_word0 = (0xb1au << 19) |
            ((4u << 7) | (4u << 10) | (4u << 13) | (4u << 16)) | 8u;
        assert(strcmp(vp_texture_surface_view_check(&view), "component-type") == 0);
        view.tic_word0 = (0xb1au << 19) | 8u; /* unknown/zero component types */
        assert(strcmp(vp_texture_surface_view_check(&view), "component-type") == 0);
        view.tic_word0 = (0xb1au << 19) | 0x24900u | 8u;
        view.texture_type = 3u;
        assert(strcmp(vp_texture_surface_view_check(&view), "type") == 0);
        view.texture_type = 1u;
        view.depth = 2u;
        assert(strcmp(vp_texture_surface_view_check(&view), "depth") == 0);
        view.depth = 1u;
        view.block_depth = 1u;
        assert(strcmp(vp_texture_surface_view_check(&view), "block-depth") == 0);
        view.block_depth = 0u;
        view.max_mip_level = 1u;
        assert(strcmp(vp_texture_surface_view_check(&view), "mip") == 0);
        view.max_mip_level = 0u;
        view.header = 1u;
        assert(strcmp(vp_texture_surface_view_check(&view), "header") == 0);
        view.header = 3u;
        view.view_width = 32u;
        assert(strcmp(vp_texture_surface_view_check(&view), "size") == 0);
        view.view_width = 0u;
        assert(strcmp(vp_texture_surface_view_check(&view), "zero-size") == 0);
        assert(strcmp(vp_texture_surface_view_check(NULL), "missing") == 0);
    }
    {
        /* BC4/BC5 base-level storage and native GOB -> linear block detile.
         * The expected byte positions below are independent hand-computed GOB
         * offsets, not output from vp_texture_blocks_linear itself. */
        uint8_t src[512], dst[512];
        assert(vp_texture_storage(256, 128, 0x27, 0) == 16384);
        assert(vp_texture_storage(256, 128, 0x28, 0) == 32768);
        assert(vp_texture_storage(32, 16, 0x24, 1) == 1024);
        assert(vp_texture_storage(32, 16, 0x26, 1) == 2048);
        assert(vp_texture_storage(32, 16, 0x27, 1) == 1024);
        assert(vp_texture_storage(32, 16, 0x28, 1) == 2048);
        assert(vp_texture_storage(9, 5, 0x27, 0) == 512);
        assert(vp_texture_storage(9, 5, 0x28, 0) == 512);
        assert(vp_texture_storage(9, 5, 0x27, 6) == 0);
        assert(vp_texture_storage(4097, 1, 0x27, 0) == 0);
        assert(vp_texture_cube_storage(8, 8, 0x27, 0, 1) == 0);
        assert(vp_texture_cube_storage(8, 8, 0x28, 0, 1) == 0);
        assert(vp_texture_upload_bytes(256, 128, 0x27) == 16384);
        assert(vp_texture_upload_bytes(256, 128, 0x28) == 32768);
        assert(vp_texture_upload_bytes(9, 5, 0x27) == 48);
        assert(vp_texture_upload_bytes(9, 5, 0x28) == 96);
        assert(vp_texture_upload_bytes(6, 6, 0x27) == 32);
        assert(vp_texture_upload_bytes(6, 6, 0x28) == 64);
        assert(vp_texture_upload_bytes(12, 4, 0x26) == 48);
        assert(vp_texture_upload_bytes(12, 4, 0x124) == 24);
        assert(vp_texture_upload_bytes(4097, 1, 0x27) == 0);
        assert(vp_texture_upload_bytes(0, 1, 0x28) == 0);
        memset(src, 0, sizeof(src));
        assert(vp_texture_rgba(src, 512, dst, 4, 4, 0x27, 0) == -1);
        assert(vp_texture_rgba(src, 512, dst, 4, 4, 0x28, 0) == -1);

        /* 9x5 BC4: block columns/rows are 3/2. Tiled block offsets are
         * (0,0)=0 (1,0)=8 (2,0)=32 (0,1)=16 (1,1)=24 (2,1)=48. */
        const unsigned bc4_tiled[6] = {0, 8, 32, 16, 24, 48};
        memset(src, 0xee, sizeof(src));
        for (unsigned i = 0; i < 6; i++) memset(src + bc4_tiled[i], 0x40 + i, 8);
        assert(vp_texture_blocks_linear(src, 512, dst, 9, 5, 0x27, 0) == 0);
        for (unsigned i = 0; i < 6; i++)
            for (unsigned b = 0; b < 8; b++)
                assert(dst[i * 8 + b] == 0x40 + i);
        assert(vp_texture_blocks_linear(src, 511, dst, 9, 5, 0x27, 0) == -1);

        /* 8x8 BC5: 16-byte blocks at (0,0)=0 (1,0)=32 (0,1)=16 (1,1)=48.
         * Linear order swaps the 1,0 and 0,1 blocks relative to BC4. */
        const unsigned bc5_tiled[4] = {0, 32, 16, 48};
        memset(src, 0xee, sizeof(src));
        for (unsigned i = 0; i < 4; i++) memset(src + bc5_tiled[i], 0x50 + i, 16);
        assert(vp_texture_blocks_linear(src, 512, dst, 8, 8, 0x28, 0) == 0);
        for (unsigned i = 0; i < 4; i++)
            for (unsigned b = 0; b < 16; b++)
                assert(dst[i * 16 + b] == 0x50 + i);
        assert(vp_texture_blocks_linear(src, 511, dst, 8, 8, 0x28, 0) == -1);

        /* Odd 9x5 BC5: (0,0)=0 (1,0)=32 (2,0)=256 (0,1)=16 (1,1)=48
         * (2,1)=272 in the tiled source; linear dst is raster block order. */
        const unsigned bc5_odd_tiled[6] = {0, 32, 256, 16, 48, 272};
        memset(src, 0xee, sizeof(src));
        for (unsigned i = 0; i < 6; i++) memset(src + bc5_odd_tiled[i], 0x60 + i, 16);
        assert(vp_texture_blocks_linear(src, 512, dst, 9, 5, 0x28, 0) == 0);
        for (unsigned i = 0; i < 6; i++)
            for (unsigned b = 0; b < 16; b++)
                assert(dst[i * 16 + b] == 0x60 + i);
    }
    {
        /* Raw TIC word0 identity. 0x70124927 is the observed village DXN1
         * descriptor: format 0x27, four UNORM fields, selectors R,0,0,1. */
        const uint32_t bc4 = 0x70124927u;
        const uint32_t type_bits = 0xfffu << 7;
        assert(vp_texture_upload_format(0x27, bc4) ==
               (0x27u | (VP_TEXTURE_BC4_SWIZZLE_R << VP_TEXTURE_UPLOAD_SWIZZLE_SHIFT)));
        /* G,0,0,1 has no single-channel GXM BC4 equivalent. */
        assert(vp_texture_upload_format(0x27, (bc4 & ~(0xfffu << 19)) |
               ((3u << 19) | (0u << 22) | (0u << 25) | (7u << 28))) == 0);
        /* Arbitrary second selector is not a blanket-accepted shuffle. */
        assert(vp_texture_upload_format(0x27, (bc4 & ~(0xfffu << 19)) |
               ((2u << 19) | (3u << 22) | (0u << 25) | (7u << 28))) == 0);
        /* OneInt(6) is not the reviewed OneFloat(7). */
        assert(vp_texture_upload_format(0x27, (bc4 & ~(0xfffu << 19)) |
               ((2u << 19) | (0u << 22) | (0u << 25) | (6u << 28))) == 0);
        /* SNORM maps to SBC4 rather than silently reusing UBC4. */
        {
            uint32_t snorm = (bc4 & ~type_bits) | ((0x249u << 7) & type_bits);
            assert(vp_texture_upload_format(0x27, snorm) ==
                   (0x27u | VP_TEXTURE_UPLOAD_SIGNED));
        }
        /* SINT and mixed component fields stay explicit refusals. */
        assert(vp_texture_upload_format(0x27, (bc4 & ~type_bits) |
               ((3u << 7) | (3u << 10) | (3u << 13) | (3u << 16))) == 0);
        assert(vp_texture_upload_format(0x27, (bc4 & ~type_bits) |
               ((2u << 7) | (2u << 10) | (1u << 13) | (1u << 16))) == 0);
        assert(vp_texture_upload_bytes(256, 128, 0x227) == 16384);
        assert(vp_texture_upload_bytes(256, 128, 0x228) == 32768);

        /* Every supported BC4/BC5 selector tuple maps to its exact variant
         * code; codes are the order of the reviewed GXM enums. */
        {
            const unsigned bc4sel[8][4] = {
                {2,0,0,7},{2,0,0,0},{2,7,7,7},{2,2,2,2},
                {2,2,2,0},{2,2,2,7},{0,0,0,2},{7,7,7,2}
            };
            for (unsigned i = 0; i < 8; i++) {
                uint32_t w = 0x27u | (0x492u << 7);
                for (unsigned c = 0; c < 4; c++) w |= bc4sel[i][c] << (19u + 3u * c);
                assert(vp_texture_upload_format(0x27, w) ==
                       (0x27u | (i << VP_TEXTURE_UPLOAD_SWIZZLE_SHIFT)));
            }
            const unsigned bc5sel[6][4] = {
                {2,3,0,7},{2,3,0,0},{2,2,2,3},{3,3,3,2},{2,3,2,3},{3,2,0,0}
            };
            for (unsigned i = 0; i < 6; i++) {
                uint32_t w = 0x28u | (0x492u << 7);
                for (unsigned c = 0; c < 4; c++) w |= bc5sel[i][c] << (19u + 3u * c);
                assert(vp_texture_upload_format(0x28, w) ==
                       (0x28u | (i << VP_TEXTURE_UPLOAD_SWIZZLE_SHIFT)));
            }
        }
        {
            const unsigned gr[4] = {2,3,0,7};
            uint32_t w5 = 0x28u | (0x492u << 7);
            for (unsigned c = 0; c < 4; c++)
                w5 |= gr[c] << (19u + 3u * c);
            assert(vp_texture_upload_format(0x28, w5) == 0x28u);
            {
                uint32_t snorm = (w5 & ~type_bits) | ((0x249u << 7) & type_bits);
                assert(vp_texture_upload_format(0x28, snorm) ==
                       (0x28u | VP_TEXTURE_UPLOAD_SIGNED));
            }
            /* G,R,0,1 is not one of the GXM SWIZZLE2 configurations. */
            assert(vp_texture_upload_format(0x28, (w5 & ~(0xfffu << 19)) |
                   ((3u << 19) | (2u << 22) | (0u << 25) | (7u << 28))) == 0);
        }
    }
    puts("PASS RGBA GOB sectors, BC1 color/transparency, RGBA32F-to-RGBA16F, bounds");
    return 0;
}

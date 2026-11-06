/* Host regression for the upload-identity -> GXM texture format mapping.
 *
 * The production function is extracted verbatim from src/platform/gxm_runtime.c
 * by tests/CMakeLists.txt. The enum values below mirror the reviewed VitaSDK
 * numeric composition for the entries the test asserts exactly:
 * SCE_GXM_TEXTURE_FORMAT_U8_R111 is BASE_FORMAT_U8 | SWIZZLE1_R111, and the
 * production _Static_assert ties SWIZZLE1_R111's code to the BC4 variant-7
 * (7,7,7,2) mapping. Remaining names only need to be distinct because the test
 * checks which named entry the mapping selected; they stay below INT_MAX so
 * every host compiler accepts them as ordinary constants. */
#include "gpu_texture.h"

#include <assert.h>
#include <stdint.h>

typedef uint32_t SceGxmTextureFormat;
#define SCE_GXM_TEXTURE_BASE_FORMAT_U8 0x00000000u
#define SCE_GXM_TEXTURE_FORMAT_U8_R SCE_GXM_TEXTURE_BASE_FORMAT_U8
#define SCE_GXM_TEXTURE_SWIZZLE1_R111 0x00007000u
#define SCE_GXM_TEXTURE_FORMAT_U8_R111 \
    (SCE_GXM_TEXTURE_BASE_FORMAT_U8 | SCE_GXM_TEXTURE_SWIZZLE1_R111)
#define SCE_GXM_TEXTURE_FORMAT_A8B8G8R8 0x00000008u
#define SCE_GXM_TEXTURE_FORMAT_F16F16F16F16_ABGR 0x0000000bu
#define SCE_GXM_TEXTURE_FORMAT_F32F32_GR 0x00000012u
#define SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR 0x00000024u
#define SCE_GXM_TEXTURE_FORMAT_UBC1_1BGR 0x00000025u
#define SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR 0x00000026u
#define SCE_GXM_TEXTURE_FORMAT_UBC3_1BGR 0x00000027u
#define SCE_GXM_TEXTURE_FORMAT_UBC4_R 0x00000100u
#define SCE_GXM_TEXTURE_FORMAT_UBC4_000R 0x00000101u
#define SCE_GXM_TEXTURE_FORMAT_UBC4_111R 0x00000102u
#define SCE_GXM_TEXTURE_FORMAT_UBC4_RRRR 0x00000103u
#define SCE_GXM_TEXTURE_FORMAT_UBC4_0RRR 0x00000104u
#define SCE_GXM_TEXTURE_FORMAT_UBC4_1RRR 0x00000105u
#define SCE_GXM_TEXTURE_FORMAT_UBC4_R000 0x00000106u
#define SCE_GXM_TEXTURE_FORMAT_UBC4_R111 0x00000107u
#define SCE_GXM_TEXTURE_FORMAT_SBC4_R 0x00000200u
#define SCE_GXM_TEXTURE_FORMAT_SBC4_000R 0x00000201u
#define SCE_GXM_TEXTURE_FORMAT_SBC4_111R 0x00000202u
#define SCE_GXM_TEXTURE_FORMAT_SBC4_RRRR 0x00000203u
#define SCE_GXM_TEXTURE_FORMAT_SBC4_0RRR 0x00000204u
#define SCE_GXM_TEXTURE_FORMAT_SBC4_1RRR 0x00000205u
#define SCE_GXM_TEXTURE_FORMAT_SBC4_R000 0x00000206u
#define SCE_GXM_TEXTURE_FORMAT_SBC4_R111 0x00000207u
#define SCE_GXM_TEXTURE_FORMAT_UBC5_GR 0x00000300u
#define SCE_GXM_TEXTURE_FORMAT_UBC5_00GR 0x00000301u
#define SCE_GXM_TEXTURE_FORMAT_UBC5_GRRR 0x00000302u
#define SCE_GXM_TEXTURE_FORMAT_UBC5_RGGG 0x00000303u
#define SCE_GXM_TEXTURE_FORMAT_UBC5_GRGR 0x00000304u
#define SCE_GXM_TEXTURE_FORMAT_UBC5_00RG 0x00000305u
#define SCE_GXM_TEXTURE_FORMAT_SBC5_GR 0x00000400u
#define SCE_GXM_TEXTURE_FORMAT_SBC5_00GR 0x00000401u
#define SCE_GXM_TEXTURE_FORMAT_SBC5_GRRR 0x00000402u
#define SCE_GXM_TEXTURE_FORMAT_SBC5_RGGG 0x00000403u
#define SCE_GXM_TEXTURE_FORMAT_SBC5_GRGR 0x00000404u
#define SCE_GXM_TEXTURE_FORMAT_SBC5_00RG 0x00000405u

#include "frame_texture_format_production.inc"

int main(void) {
    SceGxmTextureFormat format = 0u;

    assert(frame_gxm_texture_format(VP_TEXTURE_UPLOAD_ALPHA8, &format) == 0);
    assert(format == SCE_GXM_TEXTURE_FORMAT_U8_R111);
    assert((format & 0x7000u) == SCE_GXM_TEXTURE_SWIZZLE1_R111);
    assert(frame_gxm_texture_format(VP_TEXTURE_UPLOAD_ALPHA8, NULL) == -1);
    assert(frame_gxm_texture_format(VP_TEXTURE_UPLOAD_R8,&format)==0);
    assert(format==SCE_GXM_TEXTURE_FORMAT_U8_R);

    assert(frame_gxm_texture_format(0u, &format) == 0 &&
           format == SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    assert(frame_gxm_texture_format(VP_TEXTURE_UPLOAD_RGBA16F, &format) == 0 &&
           format == SCE_GXM_TEXTURE_FORMAT_F16F16F16F16_ABGR);
    assert(frame_gxm_texture_format(VP_TEXTURE_UPLOAD_RGBA32F_RG32F, &format) == 0 &&
           format == SCE_GXM_TEXTURE_FORMAT_F32F32_GR);
    assert(frame_gxm_texture_format(0x24u, &format) == 0 &&
           format == SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR);
    assert(frame_gxm_texture_format(0x124u, &format) == 0 &&
           format == SCE_GXM_TEXTURE_FORMAT_UBC1_1BGR);
    assert(frame_gxm_texture_format(0x26u, &format) == 0 &&
           format == SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR);
    assert(frame_gxm_texture_format(0x126u, &format) == 0 &&
           format == SCE_GXM_TEXTURE_FORMAT_UBC3_1BGR);
    assert(frame_gxm_texture_format(0x27u | (7u << 12), &format) == 0 &&
           format == SCE_GXM_TEXTURE_FORMAT_UBC4_R111);
    assert(frame_gxm_texture_format(0x27u | (2u << 12), &format) == 0 &&
           format == SCE_GXM_TEXTURE_FORMAT_UBC4_111R);
    assert(frame_gxm_texture_format(0x27u | VP_TEXTURE_UPLOAD_SIGNED, &format) == 0 &&
           format == SCE_GXM_TEXTURE_FORMAT_SBC4_R);
    assert(frame_gxm_texture_format(0x28u | (5u << 12), &format) == 0 &&
           format == SCE_GXM_TEXTURE_FORMAT_UBC5_00RG);
    assert(frame_gxm_texture_format(0x28u | VP_TEXTURE_UPLOAD_SIGNED |
                                    (5u << 12), &format) == 0 &&
           format == SCE_GXM_TEXTURE_FORMAT_SBC5_00RG);

    /* Unknown high bits and out-of-range BC4/BC5 swizzle codes stay refusals;
     * they must not fall through to A8B8G8R8 or any other format. */
    assert(frame_gxm_texture_format(0x1d1u, &format) == -1);
    assert(frame_gxm_texture_format(0x27u | (8u << 12), &format) == -1);
    assert(frame_gxm_texture_format(0x28u | (6u << 12), &format) == -1);

    return 0;
}

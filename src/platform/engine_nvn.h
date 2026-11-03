#ifndef VP_ENGINE_NVN_H
#define VP_ENGINE_NVN_H

#include <stdint.h>

/* Unified call boundary for the locked graphics-preparation functions.
 *
 * Every NVN call is emitted with its original main-module call-site offset and
 * the exact slot offset from config/graphics/nvn-slots-1.4.0.tsv.  The r[] and
 * stack[] words preserve the guest ABI (32-bit CPU pointers, 64-bit GPU
 * addresses/handles as two words in little-endian order, float bit patterns in
 * s[]).  Query calls (nvnBufferGetAddress, nvnDeviceGetTextureHandle) must fill
 * result->value/has_value; a missing result is an explicit failure, never a
 * zero substitute. */

#define VP_ENGINE_NVN_STACK_MAX 8u
#define VP_ENGINE_NVN_FLOAT_MAX 4u
#define VP_ENGINE_NVN_SCRATCH_MIN 0x80u

typedef struct {
    uint32_t site;
    uint32_t slot;
    uint32_t r[4];
    uint32_t stack[VP_ENGINE_NVN_STACK_MAX];
    uint32_t stack_words;
    uint32_t s[VP_ENGINE_NVN_FLOAT_MAX];
    uint32_t s_words;
} VPEngineNvnCall;

typedef struct {
    uint64_t value;
    int has_value;
} VPEngineNvnResult;

typedef struct VPEngineNvnEnv {
    void *opaque;
    void *(*resolve)(void *opaque, uint32_t guest_address);
    void (*emit)(void *opaque, const VPEngineNvnCall *call,
                 VPEngineNvnResult *result);

    /* Confirmed call shape: the imported 0x13EFD7C/0x13EFDC4 sites pass
     * (pointer, size) and the parent does not rely on a fill argument.
     * site is the original call offset (0xBBE9F0 / 0xBBEA04 / 0xBC1134). */
    void (*memclr)(void *opaque, uint32_t site, uint32_t pointer,
                   uint32_t size);

    /* 0xB03D98 / 0xB03D2C: original game resource functions.  The locked call
     * sites pass (record+8, cmd) and do not consume a return value. */
    void (*external_resource)(void *opaque, uint32_t site, uint32_t arg0,
                              uint32_t arg1);

    /* 0x13F0EA4: double pow-like helper used by the sRGB conversion paths.
     * Locked sites pass d0 (base) and d1 (exponent) and consume d0. */
    double (*numeric_pow)(void *opaque, uint32_t site, double base,
                          double exponent);

    /* Guest address of the engine global read by the queue's sRGB clear path
     * (main+0xBBF4D8): byte[*(u32*)srgb_config + 0x29] selects conversion. */
    uint32_t srgb_config;

    /* Guest address of the 16-byte default rectangle data reached through the
     * GOT entry that holds main+0x191EAA0 after relocation. */
    uint32_t default_viewport;

    /* Guest address of a writable scratch area (at least 0x80 bytes) used for
     * NVN pointer arguments that the original code kept on its stack:
     *   +0x00 color-target array, +0x20 rect array, +0x40 blend float[4]. */
    uint32_t scratch;
    uint32_t scratch_size;

    /* Guest address of the engine global used by the material GPU-address
     * translation (main+0x18A62E8 in the sample image). */
    uint32_t gpu_root;
} VPEngineNvnEnv;

/* Locked slot offsets.  Names are from the NVN slot table; values are the
 * unrelocated secondary function-pointer addresses in main. */
#define VP_SLOT_BIND_PROGRAM              0x0191fc78u
#define VP_SLOT_BIND_VERTEX_ATTRIB_STATE  0x0191fc70u
#define VP_SLOT_BIND_VERTEX_STREAM_STATE  0x0191fc74u
#define VP_SLOT_BIND_VERTEX_BUFFER        0x0191fc7cu
#define VP_SLOT_BIND_UNIFORM_BUFFER       0x0191fc84u
#define VP_SLOT_BIND_TEXTURE              0x0191fc9cu
#define VP_SLOT_BIND_BLEND_STATE          0x0191fc58u
#define VP_SLOT_BIND_CHANNEL_MASK_STATE   0x0191fc5cu
#define VP_SLOT_BIND_COLOR_STATE          0x0191fc60u
#define VP_SLOT_BIND_MULTISAMPLE_STATE    0x0191fc64u
#define VP_SLOT_BIND_POLYGON_STATE        0x0191fc68u
#define VP_SLOT_BIND_DEPTH_STENCIL_STATE  0x0191fc6cu
#define VP_SLOT_SET_RENDER_TARGETS        0x0191fdb4u
#define VP_SLOT_SET_VIEWPORT              0x0191fd0cu
#define VP_SLOT_SET_SCISSOR               0x0191fd18u
#define VP_SLOT_SET_BLEND_COLOR           0x0191fd50u
#define VP_SLOT_SET_STENCIL_VALUE_MASK    0x0191fd44u
#define VP_SLOT_SET_STENCIL_MASK          0x0191fd48u
#define VP_SLOT_SET_STENCIL_REF           0x0191fd4cu
#define VP_SLOT_SET_POLYGON_OFFSET_CLAMP  0x0191fd5cu
#define VP_SLOT_SET_DEPTH_CLAMP           0x0191fd6cu
#define VP_SLOT_SET_PRIMITIVE_RESTART     0x0191fcb8u
#define VP_SLOT_SET_LINE_WIDTH            0x0191fd58u
#define VP_SLOT_SET_POINT_SIZE            0x0191fd54u
#define VP_SLOT_SET_ALPHA_REF             0x0191fd60u
#define VP_SLOT_BUFFER_GET_ADDRESS        0x0191f8ccu
#define VP_SLOT_DEVICE_GET_TEXTURE_HANDLE 0x0191f728u
#define VP_SLOT_BARRIER                   0x0191fdd0u
#define VP_SLOT_DRAW_ELEMENTS_BASE_VERTEX 0x0191fcd8u
#define VP_SLOT_DRAW_ARRAYS               0x0191fcd0u
#define VP_SLOT_DRAW_ARRAYS_INSTANCED     0x0191fcdcu
#define VP_SLOT_DRAW_ELEMENTS_INSTANCED   0x0191fce0u
#define VP_SLOT_COPY_TEXTURE_TO_BUFFER    0x0191fd80u
#define VP_SLOT_COPY_TEXTURE_TO_TEXTURE   0x0191fd84u
#define VP_SLOT_CLEAR_COLOR               0x0191fcf4u
#define VP_SLOT_CLEAR_DEPTH_STENCIL       0x0191fd00u
#define VP_SLOT_DOWNSAMPLE                0x0191fdc0u
#define VP_SLOT_DISCARD_COLOR             0x0191fdb8u
#define VP_SLOT_DISCARD_DEPTH_STENCIL     0x0191fdbcu

enum {
    VP_ENGINE_NATIVE_OK = 0,
    VP_ENGINE_NATIVE_ERR_ARGUMENT = -1,
    VP_ENGINE_NATIVE_ERR_MEMORY = -2,
    VP_ENGINE_NATIVE_ERR_CALLBACK = -3,
    VP_ENGINE_NATIVE_ERR_MATERIAL = -4,
    VP_ENGINE_NATIVE_ERR_UNRESOLVED = -5
};

#endif

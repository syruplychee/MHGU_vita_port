#ifndef VP_ENGINE_MATERIAL_H
#define VP_ENGINE_MATERIAL_H

#include <stddef.h>
#include <stdint.h>

/* Native reconstruction of the engine's common program/material submission at
 * locked main+0xBC1764 (3192 bytes, SHA-256
 * e71e66bba88c89c18ad8d2e4651dd3188ccb3aedf53484af9b30d210c8b4657e).
 *
 * This translation unit owns that function's observable semantics only.  It
 * does not decode guest instructions and it does not replace the two
 * neighbouring state helpers at main+0xBC0E98 / main+0xBC13E0, which remain
 * original game code.
 *
 * All object references are 32-bit guest addresses, matching the AArch32 ABI.
 * "env.resolve" converts a guest address to host-accessible memory (identity
 * on the Vita build, synthetic mappings in host tests).  Every NVN call is
 * routed through a typed sink so parameter order, 64-bit handles/addresses and
 * side effects stay explicit.  Unknown structure meanings are named by offset,
 * not by an invented class name. */

/* Locked field offsets read by main+0xBC1764. */
#define VP_ENGINE_MAT_LAYOUT_OFFSET 0x08u
#define VP_ENGINE_MAT_RESOURCE_OFFSET 0x0cu
#define VP_ENGINE_MAT_RESOURCE_COUNT 1024u

#define VP_ENGINE_LAYOUT_DESCRIPTORS_OFFSET 0x04u
#define VP_ENGINE_LAYOUT_GROUPS0_OFFSET 0x08u
#define VP_ENGINE_LAYOUT_GROUPS1_OFFSET 0x0cu
#define VP_ENGINE_LAYOUT_PROGRAM_META_OFFSET 0x38u
#define VP_ENGINE_LAYOUT_DESC_OFFSET 0x3cu
#define VP_ENGINE_PROGRAM_META_FLAGS_OFFSET 0xc8u
#define VP_ENGINE_PROGRAM_OFFSET 0x08u

#define VP_ENGINE_STATE_COMMAND_BUFFER 0x04u
#define VP_ENGINE_STATE_LAST_MATERIAL 0x10u
#define VP_ENGINE_STATE_COLOR_STATE 0x18u
#define VP_ENGINE_STATE_COLOR_TARGET 0x94u
#define VP_ENGINE_STATE_DEPTH_TARGET 0xa4u
#define VP_ENGINE_STATE_CACHE_OFFSET 0xa8u
#define VP_ENGINE_STATE_CACHE_BYTES 0x100u
#define VP_ENGINE_STATE_MULTISAMPLE_CACHE 0x117cu
#define VP_ENGINE_STATE_BYTES 0x1180u

#define VP_ENGINE_CTX_NV_DEVICE 0x98u
#define VP_ENGINE_DEVICE_HANDLE_OFFSET 0x08u
#define VP_ENGINE_CTX_LRU_BASE 0x34070u
#define VP_ENGINE_CTX_LRU_ENTRIES 32u
#define VP_ENGINE_CTX_PENDING_SYNC 0x340f0u
#define VP_ENGINE_CTX_PENDING_UPDATE 0x340f4u
#define VP_ENGINE_CTX_MULTISAMPLE_BASE 0x389d8u

#define VP_ENGINE_GPU_ROOT_WRAPPER 0x14u
#define VP_ENGINE_WRAPPER_BUFFER 0x5b8u
#define VP_ENGINE_WRAPPER_BASE 0x58u

#define VP_ENGINE_TEXTURE_TYPE 0x10u
#define VP_ENGINE_TEXTURE_FLAGS 0x23u
#define VP_ENGINE_TEXTURE_ID 0x34u
#define VP_ENGINE_SAMPLER_ID 0x30u
#define VP_ENGINE_RENDER_TARGET_SAMPLES 0x20u

#define VP_ENGINE_BARRIER_TEXTURE_BITS 0x16u
#define VP_ENGINE_ALPHA_SCALE 255.0f

#define VP_ENGINE_ATTRIB_COUNT 16u
#define VP_ENGINE_STREAM_COUNT 4u

/* Raw layout of the 0x1180-byte submit state prepared by the parent queue at
 * main+0xBBE9C8.  Only fields observed to be read/written by 0xBC1764 are
 * named; everything else is reserved. */
typedef struct {
    uint32_t flags;                 /* +0x000 */
    uint32_t command_buffer;        /* +0x004 guest NVNcommandBuffer address */
    uint32_t last_target;           /* +0x008 owned by helper 0xBC0E98 */
    uint32_t last_state;            /* +0x00C owned by helper 0xBC13E0 */
    uint32_t last_material;         /* +0x010 */
    uint32_t reserved_014;          /* +0x014 */
    uint32_t color_state;           /* +0x018 guest pointer */
    uint8_t reserved_01c[VP_ENGINE_STATE_COLOR_TARGET - 0x01cu];
    uint32_t color_target;          /* +0x094 */
    uint8_t reserved_098[VP_ENGINE_STATE_DEPTH_TARGET - 0x098u];
    uint32_t depth_target;          /* +0x0A4 */
    uint32_t sampler_cache_a[16];   /* +0x0A8, textures group A */
    uint32_t sampler_cache_b[16];   /* +0x0E8, textures group B */
    uint32_t texture_cache_a[16];   /* +0x128, textures group A */
    uint32_t texture_cache_b[16];   /* +0x168, textures group B */
    uint8_t reserved_1a8[VP_ENGINE_STATE_MULTISAMPLE_CACHE - 0x1a8u];
    uint32_t multisample_cache;     /* +0x117C: initialized to 0x3E at
                                     * main+0xBBEAB8 after the bulk clear. */
} VPEngineSubmitState;

typedef struct {
    uint32_t flags;                 /* +0x00: bit2 forces stage-0 uniforms */
    uint32_t word1;                 /* +0x04: source of alpha/color fields */
    uint32_t layout;                /* +0x08 guest pointer */
    uint32_t resources[VP_ENGINE_MAT_RESOURCE_COUNT]; /* +0x0C */
} VPEngineMaterial;

typedef struct {
    uint32_t unknown0;              /* +0x00 */
    uint32_t descriptors;           /* +0x04 guest pointer to 8-byte records */
    uint32_t groups0;               /* +0x08 N0[31:22], N1[21:12] */
    uint32_t groups1;               /* +0x0C N2[31:22], N3[21:12] */
    uint8_t reserved_010[VP_ENGINE_LAYOUT_PROGRAM_META_OFFSET - 0x010u];
    uint32_t program_meta;          /* +0x38 guest pointer */
    uint32_t layout_desc;           /* +0x3C guest pointer */
} VPEngineLayout;

typedef struct {
    uint32_t words[2];
} VPEngineDescriptor;

typedef char vp_engine_state_size_check[
    sizeof(VPEngineSubmitState) == VP_ENGINE_STATE_BYTES ? 1 : -1];
typedef char vp_engine_material_layout_check[
    offsetof(VPEngineMaterial, resources) == VP_ENGINE_MAT_RESOURCE_OFFSET ? 1 : -1];
typedef char vp_engine_layout_check[
    offsetof(VPEngineLayout, program_meta) == VP_ENGINE_LAYOUT_PROGRAM_META_OFFSET ? 1 : -1];
typedef char vp_engine_layout_desc_check[
    offsetof(VPEngineLayout, layout_desc) == VP_ENGINE_LAYOUT_DESC_OFFSET ? 1 : -1];
typedef char vp_engine_state_cmd_check[
    offsetof(VPEngineSubmitState, command_buffer) == VP_ENGINE_STATE_COMMAND_BUFFER ? 1 : -1];
typedef char vp_engine_state_material_check[
    offsetof(VPEngineSubmitState, last_material) == VP_ENGINE_STATE_LAST_MATERIAL ? 1 : -1];
typedef char vp_engine_state_color_check[
    offsetof(VPEngineSubmitState, color_state) == VP_ENGINE_STATE_COLOR_STATE ? 1 : -1];
typedef char vp_engine_state_target_check[
    offsetof(VPEngineSubmitState, color_target) == VP_ENGINE_STATE_COLOR_TARGET ? 1 : -1];
typedef char vp_engine_state_depth_check[
    offsetof(VPEngineSubmitState, depth_target) == VP_ENGINE_STATE_DEPTH_TARGET ? 1 : -1];
typedef char vp_engine_state_cache_check[
    offsetof(VPEngineSubmitState, sampler_cache_a) == VP_ENGINE_STATE_CACHE_OFFSET ? 1 : -1];
typedef char vp_engine_state_msaa_check[
    offsetof(VPEngineSubmitState, multisample_cache) == VP_ENGINE_STATE_MULTISAMPLE_CACHE ? 1 : -1];

enum {
    VP_ENGINE_MATERIAL_OK = 0,
    VP_ENGINE_MATERIAL_ERR_ARGUMENT = -1,
    VP_ENGINE_MATERIAL_ERR_MEMORY = -2,
    VP_ENGINE_MATERIAL_ERR_CALLBACK = -3,
    VP_ENGINE_MATERIAL_ERR_OBJECT = -4
};

/* Typed NVN sinks.  Address/handle parameters are 64-bit where the locked call
 * sites pass a 64-bit value; command buffer and object arguments stay 32-bit
 * guest addresses. */
typedef struct VPEngineMaterialEnv {
    void *opaque;

    /* Resolve a guest address.  Returning NULL rejects the submission instead
     * of guessing another mapping. */
    void *(*resolve)(void *opaque, uint32_t guest_address);

    /* Guest address of the engine global whose +0x14 object carries the NVN
     * buffer used for CPU-to-GPU address translation.  Not hard-coded to the
     * sample image's 0x18A62E8. */
    uint32_t gpu_root;

    void (*bind_program)(void *opaque, uint32_t site, uint32_t cmd,
                         uint32_t program, uint32_t flags_mask);
    void (*bind_vertex_attrib_state)(void *opaque, uint32_t site,
                                     uint32_t cmd, unsigned count,
                                     uint32_t desc);
    void (*bind_vertex_stream_state)(void *opaque, uint32_t site,
                                     uint32_t cmd, unsigned count,
                                     uint32_t desc);
    uint64_t (*buffer_get_address)(void *opaque, uint32_t site,
                                   uint32_t buffer);
    void (*bind_uniform_buffer)(void *opaque, uint32_t site, uint32_t cmd,
                                unsigned stage, unsigned index,
                                uint64_t address, uint32_t size);
    uint64_t (*device_get_texture_handle)(void *opaque, uint32_t site,
                                          uint32_t device, uint32_t texture_id,
                                          uint32_t sampler_id);
    void (*bind_texture)(void *opaque, uint32_t site, uint32_t cmd,
                         unsigned stage, unsigned index, uint64_t handle);
    void (*barrier)(void *opaque, uint32_t site, uint32_t cmd, uint32_t bits);
    void (*set_alpha_ref)(void *opaque, uint32_t site, uint32_t cmd,
                          float ref);
    void (*bind_color_state)(void *opaque, uint32_t site, uint32_t cmd,
                             uint32_t state);
    void (*bind_multisample_state)(void *opaque, uint32_t site, uint32_t cmd,
                                   uint32_t state);
} VPEngineMaterialEnv;

/* Reproduce main+0xBC1764.  ctx/state/material are guest addresses of the
 * render context, submit state and material record.  Returns OK only when the
 * whole observed path completed; the original game code remains the fallback
 * until this is deliberately wired. */
int vp_engine_material_submit(const VPEngineMaterialEnv *env, uint32_t ctx,
                              uint32_t state, uint32_t material);

#endif

/* Bounded per-stage texture-resource layout sidecar (VPSRES1).
 *
 * The offline tool tools/shader/build_resource_layout.py derives this table
 * from the locked shader catalogue and the exact stage table of the static
 * shader index.  Each record is keyed by the exact stage SHA-256 and stage
 * type; it carries the compiler's Maxwell texture-handle bank/offset and the
 * descriptor shape.  The parent index SHA-256 and CRC are part of the header,
 * and parsing verifies every stage against the loaded index.
 *
 * This module is deliberately free of VitaSDK dependencies so the host tests
 * exercise the same parser the runtime uses.
 */
#ifndef VP_GXM_RESOURCE_LAYOUT_H
#define VP_GXM_RESOURCE_LAYOUT_H

#include "static_shader_index.h"

#include <stddef.h>
#include <stdint.h>

#define VP_RESOURCE_LAYOUT_MAGIC "VPSRES1"
#define VP_RESOURCE_LAYOUT_MAGIC_SIZE 8u
#define VP_RESOURCE_LAYOUT_HEADER_SIZE 96u
#define VP_RESOURCE_LAYOUT_VERSION 1u
#define VP_RESOURCE_LAYOUT_FLAGS 1u
#define VP_RESOURCE_LAYOUT_STAGE_RECORD 40u
#define VP_RESOURCE_LAYOUT_TEXTURE_RECORD 16u
#define VP_RESOURCE_LAYOUT_MAX_BYTES (16u * 1024u * 1024u)
#define VP_RESOURCE_LAYOUT_MAX_STAGES 65535u
/* texture_start is a u16 in the fixed record, so the total texture table must
 * stay representable.  65535 is the format limit, not a claim that any bank
 * reaches it. */
#define VP_RESOURCE_LAYOUT_MAX_TEXTURES 65535u

/* Texture-record flags. The low bits are the sampled-vector components the
 * exact compiled shader actually reads; bit 7 records that the offline GLSL
 * analysis completed. A depth view is accepted only with USAGE_ANALYZED and no
 * component other than R, because the native depth formats have only the
 * proven .r mapping. */
#define VP_RESOURCE_TEXTURE_USAGE_R 0x01u
#define VP_RESOURCE_TEXTURE_USAGE_G 0x02u
#define VP_RESOURCE_TEXTURE_USAGE_B 0x04u
#define VP_RESOURCE_TEXTURE_USAGE_A 0x08u
#define VP_RESOURCE_TEXTURE_USAGE_MASK 0x0fu
#define VP_RESOURCE_TEXTURE_USAGE_ANALYZED 0x80u
#define VP_RESOURCE_TEXTURE_FLAGS_MASK \
    (VP_RESOURCE_TEXTURE_USAGE_MASK | VP_RESOURCE_TEXTURE_USAGE_ANALYZED)

/* Shader::TextureType values used by the catalogue resource metadata.  These
 * are the *shader descriptor* types, not the guest TIC texture_type field. */
enum {
    VP_RESOURCE_TEX_1D = 0,
    VP_RESOURCE_TEX_1D_ARRAY = 1,
    VP_RESOURCE_TEX_2D = 2,
    VP_RESOURCE_TEX_2D_ARRAY = 3,
    VP_RESOURCE_TEX_3D = 4,
    VP_RESOURCE_TEX_CUBE = 5,
    VP_RESOURCE_TEX_CUBE_ARRAY = 6,
    VP_RESOURCE_TEX_BUFFER = 7,
    VP_RESOURCE_TEX_2D_RECT = 8
};

enum {
    VP_RESOURCE_LAYOUT_OK = 0,
    VP_RESOURCE_LAYOUT_ERR_ARG = -1,
    VP_RESOURCE_LAYOUT_ERR_SIZE = -2,
    VP_RESOURCE_LAYOUT_ERR_MAGIC = -3,
    VP_RESOURCE_LAYOUT_ERR_VERSION = -4,
    VP_RESOURCE_LAYOUT_ERR_BODY = -5,
    VP_RESOURCE_LAYOUT_ERR_RECORD = -6,
    VP_RESOURCE_LAYOUT_ERR_DUPLICATE = -7,
    VP_RESOURCE_LAYOUT_ERR_INDEX = -8,
    VP_RESOURCE_LAYOUT_ERR_PARENT = -9,
    VP_RESOURCE_LAYOUT_ERR_MISSING = -10,
    VP_RESOURCE_LAYOUT_ERR_UNSUPPORTED = -11
};

typedef struct {
    uint32_t stage_index;
    uint8_t stage_type;   /* VP_STATIC_INDEX_STAGE_VERTEX/FRAGMENT */
    uint8_t texture_count;
    uint16_t texture_start;
    uint8_t sha256[32];
} VPResourceStage;

typedef struct {
    uint8_t slot;          /* texN binding, equal to the descriptor ordinal */
    uint8_t texture_type;  /* VP_RESOURCE_TEX_* */
    uint8_t depth;         /* shader descriptor depth flag */
    uint8_t secondary;
    uint16_t offset;       /* byte offset of the 8-byte handle */
    uint16_t count;
    uint8_t shift;
    uint8_t size_shift;
    uint8_t env_bank;      /* offline environment cbuf index, audit only */
    uint8_t flags;
} VPResourceTexture;

typedef struct {
    VPResourceStage *stages;
    VPResourceTexture *textures;
    uint32_t stage_count;
    uint32_t texture_count;
    uint8_t parent_sha256[32];
    uint32_t parent_crc32;
} VPResourceLayout;

/* Parse one complete sidecar image.  parent_sha256/parent_crc32 identify the
 * exact shader-index.bin bytes the sidecar was generated against; index is the
 * already parsed parent table.  The parser rejects the whole file on any
 * structural error, duplicate stage, duplicate descriptor offset, unsupported
 * descriptor shape or index association mismatch.  It does not take ownership
 * of data. */
int vp_resource_layout_parse(const void *data, size_t size,
                             const unsigned char parent_sha256[32],
                             uint32_t parent_crc32,
                             const VPStaticShaderIndex *index,
                             VPResourceLayout *out,
                             char *error, size_t error_size);
int vp_resource_layout_read_file(const char *path,
                                 const unsigned char parent_sha256[32],
                                 uint32_t parent_crc32,
                                 const VPStaticShaderIndex *index,
                                 VPResourceLayout *out,
                                 char *error, size_t error_size);
void vp_resource_layout_free(VPResourceLayout *layout);

/* Binary search by exact (stage type, SHA-256).  Returns NULL for unknown or
 * ambiguous identities. */
const VPResourceStage *vp_resource_layout_stage(const VPResourceLayout *layout,
                                                unsigned stage_type,
                                                const uint8_t sha256[32]);
/* Fetch one descriptor by sampler slot.  Returns 0 on success, -1 when the
 * slot is outside this stage's validated table. */
int vp_resource_layout_texture(const VPResourceLayout *layout,
                               const VPResourceStage *stage,
                               unsigned slot, VPResourceTexture *out);

const char *vp_resource_layout_status_name(int status);

#endif

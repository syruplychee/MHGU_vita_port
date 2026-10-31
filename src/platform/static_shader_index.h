/* Static shader index v1: portable reader shared by the host tests and the
 * Vita runtime.
 *
 * The format is little-endian and fixed-record; no C struct is ever written
 * to disk. Parsing validates magic, version, record sizes, body length, CRC32,
 * stage/variant/program reference closure, duplicate keys, token ordering and
 * viewport-mask disjointness before any lookup is allowed. A malformed index is
 * rejected as a whole; the caller must not fall back to a different package
 * silently.
 */
#ifndef VP_STATIC_SHADER_INDEX_H
#define VP_STATIC_SHADER_INDEX_H

#include <stddef.h>
#include <stdint.h>

#define VP_STATIC_INDEX_MAGIC "VPSHIX1"
#define VP_STATIC_INDEX_MAGIC_SIZE 8u
#define VP_STATIC_INDEX_HEADER_SIZE 64u
#define VP_STATIC_INDEX_VERSION 1u
/* ABI 1 stored no input signature (the reserved word is zero). ABI 2 uses the
 * reserved variant word for a 16-bit vertex-input signature and permits
 * overlapping viewports only when the signatures differ. The reader accepts
 * both; writers emit ABI 2. */
#define VP_STATIC_INDEX_ABI_VERSION 2u
#define VP_STATIC_INDEX_ABI_VERSION_MIN 1u
#define VP_STATIC_INDEX_INPUT_MASK 0xffffu
#define VP_STATIC_INDEX_STAGE_RECORD 48u
#define VP_STATIC_INDEX_VARIANT_RECORD 40u
#define VP_STATIC_INDEX_PROGRAM_RECORD 16u
#define VP_STATIC_INDEX_MAX_STAGES 65535u
#define VP_STATIC_INDEX_MAX_VARIANTS 4096u
#define VP_STATIC_INDEX_MAX_PROGRAMS 1000000u
#define VP_STATIC_INDEX_MAX_BYTES (16u * 1024u * 1024u)
#define VP_STATIC_INDEX_MAX_GXP_BYTES (16u * 1024u * 1024u)
#define VP_STATIC_INDEX_STAGE_NONE 0xffffu
#define VP_STATIC_INDEX_STAGE_VERTEX 1u
#define VP_STATIC_INDEX_STAGE_FRAGMENT 5u
#define VP_STATIC_INDEX_STAGE_SOURCE_SPK 1u
#define VP_STATIC_INDEX_STAGE_SOURCE_SDK 2u
#define VP_STATIC_INDEX_STAGE_OBSERVED 4u
#define VP_STATIC_INDEX_PROGRAM_OBSERVED 1u
#define VP_STATIC_INDEX_PROGRAM_STATIC 2u
#define VP_STATIC_INDEX_VARIANT_COMPILED 1u

typedef struct {
    uint32_t stage;                 /* 1 = vertex, 5 = fragment */
    uint8_t sha256[32];             /* exact Maxwell/SPH content */
    uint16_t variant_start;
    uint16_t variant_count;
    uint32_t source_flags;
} VPStaticStage;

typedef struct {
    uint32_t stage_index;
    uint16_t viewport_mask;         /* bit 0 = viewport transform 0, bit 1 = 1 */
    uint16_t abi_version;
    uint8_t key[16];
    uint32_t gxp_bytes;
    uint32_t flags;
    uint32_t gxp_crc32;
    /* ABI 1: always zero (generic float inputs). ABI 2: bit N means the GXP
     * declares int4 in_attrN_uint and must be selected only when the draw's
     * descriptor at location N is UINT. Zero is the generic default. */
    uint32_t input_signature;
} VPStaticVariant;

typedef struct {
    uint32_t token;                 /* original SPK index + 1 candidate */
    uint16_t vertex_stage;          /* index into stages, or STAGE_NONE */
    uint16_t fragment_stage;
    uint32_t flags;
} VPStaticProgram;

typedef struct {
    VPStaticStage *stages;
    VPStaticVariant *variants;
    VPStaticProgram *programs;
    uint32_t stage_count;
    uint32_t variant_count;
    uint32_t program_count;
    uint32_t abi_version;
} VPStaticShaderIndex;

/* Parse a complete index image. On failure returns -1 and writes a bounded
 * diagnostic. The index does not borrow the input buffer. */
int vp_static_shader_index_parse(const void *data, size_t size,
                                 VPStaticShaderIndex *out,
                                 char *error, size_t error_size);
/* Read and parse a file. Returns -1 on any I/O or validation failure. */
int vp_static_shader_index_read_file(const char *path, VPStaticShaderIndex *out,
                                     char *error, size_t error_size);
void vp_static_shader_index_free(VPStaticShaderIndex *index);

const VPStaticProgram *vp_static_program_for_token(const VPStaticShaderIndex *index,
                                                   uint32_t token);
/* Returns the generic (signature zero) variant serving stage_index + viewport
 * transform when one exists; otherwise the only specialized variant. Returns
 * NULL when the package has no proven GXP for that combination or when more
 * than one specialized variant exists and none is generic. */
const VPStaticVariant *vp_static_variant_for(const VPStaticShaderIndex *index,
                                             uint16_t stage_index,
                                             unsigned viewport_transform);
/* Explicit input-signature selection. required_uint_mask has bit N set when
 * the draw's descriptor at location N is UINT. A specialized variant is
 * eligible only when its signature is a non-empty subset of that mask; the
 * eligible variant with the largest signature wins. When no specialized
 * variant is eligible the generic (signature zero) variant is returned. When
 * two eligible variants have the same largest signature, or parsing allowed
 * an ambiguous table, *ambiguous is set and NULL is returned. */
const VPStaticVariant *vp_static_variant_select(const VPStaticShaderIndex *index,
                                                uint16_t stage_index,
                                                unsigned viewport_transform,
                                                uint32_t required_uint_mask,
                                                int *ambiguous);
/* Format "<16-byte key as 32 hex>.gxp". Returns 0 on success. */
int vp_static_variant_name(const VPStaticVariant *variant, char *buffer, size_t size);
/* IEEE CRC-32 used by the package for GXP payload verification. */
uint32_t vp_static_index_crc32(const void *data, size_t size);

#endif

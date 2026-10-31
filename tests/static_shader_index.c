/* Host-side tests for the static shader index reader (ABI 1 and ABI 2).
 *
 * The valid images are built here, then every negative case mutates a copy and
 * re-seals the CRC so the intended structural check is the one that fires.
 * Selection tests cover the old generic variant, an exact UINT specialization,
 * the generic fallback for non-UINT draws, an ambiguous equal-size tie and the
 * ABI 1 compatibility path.
 */
#include "static_shader_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

static uint32_t crc32_ieee(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    size_t i;
    unsigned bit;
    for (i = 0; i < size; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8u; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

#define STAGES 2u
#define VARIANTS 5u
#define PROGRAMS 2u

static size_t build_valid(uint8_t *buffer) {
    size_t body_size = STAGES * VP_STATIC_INDEX_STAGE_RECORD +
                       VARIANTS * VP_STATIC_INDEX_VARIANT_RECORD +
                       PROGRAMS * VP_STATIC_INDEX_PROGRAM_RECORD;
    size_t size = VP_STATIC_INDEX_HEADER_SIZE + body_size;
    uint8_t *stage = buffer + VP_STATIC_INDEX_HEADER_SIZE;
    uint8_t *variant = stage + STAGES * VP_STATIC_INDEX_STAGE_RECORD;
    uint8_t *program = variant + VARIANTS * VP_STATIC_INDEX_VARIANT_RECORD;
    unsigned i;

    memset(buffer, 0, size);
    memcpy(buffer, VP_STATIC_INDEX_MAGIC, VP_STATIC_INDEX_MAGIC_SIZE);
    put16(buffer + 8, VP_STATIC_INDEX_VERSION);
    put16(buffer + 10, VP_STATIC_INDEX_HEADER_SIZE);
    put32(buffer + 12, 1u);
    put32(buffer + 16, STAGES);
    put32(buffer + 20, VARIANTS);
    put32(buffer + 24, PROGRAMS);
    put32(buffer + 28, VP_STATIC_INDEX_STAGE_RECORD);
    put32(buffer + 32, VP_STATIC_INDEX_VARIANT_RECORD);
    put32(buffer + 36, VP_STATIC_INDEX_PROGRAM_RECORD);
    put32(buffer + 40, (uint32_t)body_size);
    put32(buffer + 48, VP_STATIC_INDEX_ABI_VERSION);

    /* stage 0: vertex, variants 0..3 (generic vp0, generic vp1, UINT attr3
     * vp1, UINT attr2 vp1). */
    put32(stage + 0, VP_STATIC_INDEX_STAGE_VERTEX);
    memset(stage + 4, 0x11, 32);
    put16(stage + 36, 0u);
    put16(stage + 38, 4u);
    put32(stage + 40, VP_STATIC_INDEX_STAGE_SOURCE_SPK | VP_STATIC_INDEX_STAGE_OBSERVED);
    /* stage 1: fragment, variant 4 shared by both viewport transforms. */
    put32(stage + 48, VP_STATIC_INDEX_STAGE_FRAGMENT);
    memset(stage + 52, 0x22, 32);
    put16(stage + 84, 4u);
    put16(stage + 86, 1u);
    put32(stage + 88, VP_STATIC_INDEX_STAGE_SOURCE_SPK);

    for (i = 0; i < VARIANTS; ++i) {
        uint8_t *record = variant + i * VP_STATIC_INDEX_VARIANT_RECORD;
        uint16_t mask = i == 0u ? 1u : i == 4u ? 3u : 2u;
        uint32_t signature = i == 2u ? 8u : i == 3u ? 4u : 0u;
        put32(record + 0, i == 4u ? 1u : 0u);
        put16(record + 4, mask);
        put16(record + 6, VP_STATIC_INDEX_ABI_VERSION);
        memset(record + 8, (int)(0xaau + i * 0x11u), 16);
        put32(record + 24, 64u);
        put32(record + 28, VP_STATIC_INDEX_VARIANT_COMPILED);
        put32(record + 32, 0x12345678u + i);
        put32(record + 36, signature);
    }

    put32(program + 0, 100u);
    put16(program + 4, 0u);
    put16(program + 6, 1u);
    put32(program + 8, VP_STATIC_INDEX_PROGRAM_OBSERVED);
    put32(program + 16, 200u);
    put16(program + 20, 0u);
    put16(program + 22, 1u);
    put32(program + 24, VP_STATIC_INDEX_PROGRAM_STATIC);

    put32(buffer + 44, crc32_ieee(buffer + VP_STATIC_INDEX_HEADER_SIZE,
                                 body_size));
    return size;
}

/* Minimal ABI 1 image: generic variants only, reserved words zero. */
static size_t build_abi1(uint8_t *buffer) {
    uint8_t *stage = buffer + VP_STATIC_INDEX_HEADER_SIZE;
    uint8_t *variant = stage + STAGES * VP_STATIC_INDEX_STAGE_RECORD;
    uint8_t *program = variant + 2u * VP_STATIC_INDEX_VARIANT_RECORD;
    size_t body_size = STAGES * VP_STATIC_INDEX_STAGE_RECORD +
                       2u * VP_STATIC_INDEX_VARIANT_RECORD +
                       2u * VP_STATIC_INDEX_PROGRAM_RECORD;
    size_t size = VP_STATIC_INDEX_HEADER_SIZE + body_size;
    unsigned i;

    memset(buffer, 0, size);
    memcpy(buffer, VP_STATIC_INDEX_MAGIC, VP_STATIC_INDEX_MAGIC_SIZE);
    put16(buffer + 8, VP_STATIC_INDEX_VERSION);
    put16(buffer + 10, VP_STATIC_INDEX_HEADER_SIZE);
    put32(buffer + 12, 1u);
    put32(buffer + 16, STAGES);
    put32(buffer + 20, 2u);
    put32(buffer + 24, 2u);
    put32(buffer + 28, VP_STATIC_INDEX_STAGE_RECORD);
    put32(buffer + 32, VP_STATIC_INDEX_VARIANT_RECORD);
    put32(buffer + 36, VP_STATIC_INDEX_PROGRAM_RECORD);
    put32(buffer + 40, (uint32_t)body_size);
    put32(buffer + 48, 1u);

    put32(stage + 0, VP_STATIC_INDEX_STAGE_VERTEX);
    memset(stage + 4, 0x33, 32);
    put16(stage + 36, 0u);
    put16(stage + 38, 1u);
    put32(stage + 40, VP_STATIC_INDEX_STAGE_SOURCE_SPK);
    put32(stage + 48, VP_STATIC_INDEX_STAGE_FRAGMENT);
    memset(stage + 52, 0x44, 32);
    put16(stage + 84, 1u);
    put16(stage + 86, 1u);
    put32(stage + 88, VP_STATIC_INDEX_STAGE_SOURCE_SPK);
    for (i = 0; i < 2u; ++i) {
        uint8_t *record = variant + i * VP_STATIC_INDEX_VARIANT_RECORD;
        put32(record + 0, i);
        put16(record + 4, 1u);
        put16(record + 6, 1u);
        memset(record + 8, (int)(0xbbu + i * 0x11u), 16);
        put32(record + 24, 64u);
        put32(record + 28, VP_STATIC_INDEX_VARIANT_COMPILED);
        put32(record + 32, 0x22334455u + i);
        put32(record + 36, 0u);
    }
    for (i = 0; i < 2u; ++i) {
        uint8_t *record = program + i * VP_STATIC_INDEX_PROGRAM_RECORD;
        put32(record + 0, 100u + (uint32_t)i);
        put16(record + 4, 0u);
        put16(record + 6, 1u);
        put32(record + 8, VP_STATIC_INDEX_PROGRAM_OBSERVED);
    }
    put32(buffer + 44, crc32_ieee(buffer + VP_STATIC_INDEX_HEADER_SIZE,
                                 body_size));
    return size;
}

static void reseal(uint8_t *buffer, size_t size) {
    uint32_t body = (uint32_t)(size - VP_STATIC_INDEX_HEADER_SIZE);
    put32(buffer + 40, body);
    put32(buffer + 44, crc32_ieee(buffer + VP_STATIC_INDEX_HEADER_SIZE, body));
}

static void expect_reject(const uint8_t *source, size_t size, const char *what) {
    uint8_t *copy = (uint8_t *)malloc(size);
    VPStaticShaderIndex index;
    char error[128] = {0};
    CHECK(copy != NULL, "test allocation");
    if (copy == NULL) return;
    memcpy(copy, source, size);
    memset(&index, 0, sizeof(index));
    CHECK(vp_static_shader_index_parse(copy, size, &index, error, sizeof(error)) < 0,
          what);
    CHECK(error[0] != 0, "rejection reports a diagnostic");
    vp_static_shader_index_free(&index);
    free(copy);
}

int main(void) {
    uint8_t *valid = (uint8_t *)malloc(4096);
    uint8_t *abi1 = (uint8_t *)malloc(4096);
    uint8_t *mut;
    size_t size, abi1_size, stage_size;
    size_t stage = VP_STATIC_INDEX_HEADER_SIZE;
    size_t variant = stage + STAGES * VP_STATIC_INDEX_STAGE_RECORD;
    size_t program = variant + VARIANTS * VP_STATIC_INDEX_VARIANT_RECORD;
    VPStaticShaderIndex index;
    char error[128];
    const VPStaticProgram *p;
    const VPStaticVariant *v;
    char name[64];
    FILE *file;
    int ambiguous;

    CHECK(valid != NULL && abi1 != NULL, "image allocation");
    if (valid == NULL || abi1 == NULL) return 1;
    size = build_valid(valid);
    abi1_size = build_abi1(abi1);

    memset(&index, 0, sizeof(index));
    error[0] = 0;
    CHECK(vp_static_shader_index_parse(valid, size, &index, error, sizeof(error)) == 0,
          "valid ABI 2 image parses");
    CHECK(index.stage_count == STAGES && index.variant_count == VARIANTS &&
          index.program_count == PROGRAMS && index.abi_version == 2u, "counts");
    p = vp_static_program_for_token(&index, 100u);
    CHECK(p != NULL && p->vertex_stage == 0u && p->fragment_stage == 1u,
          "token 100 lookup");
    CHECK(vp_static_program_for_token(&index, 150u) == NULL, "missing token");
    CHECK(vp_static_program_for_token(&index, 200u) != NULL, "token 200 lookup");
    CHECK(vp_static_variant_for(&index, 0u, 0u) == &index.variants[0], "generic vp0");
    CHECK(vp_static_variant_for(&index, 0u, 1u) == &index.variants[1],
          "generic vp1 retained");
    CHECK(vp_static_variant_for(&index, 1u, 0u) == &index.variants[4], "shared vp0");
    CHECK(vp_static_variant_for(&index, 1u, 1u) == &index.variants[4], "shared vp1");
    CHECK(vp_static_variant_for(&index, 0u, 2u) == NULL, "invalid viewport");
    CHECK(vp_static_variant_for(&index, 2u, 0u) == NULL, "stage out of range");

    /* Typed selection: old float draws keep the generic GXP, UINT attr3 uses
     * the specialized GXP, UINT attr2 uses its own, and unrelated UINT bits do
     * not make a specialization eligible. */
    ambiguous = -1;
    CHECK(vp_static_variant_select(&index, 0u, 1u, 0u, &ambiguous) ==
          &index.variants[1] && ambiguous == 0, "float draw selects old generic");
    CHECK(vp_static_variant_select(&index, 0u, 1u, 8u, &ambiguous) ==
          &index.variants[2] && ambiguous == 0, "attr3 UINT selects specialization");
    CHECK(vp_static_variant_select(&index, 0u, 1u, 4u, &ambiguous) ==
          &index.variants[3] && ambiguous == 0, "attr2 UINT selects specialization");
    CHECK(vp_static_variant_select(&index, 0u, 1u, 2u, &ambiguous) ==
          &index.variants[1] && ambiguous == 0, "unrelated UINT falls back to generic");
    CHECK(vp_static_variant_select(&index, 0u, 0u, 8u, &ambiguous) ==
          &index.variants[0] && ambiguous == 0, "no specialized variant at vp0");
    CHECK(vp_static_variant_select(&index, 1u, 1u, 8u, &ambiguous) ==
          &index.variants[4] && ambiguous == 0, "fragment generic fallback");
    /* Two equal-size eligible specializations are a real ambiguity, not an
     * arbitrary first match. */
    CHECK(vp_static_variant_select(&index, 0u, 1u, 12u, &ambiguous) == NULL &&
          ambiguous == 1, "equal specialization tie rejected");
    ambiguous = -1;
    CHECK(vp_static_variant_select(&index, 0u, 2u, 8u, &ambiguous) == NULL &&
          ambiguous == 0, "invalid viewport selection");
    CHECK(vp_static_variant_select(&index, 2u, 1u, 8u, &ambiguous) == NULL &&
          ambiguous == 0, "stage out of range selection");
    v = vp_static_variant_for(&index, 0u, 0u);
    CHECK(v != NULL && vp_static_variant_name(v, name, sizeof(name)) == 0,
          "variant name");
    CHECK(v != NULL && strcmp(name, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.gxp") == 0,
          "variant name text");
    CHECK(vp_static_variant_name(v, name, 8u) < 0, "short name buffer rejected");
    vp_static_shader_index_free(&index);
    CHECK(index.stages == NULL && index.stage_count == 0u, "free resets");

    /* ABI 1 remains readable and means generic float inputs. */
    memset(&index, 0, sizeof(index));
    error[0] = 0;
    CHECK(vp_static_shader_index_parse(abi1, abi1_size, &index, error,
                                       sizeof(error)) == 0,
          "valid ABI 1 image parses");
    CHECK(index.abi_version == 1u && index.variant_count == 2u, "ABI 1 counts");
    CHECK(vp_static_variant_for(&index, 0u, 0u) == &index.variants[0],
          "ABI 1 generic lookup");
    CHECK(vp_static_variant_select(&index, 0u, 0u, 8u, &ambiguous) ==
          &index.variants[0] && ambiguous == 0,
          "ABI 1 draw with UINT descriptor falls back to generic (no proven GXP)");
    vp_static_shader_index_free(&index);

    /* Truncation and header-level failures. */
    expect_reject(valid, size - 1u, "truncated image rejected");
    expect_reject(valid, VP_STATIC_INDEX_HEADER_SIZE - 1u, "short header rejected");

    mut = (uint8_t *)malloc(size);
    CHECK(mut != NULL, "mutation allocation");
    if (mut == NULL) return 1;

#define MUTATE(block, offset, value) do { \
    memcpy(mut, valid, size); \
    (block)[(offset)] = (uint8_t)(value); \
    expect_reject(mut, size, #block " mutation rejected"); \
} while (0)

    memcpy(mut, valid, size); mut[0] ^= 0xff; expect_reject(mut, size, "magic rejected");
    memcpy(mut, valid, size); put16(mut + 8, 2u); expect_reject(mut, size, "version rejected");
    memcpy(mut, valid, size); put16(mut + 10, 32u); expect_reject(mut, size, "header size rejected");
    memcpy(mut, valid, size); put32(mut + 12, 3u); expect_reject(mut, size, "flags rejected");
    memcpy(mut, valid, size); put32(mut + 28, 40u); expect_reject(mut, size, "stage record size rejected");
    memcpy(mut, valid, size); put32(mut + 32, 32u); expect_reject(mut, size, "variant record size rejected");
    memcpy(mut, valid, size); put32(mut + 36, 8u); expect_reject(mut, size, "program record size rejected");
    memcpy(mut, valid, size); put32(mut + 48, 3u); expect_reject(mut, size, "ABI 3 rejected");
    memcpy(mut, valid, size); put32(mut + 48, 0u); expect_reject(mut, size, "ABI 0 rejected");
    memcpy(mut, valid, size); put32(mut + 48, 1u); expect_reject(mut, size, "mixed ABI rejected");
    memcpy(mut, valid, size); put32(mut + 40, 1u); expect_reject(mut, size, "body size rejected");
    memcpy(mut, valid, size); mut[size - 1] ^= 0x80; expect_reject(mut, size, "CRC rejected");
    memcpy(mut, valid, size); put32(mut + 16, 0u); expect_reject(mut, size, "zero stages rejected");
    memcpy(mut, valid, size); put32(mut + 20, 5000u); expect_reject(mut, size, "variant quota rejected");
    memcpy(mut, valid, size); put32(mut + 24, 0u); expect_reject(mut, size, "zero programs rejected");

    /* Stage structure. */
    memcpy(mut, valid, size); put32(mut + stage, 3u); reseal(mut, size);
    expect_reject(mut, size, "stage type rejected");
    memcpy(mut, valid, size); put32(mut + stage + 40, 8u); reseal(mut, size);
    expect_reject(mut, size, "stage source flags rejected");
    memcpy(mut, valid, size); put16(mut + stage + 38, 5u); reseal(mut, size);
    expect_reject(mut, size, "stage range outside table rejected");
    memcpy(mut, valid, size); put16(mut + stage + 36, 1u); reseal(mut, size);
    expect_reject(mut, size, "non-contiguous stage range rejected");
    memcpy(mut, valid, size); put32(mut + stage + 48, VP_STATIC_INDEX_STAGE_VERTEX);
    memset(mut + stage + 52, 0x11, 32); reseal(mut, size);
    expect_reject(mut, size, "duplicate stage content rejected");
    memcpy(mut, valid, size); memset(mut + stage + 52, 0x11, 32); reseal(mut, size);
    CHECK(vp_static_shader_index_parse(mut, size, &index, error, sizeof(error)) == 0,
          "same bytes in another stage are distinct content");
    vp_static_shader_index_free(&index);

    /* Variant structure. */
    memcpy(mut, valid, size); put32(mut + variant, 9u); reseal(mut, size);
    expect_reject(mut, size, "variant stage index rejected");
    memcpy(mut, valid, size); put16(mut + variant + 6, 1u); reseal(mut, size);
    expect_reject(mut, size, "variant ABI mismatch rejected");
    memcpy(mut, valid, size); put16(mut + variant + 6, 3u); reseal(mut, size);
    expect_reject(mut, size, "variant ABI out of range rejected");
    memcpy(mut, valid, size); put32(mut + variant + 28, 0u); reseal(mut, size);
    expect_reject(mut, size, "uncompiled variant rejected");
    memcpy(mut, valid, size); put16(mut + variant + 4, 0u); reseal(mut, size);
    expect_reject(mut, size, "zero viewport mask rejected");
    memcpy(mut, valid, size); put16(mut + variant + 4, 4u); reseal(mut, size);
    expect_reject(mut, size, "unknown viewport bit rejected");
    memcpy(mut, valid, size); put32(mut + variant + 24, 8u); reseal(mut, size);
    expect_reject(mut, size, "short GXP rejected");
    memcpy(mut, valid, size); put32(mut + variant + 36, 0x10000u); reseal(mut, size);
    expect_reject(mut, size, "input signature beyond 16 bits rejected");
    memcpy(mut, valid, size); memcpy(mut + variant + VP_STATIC_INDEX_VARIANT_RECORD + 8,
                                     mut + variant + 8, 16); reseal(mut, size);
    expect_reject(mut, size, "duplicate variant key rejected");
    memcpy(mut, valid, size); put16(mut + variant + VP_STATIC_INDEX_VARIANT_RECORD + 4, 1u);
    reseal(mut, size);
    expect_reject(mut, size, "overlapping generic variants rejected");
    /* Same stage and overlapping viewport with identical signature is
     * ambiguous, even when the keys differ. */
    memcpy(mut, valid, size); put32(mut + variant + 3u * VP_STATIC_INDEX_VARIANT_RECORD + 36,
                                    8u); reseal(mut, size);
    expect_reject(mut, size, "ambiguous duplicate specialization rejected");
    memcpy(mut, valid, size); put32(mut + variant + 4u * VP_STATIC_INDEX_VARIANT_RECORD, 0u);
    reseal(mut, size);
    expect_reject(mut, size, "variant in wrong stage range rejected");

    /* Program structure. */
    memcpy(mut, valid, size); put32(mut + program + 16, 50u); reseal(mut, size);
    expect_reject(mut, size, "program token order rejected");
    memcpy(mut, valid, size); put16(mut + program + 4, 0xffffu);
    put16(mut + program + 6, 0xffffu); reseal(mut, size);
    expect_reject(mut, size, "program without stages rejected");
    memcpy(mut, valid, size); put16(mut + program + 4, 1u); reseal(mut, size);
    expect_reject(mut, size, "program VS pointing at FS rejected");
    memcpy(mut, valid, size); put32(mut + program + 8, 4u); reseal(mut, size);
    expect_reject(mut, size, "program flags rejected");
    memcpy(mut, valid, size); put16(mut + program + 6, 0xffffu); reseal(mut, size);
    CHECK(vp_static_shader_index_parse(mut, size, &index, error, sizeof(error)) == 0,
          "empty fragment stage is legal");
    vp_static_shader_index_free(&index);

    /* ABI 1 structural guards. */
    stage_size = abi1_size;
    memcpy(mut, abi1, stage_size); put32(mut + variant + 36, 8u); reseal(mut, stage_size);
    expect_reject(mut, stage_size, "ABI 1 reserved word must be zero");
    memcpy(mut, abi1, stage_size); put16(mut + variant + 6, 2u); reseal(mut, stage_size);
    expect_reject(mut, stage_size, "ABI 1 record cannot claim ABI 2");

    /* File I/O round trip. */
    file = fopen("static-index-test.tmp", "wb");
    CHECK(file != NULL, "temp file open");
    if (file != NULL) {
        CHECK(fwrite(valid, 1, size, file) == size, "temp file write");
        fclose(file);
        error[0] = 0;
        CHECK(vp_static_shader_index_read_file("static-index-test.tmp", &index,
                                               error, sizeof(error)) == 0,
              "file round trip");
        vp_static_shader_index_free(&index);
        remove("static-index-test.tmp");
    }
    error[0] = 0;
    CHECK(vp_static_shader_index_read_file("static-index-missing.tmp", &index,
                                           error, sizeof(error)) < 0,
          "missing file rejected");

#undef MUTATE
    free(mut);
    free(valid);
    free(abi1);
    if (failures != 0) {
        fprintf(stderr, "static-shader-index: %u failure(s)\n", failures);
        return 1;
    }
    printf("static-shader-index: PASS\n");
    return 0;
}

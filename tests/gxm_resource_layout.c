/* Host test for the per-stage resource-layout sidecar parser.
 *
 * Two modes share one binary:
 *  - synthetic: builds a tiny sidecar + index in memory and mutates it to
 *    prove wrong-parent-SHA, duplicate-offset, unknown-type, missing-stage,
 *    CRC, ordering and truncation refusals;
 *  - real: when VP_RESOURCE_SIDECAR / VP_TEST_INDEX_PATH are defined, parses
 *    the locked installed bank and verifies the witness stages 429 (offset
 *    32) and 2377 (offset 40) against the catalogue-derived sidecar.
 */
#include "gxm_resource_layout.h"
#include "static_shader_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef VP_RESOURCE_SIDECAR
#include "nso_loader.h"
#endif

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static uint32_t crc32_ieee(const unsigned char *data, size_t size) {
    return vp_static_index_crc32(data, size);
}

static uint16_t wr16(unsigned char *p, uint16_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    return value;
}

static void wr32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

typedef struct {
    unsigned char image[4096];
    size_t size;
    unsigned char parent_sha[32];
    uint32_t parent_crc;
} Synthetic;

static const unsigned char k_vertex_sha[32] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01,
    0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78, 0x89,
    0x9a, 0xab, 0xbc, 0xcd, 0xde, 0xef, 0xf0, 0x02};
static const unsigned char k_fragment_sha[32] = {
    0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98,
    0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f, 0x10,
    0x31, 0x42, 0x53, 0x64, 0x75, 0x86, 0x97, 0xa8,
    0xb9, 0xca, 0xdb, 0xec, 0xfd, 0x0e, 0x1f, 0x20};

/* Two stages, one fragment texture at offset 40, no vertex textures. */
static void synthetic_build(Synthetic *out) {
    unsigned char *body;
    unsigned i;

    memset(out, 0, sizeof(*out));
    for (i = 0; i < 32; ++i) out->parent_sha[i] = (unsigned char)(0x40 + i);
    out->parent_crc = 0x12345678u;
    body = out->image + 96;
    /* Stage 0: vertex, no textures. */
    body[0] = 1;
    body[2] = 0;
    wr16(body + 4, 0);
    wr16(body + 6, 0);
    memcpy(body + 8, k_vertex_sha, 32);
    /* Stage 1: fragment, one texture. */
    body[40] = 5;
    body[42] = 1;
    wr16(body + 44, 0);
    wr16(body + 46, 1);
    memcpy(body + 48, k_fragment_sha, 32);
    /* Texture record: tex0, Color2D, non-depth, offset 40. */
    body[80] = 0;   /* slot */
    body[81] = 2;   /* Color2D */
    body[82] = 0;   /* depth */
    body[83] = 0;   /* secondary */
    wr16(body + 84, 40);
    wr16(body + 86, 1);
    body[88] = 0;   /* shift */
    body[89] = 3;   /* size_shift */
    body[90] = 0;   /* env bank */
    body[91] = 0;
    memset(out->image, 0, 96);
    memcpy(out->image, "VPSRES1", 8);
    wr16(out->image + 8, 1);
    wr16(out->image + 10, 96);
    wr32(out->image + 12, 1);
    wr32(out->image + 16, 2);
    wr32(out->image + 20, 1);
    wr32(out->image + 24, 40);
    wr32(out->image + 28, 16);
    wr32(out->image + 32, 96);
    wr32(out->image + 36, crc32_ieee(body, 96));
    memcpy(out->image + 40, out->parent_sha, 32);
    wr32(out->image + 72, out->parent_crc);
    out->size = 96 + 96;
}

static void synthetic_index(VPStaticShaderIndex *index, VPStaticStage stages[2]) {
    memset(index, 0, sizeof(*index));
    memset(stages, 0, sizeof(VPStaticStage) * 2);
    stages[0].stage = 1;
    memcpy(stages[0].sha256, k_vertex_sha, 32);
    stages[1].stage = 5;
    memcpy(stages[1].sha256, k_fragment_sha, 32);
    index->stages = stages;
    index->stage_count = 2;
    index->abi_version = 2;
}

static void expect_status(int rc, int expected, const char *what) {
    if (rc != expected) {
        fprintf(stderr, "FAIL %s: rc=%d expected=%d (%s)\n", what, rc, expected,
                vp_resource_layout_status_name(rc));
        failures++;
    }
}

static void test_synthetic(void) {
    Synthetic image;
    VPStaticShaderIndex index;
    VPStaticStage index_stages[2];
    VPResourceLayout layout;
    char error[128];
    unsigned char saved;

    synthetic_index(&index, index_stages);
    synthetic_build(&image);
    memset(&layout, 0, sizeof(layout));
    expect_status(vp_resource_layout_parse(image.image, image.size,
                    image.parent_sha, image.parent_crc, &index, &layout,
                    error, sizeof(error)), 0, "valid sidecar");
    CHECK(layout.stage_count == 2u);
    CHECK(layout.texture_count == 1u);
    if (layout.stages == NULL || layout.textures == NULL) {
        vp_resource_layout_free(&layout);
        return;
    }
    {
        const VPResourceStage *stage = vp_resource_layout_stage(
            &layout, 5, layout.stages[1].sha256);
        VPResourceTexture texture;
        CHECK(stage != NULL);
        CHECK(stage != NULL && stage->stage_index == 1u);
        CHECK(stage != NULL && vp_resource_layout_texture(&layout, stage, 0,
                                                          &texture) == 0);
        CHECK(texture.offset == 40u && texture.slot == 0u &&
              texture.texture_type == 2u && texture.env_bank == 0u);
        CHECK(stage != NULL && vp_resource_layout_texture(&layout, stage, 1,
                                                          &texture) == -1);
    }
    CHECK(vp_resource_layout_stage(&layout, 1, layout.stages[0].sha256) != NULL);
    {
        unsigned char unknown[32];
        memset(unknown, 0xaa, sizeof(unknown));
        CHECK(vp_resource_layout_stage(&layout, 5, unknown) == NULL);
    }
    vp_resource_layout_free(&layout);

    /* Wrong parent identity: output must be left fully zeroed on refusal. */
    synthetic_build(&image);
    image.parent_sha[0] ^= 1u;
    memset(&layout, 0xaa, sizeof(layout));
    expect_status(vp_resource_layout_parse(image.image, image.size,
                    image.parent_sha, image.parent_crc, &index, &layout,
                    error, sizeof(error)), -9, "wrong parent sha");
    CHECK(layout.stages == NULL && layout.textures == NULL);
    CHECK(layout.stage_count == 0u && layout.texture_count == 0u);
    CHECK(layout.parent_crc32 == 0u);
    synthetic_build(&image);
    image.parent_crc ^= 1u;
    expect_status(vp_resource_layout_parse(image.image, image.size,
                    image.parent_sha, image.parent_crc, &index, &layout,
                    error, sizeof(error)), -9, "wrong parent crc");

    /* Body corruption. */
    synthetic_build(&image);
    image.image[100] ^= 0x80u;
    expect_status(vp_resource_layout_parse(image.image, image.size,
                    image.parent_sha, image.parent_crc, &index, &layout,
                    error, sizeof(error)), -5, "body crc");

    /* Truncation. */
    synthetic_build(&image);
    expect_status(vp_resource_layout_parse(image.image, image.size - 8u,
                    image.parent_sha, image.parent_crc, &index, &layout,
                    error, sizeof(error)), -5, "truncated body");

    /* Duplicate offset: second texture at the same offset. */
    synthetic_build(&image);
    /* Expand the fragment stage to two textures and the texture table to two:
     * 2*40 stage bytes + 2*16 texture bytes = 112. */
    image.image[96 + 42] = 2;
    wr32(image.image + 20, 2);
    wr32(image.image + 32, 112);
    /* Second texture record at body offset 96. */
    image.image[96 + 96] = 1;
    image.image[96 + 97] = 2;
    wr16(image.image + 96 + 100, 40);
    wr16(image.image + 96 + 102, 1);
    image.image[96 + 104] = 0;
    image.image[96 + 105] = 3;
    wr32(image.image + 36, crc32_ieee(image.image + 96, 112));
    image.size = 96 + 112;
    expect_status(vp_resource_layout_parse(image.image, image.size,
                    image.parent_sha, image.parent_crc, &index, &layout,
                    error, sizeof(error)), -7, "duplicate offset");

    /* Z2: stage claims two descriptors while the actual texture table holds
     * one.  The CRC covers the malformed image, so only the table bound can
     * reject it (and ASAN proves nothing past the table is read). */
    synthetic_build(&image);
    image.image[96 + 42] = 2; /* fragment texture_count */
    wr32(image.image + 36, crc32_ieee(image.image + 96, 96));
    expect_status(vp_resource_layout_parse(image.image, image.size,
                    image.parent_sha, image.parent_crc, &index, &layout,
                    error, sizeof(error)), -6, "range outside actual table");

    /* Unknown descriptor type. */
    synthetic_build(&image);
    image.image[96 + 81] = 9;
    wr32(image.image + 36, crc32_ieee(image.image + 96, 96));
    expect_status(vp_resource_layout_parse(image.image, image.size,
                    image.parent_sha, image.parent_crc, &index, &layout,
                    error, sizeof(error)), -11, "unknown type");

    /* Unsupported descriptor shape (count != 1). */
    synthetic_build(&image);
    wr16(image.image + 96 + 86, 2);
    wr32(image.image + 36, crc32_ieee(image.image + 96, 96));
    expect_status(vp_resource_layout_parse(image.image, image.size,
                    image.parent_sha, image.parent_crc, &index, &layout,
                    error, sizeof(error)), -11, "unsupported shape");

    /* Missing metadata: sidecar claims two stages but the index has one. */
    {
        VPStaticShaderIndex short_index;
        synthetic_build(&image);
        memset(&short_index, 0, sizeof(short_index));
        short_index.stages = index_stages;
        short_index.stage_count = 1;
        expect_status(vp_resource_layout_parse(image.image, image.size,
                        image.parent_sha, image.parent_crc, &short_index,
                        &layout, error, sizeof(error)), -8,
                      "missing index stage");
    }

    /* Stage association swap: keep the SHA but point at the wrong stage. */
    synthetic_build(&image);
    wr16(image.image + 96 + 46, 0);
    wr32(image.image + 36, crc32_ieee(image.image + 96, 96));
    expect_status(vp_resource_layout_parse(image.image, image.size,
                    image.parent_sha, image.parent_crc, &index, &layout,
                    error, sizeof(error)), -6, "stage index association");

    /* Duplicate stage index. */
    synthetic_build(&image);
    wr16(image.image + 96 + 6, 1);
    wr32(image.image + 36, crc32_ieee(image.image + 96, 96));
    expect_status(vp_resource_layout_parse(image.image, image.size,
                    image.parent_sha, image.parent_crc, &index, &layout,
                    error, sizeof(error)), -6, "duplicate stage index");

    /* Reserved byte must be zero. */
    synthetic_build(&image);
    saved = image.image[96 + 1];
    image.image[96 + 1] = 1;
    wr32(image.image + 36, crc32_ieee(image.image + 96, 96));
    expect_status(vp_resource_layout_parse(image.image, image.size,
                    image.parent_sha, image.parent_crc, &index, &layout,
                    error, sizeof(error)), -6, "reserved bytes");
    image.image[96 + 1] = saved;
}

#ifdef VP_RESOURCE_SIDECAR
static void test_real_bank(void) {
    VPStaticShaderIndex index;
    VPResourceLayout layout;
    unsigned char *sidecar = NULL;
    unsigned char parent_sha[32];
    unsigned char *index_bytes = NULL;
    unsigned char digest[32];
    uint32_t parent_crc;
    FILE *file;
    long length;
    char error[160];

    memset(&index, 0, sizeof(index));
    if (vp_static_shader_index_read_file(VP_TEST_INDEX_PATH, &index,
                                         error, sizeof(error)) < 0) {
        fprintf(stderr, "FAIL real index: %s\n", error);
        failures++;
        return;
    }
    file = fopen(VP_TEST_INDEX_PATH, "rb");
    CHECK(file != NULL);
    if (file == NULL) return;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    rewind(file);
    index_bytes = (unsigned char *)malloc((size_t)length);
    CHECK(index_bytes != NULL);
    if (index_bytes == NULL) { fclose(file); return; }
    CHECK(fread(index_bytes, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    vp_sha256(index_bytes, (size_t)length, parent_sha);
    parent_crc = crc32_ieee(index_bytes, (size_t)length);
    free(index_bytes);

    file = fopen(VP_RESOURCE_SIDECAR, "rb");
    CHECK(file != NULL);
    if (file == NULL) return;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    rewind(file);
    sidecar = (unsigned char *)malloc((size_t)length);
    CHECK(sidecar != NULL);
    if (sidecar == NULL) { fclose(file); return; }
    CHECK(fread(sidecar, 1, (size_t)length, file) == (size_t)length);
    fclose(file);

    memset(&layout, 0, sizeof(layout));
    expect_status(vp_resource_layout_parse(sidecar, (size_t)length, parent_sha,
                    parent_crc, &index, &layout, error, sizeof(error)), 0,
                  "real sidecar");
    CHECK(layout.stage_count == 2402u);
    CHECK(layout.texture_count == 6217u);
    {
        static const unsigned char sha429[32] = {
            0x0d,0xdd,0x70,0x43,0x90,0xcd,0xf4,0x35,0x23,0x59,0x2e,0xf4,
            0x2e,0xa8,0x37,0xf8,0xef,0x60,0xcb,0xb7,0x1f,0x57,0x3f,0x46,
            0x05,0x26,0xd0,0x67,0x24,0x1d,0x7f,0xb6};
        static const unsigned char sha2377[32] = {
            0xfd,0x93,0x08,0xad,0xe3,0xe0,0x6b,0x1d,0x66,0x9d,0x14,0x03,
            0x1f,0xbe,0xbc,0xa6,0xe6,0xf5,0xac,0x1f,0xfe,0xed,0xf1,0x14,
            0x27,0xfa,0x9e,0x76,0x52,0x8e,0x41,0x80};
        const VPResourceStage *stage = vp_resource_layout_stage(&layout, 5, sha429);
        VPResourceTexture texture;
        CHECK(stage != NULL && stage->stage_index == 429u);
        CHECK(stage != NULL && vp_resource_layout_texture(&layout, stage, 0,
                                                          &texture) == 0);
        CHECK(texture.offset == 32u);
        CHECK((texture.flags & VP_RESOURCE_TEXTURE_USAGE_ANALYZED) != 0u);
        CHECK((texture.flags & VP_RESOURCE_TEXTURE_USAGE_MASK) ==
              VP_RESOURCE_TEXTURE_USAGE_R);
        stage = vp_resource_layout_stage(&layout, 5, sha2377);
        CHECK(stage != NULL && stage->stage_index == 2377u);
        CHECK(stage != NULL && vp_resource_layout_texture(&layout, stage, 0,
                                                          &texture) == 0);
        CHECK(texture.offset == 40u);
    }
    /* A wrong parent identity must refuse the real file too. */
    parent_sha[0] ^= 1u;
    vp_resource_layout_free(&layout);
    expect_status(vp_resource_layout_parse(sidecar, (size_t)length, parent_sha,
                    parent_crc, &index, &layout, error, sizeof(error)), -9,
                  "real sidecar wrong parent");
    free(sidecar);
    vp_static_shader_index_free(&index);
    (void)digest;
}
#endif

int main(void) {
    test_synthetic();
#ifdef VP_RESOURCE_SIDECAR
    test_real_bank();
#endif
    if (failures != 0) {
        fprintf(stderr, "gxm_resource_layout: %d failure(s)\n", failures);
        return 1;
    }
    printf("gxm_resource_layout: all checks passed\n");
    return 0;
}

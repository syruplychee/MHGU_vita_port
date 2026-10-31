/* Host checks for the shared Maxwell vertex-format decoder.
 *
 * Expected values are derived independently from the descriptor bit fields and
 * from the RGB10A2 word layout; the test does not call the production
 * conversion to compute its own expectation.
 */
#include "vertex_format.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static uint32_t descriptor(unsigned type, unsigned size, unsigned offset) {
    return (offset << 7) | (size << 21) | (type << 27);
}

static void test_decoder(void) {
    VpVertexFormat format;

    CHECK(vp_vertex_format_decode(descriptor(7, 0x12, 0), 16, &format) == 0);
    CHECK(format.kind == VP_VERTEX_FORMAT_F32 && format.components == 1 &&
          format.source_bytes == 4 && format.source_offset == 0);
    CHECK(vp_vertex_format_decode(descriptor(7, 0x01, 4), 32, &format) == 0);
    CHECK(format.kind == VP_VERTEX_FORMAT_F32 && format.components == 4 &&
          format.source_bytes == 16 && format.source_offset == 4);
    CHECK(vp_vertex_format_decode(descriptor(7, 0x03, 0), 16, &format) == 0);
    CHECK(format.kind == VP_VERTEX_FORMAT_F16 && format.components == 4 &&
          format.source_bytes == 8);
    CHECK(vp_vertex_format_decode(descriptor(2, 0x0a, 8), 16, &format) == 0);
    CHECK(format.kind == VP_VERTEX_FORMAT_U8N && format.components == 4 &&
          format.source_bytes == 4 && format.source_offset == 8);
    CHECK(vp_vertex_format_decode(descriptor(1, 0x03, 0), 16, &format) == 0);
    CHECK(format.kind == VP_VERTEX_FORMAT_S16N && format.source_bytes == 8);
    CHECK(vp_vertex_format_decode(descriptor(1, 0x0a, 0), 16, &format) == 0);
    CHECK(format.kind == VP_VERTEX_FORMAT_S8N && format.source_bytes == 4);
    CHECK(vp_vertex_format_decode(descriptor(4, 0x0a, 12), 16, &format) == 0);
    CHECK(format.kind == VP_VERTEX_FORMAT_U8 && format.source_bytes == 4 &&
          format.source_offset == 12);
    CHECK(vp_vertex_format_decode(descriptor(2, 0x30, 12), 16, &format) == 0);
    CHECK(format.kind == VP_VERTEX_FORMAT_RGB10A2_UNORM &&
          format.components == 4 && format.source_bytes == 4 &&
          format.source_offset == 12);
    /* The observed village descriptor. */
    CHECK(vp_vertex_format_decode(0x16000600u, 16, &format) == 0);
    CHECK(format.kind == VP_VERTEX_FORMAT_RGB10A2_UNORM &&
          format.source_offset == 12);

    /* Unknown or overrunning combinations must refuse. */
    CHECK(vp_vertex_format_decode(descriptor(2, 0x31, 0), 16, &format) < 0);
    CHECK(vp_vertex_format_decode(descriptor(0, 0x0a, 0), 16, &format) < 0);
    CHECK(vp_vertex_format_decode(descriptor(5, 0x0a, 0), 16, &format) < 0);
    CHECK(vp_vertex_format_decode(descriptor(7, 0x30, 0), 16, &format) < 0);
    CHECK(vp_vertex_format_decode(descriptor(4, 0x30, 0), 16, &format) < 0);
    CHECK(vp_vertex_format_decode(descriptor(2, 0x0a, 13), 16, &format) < 0);
    CHECK(vp_vertex_format_decode(descriptor(7, 0x01, 0) | 1u, 16, &format) < 0);
    CHECK(vp_vertex_format_decode(descriptor(7, 0x01, 0) | 64u, 16, &format) < 0);
    CHECK(vp_vertex_format_decode(descriptor(7, 0x01, 0) | 0x80000000u, 16, &format) < 0);
    CHECK(vp_vertex_format_decode(descriptor(7, 0x01, 0), 0, &format) < 0);
}

static void test_input_mask(void) {
    CHECK(vp_vertex_format_check_input(descriptor(4, 0x0a, 16), 0, 1) == 0);
    CHECK(vp_vertex_format_check_input(descriptor(4, 0x0a, 16), 1, 0) < 0);
    CHECK(vp_vertex_format_check_input(descriptor(4, 0x0a, 16), 1, 1) < 0);
    CHECK(vp_vertex_format_check_input(descriptor(2, 0x30, 12), 1, 0) == 0);
    CHECK(vp_vertex_format_check_input(descriptor(2, 0x30, 12), 0, 1) < 0);
    CHECK(vp_vertex_format_check_input(descriptor(7, 0x01, 0), 1, 0) == 0);
    CHECK(vp_vertex_format_check_input(descriptor(7, 0x01, 0), 0, 1) < 0);
}

static uint32_t packed_word(unsigned r, unsigned g, unsigned b, unsigned a) {
    return (r & 0x3ffu) | ((g & 0x3ffu) << 10) |
           ((b & 0x3ffu) << 20) | ((a & 3u) << 30);
}

static void test_packed_conversion(void) {
    static const unsigned samples[][4] = {
        {0, 0, 0, 0}, {1023, 0, 0, 3}, {0, 1023, 0, 1},
        {0, 0, 1023, 2}, {1, 2, 3, 1}, {512, 511, 513, 2}
    };
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        float out[4];
        uint32_t word = packed_word(samples[i][0], samples[i][1],
                                    samples[i][2], samples[i][3]);
        vp_vertex_rgb10a2_to_float4(word, out);
        /* Independent expectation: exact integer ratio rounded once to float. */
        CHECK(out[0] == (float)samples[i][0] / 1023.0f);
        CHECK(out[1] == (float)samples[i][1] / 1023.0f);
        CHECK(out[2] == (float)samples[i][2] / 1023.0f);
        CHECK(out[3] == (float)samples[i][3] / 3.0f);
    }
    {
        float out[4];
        vp_vertex_rgb10a2_to_float4(packed_word(1023, 1023, 1023, 3), out);
        CHECK(out[0] == 1.0f && out[1] == 1.0f && out[2] == 1.0f && out[3] == 1.0f);
    }
}

static void test_expansion(void) {
    uint32_t descriptors[16];
    uint8_t source[2 * 16];
    float out[2 * 32];
    unsigned stride = 0;
    uint32_t word0 = packed_word(1023, 0, 0, 3);
    uint32_t word1 = packed_word(0, 1023, 0, 0);

    memset(descriptors, 0, sizeof(descriptors));
    memset(source, 0, sizeof(source));
    descriptors[1] = descriptor(2, 0x30, 12);
    memcpy(source + 12, &word0, 4);
    memcpy(source + 16 + 12, &word1, 4);
    CHECK(vp_vertex_expand_rgb10a2(source, 2, 16, descriptors, 1u << 1,
                                   out, sizeof(out), &stride) == 1);
    CHECK(stride == 16);
    CHECK(out[0] == 1.0f && out[1] == 0.0f && out[2] == 0.0f && out[3] == 1.0f);
    CHECK(out[4] == 0.0f && out[5] == 1.0f && out[6] == 0.0f && out[7] == 0.0f);

    /* Two packed attributes receive two consecutive float4 slots. */
    descriptors[4] = descriptor(2, 0x30, 0);
    word0 = packed_word(1, 2, 3, 1);
    memcpy(source, &word0, 4);
    CHECK(vp_vertex_expand_rgb10a2(source, 2, 16, descriptors,
                                   (1u << 1) | (1u << 4), out, sizeof(out),
                                   &stride) == 2);
    CHECK(stride == 32);
    CHECK(out[0] == 1.0f && out[1] == 0.0f && out[2] == 0.0f && out[3] == 1.0f);
    CHECK(out[4] == (float)1 / 1023.0f && out[5] == (float)2 / 1023.0f &&
          out[6] == (float)3 / 1023.0f && out[7] == (float)1 / 3.0f);

    /* A mask entry that is not a packed attribute is a hard error. */
    CHECK(vp_vertex_expand_rgb10a2(source, 2, 16, descriptors, 1u << 2,
                                   out, sizeof(out), &stride) < 0);
    /* Capacity and source-range failures must refuse. */
    CHECK(vp_vertex_expand_rgb10a2(source, 2, 16, descriptors, 1u << 1,
                                   out, 16, &stride) < 0);
    /* A mask mixing a valid location with an out-of-range bit must refuse. */
    CHECK(vp_vertex_expand_rgb10a2(source, 2, 16, descriptors,
                                   (1u << 1) | 0x80000000u, out, sizeof(out),
                                   &stride) < 0);
    CHECK(vp_vertex_expand_rgb10a2(source, 2, 16, descriptors, 0u,
                                   out, sizeof(out), &stride) < 0);
    descriptors[1] = descriptor(2, 0x30, 13);
    CHECK(vp_vertex_expand_rgb10a2(source, 2, 16, descriptors, 1u << 1,
                                   out, sizeof(out), &stride) < 0);
}

int main(void) {
    test_decoder();
    test_input_mask();
    test_packed_conversion();
    test_expansion();
    if (failures) {
        fprintf(stderr, "vertex_format: %d failure(s)\n", failures);
        return 1;
    }
    printf("vertex_format: PASS\n");
    return 0;
}

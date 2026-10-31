/* Optional integration check against the built M6-B3 candidate bank.
 *
 * Usage: static-shader-candidate <candidate-bank-directory>
 * Structural acceptance alone is recorded as a host check; it does not prove a
 * target draw, pixels or hardware behaviour.
 */
#include "static_shader_index.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

static const VPStaticVariant *pick(const VPStaticShaderIndex *index,
                                   uint16_t stage, uint32_t mask,
                                   uint32_t signature, int *ambiguous) {
    const VPStaticVariant *variant =
        vp_static_variant_select(index, stage, 1u, mask, ambiguous);
    return variant != NULL && variant->input_signature == signature ? variant : NULL;
}

int main(int argc, char **argv) {
    VPStaticShaderIndex index;
    char error[256];
    char path[1024];
    const VPStaticVariant *generic, *specialized;
    int ambiguous;
    if (argc != 2) {
        fprintf(stderr, "usage: %s <candidate-bank>\n", argv[0]);
        return 2;
    }
    snprintf(path, sizeof(path), "%s/shader-index.bin", argv[1]);
    memset(&index, 0, sizeof(index));
    error[0] = 0;
    if (vp_static_shader_index_read_file(path, &index, error, sizeof(error)) < 0) {
        fprintf(stderr, "FAIL candidate index: %s\n", error);
        return 1;
    }
    CHECK(index.abi_version == 2u, "candidate bank is ABI 2");
    CHECK(index.stage_count == 2402u, "candidate keeps 2402 stages");
    CHECK(index.variant_count == 278u, "candidate has 221+57 variants");
    CHECK(index.program_count == 6311u, "candidate keeps 6311 programs");

    generic = pick(&index, 266u, 0u, 0u, &ambiguous);
    CHECK(generic != NULL && ambiguous == 0, "stage 266 old float variant retained");
    specialized = pick(&index, 266u, 1u << 3, 1u << 3, &ambiguous);
    CHECK(specialized != NULL && ambiguous == 0,
          "stage 266 attr3 UINT specialization selected");
    CHECK(generic != NULL && specialized != NULL &&
          memcmp(generic->key, specialized->key, 16) != 0,
          "stage 266 variants are distinct");
    specialized = pick(&index, 61u, 1u << 3, 1u << 3, &ambiguous);
    CHECK(specialized != NULL && ambiguous == 0,
          "stage 61 attr3 UINT specialization selected");
    specialized = pick(&index, 178u, 1u << 3, 1u << 3, &ambiguous);
    CHECK(specialized != NULL && ambiguous == 0,
          "stage 178 attr3 UINT specialization selected");

    generic = pick(&index, 40u, 0u, 0u, &ambiguous);
    CHECK(generic != NULL && ambiguous == 0, "stage 40 viewport-1 variant added");
    generic = pick(&index, 243u, 0u, 0u, &ambiguous);
    CHECK(generic != NULL && ambiguous == 0, "stage 243 viewport-1 variant added");
    generic = pick(&index, 117u, 0u, 0u, &ambiguous);
    CHECK(generic != NULL && ambiguous == 0, "stage 117 viewport-1 variant added");

    /* The eight blocked fragment stages stay absent rather than being paired
     * with an arbitrary substitute. */
    CHECK(vp_static_variant_select(&index, 1133u, 1u, 0u, &ambiguous) == NULL,
          "blocked stage 1133 has no variant");
    CHECK(vp_static_variant_select(&index, 423u, 1u, 0u, &ambiguous) == NULL,
          "blocked stage 423 has no variant");

    vp_static_shader_index_free(&index);
    if (failures != 0) {
        fprintf(stderr, "static-shader-candidate: %u failure(s)\n", failures);
        return 1;
    }
    printf("static-shader-candidate: PASS\n");
    return 0;
}

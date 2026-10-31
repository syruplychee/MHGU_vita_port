#include "static_shader_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void set_error(char *error, size_t size, const char *message) {
    if (error == NULL || size == 0u) return;
    snprintf(error, size, "%s", message);
}

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

uint32_t vp_static_index_crc32(const void *data, size_t size) {
    if (data == NULL) return 0u;
    return crc32_ieee((const uint8_t *)data, size);
}

static uint64_t fnv1a64(const uint8_t *data, size_t size) {
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;
    for (i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static int checked_body_size(uint32_t stages, uint32_t variants, uint32_t programs,
                             uint64_t *size) {
    uint64_t total = 0;
    total += (uint64_t)stages * VP_STATIC_INDEX_STAGE_RECORD;
    total += (uint64_t)variants * VP_STATIC_INDEX_VARIANT_RECORD;
    total += (uint64_t)programs * VP_STATIC_INDEX_PROGRAM_RECORD;
    if (total > VP_STATIC_INDEX_MAX_BYTES) return -1;
    *size = total;
    return 0;
}

/* Open-addressed set of (stage, sha256) pairs. Returns -1 when a duplicate is
 * found. Slots store a hash; zero means empty (no real FNV hash is zero). */
static int check_stage_duplicates(const VPStaticStage *stages, uint32_t count,
                                  char *error, size_t error_size) {
    size_t capacity = 16;
    uint64_t *hashes;
    uint8_t *used;
    uint32_t i;
    int rc = 0;
    while (capacity < (size_t)count * 2u) capacity <<= 1;
    hashes = (uint64_t *)calloc(capacity, sizeof(*hashes));
    used = (uint8_t *)calloc(capacity, sizeof(*used));
    if (hashes == NULL || used == NULL) {
        free(hashes);
        free(used);
        set_error(error, error_size, "out of memory for duplicate table");
        return -2;
    }
    for (i = 0; i < count; ++i) {
        uint8_t key[36];
        uint64_t hash;
        size_t slot;
        memset(key, 0, sizeof(key));
        key[0] = (uint8_t)stages[i].stage;
        memcpy(key + 4, stages[i].sha256, 32);
        hash = fnv1a64(key, sizeof(key));
        if (hash == 0u) hash = 1u;
        slot = (size_t)(hash & (uint64_t)(capacity - 1u));
        while (used[slot]) {
            if (hashes[slot] == hash) {
                set_error(error, error_size, "duplicate stage content");
                rc = -1;
                goto done;
            }
            slot = (slot + 1u) & (capacity - 1u);
        }
        used[slot] = 1u;
        hashes[slot] = hash;
    }
done:
    free(hashes);
    free(used);
    return rc;
}

static int key_equal(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 16) == 0;
}

int vp_static_shader_index_parse(const void *data, size_t size,
                                 VPStaticShaderIndex *out,
                                 char *error, size_t error_size) {
    const uint8_t *bytes = (const uint8_t *)data;
    const uint8_t *body;
    uint32_t stage_count, variant_count, program_count;
    uint32_t stage_size, variant_size, program_size;
    uint32_t body_bytes, body_crc, flags, abi_version;
    uint64_t expected;
    uint32_t running = 0;
    uint32_t i;
    VPStaticShaderIndex index;

    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    if (bytes == NULL || size < VP_STATIC_INDEX_HEADER_SIZE ||
        size > VP_STATIC_INDEX_MAX_BYTES) {
        set_error(error, error_size, "index size out of range");
        return -1;
    }
    if (memcmp(bytes, VP_STATIC_INDEX_MAGIC, VP_STATIC_INDEX_MAGIC_SIZE) != 0) {
        set_error(error, error_size, "bad magic");
        return -1;
    }
    if (rd16(bytes + 8) != VP_STATIC_INDEX_VERSION ||
        rd16(bytes + 10) != VP_STATIC_INDEX_HEADER_SIZE) {
        set_error(error, error_size, "unsupported version/header size");
        return -1;
    }
    flags = rd32(bytes + 12);
    if (flags != 1u) {
        set_error(error, error_size, "unsupported flags");
        return -1;
    }
    stage_count = rd32(bytes + 16);
    variant_count = rd32(bytes + 20);
    program_count = rd32(bytes + 24);
    stage_size = rd32(bytes + 28);
    variant_size = rd32(bytes + 32);
    program_size = rd32(bytes + 36);
    body_bytes = rd32(bytes + 40);
    body_crc = rd32(bytes + 44);
    abi_version = rd32(bytes + 48);
    if (stage_size != VP_STATIC_INDEX_STAGE_RECORD ||
        variant_size != VP_STATIC_INDEX_VARIANT_RECORD ||
        program_size != VP_STATIC_INDEX_PROGRAM_RECORD) {
        set_error(error, error_size, "unsupported record size");
        return -1;
    }
    if (abi_version < VP_STATIC_INDEX_ABI_VERSION_MIN ||
        abi_version > VP_STATIC_INDEX_ABI_VERSION) {
        set_error(error, error_size, "unsupported ABI version");
        return -1;
    }
    if (stage_count == 0u || stage_count > VP_STATIC_INDEX_MAX_STAGES ||
        variant_count > VP_STATIC_INDEX_MAX_VARIANTS ||
        program_count == 0u || program_count > VP_STATIC_INDEX_MAX_PROGRAMS) {
        set_error(error, error_size, "record count out of range");
        return -1;
    }
    if (checked_body_size(stage_count, variant_count, program_count, &expected) < 0 ||
        expected != (uint64_t)body_bytes ||
        (uint64_t)body_bytes != (uint64_t)size - VP_STATIC_INDEX_HEADER_SIZE) {
        set_error(error, error_size, "body size mismatch");
        return -1;
    }
    body = bytes + VP_STATIC_INDEX_HEADER_SIZE;
    if (crc32_ieee(body, body_bytes) != body_crc) {
        set_error(error, error_size, "body CRC mismatch");
        return -1;
    }

    memset(&index, 0, sizeof(index));
    index.stage_count = stage_count;
    index.variant_count = variant_count;
    index.program_count = program_count;
    index.abi_version = abi_version;
    index.stages = (VPStaticStage *)calloc(stage_count, sizeof(*index.stages));
    index.variants = (VPStaticVariant *)calloc(variant_count ? variant_count : 1u,
                                               sizeof(*index.variants));
    index.programs = (VPStaticProgram *)calloc(program_count, sizeof(*index.programs));
    if (index.stages == NULL || index.variants == NULL || index.programs == NULL) {
        set_error(error, error_size, "out of memory");
        vp_static_shader_index_free(&index);
        return -1;
    }

    for (i = 0; i < stage_count; ++i) {
        const uint8_t *record = body + (size_t)i * VP_STATIC_INDEX_STAGE_RECORD;
        VPStaticStage *stage = &index.stages[i];
        stage->stage = rd32(record);
        memcpy(stage->sha256, record + 4, 32);
        stage->variant_start = rd16(record + 36);
        stage->variant_count = rd16(record + 38);
        stage->source_flags = rd32(record + 40);
        if (stage->stage != VP_STATIC_INDEX_STAGE_VERTEX &&
            stage->stage != VP_STATIC_INDEX_STAGE_FRAGMENT) {
            set_error(error, error_size, "invalid stage type");
            goto fail;
        }
        if ((stage->source_flags & ~7u) != 0u) {
            set_error(error, error_size, "invalid stage source flags");
            goto fail;
        }
        if ((uint32_t)stage->variant_start + stage->variant_count > variant_count) {
            set_error(error, error_size, "stage variant range outside table");
            goto fail;
        }
        if (stage->variant_start != running) {
            set_error(error, error_size, "stage variant ranges are not contiguous");
            goto fail;
        }
        running += stage->variant_count;
    }
    if (running != variant_count) {
        set_error(error, error_size, "variant table not fully owned by stages");
        goto fail;
    }
    if (check_stage_duplicates(index.stages, stage_count, error, error_size) < 0) goto fail;

    for (i = 0; i < variant_count; ++i) {
        const uint8_t *record = body + (size_t)stage_count * VP_STATIC_INDEX_STAGE_RECORD +
                                (size_t)i * VP_STATIC_INDEX_VARIANT_RECORD;
        VPStaticVariant *variant = &index.variants[i];
        variant->stage_index = rd32(record);
        variant->viewport_mask = rd16(record + 4);
        variant->abi_version = rd16(record + 6);
        memcpy(variant->key, record + 8, 16);
        variant->gxp_bytes = rd32(record + 24);
        variant->flags = rd32(record + 28);
        variant->gxp_crc32 = rd32(record + 32);
        variant->input_signature = abi_version >= 2u ? rd32(record + 36) : 0u;
        if (variant->stage_index >= stage_count) {
            set_error(error, error_size, "variant stage index out of range");
            goto fail;
        }
        if (variant->abi_version != abi_version ||
            (variant->flags & ~7u) != 0u ||
            (variant->flags & VP_STATIC_INDEX_VARIANT_COMPILED) == 0u) {
            set_error(error, error_size, "invalid variant flags/ABI");
            goto fail;
        }
        if ((variant->input_signature & ~VP_STATIC_INDEX_INPUT_MASK) != 0u) {
            set_error(error, error_size, "variant input signature out of range");
            goto fail;
        }
        if (abi_version == 1u && rd32(record + 36) != 0u) {
            set_error(error, error_size, "ABI 1 variant reserved word not zero");
            goto fail;
        }
        if (variant->viewport_mask == 0u || (variant->viewport_mask & ~3u) != 0u) {
            set_error(error, error_size, "invalid viewport mask");
            goto fail;
        }
        if (variant->gxp_bytes < 32u || variant->gxp_bytes > VP_STATIC_INDEX_MAX_GXP_BYTES) {
            set_error(error, error_size, "variant GXP size out of range");
            goto fail;
        }
        {
            uint32_t j;
            for (j = 0; j < i; ++j) {
                if (key_equal(index.variants[j].key, variant->key)) {
                    set_error(error, error_size, "duplicate variant key");
                    goto fail;
                }
            }
        }
    }
    for (i = 0; i < stage_count; ++i) {
        const VPStaticStage *stage = &index.stages[i];
        uint32_t j, k;
        for (j = 0; j < stage->variant_count; ++j) {
            const VPStaticVariant *variant =
                &index.variants[stage->variant_start + j];
            if (variant->stage_index != i) {
                set_error(error, error_size, "variant in wrong stage range");
                goto fail;
            }
            for (k = 0; k < j; ++k) {
                const VPStaticVariant *previous =
                    &index.variants[stage->variant_start + k];
                if ((previous->viewport_mask & variant->viewport_mask) != 0u &&
                    previous->input_signature == variant->input_signature) {
                    set_error(error, error_size,
                              "ambiguous overlapping specialization");
                    goto fail;
                }
            }
        }
    }

    for (i = 0; i < program_count; ++i) {
        const uint8_t *record = body + (size_t)stage_count * VP_STATIC_INDEX_STAGE_RECORD +
                                (size_t)variant_count * VP_STATIC_INDEX_VARIANT_RECORD +
                                (size_t)i * VP_STATIC_INDEX_PROGRAM_RECORD;
        VPStaticProgram *program = &index.programs[i];
        program->token = rd32(record);
        program->vertex_stage = rd16(record + 4);
        program->fragment_stage = rd16(record + 6);
        program->flags = rd32(record + 8);
        if (i > 0u && index.programs[i - 1u].token >= program->token) {
            set_error(error, error_size, "program tokens not strictly increasing");
            goto fail;
        }
        if ((program->flags & ~3u) != 0u) {
            set_error(error, error_size, "invalid program flags");
            goto fail;
        }
        if (program->vertex_stage == VP_STATIC_INDEX_STAGE_NONE &&
            program->fragment_stage == VP_STATIC_INDEX_STAGE_NONE) {
            set_error(error, error_size, "program has no stage");
            goto fail;
        }
        if (program->vertex_stage != VP_STATIC_INDEX_STAGE_NONE &&
            (program->vertex_stage >= stage_count ||
             index.stages[program->vertex_stage].stage != VP_STATIC_INDEX_STAGE_VERTEX)) {
            set_error(error, error_size, "program vertex stage invalid");
            goto fail;
        }
        if (program->fragment_stage != VP_STATIC_INDEX_STAGE_NONE &&
            (program->fragment_stage >= stage_count ||
             index.stages[program->fragment_stage].stage != VP_STATIC_INDEX_STAGE_FRAGMENT)) {
            set_error(error, error_size, "program fragment stage invalid");
            goto fail;
        }
    }

    *out = index;
    return 0;

fail:
    vp_static_shader_index_free(&index);
    return -1;
}

int vp_static_shader_index_read_file(const char *path, VPStaticShaderIndex *out,
                                     char *error, size_t error_size) {
    FILE *file;
    long length;
    uint8_t *data;
    int rc;

    if (path == NULL || out == NULL) return -1;
    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, error_size, "index file open failed");
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        (unsigned long)length > VP_STATIC_INDEX_MAX_BYTES) {
        set_error(error, error_size, "index file size out of range");
        fclose(file);
        return -1;
    }
    rewind(file);
    data = (uint8_t *)malloc((size_t)length ? (size_t)length : 1u);
    if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length) {
        set_error(error, error_size, "index file read failed");
        free(data);
        fclose(file);
        return -1;
    }
    fclose(file);
    rc = vp_static_shader_index_parse(data, (size_t)length, out, error, error_size);
    free(data);
    return rc;
}

void vp_static_shader_index_free(VPStaticShaderIndex *index) {
    if (index == NULL) return;
    free(index->stages);
    free(index->variants);
    free(index->programs);
    memset(index, 0, sizeof(*index));
}

const VPStaticProgram *vp_static_program_for_token(const VPStaticShaderIndex *index,
                                                   uint32_t token) {
    uint32_t low = 0, high;
    if (index == NULL || index->programs == NULL) return NULL;
    high = index->program_count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2u;
        if (index->programs[mid].token == token) return &index->programs[mid];
        if (index->programs[mid].token < token) low = mid + 1u;
        else high = mid;
    }
    return NULL;
}

const VPStaticVariant *vp_static_variant_for(const VPStaticShaderIndex *index,
                                             uint16_t stage_index,
                                             unsigned viewport_transform) {
    const VPStaticStage *stage;
    const VPStaticVariant *first = NULL;
    uint32_t i, specialized = 0;
    if (index == NULL || viewport_transform > 1u || stage_index >= index->stage_count)
        return NULL;
    stage = &index->stages[stage_index];
    for (i = 0; i < stage->variant_count; ++i) {
        const VPStaticVariant *variant = &index->variants[stage->variant_start + i];
        if ((variant->viewport_mask & (uint16_t)(1u << viewport_transform)) == 0u)
            continue;
        if (variant->input_signature == 0u) return variant;
        if (first == NULL) first = variant;
        specialized++;
    }
    /* Callers that have no input descriptors (bring-up, preload) must not
     * pick an arbitrary specialization when several exist. */
    return specialized == 1u ? first : NULL;
}

static unsigned signature_bits(uint32_t signature) {
    unsigned bits = 0;
    while (signature != 0u) {
        bits += signature & 1u;
        signature >>= 1;
    }
    return bits;
}

const VPStaticVariant *vp_static_variant_select(const VPStaticShaderIndex *index,
                                                uint16_t stage_index,
                                                unsigned viewport_transform,
                                                uint32_t required_uint_mask,
                                                int *ambiguous) {
    const VPStaticStage *stage;
    const VPStaticVariant *generic = NULL;
    const VPStaticVariant *best = NULL;
    unsigned best_bits = 0;
    int tie = 0;
    uint32_t i;
    if (ambiguous != NULL) *ambiguous = 0;
    if (index == NULL || viewport_transform > 1u || stage_index >= index->stage_count)
        return NULL;
    stage = &index->stages[stage_index];
    for (i = 0; i < stage->variant_count; ++i) {
        const VPStaticVariant *variant = &index->variants[stage->variant_start + i];
        unsigned bits;
        if ((variant->viewport_mask & (uint16_t)(1u << viewport_transform)) == 0u)
            continue;
        if (variant->input_signature == 0u) {
            generic = variant;
            continue;
        }
        /* Exact signature matching: every specialized location must be a UINT
         * descriptor in this draw. A specialized variant never substitutes for
         * a float input. */
        if ((variant->input_signature & ~required_uint_mask) != 0u) continue;
        bits = signature_bits(variant->input_signature);
        if (bits > best_bits) {
            best = variant;
            best_bits = bits;
            tie = 0;
        } else if (bits == best_bits && best != NULL &&
                   memcmp(best->key, variant->key, sizeof(best->key)) != 0) {
            tie = 1;
        }
    }
    if (tie) {
        if (ambiguous != NULL) *ambiguous = 1;
        return NULL;
    }
    return best != NULL ? best : generic;
}

int vp_static_variant_name(const VPStaticVariant *variant, char *buffer, size_t size) {
    static const char hex[] = "0123456789abcdef";
    size_t i;
    if (variant == NULL || buffer == NULL || size < 38u) return -1;
    for (i = 0; i < 16u; ++i) {
        buffer[i * 2u] = hex[variant->key[i] >> 4];
        buffer[i * 2u + 1u] = hex[variant->key[i] & 0x0fu];
    }
    memcpy(buffer + 32, ".gxp", 5);
    return 0;
}

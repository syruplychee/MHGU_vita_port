#include "gxm_resource_layout.h"

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

static int stage_compare(unsigned type_a, const uint8_t sha_a[32],
                         unsigned type_b, const uint8_t sha_b[32]) {
    if (type_a != type_b) return type_a < type_b ? -1 : 1;
    return memcmp(sha_a, sha_b, 32);
}

static int validate_textures(const VPResourceTexture *textures,
                             uint32_t total, uint32_t start, uint32_t count,
                             char *error, size_t error_size) {
    uint32_t i, j;
    if (start > total || count > total - start) {
        set_error(error, error_size, "texture range outside the actual table");
        return VP_RESOURCE_LAYOUT_ERR_RECORD;
    }
    for (i = 0; i < count; ++i) {
        const VPResourceTexture *texture = &textures[start + i];
        if (texture->slot != i) {
            set_error(error, error_size, "texture slot is not its ordinal");
            return VP_RESOURCE_LAYOUT_ERR_RECORD;
        }
        if (texture->texture_type > VP_RESOURCE_TEX_2D_RECT) {
            set_error(error, error_size, "unknown texture descriptor type");
            return VP_RESOURCE_LAYOUT_ERR_UNSUPPORTED;
        }
        if (texture->depth > 1u || texture->secondary != 0u) {
            set_error(error, error_size, "unsupported depth/secondary flag");
            return VP_RESOURCE_LAYOUT_ERR_UNSUPPORTED;
        }
        if (texture->count != 1u || texture->shift != 0u ||
            texture->size_shift != 3u) {
            set_error(error, error_size, "unsupported descriptor shape");
            return VP_RESOURCE_LAYOUT_ERR_UNSUPPORTED;
        }
        if (texture->offset > 4088u || (texture->offset & 7u) != 0u) {
            set_error(error, error_size, "texture handle offset invalid");
            return VP_RESOURCE_LAYOUT_ERR_RECORD;
        }
        if (texture->env_bank >= 18u) {
            set_error(error, error_size, "offline environment bank invalid");
            return VP_RESOURCE_LAYOUT_ERR_RECORD;
        }
        if ((texture->flags & ~VP_RESOURCE_TEXTURE_FLAGS_MASK) != 0u) {
            set_error(error, error_size, "unknown texture usage flags");
            return VP_RESOURCE_LAYOUT_ERR_RECORD;
        }
        for (j = 0; j < i; ++j) {
            if (textures[start + j].offset == texture->offset) {
                set_error(error, error_size, "duplicate texture handle offset");
                return VP_RESOURCE_LAYOUT_ERR_DUPLICATE;
            }
        }
    }
    return VP_RESOURCE_LAYOUT_OK;
}

int vp_resource_layout_parse(const void *data, size_t size,
                             const unsigned char parent_sha256[32],
                             uint32_t parent_crc32,
                             const VPStaticShaderIndex *index,
                             VPResourceLayout *out,
                             char *error, size_t error_size) {
    const uint8_t *bytes = (const uint8_t *)data;
    const uint8_t *body;
    uint32_t stage_count, texture_count, stage_size, texture_size;
    uint32_t body_bytes, body_crc, running = 0;
    uint8_t *seen = NULL;
    VPResourceLayout layout;
    uint32_t i;
    int rc = VP_RESOURCE_LAYOUT_ERR_RECORD;

    if (out == NULL) return VP_RESOURCE_LAYOUT_ERR_ARG;
    memset(out, 0, sizeof(*out));
    if (bytes == NULL || parent_sha256 == NULL || index == NULL) {
        set_error(error, error_size, "null argument");
        return VP_RESOURCE_LAYOUT_ERR_ARG;
    }
    if (size < VP_RESOURCE_LAYOUT_HEADER_SIZE ||
        size > VP_RESOURCE_LAYOUT_MAX_BYTES) {
        set_error(error, error_size, "sidecar size out of range");
        return VP_RESOURCE_LAYOUT_ERR_SIZE;
    }
    if (memcmp(bytes, VP_RESOURCE_LAYOUT_MAGIC,
               VP_RESOURCE_LAYOUT_MAGIC_SIZE) != 0) {
        set_error(error, error_size, "bad sidecar magic");
        return VP_RESOURCE_LAYOUT_ERR_MAGIC;
    }
    if (rd16(bytes + 8) != VP_RESOURCE_LAYOUT_VERSION ||
        rd16(bytes + 10) != VP_RESOURCE_LAYOUT_HEADER_SIZE ||
        rd32(bytes + 12) != VP_RESOURCE_LAYOUT_FLAGS) {
        set_error(error, error_size, "unsupported sidecar version/header/flags");
        return VP_RESOURCE_LAYOUT_ERR_VERSION;
    }
    stage_count = rd32(bytes + 16);
    texture_count = rd32(bytes + 20);
    stage_size = rd32(bytes + 24);
    texture_size = rd32(bytes + 28);
    body_bytes = rd32(bytes + 32);
    body_crc = rd32(bytes + 36);
    if (stage_size != VP_RESOURCE_LAYOUT_STAGE_RECORD ||
        texture_size != VP_RESOURCE_LAYOUT_TEXTURE_RECORD) {
        set_error(error, error_size, "unsupported sidecar record size");
        return VP_RESOURCE_LAYOUT_ERR_RECORD;
    }
    if (stage_count == 0u || stage_count > VP_RESOURCE_LAYOUT_MAX_STAGES ||
        texture_count > VP_RESOURCE_LAYOUT_MAX_TEXTURES) {
        set_error(error, error_size, "sidecar record count out of range");
        return VP_RESOURCE_LAYOUT_ERR_RECORD;
    }
    {
        uint64_t expected = (uint64_t)stage_count *
                                VP_RESOURCE_LAYOUT_STAGE_RECORD +
                            (uint64_t)texture_count *
                                VP_RESOURCE_LAYOUT_TEXTURE_RECORD;
        if (expected != body_bytes ||
            (uint64_t)body_bytes != (uint64_t)size -
                                         VP_RESOURCE_LAYOUT_HEADER_SIZE) {
            set_error(error, error_size, "sidecar body size mismatch");
            return VP_RESOURCE_LAYOUT_ERR_BODY;
        }
    }
    body = bytes + VP_RESOURCE_LAYOUT_HEADER_SIZE;
    if (vp_static_index_crc32(body, body_bytes) != body_crc) {
        set_error(error, error_size, "sidecar body CRC mismatch");
        return VP_RESOURCE_LAYOUT_ERR_BODY;
    }
    if (memcmp(bytes + 40, parent_sha256, 32) != 0) {
        set_error(error, error_size, "sidecar parent index SHA mismatch");
        return VP_RESOURCE_LAYOUT_ERR_PARENT;
    }
    if (rd32(bytes + 72) != parent_crc32) {
        set_error(error, error_size, "sidecar parent index CRC mismatch");
        return VP_RESOURCE_LAYOUT_ERR_PARENT;
    }
    if (rd32(bytes + 76) != 0u) {
        set_error(error, error_size, "sidecar reserved word not zero");
        return VP_RESOURCE_LAYOUT_ERR_RECORD;
    }
    for (i = 80u; i < VP_RESOURCE_LAYOUT_HEADER_SIZE; ++i) {
        if (bytes[i] != 0u) {
            set_error(error, error_size, "sidecar reserved bytes not zero");
            return VP_RESOURCE_LAYOUT_ERR_RECORD;
        }
    }
    if (stage_count != index->stage_count) {
        set_error(error, error_size, "sidecar stage count differs from index");
        return VP_RESOURCE_LAYOUT_ERR_INDEX;
    }

    memset(&layout, 0, sizeof(layout));
    layout.stages = (VPResourceStage *)calloc(stage_count, sizeof(*layout.stages));
    layout.textures = (VPResourceTexture *)calloc(
        texture_count ? texture_count : 1u, sizeof(*layout.textures));
    seen = (uint8_t *)calloc(stage_count, 1u);
    if (layout.stages == NULL || layout.textures == NULL || seen == NULL) {
        set_error(error, error_size, "out of memory");
        goto fail;
    }
    layout.stage_count = stage_count;
    layout.texture_count = texture_count;
    memcpy(layout.parent_sha256, parent_sha256, 32);
    layout.parent_crc32 = parent_crc32;

    for (i = 0; i < texture_count; ++i) {
        const uint8_t *record = body +
            (size_t)stage_count * VP_RESOURCE_LAYOUT_STAGE_RECORD +
            (size_t)i * VP_RESOURCE_LAYOUT_TEXTURE_RECORD;
        VPResourceTexture *texture = &layout.textures[i];
        texture->slot = record[0];
        texture->texture_type = record[1];
        texture->depth = record[2];
        texture->secondary = record[3];
        texture->offset = rd16(record + 4);
        texture->count = rd16(record + 6);
        texture->shift = record[8];
        texture->size_shift = record[9];
        texture->env_bank = record[10];
        texture->flags = record[11];
        if (rd32(record + 12) != 0u) {
            set_error(error, error_size, "texture record reserved word not zero");
            goto fail;
        }
    }

    for (i = 0; i < stage_count; ++i) {
        const uint8_t *record = body + (size_t)i *
                                          VP_RESOURCE_LAYOUT_STAGE_RECORD;
        VPResourceStage *stage = &layout.stages[i];
        stage->stage_type = record[0];
        stage->texture_count = record[2];
        stage->texture_start = rd16(record + 4);
        stage->stage_index = rd16(record + 6);
        memcpy(stage->sha256, record + 8, 32);
        if (record[1] != 0u || record[3] != 0u) {
            set_error(error, error_size, "stage record reserved bytes not zero");
            rc = VP_RESOURCE_LAYOUT_ERR_RECORD;
            goto fail;
        }
        if (stage->stage_type != VP_STATIC_INDEX_STAGE_VERTEX &&
            stage->stage_type != VP_STATIC_INDEX_STAGE_FRAGMENT) {
            set_error(error, error_size, "invalid stage type");
            rc = VP_RESOURCE_LAYOUT_ERR_RECORD;
            goto fail;
        }
        if (stage->texture_count > 16u) {
            set_error(error, error_size, "stage texture count out of range");
            rc = VP_RESOURCE_LAYOUT_ERR_RECORD;
            goto fail;
        }
        if (stage->texture_start != running) {
            set_error(error, error_size, "stage texture ranges not contiguous");
            rc = VP_RESOURCE_LAYOUT_ERR_RECORD;
            goto fail;
        }
        /* Bound the stage's range against the *actual* table, not the format
         * maximum, before validate_textures dereferences it. */
        if (stage->texture_start > texture_count ||
            stage->texture_count > texture_count - stage->texture_start) {
            set_error(error, error_size,
                      "stage texture range outside the actual table");
            rc = VP_RESOURCE_LAYOUT_ERR_RECORD;
            goto fail;
        }
        rc = validate_textures(layout.textures, texture_count,
                               stage->texture_start, stage->texture_count,
                               error, error_size);
        if (rc != VP_RESOURCE_LAYOUT_OK) goto fail;
        running += stage->texture_count;
        if (stage->stage_index >= index->stage_count) {
            set_error(error, error_size, "stage index out of range");
            rc = VP_RESOURCE_LAYOUT_ERR_RECORD;
            goto fail;
        }
        if (seen[stage->stage_index]) {
            set_error(error, error_size, "duplicate stage index");
            rc = VP_RESOURCE_LAYOUT_ERR_RECORD;
            goto fail;
        }
        seen[stage->stage_index] = 1u;
        if (index->stages[stage->stage_index].stage != stage->stage_type ||
            memcmp(index->stages[stage->stage_index].sha256,
                   stage->sha256, 32) != 0) {
            set_error(error, error_size,
                      "sidecar stage does not match the parent index");
            rc = VP_RESOURCE_LAYOUT_ERR_RECORD;
            goto fail;
        }
        if (i > 0u &&
            stage_compare(layout.stages[i - 1u].stage_type,
                          layout.stages[i - 1u].sha256,
                          stage->stage_type, stage->sha256) >= 0) {
            set_error(error, error_size, "stage table is not strictly sorted");
            rc = VP_RESOURCE_LAYOUT_ERR_RECORD;
            goto fail;
        }
    }
    if (running != texture_count) {
        set_error(error, error_size, "texture table is not fully owned");
        rc = VP_RESOURCE_LAYOUT_ERR_RECORD;
        goto fail;
    }
    for (i = 0; i < stage_count; ++i) {
        if (!seen[i]) {
            set_error(error, error_size, "index stage missing from sidecar");
            rc = VP_RESOURCE_LAYOUT_ERR_RECORD;
            goto fail;
        }
    }
    free(seen);
    *out = layout;
    return VP_RESOURCE_LAYOUT_OK;

fail:
    free(seen);
    vp_resource_layout_free(&layout);
    return rc;
}

int vp_resource_layout_read_file(const char *path,
                                 const unsigned char parent_sha256[32],
                                 uint32_t parent_crc32,
                                 const VPStaticShaderIndex *index,
                                 VPResourceLayout *out,
                                 char *error, size_t error_size) {
    FILE *file;
    long length;
    uint8_t *data;
    int rc;

    if (path == NULL || out == NULL) return VP_RESOURCE_LAYOUT_ERR_ARG;
    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, error_size, "sidecar file open failed");
        return VP_RESOURCE_LAYOUT_ERR_ARG;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        (unsigned long)length > VP_RESOURCE_LAYOUT_MAX_BYTES) {
        set_error(error, error_size, "sidecar file size out of range");
        fclose(file);
        return VP_RESOURCE_LAYOUT_ERR_SIZE;
    }
    rewind(file);
    data = (uint8_t *)malloc((size_t)length ? (size_t)length : 1u);
    if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length) {
        set_error(error, error_size, "sidecar file read failed");
        free(data);
        fclose(file);
        return VP_RESOURCE_LAYOUT_ERR_SIZE;
    }
    fclose(file);
    rc = vp_resource_layout_parse(data, (size_t)length, parent_sha256,
                                  parent_crc32, index, out, error, error_size);
    free(data);
    return rc;
}

void vp_resource_layout_free(VPResourceLayout *layout) {
    if (layout == NULL) return;
    free(layout->stages);
    free(layout->textures);
    memset(layout, 0, sizeof(*layout));
}

const VPResourceStage *vp_resource_layout_stage(const VPResourceLayout *layout,
                                                unsigned stage_type,
                                                const uint8_t sha256[32]) {
    uint32_t low = 0, high;
    if (layout == NULL || layout->stages == NULL || sha256 == NULL) return NULL;
    high = layout->stage_count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2u;
        const VPResourceStage *stage = &layout->stages[mid];
        int compare = stage_compare(stage_type, sha256, stage->stage_type,
                                    stage->sha256);
        if (compare == 0) return stage;
        if (compare < 0) high = mid;
        else low = mid + 1u;
    }
    return NULL;
}

int vp_resource_layout_texture(const VPResourceLayout *layout,
                               const VPResourceStage *stage,
                               unsigned slot, VPResourceTexture *out) {
    if (layout == NULL || stage == NULL || out == NULL) return -1;
    if (slot >= stage->texture_count) return -1;
    if ((uint32_t)stage->texture_start + slot >= layout->texture_count)
        return -1;
    *out = layout->textures[stage->texture_start + slot];
    return 0;
}

const char *vp_resource_layout_status_name(int status) {
    switch (status) {
    case VP_RESOURCE_LAYOUT_OK: return "ok";
    case VP_RESOURCE_LAYOUT_ERR_ARG: return "bad-argument";
    case VP_RESOURCE_LAYOUT_ERR_SIZE: return "size";
    case VP_RESOURCE_LAYOUT_ERR_MAGIC: return "magic";
    case VP_RESOURCE_LAYOUT_ERR_VERSION: return "version";
    case VP_RESOURCE_LAYOUT_ERR_BODY: return "body-crc";
    case VP_RESOURCE_LAYOUT_ERR_RECORD: return "record";
    case VP_RESOURCE_LAYOUT_ERR_DUPLICATE: return "duplicate";
    case VP_RESOURCE_LAYOUT_ERR_INDEX: return "index-association";
    case VP_RESOURCE_LAYOUT_ERR_PARENT: return "parent-index";
    case VP_RESOURCE_LAYOUT_ERR_MISSING: return "missing";
    case VP_RESOURCE_LAYOUT_ERR_UNSUPPORTED: return "unsupported";
    default: return "unknown";
    }
}

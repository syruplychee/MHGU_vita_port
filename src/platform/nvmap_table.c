#include "nvmap_table.h"

#include <string.h>

VPNvMapResult vp_nvmap_table_create(VPGuestNvMapHandle *table, uint32_t capacity,
                                    uint32_t *count, uint32_t *next_id,
                                    uint32_t size, uint32_t *id_out) {
    VPGuestNvMapHandle *handle;
    uint32_t id;

    if (table == NULL || count == NULL || next_id == NULL || id_out == NULL)
        return VP_NVMAP_INVALID_ARGUMENT;
    if (*count > capacity) return VP_NVMAP_INVALID_ARGUMENT;
    if (*count == capacity) return VP_NVMAP_FULL;
    /* The id after 0xffffffff would wrap to the reserved value 0 and start
     * aliasing retired handles. Report the exhausted resource instead of
     * reusing an id. */
    if (*next_id == UINT32_MAX) return VP_NVMAP_ID_EXHAUSTED;
    id = *next_id + 1u;
    handle = &table[*count];
    memset(handle, 0, sizeof(*handle));
    handle->id = id;
    handle->size = size;
    *next_id = id;
    ++*count;
    *id_out = id;
    return VP_NVMAP_OK;
}

VPGuestNvMapHandle *vp_nvmap_table_find(VPGuestNvMapHandle *table,
                                        uint32_t count, uint32_t id) {
    if (table == NULL) return NULL;
    for (uint32_t i = 0; i < count; ++i)
        if (table[i].id == id) return &table[i];
    return NULL;
}

VPNvMapResult vp_nvmap_table_retire(VPGuestNvMapHandle *table, uint32_t *count,
                                    uint32_t id, VPGuestNvMapHandle *retired) {
    uint32_t last;

    if (table == NULL || count == NULL) return VP_NVMAP_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < *count; ++i) {
        if (table[i].id != id) continue;
        if (retired != NULL) *retired = table[i];
        last = --*count;
        if (i != last) table[i] = table[last];
        memset(&table[last], 0, sizeof(table[last]));
        return VP_NVMAP_OK;
    }
    return VP_NVMAP_NOT_FOUND;
}

#include "texture_pool.h"
#include "texture_snapshot.h"

#include <string.h>

static int pool_key_equal(const VPTexturePoolKey *left,
                          const VPTexturePoolKey *right) {
    return left->va == right->va && left->width == right->width &&
           left->height == right->height && left->format == right->format &&
           left->header == right->header && left->pitch == right->pitch &&
           left->block_height == right->block_height &&
           left->swizzle == right->swizzle &&
           left->texture_type == right->texture_type &&
           left->mip_count == right->mip_count && left->upload == right->upload;
}

static size_t pool_map_bytes(size_t bytes) {
    if (bytes == 0u || bytes > (size_t)-1 - VP_TEXTURE_POOL_PAGE_MASK)
        return 0u;
    return (bytes + VP_TEXTURE_POOL_PAGE_MASK) & ~(size_t)VP_TEXTURE_POOL_PAGE_MASK;
}

static uint32_t pool_next_generation(uint32_t generation) {
    generation++;
    if (generation == 0u) generation++;
    return generation;
}

/* First-fit over the page map. Adjacent free pages form one run because the
 * scan tests consecutive zero entries, so a free automatically coalesces with
 * its neighbours without a separate free-list merge. */
static int pool_page_alloc(VPTexturePool *pool, size_t map_bytes,
                           size_t *offset_out) {
    unsigned need, start = 0u, run = 0u;
    size_t offset;

    if (pool == NULL || offset_out == NULL || map_bytes == 0u ||
        (map_bytes & VP_TEXTURE_POOL_PAGE_MASK) != 0u)
        return -1;
    need = (unsigned)(map_bytes / VP_TEXTURE_POOL_PAGE_BYTES);
    if (need == 0u || need > pool->pages) return -1;
    for (unsigned page = 0u; page < pool->pages; ++page) {
        if (pool->page_map[page] == 0u) {
            if (run == 0u) start = page;
            run++;
            if (run == need) {
                memset(pool->page_map + start, 1, need);
                offset = (size_t)start * VP_TEXTURE_POOL_PAGE_BYTES;
                pool->alloc_map_calls++;
                pool->used_bytes += map_bytes;
                if (pool->used_bytes > pool->peak_used_bytes)
                    pool->peak_used_bytes = pool->used_bytes;
                *offset_out = offset;
                return 0;
            }
        } else {
            run = 0u;
        }
    }
    return -1;
}

static void pool_page_free(VPTexturePool *pool, size_t offset,
                           size_t map_bytes) {
    unsigned first, pages;

    if (pool == NULL || map_bytes == 0u) return;
    if ((offset & VP_TEXTURE_POOL_PAGE_MASK) != 0u ||
        (map_bytes & VP_TEXTURE_POOL_PAGE_MASK) != 0u ||
        offset > pool->bytes || map_bytes > pool->bytes - offset)
        return;
    first = (unsigned)(offset / VP_TEXTURE_POOL_PAGE_BYTES);
    pages = (unsigned)(map_bytes / VP_TEXTURE_POOL_PAGE_BYTES);
    if (first >= pool->pages || pages > pool->pages - first) return;
    memset(pool->page_map + first, 0, pages);
    if (pool->used_bytes >= map_bytes) pool->used_bytes -= map_bytes;
    else pool->used_bytes = 0u;
    pool->free_map_calls++;
}

/* The decoder's retirement event must run before the old payload is freed or
 * overwritten. pool_slot_retire is the only place that returns a used slot. */
static void pool_slot_retire(VPTexturePool *pool, VPTexturePoolSlot *slot) {
    if (slot == NULL || !slot->used) return;
    if (slot->live && pool->retire != NULL && slot->rgba != NULL) {
        pool->retire(pool->retire_user, slot->rgba);
        pool->retire_calls++;
    }
    if (pool->live_slots > 0u && slot->live) pool->live_slots--;
    if (pool->gpu_refs >= slot->gpu_refs) pool->gpu_refs -= slot->gpu_refs;
    else pool->gpu_refs = 0u;
    if (pool->release) {
        if ((slot->rgba && pool->release(pool->memory_user, slot->rgba) < 0) ||
            (slot->raw && pool->release(pool->memory_user, slot->raw) < 0)) {
            pool->failed = 1;
            return;
        }
        pool->used_bytes -= slot->raw_map_bytes + slot->rgba_map_bytes;
        pool->free_map_calls += (slot->raw != NULL) + (slot->rgba != NULL);
    } else {
        if (slot->raw != NULL)
            pool_page_free(pool, slot->raw_offset, slot->raw_map_bytes);
        if (slot->rgba != NULL)
            pool_page_free(pool, slot->rgba_offset, slot->rgba_map_bytes);
    }
    slot->raw = NULL;
    slot->rgba = NULL;
    slot->raw_offset = slot->rgba_offset = 0u;
    slot->raw_bytes = slot->rgba_bytes = 0u;
    slot->raw_map_bytes = slot->rgba_map_bytes = 0u;
    slot->cpu_pins = 0u;
    slot->gpu_refs = 0u;
    slot->live = 0u;
    slot->used = 0u;
    slot->stamp = 0u;
    slot->generation = pool_next_generation(slot->generation);
}

static int pool_first_free_slot(const VPTexturePool *pool) {
    for (unsigned i = 0u; i < VP_TEXTURE_POOL_SLOTS; ++i)
        if (!pool->slots[i].used) return (int)i;
    return -1;
}

/* Mapped region pages can be reassigned without kernel work, so use true LRU
 * there: preserving a cold large texture just to reuse a hot small allocation
 * would permanently shrink the working set. The optional contiguous arena
 * keeps its size preference to reduce contiguous-run fragmentation. */
static int pool_choose_victim(VPTexturePool *pool, size_t raw_bytes,
                              size_t rgba_bytes) {
    VPTexturePoolSlot *fit = NULL, *old = NULL;

    for (unsigned i = 0u; i < VP_TEXTURE_POOL_SLOTS; ++i) {
        VPTexturePoolSlot *slot = &pool->slots[i];
        if (!slot->used || !slot->live || slot->cpu_pins != 0u ||
            slot->gpu_refs != 0u)
            continue;
        if (old == NULL || slot->stamp < old->stamp) old = slot;
        if (slot->raw_bytes == raw_bytes && slot->rgba_bytes == rgba_bytes &&
            (fit == NULL || slot->stamp < fit->stamp))
            fit = slot;
    }
    if (!pool->allocate && fit != NULL) return (int)(fit - pool->slots);
    return old != NULL ? (int)(old - pool->slots) : -1;
}

static int pool_pressure_possible(const VPTexturePool *pool) {
    for (unsigned i = 0u; i < VP_TEXTURE_POOL_SLOTS; ++i) {
        const VPTexturePoolSlot *slot = &pool->slots[i];
        /* A slot pinned by the current request cannot become evictable by
         * waiting for the GPU; only a pure GPU lease can. */
        if (slot->used && slot->live && slot->gpu_refs != 0u &&
            slot->cpu_pins == 0u)
            return 1;
    }
    return 0;
}

int vp_texture_pool_init(VPTexturePool *pool, void *base, size_t bytes) {
    if (pool == NULL || base == NULL ||
        ((uintptr_t)base & VP_TEXTURE_POOL_PAGE_MASK) != 0u)
        return -1;
    bytes &= ~(size_t)VP_TEXTURE_POOL_PAGE_MASK;
    if (bytes < VP_TEXTURE_POOL_PAGE_BYTES ||
        bytes > VP_TEXTURE_POOL_MAX_BYTES)
        return -1;
    memset(pool, 0, sizeof(*pool));
    pool->base = (uint8_t *)base;
    pool->bytes = bytes;
    pool->pages = (unsigned)(bytes / VP_TEXTURE_POOL_PAGE_BYTES);
    for (unsigned i = 0u; i < VP_TEXTURE_POOL_SLOTS; ++i)
        pool->slots[i].generation = 1u;
    return 0;
}

int vp_texture_pool_init_regions(VPTexturePool *pool, size_t bytes,
    unsigned raw_class, void *user,
    void *(*allocate)(void *, size_t, unsigned), int (*release)(void *, void *)) {
    if (!pool || !allocate || !release || !bytes ||
        bytes > VP_TEXTURE_POOL_MAX_BYTES) return -1;
    memset(pool, 0, sizeof(*pool));
    pool->bytes = bytes;
    pool->raw_class = raw_class;
    pool->allocate = allocate;
    pool->release = release;
    pool->memory_user = user;
    for (unsigned i = 0; i < VP_TEXTURE_POOL_SLOTS; ++i)
        pool->slots[i].generation = 1u;
    return 0;
}

void vp_texture_pool_set_retire(VPTexturePool *pool,
                                VPTexturePoolRetireFn retire, void *user) {
    if (pool == NULL) return;
    pool->retire = retire;
    pool->retire_user = user;
}

int vp_texture_pool_reset(VPTexturePool *pool) {
    if (pool == NULL) return -1;
    if (pool->gpu_refs != 0u) return -1;
    for (unsigned i = 0u; i < VP_TEXTURE_POOL_SLOTS; ++i) {
        if (pool->slots[i].used) pool_slot_retire(pool, &pool->slots[i]);
        if (pool->failed) return -1;
    }
    pool->live_slots = 0u;
    pool->used_bytes = 0u;
    pool->cpu_pins = 0u;
    pool->gpu_refs = 0u;
    return 0;
}

void vp_texture_pool_begin_request(VPTexturePool *pool) {
    if (pool == NULL) return;
    for (unsigned i = 0u; i < VP_TEXTURE_POOL_SLOTS; ++i)
        pool->slots[i].cpu_pins = 0u;
    pool->cpu_pins = 0u;
}

int vp_texture_pool_lookup(VPTexturePool *pool, const VPTexturePoolKey *key,
                           const uint8_t *source, size_t raw_bytes,
                           VPTexturePoolToken *token, uint8_t **rgba) {
    return vp_texture_pool_lookup_trace(pool, key, source, raw_bytes, token, rgba, NULL);
}

int vp_texture_pool_lookup_trace(VPTexturePool *pool, const VPTexturePoolKey *key,
                           const uint8_t *source, size_t raw_bytes,
                           VPTexturePoolToken *token, uint8_t **rgba,
                           VPTexturePoolLookupTrace *trace) {
    if (trace) memset(trace, 0, sizeof(*trace));
    if (pool == NULL || key == NULL || source == NULL || raw_bytes == 0u)
        return 0;
    for (unsigned i = 0u; i < VP_TEXTURE_POOL_SLOTS; ++i) {
        VPTexturePoolSlot *slot = &pool->slots[i];
        if (!slot->used || !slot->live) continue;
        if (!slot->raw) {
            if (trace && slot->raw_bytes == raw_bytes && pool_key_equal(&slot->key, key))
                trace->gpu_only_keys++;
            continue;
        }
        if (slot->raw_bytes != raw_bytes || !pool_key_equal(&slot->key, key))
            continue;
        if (trace) {
            trace->live_keys++;
            trace->comparisons++;
            trace->compare_bytes += raw_bytes;
        }
        if (!vp_texture_bytes_equal(slot->raw, source, raw_bytes)) {
            if (trace) trace->changed++;
            /* Only the decoded payload is read by the GPU. Once this exact
             * key has changed, an older GPU-only version needs no raw identity
             * snapshot. Keep its immutable pixels and token until completion,
             * but return the CPU-only pages immediately. */
            if (slot->gpu_refs && !slot->cpu_pins) {
                if (pool->release) {
                    if (pool->release(pool->memory_user, slot->raw) < 0) {
                        pool->failed = 1;
                        return 0;
                    }
                    pool->used_bytes -= slot->raw_map_bytes;
                    pool->free_map_calls++;
                } else {
                    pool_page_free(pool, slot->raw_offset, slot->raw_map_bytes);
                }
                pool->raw_reclaims++;
                pool->raw_reclaimed_bytes += slot->raw_map_bytes;
                slot->raw = NULL;
                slot->raw_offset = 0u;
                slot->raw_map_bytes = 0u;
            }
            continue;
        }
        slot->stamp = ++pool->stamp;
        slot->cpu_pins++;
        pool->cpu_pins++;
        if (token != NULL)
            *token = (VPTexturePoolToken){ (uint32_t)i, slot->generation };
        if (rgba != NULL) *rgba = slot->rgba;
        return 1;
    }
    return 0;
}

int vp_texture_pool_begin_store(VPTexturePool *pool,
                                const VPTexturePoolKey *key,
                                size_t raw_bytes, size_t rgba_bytes,
                                VPTexturePoolToken *token, uint8_t **raw,
                                uint8_t **rgba, int *reused) {
    size_t raw_map, rgba_map;

    if (pool == NULL || pool->failed || token == NULL || raw == NULL || rgba == NULL ||
        raw_bytes == 0u || rgba_bytes == 0u)
        return VP_TEXTURE_POOL_STORE_INVALID;
    raw_map = pool_map_bytes(raw_bytes);
    rgba_map = pool_map_bytes(rgba_bytes);
    if (raw_map == 0u || rgba_map == 0u ||
        raw_map > pool->bytes || rgba_map > pool->bytes - raw_map)
        return VP_TEXTURE_POOL_STORE_NOMEM;
    for (unsigned attempt = 0u; attempt <= VP_TEXTURE_POOL_SLOTS + 1u;
         ++attempt) {
        int index = pool_first_free_slot(pool);
        int victim = pool_choose_victim(pool, raw_bytes, rgba_bytes);
        int same_key = 0;
        for (unsigned i = 0; key && i < VP_TEXTURE_POOL_SLOTS; ++i) {
            VPTexturePoolSlot *slot = &pool->slots[i];
            if (slot->used && slot->live && !slot->cpu_pins && !slot->gpu_refs &&
                slot->raw_bytes == raw_bytes && slot->rgba_bytes == rgba_bytes &&
                pool_key_equal(&slot->key, key)) {
                victim = (int)i;
                same_key = 1;
                break;
            }
        }
        /* A same-size idle version already owns suitable regions. Reuse them
         * before any kernel allocation when the budget or slot table is full. */
        if (victim >= 0 && (same_key || index < 0 ||
            raw_map + rgba_map > pool->bytes - pool->used_bytes)) {
            VPTexturePoolSlot *slot = &pool->slots[victim];
            if (slot->raw_bytes == raw_bytes && slot->rgba_bytes == rgba_bytes) {
                if (pool->retire) pool->retire(pool->retire_user, slot->rgba);
                pool->retire_calls++;
                pool->live_slots--;
                pool->size_reuses++;
                pool->evictions++;
                slot->generation = pool_next_generation(slot->generation);
                slot->live = 0u;
                slot->stamp = 0u;
                *token = (VPTexturePoolToken){(uint32_t)victim, slot->generation};
                *raw = slot->raw; *rgba = slot->rgba;
                if (reused) *reused = 1;
                return VP_TEXTURE_POOL_STORE_OK;
            }
        }
        if (pool->allocate && index >= 0 &&
            raw_map + rgba_map <= pool->bytes - pool->used_bytes) {
            VPTexturePoolSlot *slot = &pool->slots[index];
            uint8_t *new_raw = pool->allocate(pool->memory_user, raw_map,
                                               pool->raw_class);
            uint8_t *new_rgba = new_raw ?
                pool->allocate(pool->memory_user, rgba_map, 0u) : NULL;
            if (new_rgba) {
                slot->used = 1u; slot->live = 0u;
                slot->raw = new_raw; slot->rgba = new_rgba;
                slot->raw_bytes = raw_bytes; slot->rgba_bytes = rgba_bytes;
                slot->raw_map_bytes = raw_map; slot->rgba_map_bytes = rgba_map;
                pool->used_bytes += raw_map + rgba_map;
                if (pool->used_bytes > pool->peak_used_bytes)
                    pool->peak_used_bytes = pool->used_bytes;
                pool->alloc_map_calls += 2u;
                *token = (VPTexturePoolToken){(uint32_t)index, slot->generation};
                *raw = new_raw; *rgba = new_rgba;
                if (reused) *reused = slot->generation > 1u;
                return VP_TEXTURE_POOL_STORE_OK;
            }
            if (new_raw && pool->release(pool->memory_user, new_raw) < 0)
                pool->failed = 1;
            if (pool->failed) return VP_TEXTURE_POOL_STORE_INVALID;
            /* Kernel address-space pressure can precede the byte budget.
             * Reuse an idle fitting pair instead of freeing that very pair
             * and immediately asking the kernel to allocate it again. */
            if (victim >= 0 &&
                pool->slots[victim].raw_bytes == raw_bytes &&
                pool->slots[victim].rgba_bytes == rgba_bytes) {
                VPTexturePoolSlot *slot = &pool->slots[victim];
                if (pool->retire) pool->retire(pool->retire_user, slot->rgba);
                pool->retire_calls++;
                pool->live_slots--;
                pool->size_reuses++;
                pool->evictions++;
                slot->generation = pool_next_generation(slot->generation);
                slot->live = 0u;
                slot->stamp = 0u;
                *token = (VPTexturePoolToken){(uint32_t)victim, slot->generation};
                *raw = slot->raw; *rgba = slot->rgba;
                if (reused) *reused = 1;
                return VP_TEXTURE_POOL_STORE_OK;
            }
            /* Allocation failed: discard an idle version before retrying.
             * All attempts are bounded by the live-slot count. */
        }
        if (!pool->allocate && index >= 0) {
            VPTexturePoolSlot *slot = &pool->slots[index];
            size_t raw_offset = 0u, rgba_offset = 0u;
            if (pool_page_alloc(pool, raw_map, &raw_offset) == 0) {
                if (pool_page_alloc(pool, rgba_map, &rgba_offset) == 0) {
                    slot->used = 1u;
                    slot->live = 0u;
                    slot->raw_offset = raw_offset;
                    slot->raw_bytes = raw_bytes;
                    slot->raw_map_bytes = raw_map;
                    slot->rgba_offset = rgba_offset;
                    slot->rgba_bytes = rgba_bytes;
                    slot->rgba_map_bytes = rgba_map;
                    slot->raw = pool->base + raw_offset;
                    slot->rgba = pool->base + rgba_offset;
                    slot->stamp = 0u;
                    slot->cpu_pins = 0u;
                    slot->gpu_refs = 0u;
                    *token = (VPTexturePoolToken){ (uint32_t)index,
                                                   slot->generation };
                    *raw = slot->raw;
                    *rgba = slot->rgba;
                    if (reused != NULL) *reused = slot->generation > 1u;
                    return VP_TEXTURE_POOL_STORE_OK;
                }
                pool_page_free(pool, raw_offset, raw_map);
            }
        }
        {
            int victim = pool_choose_victim(pool, raw_bytes, rgba_bytes);
            if (victim < 0) {
                if (pool_pressure_possible(pool)) {
                    pool->pressure_events++;
                    return VP_TEXTURE_POOL_STORE_PRESSURE;
                }
                pool->nomem_events++;
                return VP_TEXTURE_POOL_STORE_NOMEM;
            }
            /* Before destroying a differently sized region, let completed
             * scenes return their leases. Their fitting regions otherwise
             * remain invisible to the allocator and trigger kernel churn. */
            if ((pool->slots[victim].raw_bytes != raw_bytes ||
                 pool->slots[victim].rgba_bytes != rgba_bytes) &&
                !pool->idle_resize &&
                pool_pressure_possible(pool)) {
                pool->pressure_events++;
                return VP_TEXTURE_POOL_STORE_PRESSURE;
            }
            pool->evictions++;
            pool_slot_retire(pool, &pool->slots[victim]);
            if (pool->failed) return VP_TEXTURE_POOL_STORE_INVALID;
        }
    }
    pool->nomem_events++;
    return VP_TEXTURE_POOL_STORE_NOMEM;
}

int vp_texture_pool_commit_store(VPTexturePool *pool, VPTexturePoolToken token,
                                 const VPTexturePoolKey *key,
                                 const uint8_t *source, size_t raw_bytes) {
    VPTexturePoolSlot *slot;
    if (pool == NULL || key == NULL || source == NULL ||
        token.slot >= VP_TEXTURE_POOL_SLOTS)
        return -1;
    slot = &pool->slots[token.slot];
    if (!slot->used || slot->live || slot->generation != token.generation ||
        slot->raw_bytes != raw_bytes || slot->raw == NULL)
        return -1;
    memcpy(slot->raw, source, raw_bytes);
    slot->key = *key;
    slot->live = 1u;
    slot->stamp = ++pool->stamp;
    slot->cpu_pins = 1u;
    pool->cpu_pins++;
    pool->live_slots++;
    if (pool->live_slots > pool->peak_live_slots)
        pool->peak_live_slots = pool->live_slots;
    return 0;
}

void vp_texture_pool_abort_store(VPTexturePool *pool, VPTexturePoolToken token) {
    if (pool == NULL || token.slot >= VP_TEXTURE_POOL_SLOTS) return;
    {
        VPTexturePoolSlot *slot = &pool->slots[token.slot];
        if (!slot->used || slot->live ||
            slot->generation != token.generation)
            return;
        pool_slot_retire(pool, slot);
    }
}

int vp_texture_pool_owns(const VPTexturePool *pool, const void *rgba,
                         size_t bytes) {
    for (unsigned i = 0u; i < VP_TEXTURE_POOL_SLOTS; ++i) {
        const VPTexturePoolSlot *slot = &pool->slots[i];
        if (slot->used && slot->live && slot->cpu_pins &&
            slot->rgba == rgba && slot->rgba_bytes == bytes)
            return 1;
    }
    return 0;
}

int vp_texture_pool_retain_gpu(VPTexturePool *pool, const void *rgba,
                               VPTexturePoolToken *token) {
    if (pool == NULL || rgba == NULL || token == NULL) return 0;
    for (unsigned i = 0u; i < VP_TEXTURE_POOL_SLOTS; ++i) {
        VPTexturePoolSlot *slot = &pool->slots[i];
        if (!slot->used || !slot->live || slot->rgba != rgba) continue;
        slot->gpu_refs++;
        pool->gpu_refs++;
        *token = (VPTexturePoolToken){ (uint32_t)i, slot->generation };
        return 1;
    }
    return 0;
}

int vp_texture_pool_release_gpu(VPTexturePool *pool, VPTexturePoolToken token) {
    VPTexturePoolSlot *slot;
    if (pool == NULL || token.slot >= VP_TEXTURE_POOL_SLOTS) return -1;
    slot = &pool->slots[token.slot];
    if (!slot->used || !slot->live ||
        slot->generation != token.generation || slot->gpu_refs == 0u)
        return -1;
    slot->gpu_refs--;
    if (pool->gpu_refs > 0u) pool->gpu_refs--;
    /* A GPU-only version lost its raw snapshot at a proven content change.
     * Its final lease is now complete, so the remaining decoded pages return
     * without keeping an unmatchable cache entry alive. */
    if (!slot->gpu_refs && !slot->raw) pool_slot_retire(pool, slot);
    return 0;
}

unsigned vp_texture_pool_gpu_refs(const VPTexturePool *pool) {
    return pool != NULL ? pool->gpu_refs : 0u;
}

void vp_texture_pool_stats(const VPTexturePool *pool, VPTexturePoolStats *out) {
    unsigned runs = 0u, run = 0u, largest = 0u;
    unsigned cpu_pins = 0u, gpu_refs = 0u;

    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (pool == NULL) return;
    for (unsigned page = 0u; page < pool->pages; ++page) {
        if (pool->page_map[page] == 0u) {
            run++;
            if (run > largest) largest = run;
            if (page + 1u == pool->pages || pool->page_map[page + 1u] != 0u)
                runs++;
        } else {
            run = 0u;
        }
    }
    for (unsigned i = 0u; i < VP_TEXTURE_POOL_SLOTS; ++i) {
        cpu_pins += pool->slots[i].cpu_pins;
        gpu_refs += pool->slots[i].gpu_refs;
    }
    out->alloc_map_calls = pool->alloc_map_calls;
    out->free_map_calls = pool->free_map_calls;
    out->evictions = pool->evictions;
    out->size_reuses = pool->size_reuses;
    out->pressure_events = pool->pressure_events;
    out->nomem_events = pool->nomem_events;
    out->retire_calls = pool->retire_calls;
    out->raw_reclaims = pool->raw_reclaims;
    out->raw_reclaimed_bytes = pool->raw_reclaimed_bytes;
    out->live_slots = pool->live_slots;
    out->peak_live_slots = pool->peak_live_slots;
    out->budget_bytes = pool->bytes;
    out->used_bytes = pool->used_bytes;
    out->peak_used_bytes = pool->peak_used_bytes;
    out->raw_class = pool->raw_class;
    out->free_runs = runs;
    out->largest_free_run_bytes =
        (size_t)largest * VP_TEXTURE_POOL_PAGE_BYTES;
    out->gpu_refs = gpu_refs;
    out->cpu_pins = cpu_pins;
}

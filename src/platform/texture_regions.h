#ifndef VP_TEXTURE_REGIONS_H
#define VP_TEXTURE_REGIONS_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VP_TEXTURE_REGION_PAGE 4096u
#define VP_TEXTURE_REGION_BUDGET (64u * 1024u * 1024u)
#define VP_TEXTURE_REGION_CHUNK (4u * 1024u * 1024u)
#define VP_TEXTURE_REGION_CHUNKS 256u
#define VP_TEXTURE_REGION_ALLOCS 256u

/* A mapped chunk remains mapped while its suballocations change owners.
 * Texture CPU/GPU leases are enforced by texture_pool, above this allocator. */
typedef struct {
    uint8_t *base;
    size_t bytes;
    unsigned used_pages;
    uint32_t *pages;
} VPTextureRegionChunk;

typedef struct {
    void *base;
    unsigned chunk, first, pages;
} VPTextureRegionAllocation;

typedef struct {
    VPTextureRegionChunk chunks[VP_TEXTURE_REGION_CHUNKS];
    VPTextureRegionAllocation allocations[VP_TEXTURE_REGION_ALLOCS];
    void *user;
    void *(*create)(void *, size_t);
    int (*destroy)(void *, void *);
    size_t physical_bytes, peak_bytes, failed_block_bytes;
    uint64_t kernel_allocs, kernel_frees, suballocs, subfrees;
    unsigned live, failed;
} VPTextureRegions;

static inline void vp_texture_region_mark(VPTextureRegionChunk *chunk,
    unsigned first, unsigned count, int occupied) {
    while (count) {
        unsigned offset = first & 31u;
        unsigned bits = count < 32u - offset ? count : 32u - offset;
        uint32_t mask = bits == 32u ? UINT32_MAX :
            ((UINT32_C(1) << bits) - 1u) << offset;
        if (occupied) chunk->pages[first >> 5] |= mask;
        else chunk->pages[first >> 5] &= ~mask;
        first += bits;
        count -= bits;
    }
}

static inline void *vp_texture_region_take(VPTextureRegions *pool,
    unsigned chunk_index, unsigned slot, unsigned needed) {
    VPTextureRegionChunk *chunk = &pool->chunks[chunk_index];
    unsigned run = 0u;
    if (needed > chunk->bytes / VP_TEXTURE_REGION_PAGE - chunk->used_pages)
        return NULL;
    for (unsigned page = 0; page < chunk->bytes / VP_TEXTURE_REGION_PAGE; ++page) {
        if (!(page & 31u) && chunk->pages[page >> 5] == UINT32_MAX) {
            run = 0u;
            page += 31u;
            continue;
        }
        run = chunk->pages[page >> 5] & (UINT32_C(1) << (page & 31u)) ?
            0u : run + 1u;
        if (run != needed) continue;
        unsigned first = page + 1u - needed;
        void *base = chunk->base + (size_t)first * VP_TEXTURE_REGION_PAGE;
        vp_texture_region_mark(chunk, first, needed, 1);
        chunk->used_pages += needed;
        pool->allocations[slot] = (VPTextureRegionAllocation){base,
            chunk_index, first, needed};
        pool->live++;
        pool->suballocs++;
        return base;
    }
    return NULL;
}

static inline int vp_texture_region_drop_chunk(VPTextureRegions *pool,
    unsigned index) {
    VPTextureRegionChunk *chunk = &pool->chunks[index];
    if (!chunk->base || chunk->used_pages) return -1;
    if (pool->destroy(pool->user, chunk->base) < 0) {
        pool->failed = 1;
        return -1;
    }
    pool->physical_bytes -= chunk->bytes;
    pool->kernel_frees++;
    free(chunk->pages);
    memset(chunk, 0, sizeof(*chunk));
    return 0;
}

static inline void *vp_texture_regions_alloc(VPTextureRegions *pool,
    size_t bytes) {
    unsigned slot;
    if (pool->failed || !bytes || bytes > VP_TEXTURE_REGION_BUDGET ||
        (bytes & (VP_TEXTURE_REGION_PAGE - 1u))) return NULL;
    for (slot = 0; slot < VP_TEXTURE_REGION_ALLOCS; ++slot)
        if (!pool->allocations[slot].base) break;
    if (slot == VP_TEXTURE_REGION_ALLOCS) return NULL;
    size_t largest = 0u;
    /* Pack small allocations into the fallback chunks first. Otherwise they
     * would fragment the few physically contiguous large chunks. */
    for (unsigned pass = 0; pass < 2u; ++pass) {
        for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNKS; ++i) {
            VPTextureRegionChunk *chunk = &pool->chunks[i];
            if (!chunk->base) continue;
            if (chunk->bytes > largest) largest = chunk->bytes;
            if ((chunk->bytes < VP_TEXTURE_REGION_CHUNK) != (pass == 0u))
                continue;
            void *base = vp_texture_region_take(pool, i, slot,
                (unsigned)(bytes / VP_TEXTURE_REGION_PAGE));
            if (base) return base;
        }
    }
    size_t chunk_bytes = bytes < VP_TEXTURE_REGION_CHUNK ?
        VP_TEXTURE_REGION_CHUNK : bytes;
    if (pool->failed_block_bytes && largest >= bytes) {
        if (bytes >= pool->failed_block_bytes) return NULL;
        chunk_bytes = bytes < 256u * 1024u ? 256u * 1024u : bytes;
    }
    size_t available = VP_TEXTURE_REGION_BUDGET - pool->physical_bytes;
    if (chunk_bytes > available && bytes <= available) chunk_bytes = available;
    /* If existing chunks can serve this size, return pressure to the texture
     * cache and reuse them after eviction/completion. Freeing smaller chunks
     * cannot manufacture a physically contiguous large block. Only a newly
     * encountered larger texture justifies rebuilding empty chunks. */
    if (chunk_bytes > available && largest >= bytes) return NULL;
    for (unsigned i = 0; chunk_bytes > VP_TEXTURE_REGION_BUDGET -
         pool->physical_bytes && i < VP_TEXTURE_REGION_CHUNKS; ++i) {
        if (pool->chunks[i].base && !pool->chunks[i].used_pages &&
            vp_texture_region_drop_chunk(pool, i) < 0) return NULL;
    }
    if (chunk_bytes > VP_TEXTURE_REGION_BUDGET - pool->physical_bytes)
        return NULL;
    for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNKS; ++i) {
        VPTextureRegionChunk *chunk = &pool->chunks[i];
        if (chunk->base) continue;
        uint8_t *base = pool->create(pool->user, chunk_bytes);
        if (!base) {
            if (!pool->failed_block_bytes || chunk_bytes < pool->failed_block_bytes)
                pool->failed_block_bytes = chunk_bytes;
            if (largest < bytes) {
                for (unsigned j = 0; j < VP_TEXTURE_REGION_CHUNKS; ++j)
                    if (pool->chunks[j].base && !pool->chunks[j].used_pages &&
                        vp_texture_region_drop_chunk(pool, j) < 0) return NULL;
                base = pool->create(pool->user, chunk_bytes);
            }
            if (!base && bytes < chunk_bytes) {
                /* Earlier legacy allocations can leave smaller physical
                 * holes. Reuse those rather than treating a failed 4 MiB
                 * request as proof that the entire byte budget is exhausted. */
                chunk_bytes = bytes < 256u * 1024u ? 256u * 1024u : bytes;
                available = VP_TEXTURE_REGION_BUDGET - pool->physical_bytes;
                if (chunk_bytes > available) chunk_bytes = available;
                base = pool->create(pool->user, chunk_bytes);
            }
            if (!base) return NULL;
        }
        if (pool->failed_block_bytes && chunk_bytes >= pool->failed_block_bytes)
            pool->failed_block_bytes = 0u;
        unsigned page_count = (unsigned)(chunk_bytes / VP_TEXTURE_REGION_PAGE);
        uint32_t *bitmap = calloc((page_count + 31u) / 32u, sizeof(*bitmap));
        if (!bitmap) {
            if (pool->destroy(pool->user, base) < 0) pool->failed = 1;
            return NULL;
        }
        chunk->base = base;
        chunk->bytes = chunk_bytes;
        chunk->pages = bitmap;
        pool->physical_bytes += chunk_bytes;
        if (pool->physical_bytes > pool->peak_bytes)
            pool->peak_bytes = pool->physical_bytes;
        pool->kernel_allocs++;
        return vp_texture_region_take(pool, i, slot,
            (unsigned)(bytes / VP_TEXTURE_REGION_PAGE));
    }
    return NULL;
}

static inline int vp_texture_regions_free(VPTextureRegions *pool, void *base) {
    for (unsigned i = 0; i < VP_TEXTURE_REGION_ALLOCS; ++i) {
        VPTextureRegionAllocation *allocation = &pool->allocations[i];
        if (!base || allocation->base != base) continue;
        VPTextureRegionChunk *chunk = &pool->chunks[allocation->chunk];
        vp_texture_region_mark(chunk, allocation->first, allocation->pages, 0);
        chunk->used_pages -= allocation->pages;
        memset(allocation, 0, sizeof(*allocation));
        pool->live--;
        pool->subfrees++;
        return 0;
    }
    return -1;
}

static inline int vp_texture_regions_reset(VPTextureRegions *pool) {
    if (pool->live || pool->failed) return -1;
    for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNKS; ++i)
        if (pool->chunks[i].base && vp_texture_region_drop_chunk(pool, i) < 0)
            return -1;
    pool->failed_block_bytes = 0u;
    return 0;
}

#endif

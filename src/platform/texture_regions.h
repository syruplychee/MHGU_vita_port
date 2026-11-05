#ifndef VP_TEXTURE_REGIONS_H
#define VP_TEXTURE_REGIONS_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VP_TEXTURE_REGION_PAGE 4096u
/* The default call site explicitly selects the 64 MiB value. The maximum is
 * only selected by the deep-large experiment; it is not a default and not a
 * statement about real hardware capacity. */
#define VP_TEXTURE_REGION_BUDGET (64u * 1024u * 1024u)
#define VP_TEXTURE_REGION_MAX_BUDGET (128u * 1024u * 1024u)
#define VP_TEXTURE_REGION_CHUNK (4u * 1024u * 1024u)
#define VP_TEXTURE_REGION_CHUNKS 256u
#define VP_TEXTURE_REGION_ALLOCS 256u

/* Chunks are dedicated to one allocation class for their whole lifetime.
 * Decoded GPU payloads use class 0 (uncached and mapped for GPU read); raw
 * CPU-only snapshots use class 1 in split mode (cached and never mapped).
 * An empty chunk may be dropped and recreated for another class; a chunk with
 * any live suballocation may not. */
#define VP_TEXTURE_REGION_CLASS_MAPPED 0u
#define VP_TEXTURE_REGION_CLASS_CPU_CACHED 1u
#define VP_TEXTURE_REGION_CLASSES 2u

/* A chunk remains allocated while its suballocations change owners.
 * Texture CPU/GPU leases are enforced by texture_pool, above this allocator. */
typedef struct {
    uint8_t *base;
    size_t bytes;
    unsigned used_pages;
    unsigned allocation_class;
    uint32_t *pages;
} VPTextureRegionChunk;

typedef struct {
    void *base;
    unsigned chunk, first, pages, allocation_class;
} VPTextureRegionAllocation;

typedef struct {
    VPTextureRegionChunk chunks[VP_TEXTURE_REGION_CHUNKS];
    VPTextureRegionAllocation allocations[VP_TEXTURE_REGION_ALLOCS];
    void *user;
    void *(*create)(void *, size_t, unsigned allocation_class);
    int (*destroy)(void *, void *);
    /* One shared physical budget for every class. There are never two pools
     * whose separate limits could be mistaken for the configured total. */
    size_t budget;
    size_t physical_bytes, peak_bytes;
    size_t class_physical[VP_TEXTURE_REGION_CLASSES];
    size_t class_allocated[VP_TEXTURE_REGION_CLASSES];
    /* A failed block size is remembered per class so a failure to grow the
     * cached raw class cannot suppress retries of the mapped decoded class. */
    size_t failed_block_bytes[VP_TEXTURE_REGION_CLASSES];
    uint64_t kernel_allocs, kernel_frees, suballocs, subfrees;
    unsigned live, failed;
} VPTextureRegions;

/* Reconfigure an already zero-initialized or fully reset allocator. Cumulative
 * alloc/free counters survive a mode switch; the physical high-water mark is
 * restarted so each experiment mode has its own peak. Failed block sizes are
 * retried under the new budget. A sticky destroy failure is never cleared. */
static inline int vp_texture_regions_configure(VPTextureRegions *pool,
    size_t budget, void *user,
    void *(*create)(void *, size_t, unsigned allocation_class),
    int (*destroy)(void *, void *)) {
    if (!pool || !create || !destroy || !budget ||
        budget > VP_TEXTURE_REGION_MAX_BUDGET ||
        (budget & (VP_TEXTURE_REGION_PAGE - 1u))) return -1;
    if (pool->failed || pool->live || pool->physical_bytes) return -1;
    pool->user = user;
    pool->create = create;
    pool->destroy = destroy;
    pool->budget = budget;
    pool->peak_bytes = 0u;
    memset(pool->failed_block_bytes, 0, sizeof(pool->failed_block_bytes));
    return 0;
}

/* Fresh allocator initialization. The caller must not have live chunks. */
static inline int vp_texture_regions_init(VPTextureRegions *pool,
    size_t budget, void *user,
    void *(*create)(void *, size_t, unsigned allocation_class),
    int (*destroy)(void *, void *)) {
    if (!pool) return -1;
    memset(pool, 0, sizeof(*pool));
    return vp_texture_regions_configure(pool, budget, user, create, destroy);
}

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
            chunk_index, first, needed, chunk->allocation_class};
        pool->class_allocated[chunk->allocation_class] +=
            (size_t)needed * VP_TEXTURE_REGION_PAGE;
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
    pool->class_physical[chunk->allocation_class] -= chunk->bytes;
    pool->kernel_frees++;
    free(chunk->pages);
    memset(chunk, 0, sizeof(*chunk));
    return 0;
}

static inline void *vp_texture_regions_alloc(VPTextureRegions *pool,
    size_t bytes, unsigned allocation_class) {
    unsigned slot;
    if (!pool || pool->failed || !pool->budget || !bytes ||
        bytes > pool->budget || allocation_class >= VP_TEXTURE_REGION_CLASSES ||
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
            if (!chunk->base ||
                chunk->allocation_class != allocation_class) continue;
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
    if (pool->failed_block_bytes[allocation_class] && largest >= bytes) {
        if (bytes >= pool->failed_block_bytes[allocation_class]) return NULL;
        chunk_bytes = bytes < 256u * 1024u ? 256u * 1024u : bytes;
    }
    size_t available = pool->budget - pool->physical_bytes;
    if (chunk_bytes > available && bytes <= available) chunk_bytes = available;
    /* If existing chunks of this class can serve the size, return pressure to
     * the texture cache and reuse them after eviction/completion. Freeing
     * smaller chunks cannot manufacture a physically contiguous large block.
     * Only a newly encountered larger texture justifies rebuilding empty
     * chunks. Empty chunks from the other class are still droppable below. */
    if (chunk_bytes > available && largest >= bytes) return NULL;
    for (unsigned i = 0; chunk_bytes > pool->budget -
         pool->physical_bytes && i < VP_TEXTURE_REGION_CHUNKS; ++i) {
        if (pool->chunks[i].base && !pool->chunks[i].used_pages &&
            vp_texture_region_drop_chunk(pool, i) < 0) return NULL;
    }
    if (chunk_bytes > pool->budget - pool->physical_bytes)
        return NULL;
    for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNKS; ++i) {
        VPTextureRegionChunk *chunk = &pool->chunks[i];
        if (chunk->base) continue;
        uint8_t *base = pool->create(pool->user, chunk_bytes, allocation_class);
        if (!base) {
            if (!pool->failed_block_bytes[allocation_class] ||
                chunk_bytes < pool->failed_block_bytes[allocation_class])
                pool->failed_block_bytes[allocation_class] = chunk_bytes;
            if (largest < bytes) {
                for (unsigned j = 0; j < VP_TEXTURE_REGION_CHUNKS; ++j)
                    if (pool->chunks[j].base && !pool->chunks[j].used_pages &&
                        vp_texture_region_drop_chunk(pool, j) < 0) return NULL;
                base = pool->create(pool->user, chunk_bytes, allocation_class);
            }
            if (!base && bytes < chunk_bytes) {
                /* Earlier legacy allocations can leave smaller physical
                 * holes. Reuse those rather than treating a failed 4 MiB
                 * request as proof that the entire byte budget is exhausted. */
                chunk_bytes = bytes < 256u * 1024u ? 256u * 1024u : bytes;
                available = pool->budget - pool->physical_bytes;
                if (chunk_bytes > available) chunk_bytes = available;
                base = pool->create(pool->user, chunk_bytes, allocation_class);
            }
            if (!base) return NULL;
        }
        if (pool->failed_block_bytes[allocation_class] &&
            chunk_bytes >= pool->failed_block_bytes[allocation_class])
            pool->failed_block_bytes[allocation_class] = 0u;
        unsigned page_count = (unsigned)(chunk_bytes / VP_TEXTURE_REGION_PAGE);
        uint32_t *bitmap = calloc((page_count + 31u) / 32u, sizeof(*bitmap));
        if (!bitmap) {
            if (pool->destroy(pool->user, base) < 0) pool->failed = 1;
            return NULL;
        }
        chunk->base = base;
        chunk->bytes = chunk_bytes;
        chunk->allocation_class = allocation_class;
        chunk->pages = bitmap;
        pool->physical_bytes += chunk_bytes;
        pool->class_physical[allocation_class] += chunk_bytes;
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
        pool->class_allocated[allocation->allocation_class] -=
            (size_t)allocation->pages * VP_TEXTURE_REGION_PAGE;
        memset(allocation, 0, sizeof(*allocation));
        pool->live--;
        pool->subfrees++;
        return 0;
    }
    return -1;
}

static inline int vp_texture_regions_reset(VPTextureRegions *pool) {
    if (!pool || pool->live || pool->failed) return -1;
    for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNKS; ++i)
        if (pool->chunks[i].base && vp_texture_region_drop_chunk(pool, i) < 0)
            return -1;
    memset(pool->failed_block_bytes, 0, sizeof(pool->failed_block_bytes));
    return 0;
}

#endif

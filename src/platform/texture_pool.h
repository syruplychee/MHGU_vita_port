/* Bounded unified texture pool.
 *
 * The caller supplies either an arena or region allocation callbacks. Both
 * account for exact raw copies and decoded payloads within one byte budget.
 * Arena allocations use a 4 KiB first-fit page map with adjacent free pages
 * naturally coalescing; region allocations reuse idle matching buffers.
 *
 * The module owns the 128-slot identity table, the exact raw-byte comparison,
 * CPU request pins, GPU reference pins, per-slot generations and the eviction
 * order. It never decodes, never guesses content from an address and never
 * calls platform APIs directly. Memory for a candidate is only reused after the
 * retire callback has dropped every GXM association to the old payload.
 *
 * This header is deliberately free of VitaSDK types so the host tests can
 * compile the production allocator. */
#ifndef VP_TEXTURE_POOL_H
#define VP_TEXTURE_POOL_H

#include <stddef.h>
#include <stdint.h>

#define VP_TEXTURE_POOL_SLOTS 128u
#define VP_TEXTURE_POOL_PAGE_BYTES 4096u
#define VP_TEXTURE_POOL_PAGE_MASK (VP_TEXTURE_POOL_PAGE_BYTES - 1u)
/* The default mode explicitly initializes the pool with 64 MiB. The larger
 * maximum exists only for the deep-large experiment; slot count does not grow
 * with it. */
#define VP_TEXTURE_POOL_DEFAULT_BYTES (64u * 1024u * 1024u)
#define VP_TEXTURE_POOL_MAX_BYTES (128u * 1024u * 1024u)
#define VP_TEXTURE_POOL_MAX_PAGES \
    (VP_TEXTURE_POOL_MAX_BYTES / VP_TEXTURE_POOL_PAGE_BYTES)

/* The complete cache identity. raw_bytes is carried separately because it is
 * also the length of the exact comparison. */
typedef struct {
    uint64_t va;
    unsigned width, height, format, header, pitch;
    unsigned block_height, swizzle, texture_type, mip_count, upload;
} VPTexturePoolKey;

/* A live-slot lease. The generation makes a stale release after slot reuse
 * observably invalid instead of decrementing the new occupant. */
typedef struct {
    uint32_t slot;
    uint32_t generation;
} VPTexturePoolToken;

/* Optional read-only lookup observations. compare_bytes is the requested
 * comparison length, not the number of bytes read before an early mismatch. */
typedef struct {
    uint64_t live_keys, gpu_only_keys, comparisons, compare_bytes, changed;
} VPTexturePoolLookupTrace;

enum {
    VP_TEXTURE_POOL_STORE_OK = 1,
    /* No evictable slot exists, but at least one GPU-referenced slot could
     * become evictable after a real GPU drain. */
    VP_TEXTURE_POOL_STORE_PRESSURE = 0,
    /* Even after all legal evictions the request cannot fit, or every
     * blocking slot is held by the current CPU request. Waiting cannot help. */
    VP_TEXTURE_POOL_STORE_NOMEM = -1,
    VP_TEXTURE_POOL_STORE_INVALID = -2
};

typedef void (*VPTexturePoolRetireFn)(void *user, const void *rgba);

typedef struct {
    unsigned used;   /* descriptor owns ranges or is live */
    unsigned live;   /* decoded payload valid; raw may be retired for GPU-only versions */
    VPTexturePoolKey key;
    size_t raw_offset, raw_bytes, raw_map_bytes;
    size_t rgba_offset, rgba_bytes, rgba_map_bytes;
    uint8_t *raw, *rgba;
    unsigned stamp;
    unsigned cpu_pins;
    unsigned gpu_refs;
    uint32_t generation;
} VPTexturePoolSlot;

typedef struct {
    uint64_t alloc_map_calls;     /* page-range allocations */
    uint64_t free_map_calls;      /* page-range frees */
    uint64_t evictions;
    uint64_t size_reuses;         /* victim had the requested raw/rgba sizes */
    uint64_t pressure_events;     /* begin_store returned PRESSURE */
    uint64_t nomem_events;
    uint64_t retire_calls;
    uint64_t raw_reclaims, raw_reclaimed_bytes;
    unsigned live_slots;
    unsigned peak_live_slots;
    size_t budget_bytes;
    size_t used_bytes;
    size_t peak_used_bytes;
    unsigned raw_class;
    unsigned free_runs;
    size_t largest_free_run_bytes;
    unsigned gpu_refs;
    unsigned cpu_pins;
} VPTexturePoolStats;

typedef struct {
    uint8_t *base;
    void *memory_user;
    /* The class argument is the allocator's raw/decoded separation. Decoded
     * RGBA always requests class 0; raw snapshots request raw_class (0 for the
     * unified experiment, 1 for split cached CPU-only payloads). */
    void *(*allocate)(void *user, size_t bytes, unsigned allocation_class);
    int (*release)(void *user, void *base);
    unsigned raw_class;
    int failed;
    /* Page-backed providers can resize idle regions without a kernel remap.
     * This never permits eviction of CPU pins or GPU references. */
    unsigned idle_resize;
    size_t bytes;
    unsigned pages;
    uint8_t page_map[VP_TEXTURE_POOL_MAX_PAGES];
    VPTexturePoolSlot slots[VP_TEXTURE_POOL_SLOTS];
    unsigned stamp;
    VPTexturePoolRetireFn retire;
    void *retire_user;
    uint64_t alloc_map_calls, free_map_calls;
    uint64_t evictions, size_reuses, pressure_events, nomem_events;
    uint64_t retire_calls;
    uint64_t raw_reclaims, raw_reclaimed_bytes;
    unsigned live_slots, peak_live_slots;
    size_t used_bytes, peak_used_bytes;
    unsigned gpu_refs, cpu_pins;
} VPTexturePool;

/* base must be non-NULL and page aligned; bytes is rounded down to pages and
 * must not exceed VP_TEXTURE_POOL_MAX_BYTES. */
int vp_texture_pool_init(VPTexturePool *pool, void *base, size_t bytes);
/* Bounded reusable regions: no single contiguous allocation is required.
 * raw and RGBA regions count against the same capacity, including alignment.
 * raw_class selects the region class for raw snapshots; RGBA always uses 0. */
int vp_texture_pool_init_regions(VPTexturePool *pool, size_t bytes,
    unsigned raw_class, void *user,
    void *(*allocate)(void *, size_t, unsigned), int (*release)(void *, void *));
void vp_texture_pool_set_retire(VPTexturePool *pool,
                                VPTexturePoolRetireFn retire, void *user);
/* Drop every payload association and free all ranges. Returns -1 while any
 * GPU lease is still outstanding; nothing is released in that case. */
int vp_texture_pool_reset(VPTexturePool *pool);
/* Clear the current request's CPU pins. Call once at each draw boundary, after
 * the previous draw's GPU leases were bound. */
void vp_texture_pool_begin_request(VPTexturePool *pool);

/* Exact lookup: full key and full raw bytes. A hit pins the slot for the
 * current CPU request. Returns 1 on hit, 0 on miss. */
int vp_texture_pool_lookup(VPTexturePool *pool, const VPTexturePoolKey *key,
                           const uint8_t *source, size_t raw_bytes,
                           VPTexturePoolToken *token, uint8_t **rgba);
int vp_texture_pool_lookup_trace(VPTexturePool *pool, const VPTexturePoolKey *key,
                           const uint8_t *source, size_t raw_bytes,
                           VPTexturePoolToken *token, uint8_t **rgba,
                           VPTexturePoolLookupTrace *trace);

/* Reserve payload ranges and a slot for new content. The slot is not visible
 * to lookups until commit. On VP_TEXTURE_POOL_STORE_PRESSURE the caller may
 * flush and wait for real GPU completion, then retry once.
 * *reused reports that the chosen slot descriptor previously held content. */
int vp_texture_pool_begin_store(VPTexturePool *pool,
                                const VPTexturePoolKey *key,
                                size_t raw_bytes, size_t rgba_bytes,
                                VPTexturePoolToken *token, uint8_t **raw,
                                uint8_t **rgba, int *reused);
/* Copy the exact raw source, publish the key and pin the slot for this
 * request. */
int vp_texture_pool_commit_store(VPTexturePool *pool, VPTexturePoolToken token,
                                 const VPTexturePoolKey *key,
                                 const uint8_t *source, size_t raw_bytes);
/* Decode/allocation failure: return the reserved ranges to the pool. */
void vp_texture_pool_abort_store(VPTexturePool *pool, VPTexturePoolToken token);

/* GPU lifetime leases are recognized only through a live slot whose payload
 * pointer matches exactly. A pointer range is never interpreted as identity. */
/* Exact payload length and current-request CPU ownership, without retaining. */
int vp_texture_pool_owns(const VPTexturePool *pool, const void *rgba,
                         size_t bytes);
int vp_texture_pool_retain_gpu(VPTexturePool *pool, const void *rgba,
                               VPTexturePoolToken *token);
int vp_texture_pool_release_gpu(VPTexturePool *pool, VPTexturePoolToken token);
unsigned vp_texture_pool_gpu_refs(const VPTexturePool *pool);

void vp_texture_pool_stats(const VPTexturePool *pool, VPTexturePoolStats *out);

#endif

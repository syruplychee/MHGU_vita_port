/* Host regression for the bounded unified texture pool.
 *
 * This test compiles the production allocator and slot table from
 * src/platform/texture_pool.c directly; only the backing memory is a host
 * allocation so the real first-fit/coalesce, 128-slot, pin, generation and
 * teardown semantics are exercised. */
#include "texture_pool.h"
#include "texture_regions.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

#define PAGE VP_TEXTURE_POOL_PAGE_BYTES

static void *test_page_alloc(size_t bytes) {
#if defined(_MSC_VER)
    return _aligned_malloc(bytes, PAGE);
#else
    void *pointer = NULL;
    if (posix_memalign(&pointer, PAGE, bytes) != 0) return NULL;
    return pointer;
#endif
}

static void test_page_free(void *pointer) {
#if defined(_MSC_VER)
    _aligned_free(pointer);
#else
    free(pointer);
#endif
}

static unsigned retire_calls;
static const void *last_retired;
static void retire_cb(void *user, const void *rgba) {
    (void)user;
    retire_calls++;
    last_retired = rgba;
}

static VPTexturePoolKey key_for(uint64_t va) {
    VPTexturePoolKey key;
    memset(&key, 0, sizeof(key));
    key.va = va;
    key.width = 256u;
    key.height = 256u;
    key.format = 8u;
    key.header = 2u;
    key.pitch = 256u * 4u;
    key.block_height = 0u;
    key.swizzle = 0xb1au;
    key.texture_type = 1u;
    key.mip_count = 1u;
    key.upload = 0u;
    return key;
}

typedef struct {
    VPTexturePool pool;
    void *memory;
} Fixture;

static void fixture_init(Fixture *fixture, size_t bytes) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->memory = test_page_alloc(bytes);
    assert(fixture->memory != NULL);
    assert(vp_texture_pool_init(&fixture->pool, fixture->memory, bytes) == 0);
    vp_texture_pool_set_retire(&fixture->pool, retire_cb, NULL);
    retire_calls = 0u;
    last_retired = NULL;
}

static void fixture_done(Fixture *fixture) {
    assert(vp_texture_pool_gpu_refs(&fixture->pool) == 0u);
    assert(vp_texture_pool_reset(&fixture->pool) == 0);
    test_page_free(fixture->memory);
}

static VPTexturePoolToken store(Fixture *fixture, uint64_t va,
                                const uint8_t *source, size_t raw_bytes,
                                size_t rgba_bytes, int *status) {
    VPTexturePoolToken token = {0u, 0u};
    uint8_t *raw = NULL, *rgba = NULL;
    int reused = 0;
    VPTexturePoolKey store_key = key_for(va);
    int rc = vp_texture_pool_begin_store(&fixture->pool, &store_key,
                                         raw_bytes, rgba_bytes, &token, &raw,
                                         &rgba, &reused);
    (void)reused;
    if (status != NULL) *status = rc;
    assert(rc == VP_TEXTURE_POOL_STORE_OK);
    memset(raw, 0x5a, raw_bytes);
    {
        VPTexturePoolKey key = key_for(va);
        assert(vp_texture_pool_commit_store(&fixture->pool, token, &key,
                                            source, raw_bytes) == 0);
    }
    return token;
}

static void test_init_validation(void) {
    VPTexturePool pool;
    uint8_t *memory = (uint8_t *)test_page_alloc(PAGE * 4u);
    assert(memory != NULL);
    assert(vp_texture_pool_init(&pool, memory + 1u, PAGE * 4u) < 0);
    assert(vp_texture_pool_init(&pool, memory, 0u) < 0);
    assert(vp_texture_pool_init(&pool, memory,
                                VP_TEXTURE_POOL_MAX_BYTES + PAGE) < 0);
    assert(vp_texture_pool_init(&pool, memory, PAGE * 4u - 1u) == 0);
    assert(pool.pages == 3u);
    test_page_free(memory);
    puts("PASS pool rejects unaligned/oversize/empty backing");
}

static void test_exact_identity(void) {
    Fixture fixture;
    uint8_t source[100];
    VPTexturePoolKey key = key_for(0x1000u);
    VPTexturePoolToken token = {0u, 0u}, hit_token = {0u, 0u};
    uint8_t *hit_rgba = NULL;
    VPTexturePoolStats stats;
    unsigned allocations;

    memset(source, 0x11, sizeof(source));
    fixture_init(&fixture, PAGE * 16u);
    token = store(&fixture, 0x1000u, source, sizeof(source), 200u, NULL);
    assert(token.slot == 0u && token.generation == 1u);
    vp_texture_pool_begin_request(&fixture.pool);
    assert(vp_texture_pool_lookup(&fixture.pool, &key, source, sizeof(source),
                                  &hit_token, &hit_rgba) == 1);
    assert(hit_token.slot == token.slot);
    assert(hit_token.generation == token.generation);
    assert(hit_rgba != NULL);
    assert(vp_texture_pool_owns(&fixture.pool, hit_rgba, 200u));
    assert(!vp_texture_pool_owns(&fixture.pool, hit_rgba, 201u));
    assert(!vp_texture_pool_owns(&fixture.pool, hit_rgba + 1u, 199u));
    vp_texture_pool_stats(&fixture.pool, &stats);
    assert(stats.live_slots == 1u);
    assert(stats.alloc_map_calls == 2u);
    assert(stats.used_bytes == 2u * PAGE);
    assert(stats.free_runs == 1u);
    assert(stats.largest_free_run_bytes == 14u * PAGE);

    /* Same key, different byte: exact comparison must miss. */
    source[17] ^= 1u;
    vp_texture_pool_begin_request(&fixture.pool);
    assert(vp_texture_pool_lookup(&fixture.pool, &key, source, sizeof(source),
                                  &hit_token, &hit_rgba) == 0);
    /* Different key with the same bytes must also miss. */
    key = key_for(0x2000u);
    source[17] ^= 1u;
    assert(vp_texture_pool_lookup(&fixture.pool, &key, source, sizeof(source),
                                  &hit_token, &hit_rgba) == 0);
    allocations = (unsigned)fixture.pool.alloc_map_calls;

    /* Unchanged content and key hit without allocating or retiring. */
    key = key_for(0x1000u);
    vp_texture_pool_begin_request(&fixture.pool);
    assert(vp_texture_pool_lookup(&fixture.pool, &key, source, sizeof(source),
                                  &hit_token, &hit_rgba) == 1);
    assert(fixture.pool.alloc_map_calls == allocations);
    assert(retire_calls == 0u);
    fixture_done(&fixture);
    puts("PASS pool lookup uses the complete key and every raw byte");
}

static void test_changed_content_reuse(void) {
    Fixture fixture;
    uint8_t first[64], second[64];
    VPTexturePoolKey key = key_for(0x3000u);
    VPTexturePoolToken token = {0u, 0u}, hit = {0u, 0u};
    uint8_t *rgba = NULL;
    int status = 0;

    memset(first, 0x22, sizeof(first));
    memset(second, 0x33, sizeof(second));
    /* One slot uses two pages; a two-page pool leaves no free descriptor, so
     * the changed content must reuse and retire the existing slot. */
    fixture_init(&fixture, PAGE * 2u);
    token = store(&fixture, 0x3000u, first, sizeof(first), 128u, NULL);
    vp_texture_pool_begin_request(&fixture.pool);
    assert(vp_texture_pool_lookup(&fixture.pool, &key, first, sizeof(first),
                                  &hit, &rgba) == 1);
    /* Same key with new bytes and no GPU lease: the old payload is retired and
     * the slot is overwritten in the same generation sequence. */
    vp_texture_pool_begin_request(&fixture.pool);
    {
        VPTexturePoolToken changed = store(&fixture, 0x3000u, second,
                                           sizeof(second), 128u, &status);
        (void)changed;
    }
    assert(retire_calls == 1u);
    assert(fixture.pool.evictions == 1u);
    assert(fixture.pool.live_slots == 1u);
    vp_texture_pool_begin_request(&fixture.pool);
    assert(vp_texture_pool_lookup(&fixture.pool, &key, first, sizeof(first),
                                  &hit, &rgba) == 0);
    assert(vp_texture_pool_lookup(&fixture.pool, &key, second, sizeof(second),
                                  &hit, &rgba) == 1);
    assert(hit.slot != token.slot || hit.generation != token.generation);
    fixture_done(&fixture);
    puts("PASS changed same-key content retires and replaces the payload");
}

static void test_pins_and_pressure(void) {
    Fixture fixture;
    uint8_t source[16];
    VPTexturePoolKey key = key_for(0x4000u);
    VPTexturePoolToken token = {0u, 0u}, pin = {0u, 0u}, probe = {0u, 0u};
    uint8_t *raw = NULL, *rgba = NULL, *pinned_rgba = NULL;
    int reused = 0, rc;

    memset(source, 0x44, sizeof(source));
    fixture_init(&fixture, PAGE * 8u);
    token = store(&fixture, 0x4000u, source, sizeof(source), 64u, NULL);
    vp_texture_pool_begin_request(&fixture.pool);
    assert(vp_texture_pool_lookup(&fixture.pool, &key, source, sizeof(source),
                                  &pin, &pinned_rgba) == 1);

    /* Six free pages remain. A 7-page request needs the pinned slot. */
    rc = vp_texture_pool_begin_store(&fixture.pool, &key, PAGE * 4u, PAGE * 3u,
                                     &probe, &raw, &rgba, &reused);
    assert(rc == VP_TEXTURE_POOL_STORE_NOMEM);
    assert(fixture.pool.pressure_events == 0u);

    /* A GPU lease on the same slot is releasable by waiting. */
    assert(vp_texture_pool_retain_gpu(&fixture.pool, pinned_rgba, &token) == 1);
    vp_texture_pool_begin_request(&fixture.pool);
    rc = vp_texture_pool_begin_store(&fixture.pool, &key, PAGE * 4u, PAGE * 3u,
                                     &probe, &raw, &rgba, &reused);
    assert(rc == VP_TEXTURE_POOL_STORE_PRESSURE);
    assert(fixture.pool.pressure_events == 1u);
    assert(vp_texture_pool_release_gpu(&fixture.pool, token) == 0);
    /* After the drain the lease is gone; the slot is evictable again. */
    rc = vp_texture_pool_begin_store(&fixture.pool, &key, PAGE * 4u, PAGE * 3u,
                                     &probe, &raw, &rgba, &reused);
    assert(rc == VP_TEXTURE_POOL_STORE_OK);
    vp_texture_pool_abort_store(&fixture.pool, probe);
    fixture_done(&fixture);
    puts("PASS CPU pins block eviction; GPU leases report pressure and release");
}

static void test_cpu_plus_gpu_slot_is_not_pressure(void) {
    Fixture fixture;
    uint8_t source[16];
    VPTexturePoolKey key = key_for(0x5000u);
    VPTexturePoolToken pin = {0u, 0u}, lease = {0u, 0u}, probe = {0u, 0u};
    uint8_t *rgba = NULL, *raw = NULL;
    int reused = 0, rc;

    memset(source, 0x55, sizeof(source));
    fixture_init(&fixture, PAGE * 8u);
    store(&fixture, 0x5000u, source, sizeof(source), 64u, NULL);
    vp_texture_pool_begin_request(&fixture.pool);
    assert(vp_texture_pool_lookup(&fixture.pool, &key, source, sizeof(source),
                                  &pin, &rgba) == 1);
    assert(vp_texture_pool_retain_gpu(&fixture.pool, rgba, &lease) == 1);
    /* Waiting cannot clear the current request's CPU pin, so this is a hard
     * allocation failure rather than a pressure hint. */
    rc = vp_texture_pool_begin_store(&fixture.pool, &key, PAGE * 4u, PAGE * 3u,
                                     &probe, &raw, &rgba, &reused);
    assert(rc == VP_TEXTURE_POOL_STORE_NOMEM);
    assert(vp_texture_pool_release_gpu(&fixture.pool, lease) == 0);
    fixture_done(&fixture);
    puts("PASS a slot held by both CPU and GPU is not waiting-pressure");
}

static void test_generation_stale_release(void) {
    Fixture fixture;
    uint8_t source[32];
    VPTexturePoolKey key = key_for(0x6000u);
    VPTexturePoolToken lease = {0u, 0u}, old, probe = {0u, 0u};
    uint8_t *rgba = NULL, *raw = NULL;
    int reused = 0;

    memset(source, 0x66, sizeof(source));
    fixture_init(&fixture, PAGE * 8u);
    store(&fixture, 0x6000u, source, sizeof(source), 64u, NULL);
    assert(vp_texture_pool_retain_gpu(&fixture.pool, fixture.pool.slots[0].rgba,
                                      &lease) == 1);
    assert(vp_texture_pool_release_gpu(&fixture.pool, lease) == 0);
    old = lease;
    vp_texture_pool_begin_request(&fixture.pool);
    /* Evict the slot through a request it cannot satisfy; store fills it. */
    assert(vp_texture_pool_begin_store(&fixture.pool, &key, PAGE * 4u, PAGE * 3u,
                                       &probe, &raw, &rgba, &reused) ==
           VP_TEXTURE_POOL_STORE_OK);
    vp_texture_pool_abort_store(&fixture.pool, probe);
    assert(vp_texture_pool_release_gpu(&fixture.pool, old) < 0);
    fixture_done(&fixture);
    puts("PASS a stale lease cannot decrement a reused slot");
}

static void test_fragmentation_and_coalesce(void) {
    Fixture fixture;
    uint8_t source[16];
    VPTexturePoolStats stats;
    VPTexturePoolToken probe = {0u, 0u};
    uint8_t *raw = NULL, *rgba = NULL;
    int reused = 0;

    memset(source, 0x77, sizeof(source));
    fixture_init(&fixture, PAGE * 8u);
    store(&fixture, 0x7000u, source, sizeof(source), 64u, NULL);
    store(&fixture, 0x7001u, source, sizeof(source), 64u, NULL);
    store(&fixture, 0x7002u, source, sizeof(source), 64u, NULL);
    vp_texture_pool_begin_request(&fixture.pool);
    /* Touch slots 0 and 2 so the middle slot is the LRU victim. */
    fixture.pool.slots[0].stamp = ++fixture.pool.stamp;
    fixture.pool.slots[2].stamp = ++fixture.pool.stamp;
    assert(vp_texture_pool_begin_store(&fixture.pool, &(VPTexturePoolKey){0},
                                       PAGE * 4u, PAGE, &probe, &raw, &rgba,
                                       &reused) == VP_TEXTURE_POOL_STORE_OK);
    vp_texture_pool_stats(&fixture.pool, &stats);
    /* The two pages freed by the middle slot merged with the following two
     * free pages, so evicting one more slot makes the four-page request fit in
     * one contiguous run. */
    assert(stats.used_bytes == 2u * PAGE + 5u * PAGE);
    assert(stats.free_runs == 1u);
    vp_texture_pool_abort_store(&fixture.pool, probe);
    fixture_done(&fixture);
    puts("PASS first-fit reuses coalesced page runs after eviction");
}

static void test_128_slots_and_all_gpu_refs(void) {
    Fixture fixture;
    uint8_t source[8];
    VPTexturePoolToken token = {0u, 0u};
    VPTexturePoolStats stats;
    unsigned i;

    memset(source, 0x88, sizeof(source));
    fixture_init(&fixture, PAGE * 512u);
    for (i = 0u; i < VP_TEXTURE_POOL_SLOTS; ++i)
        store(&fixture, 0x8000u + i, source, sizeof(source), 8u, NULL);
    vp_texture_pool_stats(&fixture.pool, &stats);
    assert(stats.live_slots == VP_TEXTURE_POOL_SLOTS);
    assert(stats.peak_live_slots == VP_TEXTURE_POOL_SLOTS);
    for (i = 0u; i < VP_TEXTURE_POOL_SLOTS; ++i) {
        assert(vp_texture_pool_retain_gpu(&fixture.pool,
                                          fixture.pool.slots[i].rgba,
                                          &token) == 1);
    }
    /* The retained CPU pins belong to the commits above; the new allocation
     * request starts a fresh draw and must see only the GPU leases. */
    vp_texture_pool_begin_request(&fixture.pool);
    {
        VPTexturePoolToken probe = {0u, 0u};
        uint8_t *raw = NULL, *rgba = NULL;
        int reused = 0;
        assert(vp_texture_pool_begin_store(&fixture.pool,
                                           &(VPTexturePoolKey){0}, 16u, 16u,
                                           &probe, &raw, &rgba, &reused) ==
               VP_TEXTURE_POOL_STORE_PRESSURE);
    }
    for (i = 0u; i < VP_TEXTURE_POOL_SLOTS; ++i) {
        VPTexturePoolToken lease = { (uint32_t)i,
                                     fixture.pool.slots[i].generation };
        assert(vp_texture_pool_release_gpu(&fixture.pool, lease) == 0);
    }
    fixture_done(&fixture);
    puts("PASS 128 live slots and an all-GPU-referenced pool reports pressure");
}

static void test_reset_and_teardown(void) {
    Fixture fixture;
    uint8_t source[16];
    VPTexturePoolToken lease = {0u, 0u}, probe = {0u, 0u};
    VPTexturePoolStats stats;
    uint8_t *raw = NULL, *rgba = NULL;
    int reused = 0;

    memset(source, 0x99, sizeof(source));
    fixture_init(&fixture, PAGE * 8u);
    store(&fixture, 0x9000u, source, sizeof(source), 32u, NULL);
    assert(vp_texture_pool_retain_gpu(&fixture.pool, fixture.pool.slots[0].rgba,
                                      &lease) == 1);
    assert(vp_texture_pool_reset(&fixture.pool) < 0);
    assert(retire_calls == 0u);
    assert(vp_texture_pool_release_gpu(&fixture.pool, lease) == 0);
    assert(vp_texture_pool_reset(&fixture.pool) == 0);
    assert(retire_calls == 1u);
    vp_texture_pool_stats(&fixture.pool, &stats);
    assert(stats.live_slots == 0u);
    assert(stats.used_bytes == 0u);
    assert(stats.free_runs == 1u);
    assert(stats.largest_free_run_bytes == 8u * PAGE);
    /* A full-size request fits after reset, proving every page returned. */
    assert(vp_texture_pool_begin_store(&fixture.pool, &(VPTexturePoolKey){0},
                                       PAGE * 4u, PAGE * 4u, &probe, &raw,
                                       &rgba, &reused) ==
           VP_TEXTURE_POOL_STORE_OK);
    vp_texture_pool_abort_store(&fixture.pool, probe);
    fixture_done(&fixture);
    puts("PASS teardown refuses live GPU leases and returns every page");
}

static struct { void *base; size_t bytes; } regions[256];
static unsigned region_allocs, region_frees;
static int region_fail;
static void *region_allocate(void *user, size_t bytes, unsigned allocation_class) {
    (void)user; (void)allocation_class;
    if (region_fail) return NULL;
    for (unsigned i = 0; i < 256u; ++i) {
        if (regions[i].base) continue;
        regions[i].base = test_page_alloc(bytes);
        regions[i].bytes = bytes;
        if (regions[i].base) region_allocs++;
        return regions[i].base;
    }
    return NULL;
}
static int region_release(void *user, void *base) {
    (void)user;
    for (unsigned i = 0; i < 256u; ++i) {
        if (regions[i].base != base) continue;
        test_page_free(base);
        memset(&regions[i], 0, sizeof(regions[i]));
        region_frees++;
        return 0;
    }
    return -1;
}
static void test_reusable_regions(void) {
    Fixture f;
    uint8_t bytes[32];
    VPTexturePoolToken token;
    uint8_t *raw, *rgba;
    int reused;
    memset(&f, 0, sizeof(f));
    memset(bytes, 0x24, sizeof(bytes));
    region_allocs = region_frees = 0u; region_fail = 0;
    assert(vp_texture_pool_init_regions(&f.pool, 4u * PAGE, 0u, NULL,
        region_allocate, region_release) == 0);
    vp_texture_pool_set_retire(&f.pool, retire_cb, NULL);
    store(&f, 1u, bytes, sizeof(bytes), 32u, NULL);
    vp_texture_pool_begin_request(&f.pool);
    store(&f, 2u, bytes, sizeof(bytes), 32u, NULL);
    for (unsigned i = 0; i < 10000u; ++i) {
        vp_texture_pool_begin_request(&f.pool);
        store(&f, 3u + i, bytes, sizeof(bytes), 32u, NULL);
        assert(f.pool.used_bytes <= 4u * PAGE);
    }
    assert(region_allocs == 4u && region_frees == 0u);
    assert(f.pool.size_reuses >= 10000u);
    vp_texture_pool_begin_request(&f.pool);
    region_fail = 1;
    assert(vp_texture_pool_begin_store(&f.pool, &(VPTexturePoolKey){0},
        PAGE * 2u, PAGE, &token, &raw, &rgba, &reused) == VP_TEXTURE_POOL_STORE_NOMEM);
    assert(vp_texture_pool_reset(&f.pool) == 0);
    assert(region_allocs == region_frees);
    region_fail = 0;
    puts("PASS region pool reuses 10000 payloads without kernel allocation churn");
}
static void test_inflight_content_version(void) {
    Fixture f;
    uint8_t first[16], second[16];
    VPTexturePoolToken original, lease, changed, probe;
    uint8_t *rgba = NULL;
    VPTexturePoolKey key = key_for(9u);
    memset(first, 0x1a, sizeof(first)); memset(second, 0x2b, sizeof(second));
    fixture_init(&f, 8u * PAGE);
    original = store(&f, 9u, first, sizeof(first), 16u, NULL);
    memset(f.pool.slots[original.slot].rgba, 0x11, 16u);
    assert(vp_texture_pool_retain_gpu(&f.pool,
        f.pool.slots[original.slot].rgba, &lease) == 1);
    vp_texture_pool_begin_request(&f.pool);
    assert(!vp_texture_pool_lookup(&f.pool, &key, second, sizeof(second), &probe, &rgba));
    assert(f.pool.slots[original.slot].raw == NULL);
    assert(f.pool.used_bytes == PAGE);
    changed = store(&f, 9u, second, sizeof(second), 16u, NULL);
    assert(changed.slot != original.slot);
    memset(f.pool.slots[changed.slot].rgba, 0x22, 16u);
    for (unsigned i = 0; i < 16u; ++i)
        assert(f.pool.slots[original.slot].rgba[i] == 0x11);
    assert(vp_texture_pool_release_gpu(&f.pool, lease) == 0);
    assert(!f.pool.slots[original.slot].used);
    fixture_done(&f);
    puts("PASS a changed same-key texture preserves the GPU-owned old version");
}

static void test_region_updates_and_kernel_pressure(void) {
    Fixture f;
    uint8_t bytes[32];
    VPTexturePoolToken first, next;
    uint8_t *raw, *rgba;
    int reused;
    memset(&f, 0, sizeof(f));
    memset(bytes, 0x35, sizeof(bytes));
    region_allocs = region_frees = 0u; region_fail = 0;
    assert(vp_texture_pool_init_regions(&f.pool, 16u * PAGE, 0u, NULL,
        region_allocate, region_release) == 0);
    first = store(&f, 4u, bytes, sizeof(bytes), 32u, NULL);
    for (unsigned i = 0; i < 1000u; ++i) {
        vp_texture_pool_begin_request(&f.pool);
        bytes[0]++;
        next = store(&f, 4u, bytes, sizeof(bytes), 32u, NULL);
        assert(next.slot == first.slot);
    }
    assert(region_allocs == 2u && region_frees == 0u);
    vp_texture_pool_begin_request(&f.pool);
    region_fail = 1;
    assert(vp_texture_pool_begin_store(&f.pool, &(VPTexturePoolKey){0},
        sizeof(bytes), 32u, &next, &raw, &rgba, &reused) ==
        VP_TEXTURE_POOL_STORE_OK);
    assert(reused && next.slot == first.slot);
    assert(region_allocs == 2u && region_frees == 0u);
    vp_texture_pool_abort_store(&f.pool, next);
    assert(vp_texture_pool_reset(&f.pool) == 0);
    assert(region_allocs == region_frees);
    region_fail = 0;
    puts("PASS same-key updates and kernel pressure reuse existing regions");
}

static void test_pressure_preserves_wrong_size_regions(void) {
    Fixture f;
    uint8_t bytes[PAGE * 2u];
    VPTexturePoolToken small, lease, next;
    uint8_t *raw, *rgba;
    int reused;
    memset(&f, 0, sizeof(f)); memset(bytes, 0x17, sizeof(bytes));
    region_allocs = region_frees = 0u; region_fail = 0;
    assert(vp_texture_pool_init_regions(&f.pool, 6u * PAGE, 0u, NULL,
        region_allocate, region_release) == 0);
    small = store(&f, 1u, bytes, PAGE, PAGE, NULL);
    assert(vp_texture_pool_retain_gpu(&f.pool, f.pool.slots[small.slot].rgba,
        &lease) == 1);
    vp_texture_pool_begin_request(&f.pool);
    store(&f, 2u, bytes, 2u * PAGE, 2u * PAGE, NULL);
    vp_texture_pool_begin_request(&f.pool);
    VPTexturePoolKey key = key_for(3u);
    assert(vp_texture_pool_begin_store(&f.pool, &key, PAGE, PAGE, &next,
        &raw, &rgba, &reused) == VP_TEXTURE_POOL_STORE_PRESSURE);
    assert(region_allocs == 4u && region_frees == 0u);
    assert(vp_texture_pool_release_gpu(&f.pool, lease) == 0);
    assert(vp_texture_pool_begin_store(&f.pool, &key, PAGE, PAGE, &next,
        &raw, &rgba, &reused) == VP_TEXTURE_POOL_STORE_OK);
    assert(reused && next.slot == small.slot);
    assert(region_allocs == 4u && region_frees == 0u);
    vp_texture_pool_abort_store(&f.pool, next);
    assert(vp_texture_pool_reset(&f.pool) == 0);
    assert(region_allocs == region_frees);
    puts("PASS pressure returns GPU leases before destroying wrong-size regions");
}

static VPTextureRegions mapped_regions;
static void test_idle_resize_keeps_gpu_payload(void) {
    Fixture f;
    uint8_t bytes[PAGE * 2u];
    VPTexturePoolToken held, lease, next;
    uint8_t *raw, *rgba, *held_pixels;
    int reused;
    memset(&f, 0, sizeof(f)); memset(bytes, 0x39, sizeof(bytes));
    region_allocs = region_frees = 0u; region_fail = 0;
    assert(vp_texture_pool_init_regions(&f.pool, 6u * PAGE, 0u, NULL,
        region_allocate, region_release) == 0);
    f.pool.idle_resize = 1u;
    held = store(&f, 1u, bytes, PAGE, PAGE, NULL);
    held_pixels = f.pool.slots[held.slot].rgba;
    memset(held_pixels, 0xa7, PAGE);
    assert(vp_texture_pool_retain_gpu(&f.pool, held_pixels, &lease) == 1);
    vp_texture_pool_begin_request(&f.pool);
    store(&f, 2u, bytes, 2u * PAGE, 2u * PAGE, NULL);
    vp_texture_pool_begin_request(&f.pool);
    VPTexturePoolKey key = key_for(3u);
    assert(vp_texture_pool_begin_store(&f.pool, &key, PAGE, PAGE, &next,
        &raw, &rgba, &reused) == VP_TEXTURE_POOL_STORE_OK);
    assert(next.slot != held.slot && f.pool.pressure_events == 0u);
    for (unsigned i = 0; i < PAGE; ++i) assert(held_pixels[i] == 0xa7);
    assert(f.pool.slots[held.slot].gpu_refs == 1u);
    vp_texture_pool_abort_store(&f.pool, next);
    /* CPU-pinned regions still cannot be resized, even by the experiment. */
    vp_texture_pool_begin_request(&f.pool);
    store(&f, 4u, bytes, 2u * PAGE, 2u * PAGE, NULL);
    assert(vp_texture_pool_begin_store(&f.pool, &key, PAGE, PAGE, &next,
        &raw, &rgba, &reused) == VP_TEXTURE_POOL_STORE_PRESSURE);
    assert(vp_texture_pool_release_gpu(&f.pool, lease) == 0);
    assert(vp_texture_pool_reset(&f.pool) == 0);
    assert(region_allocs == region_frees);
    puts("PASS idle resize evicts only idle payloads and preserves GPU/CPU pins");
}
static void *mapped_create(void *user, size_t bytes, unsigned allocation_class) {
    return region_allocate(user, bytes, allocation_class);
}
static void *mapped_suballocate(void *user, size_t bytes, unsigned allocation_class) {
    (void)user;
    return vp_texture_regions_alloc(&mapped_regions, bytes, allocation_class);
}
static int mapped_subfree(void *user, void *base) {
    (void)user;
    return vp_texture_regions_free(&mapped_regions, base);
}
static void test_working_set_replaces_cold_large_textures(void) {
    Fixture f;
    uint8_t *source = malloc(4u * 1024u * 1024u);
    assert(source);
    memset(source, 0x44, 4u * 1024u * 1024u);
    memset(&f, 0, sizeof(f)); memset(&mapped_regions, 0, sizeof(mapped_regions));
    assert(vp_texture_regions_init(&mapped_regions,
        VP_TEXTURE_REGION_BUDGET, NULL, mapped_create, region_release) == 0);
    assert(vp_texture_pool_init_regions(&f.pool, 48u * 1024u * 1024u, 0u,
        NULL, mapped_suballocate, mapped_subfree) == 0);
    for (unsigned i = 0; i < 3u; ++i) {
        vp_texture_pool_begin_request(&f.pool);
        store(&f, i, source, 4u * 1024u * 1024u, 4u * 1024u * 1024u, NULL);
    }
    for (unsigned i = 0; i < 40u; ++i) {
        vp_texture_pool_begin_request(&f.pool);
        store(&f, 100u + i, source, 512u * 1024u, 512u * 1024u, NULL);
    }
    for (unsigned i = 0; i < 40u; ++i) {
        VPTexturePoolKey key = key_for(100u + i);
        VPTexturePoolToken token;
        uint8_t *rgba;
        vp_texture_pool_begin_request(&f.pool);
        assert(vp_texture_pool_lookup(&f.pool, &key, source, 512u * 1024u,
            &token, &rgba) == 1);
    }
    assert(mapped_regions.kernel_allocs == 12u);
    assert(mapped_regions.kernel_frees == 0u);
    assert(vp_texture_pool_reset(&f.pool) == 0);
    assert(vp_texture_regions_reset(&mapped_regions) == 0);
    free(source);
    puts("PASS cold large textures yield to a complete mixed-size working set");
}

static void test_lookup_trace_preserves_versions(void) {
    Fixture fixture;
    uint8_t source[100];
    uint8_t *rgba = NULL;
    VPTexturePoolKey key = key_for(0x1000u);
    VPTexturePoolLookupTrace trace;
    VPTexturePoolToken token, lease;
    memset(source, 0x23, sizeof(source));
    fixture_init(&fixture, PAGE * 8u);
    token = store(&fixture, key.va, source, sizeof(source), 200u, NULL);
    vp_texture_pool_begin_request(&fixture.pool);
    assert(vp_texture_pool_lookup_trace(&fixture.pool, &key, source, sizeof(source),
        &token, &rgba, &trace) == 1);
    assert(trace.live_keys == 1u && trace.comparisons == 1u);
    assert(trace.compare_bytes == sizeof(source) && trace.changed == 0u);
    assert(vp_texture_pool_retain_gpu(&fixture.pool, rgba, &lease) == 1);
    vp_texture_pool_begin_request(&fixture.pool);
    source[17] ^= 1u;
    assert(vp_texture_pool_lookup_trace(&fixture.pool, &key, source, sizeof(source),
        &token, &rgba, &trace) == 0);
    assert(trace.changed == 1u && trace.comparisons == 1u);
    assert(fixture.pool.slots[lease.slot].raw == NULL);
    assert(vp_texture_pool_gpu_refs(&fixture.pool) == 1u);
    assert(vp_texture_pool_lookup_trace(&fixture.pool, &key, source, sizeof(source),
        &token, &rgba, &trace) == 0);
    assert(trace.gpu_only_keys == 1u && trace.comparisons == 0u && trace.changed == 0u);
    key.va++;
    assert(vp_texture_pool_lookup_trace(&fixture.pool, &key, source, sizeof(source),
        &token, &rgba, &trace) == 0);
    assert(trace.gpu_only_keys == 0u && trace.live_keys == 0u);
    assert(vp_texture_pool_release_gpu(&fixture.pool, lease) == 0);
    fixture_done(&fixture);
    puts("PASS lookup diagnostics retain exact comparison and GPU version lifetime");
}

static VPTextureRegions split_regions;
static struct {
    void *base; size_t bytes; unsigned allocation_class;
} split_backings[VP_TEXTURE_REGION_CHUNKS];
static size_t split_provider_bytes;
static void *split_create(void *user, size_t bytes, unsigned allocation_class) {
    (void)user;
    if (allocation_class >= VP_TEXTURE_REGION_CLASSES) return NULL;
    for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNKS; ++i) {
        if (split_backings[i].base) continue;
        void *base = test_page_alloc(bytes);
        if (!base) return NULL;
        split_backings[i].base = base;
        split_backings[i].bytes = bytes;
        split_backings[i].allocation_class = allocation_class;
        split_provider_bytes += bytes;
        return base;
    }
    return NULL;
}
static int split_destroy(void *user, void *base) {
    (void)user;
    for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNKS; ++i) {
        if (split_backings[i].base != base) continue;
        split_provider_bytes -= split_backings[i].bytes;
        test_page_free(base);
        memset(&split_backings[i], 0, sizeof(split_backings[i]));
        return 0;
    }
    return -1;
}
static void *split_suballocate(void *user, size_t bytes,
                               unsigned allocation_class) {
    (void)user;
    return vp_texture_regions_alloc(&split_regions, bytes, allocation_class);
}
static int split_subfree(void *user, void *base) {
    (void)user;
    return vp_texture_regions_free(&split_regions, base);
}
static unsigned split_chunk_class(const void *base) {
    uintptr_t pointer = (uintptr_t)base;
    for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNKS; ++i) {
        const VPTextureRegionChunk *chunk = &split_regions.chunks[i];
        if (!chunk->base) continue;
        if (pointer >= (uintptr_t)chunk->base &&
            pointer < (uintptr_t)chunk->base + chunk->bytes)
            return chunk->allocation_class;
    }
    assert(0);
    return VP_TEXTURE_REGION_CLASSES;
}
static void test_split_classes_and_shared_budget(void) {
    Fixture f;
    const size_t four_mib = 4u * 1024u * 1024u;
    uint8_t *source = malloc(four_mib);
    VPTexturePoolStats stats;
    assert(source);
    memset(source, 0x6d, four_mib);
    memset(&f, 0, sizeof(f));
    memset(&split_regions, 0, sizeof(split_regions));
    memset(split_backings, 0, sizeof(split_backings));
    split_provider_bytes = 0u;
    assert(vp_texture_regions_init(&split_regions, VP_TEXTURE_REGION_BUDGET,
        NULL, split_create, split_destroy) == 0);
    assert(vp_texture_pool_init_regions(&f.pool, VP_TEXTURE_REGION_BUDGET,
        VP_TEXTURE_REGION_CLASS_CPU_CACHED, NULL, split_suballocate,
        split_subfree) == 0);
    vp_texture_pool_set_retire(&f.pool, retire_cb, NULL);
    for (unsigned i = 0; i < 8u; ++i) {
        vp_texture_pool_begin_request(&f.pool);
        store(&f, 100u + i, source, four_mib, four_mib, NULL);
        assert(split_chunk_class(f.pool.slots[i].raw) ==
               VP_TEXTURE_REGION_CLASS_CPU_CACHED);
        assert(split_chunk_class(f.pool.slots[i].rgba) ==
               VP_TEXTURE_REGION_CLASS_MAPPED);
        assert(f.pool.slots[i].raw != f.pool.slots[i].rgba);
    }
    vp_texture_pool_stats(&f.pool, &stats);
    assert(stats.budget_bytes == VP_TEXTURE_REGION_BUDGET);
    assert(stats.raw_class == VP_TEXTURE_REGION_CLASS_CPU_CACHED);
    assert(stats.used_bytes == VP_TEXTURE_REGION_BUDGET);
    assert(split_regions.physical_bytes == VP_TEXTURE_REGION_BUDGET);
    assert(split_regions.class_physical[VP_TEXTURE_REGION_CLASS_MAPPED] ==
           VP_TEXTURE_REGION_BUDGET / 2u);
    assert(split_regions.class_physical[VP_TEXTURE_REGION_CLASS_CPU_CACHED] ==
           VP_TEXTURE_REGION_BUDGET / 2u);
    for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNKS; ++i) {
        const VPTextureRegionChunk *chunk = &split_regions.chunks[i];
        if (!chunk->base) continue;
        for (unsigned j = 0; j < VP_TEXTURE_REGION_ALLOCS; ++j) {
            const VPTextureRegionAllocation *allocation =
                &split_regions.allocations[j];
            if (allocation->base != chunk->base + (size_t)allocation->first *
                VP_TEXTURE_REGION_PAGE) continue;
            assert(allocation->allocation_class == chunk->allocation_class);
        }
    }
    /* A ninth pair evicts an idle version but the combined physical total
     * remains the same 64 MiB. */
    vp_texture_pool_begin_request(&f.pool);
    store(&f, 200u, source, four_mib, four_mib, NULL);
    vp_texture_pool_stats(&f.pool, &stats);
    assert(stats.evictions >= 1u);
    assert(stats.used_bytes <= VP_TEXTURE_REGION_BUDGET);
    assert(split_regions.physical_bytes <= VP_TEXTURE_REGION_BUDGET);
    assert(split_regions.physical_bytes == VP_TEXTURE_REGION_BUDGET);
    assert(split_regions.class_physical[VP_TEXTURE_REGION_CLASS_MAPPED] <=
           VP_TEXTURE_REGION_BUDGET / 2u);
    /* A small mixed pair reuses the empty chunks of each class. */
    vp_texture_pool_begin_request(&f.pool);
    store(&f, 300u, source, PAGE, PAGE, NULL);
    assert(split_regions.physical_bytes <= VP_TEXTURE_REGION_BUDGET);
    assert(split_regions.class_allocated[VP_TEXTURE_REGION_CLASS_MAPPED] > 0u);
    assert(split_regions.class_allocated[VP_TEXTURE_REGION_CLASS_CPU_CACHED] > 0u);
    assert(vp_texture_pool_reset(&f.pool) == 0);
    assert(vp_texture_regions_reset(&split_regions) == 0);
    assert(split_provider_bytes == 0u);
    free(source);
    puts("PASS split raw/decoded classes never mix and share one 64 MiB budget");
}

int main(void) {
    test_lookup_trace_preserves_versions();
    test_reusable_regions();
    test_region_updates_and_kernel_pressure();
    test_pressure_preserves_wrong_size_regions();
    test_idle_resize_keeps_gpu_payload();
    test_working_set_replaces_cold_large_textures();
    test_split_classes_and_shared_budget();
    test_inflight_content_version();
    test_init_validation();
    test_exact_identity();
    test_changed_content_reuse();
    test_pins_and_pressure();
    test_cpu_plus_gpu_slot_is_not_pressure();
    test_generation_stale_release();
    test_fragmentation_and_coalesce();
    test_128_slots_and_all_gpu_refs();
    test_reset_and_teardown();
    puts("texture_pool: all checks passed");
    return 0;
}

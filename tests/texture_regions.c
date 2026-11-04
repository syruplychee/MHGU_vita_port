#include "texture_regions.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static VPTextureRegions regions;
static size_t provider_bytes, provider_limit, provider_max_block;
static unsigned provider_calls;
static struct { void *base; size_t bytes; } backings[VP_TEXTURE_REGION_CHUNKS];

static void *create(void *user, size_t bytes) {
    (void)user;
    provider_calls++;
    if (bytes > provider_max_block || bytes > provider_limit - provider_bytes)
        return NULL;
    for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNKS; ++i) {
        if (backings[i].base) continue;
        void *base = malloc(bytes);
        assert(base);
        backings[i].base = base; backings[i].bytes = bytes;
        provider_bytes += bytes;
        return base;
    }
    return NULL;
}

static int destroy(void *user, void *base) {
    (void)user;
    for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNKS; ++i) {
        if (backings[i].base != base) continue;
        provider_bytes -= backings[i].bytes;
        free(base);
        memset(&backings[i], 0, sizeof(backings[i]));
        return 0;
    }
    return -1;
}

static void init(void) {
    assert(!provider_bytes);
    memset(&regions, 0, sizeof(regions));
    regions.create = create; regions.destroy = destroy;
    provider_limit = VP_TEXTURE_REGION_BUDGET;
    provider_max_block = VP_TEXTURE_REGION_BUDGET;
    provider_calls = 0u;
}

static void test_mixed_size_reuse(void) {
    init();
    void *persistent = vp_texture_regions_alloc(&regions, 4096u);
    memset(persistent, 0x39, 4096u);
    for (unsigned i = 0; i < 20000u; ++i) {
        size_t bytes = ((i % 31u) + 1u) * 4096u;
        void *base = vp_texture_regions_alloc(&regions, bytes);
        assert(base && base != persistent);
        memset(base, 0x82, bytes);
        assert(vp_texture_regions_free(&regions, base) == 0);
        for (unsigned at = 0; at < 4096u; ++at)
            assert(((uint8_t *)persistent)[at] == 0x39);
    }
    assert(regions.kernel_allocs == 1u && regions.kernel_frees == 0u);
    assert(vp_texture_regions_reset(&regions) < 0);
    assert(vp_texture_regions_free(&regions, persistent) == 0);
    assert(vp_texture_regions_free(&regions, persistent) < 0);
    assert(vp_texture_regions_reset(&regions) == 0);
    assert(!provider_bytes);
    puts("PASS 20000 mixed-size replacements use one mapped chunk");
}

static void test_budget_and_coalescing(void) {
    enum { count = VP_TEXTURE_REGION_BUDGET / VP_TEXTURE_REGION_CHUNK };
    void *blocks[count];
    init();
    for (unsigned i = 0; i < count; ++i) {
        blocks[i] = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK);
        assert(blocks[i]);
    }
    assert(regions.physical_bytes == VP_TEXTURE_REGION_BUDGET);
    assert(!vp_texture_regions_alloc(&regions, 4096u));
    assert(vp_texture_regions_free(&regions, blocks[0]) == 0);
    void *small[3];
    for (unsigned i = 0; i < 3u; ++i) {
        small[i] = vp_texture_regions_alloc(&regions, 4096u);
        assert(small[i] == (uint8_t *)blocks[0] + i * 4096u);
    }
    assert(vp_texture_regions_free(&regions, small[0]) == 0);
    assert(vp_texture_regions_free(&regions, small[1]) == 0);
    void *merged = vp_texture_regions_alloc(&regions, 8192u);
    assert(merged == blocks[0]);
    assert(vp_texture_regions_free(&regions, merged) == 0);
    assert(vp_texture_regions_free(&regions, small[2]) == 0);
    for (unsigned i = 1; i < count; ++i)
        assert(vp_texture_regions_free(&regions, blocks[i]) == 0);
    void *large = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK * 2u);
    assert(large && regions.physical_bytes <= VP_TEXTURE_REGION_BUDGET);
    assert(vp_texture_regions_free(&regions, large) == 0);
    assert(vp_texture_regions_reset(&regions) == 0 && !provider_bytes);
    puts("PASS physical budget, merged pages and larger chunk replacement");
}

static void test_kernel_pressure_and_slots(void) {
    void *blocks[256];
    init();
    provider_limit = 8u * 1024u * 1024u;
    void *old = vp_texture_regions_alloc(&regions, 4096u);
    assert(old);
    assert(vp_texture_regions_free(&regions, old) == 0);
    /* The provider cannot hold the old 4 MiB plus the new 8 MiB. */
    void *large = vp_texture_regions_alloc(&regions, provider_limit);
    assert(large && regions.kernel_frees == 1u);
    assert(vp_texture_regions_free(&regions, large) == 0);
    for (unsigned i = 0; i < 256u; ++i) {
        blocks[i] = vp_texture_regions_alloc(&regions, 4096u);
        assert(blocks[i]);
    }
    assert(!vp_texture_regions_alloc(&regions, 4096u));
    for (unsigned i = 0; i < 256u; ++i)
        assert(vp_texture_regions_free(&regions, blocks[i]) == 0);
    assert(vp_texture_regions_reset(&regions) == 0 && !provider_bytes);
    puts("PASS provider pressure and 256 live suballocation limit");
}

static void test_partial_bitmap_words(void) {
    init();
    void *prefix = vp_texture_regions_alloc(&regions, 31u * 4096u);
    void *middle = vp_texture_regions_alloc(&regions, 65u * 4096u);
    void *suffix = vp_texture_regions_alloc(&regions, 4096u);
    assert(prefix && middle && suffix);
    memset(prefix, 0x73, 31u * 4096u); memset(suffix, 0x27, 4096u);
    assert(vp_texture_regions_free(&regions, middle) == 0);
    void *reused = vp_texture_regions_alloc(&regions, 65u * 4096u);
    assert(reused == middle);
    memset(reused, 0x84, 65u * 4096u);
    for (unsigned i = 0; i < 31u * 4096u; ++i)
        assert(((uint8_t *)prefix)[i] == 0x73);
    for (unsigned i = 0; i < 4096u; ++i)
        assert(((uint8_t *)suffix)[i] == 0x27);
    assert(vp_texture_regions_free(&regions, prefix) == 0);
    assert(vp_texture_regions_free(&regions, reused) == 0);
    assert(vp_texture_regions_free(&regions, suffix) == 0);
    assert(vp_texture_regions_reset(&regions) == 0);
    puts("PASS partial bitmap words retain both neighbouring allocations");
}

static void test_fragmented_provider(void) {
    void *blocks[28];
    init();
    for (unsigned i = 0; i < 12u; ++i) {
        blocks[i] = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK);
        assert(blocks[i]);
    }
    provider_max_block = 1024u * 1024u;
    for (unsigned i = 12u; i < 28u; ++i) {
        blocks[i] = vp_texture_regions_alloc(&regions, 1024u * 1024u);
        assert(blocks[i]);
    }
    assert(regions.physical_bytes == VP_TEXTURE_REGION_BUDGET);
    assert(regions.kernel_allocs == 28u && regions.kernel_frees == 0u);
    for (unsigned i = 12u; i < 28u; ++i)
        assert(vp_texture_regions_free(&regions, blocks[i]) == 0);
    /* No combination of free 1 MiB kernel blocks can satisfy a 4 MiB
     * contiguous request. Keep their mappings and wait for a large chunk. */
    unsigned calls_before = provider_calls;
    for (unsigned i = 0; i < 1000u; ++i)
        assert(!vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK));
    assert(provider_calls == calls_before);
    assert(regions.kernel_allocs == 28u && regions.kernel_frees == 0u);
    assert(vp_texture_regions_free(&regions, blocks[0]) == 0);
    void *small = vp_texture_regions_alloc(&regions, 4096u);
    assert(small && small != blocks[0]);
    void *large = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK);
    assert(large == blocks[0]);
    assert(vp_texture_regions_free(&regions, small) == 0);
    assert(vp_texture_regions_free(&regions, large) == 0);
    for (unsigned i = 1; i < 12u; ++i)
        assert(vp_texture_regions_free(&regions, blocks[i]) == 0);
    assert(vp_texture_regions_reset(&regions) == 0 && !provider_bytes);
    puts("PASS smaller physical holes fill the same bounded byte budget");
}

int main(void) {
    test_mixed_size_reuse();
    test_budget_and_coalescing();
    test_kernel_pressure_and_slots();
    test_partial_bitmap_words();
    test_fragmented_provider();
    return 0;
}

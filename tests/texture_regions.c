#include "texture_regions.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static VPTextureRegions regions;
static size_t provider_bytes, provider_limit, provider_max_block;
static unsigned provider_calls;
static int provider_fail_class = -1;
static size_t provider_fail_bytes;
static struct {
    void *base; size_t bytes; unsigned allocation_class;
} backings[VP_TEXTURE_REGION_CHUNKS];

static void *create(void *user, size_t bytes, unsigned allocation_class) {
    (void)user;
    provider_calls++;
    assert(allocation_class < VP_TEXTURE_REGION_CLASSES);
    if ((int)allocation_class == provider_fail_class &&
        bytes == provider_fail_bytes)
        return NULL;
    if (bytes > provider_max_block || bytes > provider_limit - provider_bytes)
        return NULL;
    for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNKS; ++i) {
        if (backings[i].base) continue;
        void *base = malloc(bytes);
        assert(base);
        backings[i].base = base;
        backings[i].bytes = bytes;
        backings[i].allocation_class = allocation_class;
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

static void init_with_budget(size_t budget) {
    assert(!provider_bytes);
    memset(backings, 0, sizeof(backings));
    assert(vp_texture_regions_init(&regions, budget, NULL, create, destroy) == 0);
    provider_limit = budget;
    provider_max_block = budget;
    provider_calls = 0u;
    provider_fail_class = -1;
    provider_fail_bytes = 0u;
}

static void init(void) {
    init_with_budget(VP_TEXTURE_REGION_BUDGET);
}

static void check_class_integrity(void) {
    for (unsigned i = 0; i < VP_TEXTURE_REGION_ALLOCS; ++i) {
        const VPTextureRegionAllocation *allocation = &regions.allocations[i];
        if (!allocation->base) continue;
        const VPTextureRegionChunk *chunk = &regions.chunks[allocation->chunk];
        assert(chunk->base);
        assert(chunk->allocation_class == allocation->allocation_class);
    }
    for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNKS; ++i) {
        const VPTextureRegionChunk *chunk = &regions.chunks[i];
        if (!chunk->base || !chunk->used_pages) continue;
        assert((size_t)chunk->used_pages * VP_TEXTURE_REGION_PAGE <=
               regions.class_allocated[chunk->allocation_class]);
    }
}

static void test_mixed_size_reuse(void) {
    init();
    void *persistent = vp_texture_regions_alloc(&regions, 4096u,
                                                VP_TEXTURE_REGION_CLASS_MAPPED);
    memset(persistent, 0x39, 4096u);
    for (unsigned i = 0; i < 20000u; ++i) {
        size_t bytes = ((i % 31u) + 1u) * 4096u;
        void *base = vp_texture_regions_alloc(&regions, bytes,
                                              VP_TEXTURE_REGION_CLASS_MAPPED);
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
    assert(regions.budget == VP_TEXTURE_REGION_BUDGET);
    for (unsigned i = 0; i < count; ++i) {
        blocks[i] = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK,
                                             VP_TEXTURE_REGION_CLASS_MAPPED);
        assert(blocks[i]);
    }
    assert(regions.physical_bytes == VP_TEXTURE_REGION_BUDGET);
    assert(!vp_texture_regions_alloc(&regions, 4096u,
                                     VP_TEXTURE_REGION_CLASS_MAPPED));
    assert(vp_texture_regions_free(&regions, blocks[0]) == 0);
    void *small[3];
    for (unsigned i = 0; i < 3u; ++i) {
        small[i] = vp_texture_regions_alloc(&regions, 4096u,
                                            VP_TEXTURE_REGION_CLASS_MAPPED);
        assert(small[i] == (uint8_t *)blocks[0] + i * 4096u);
    }
    assert(vp_texture_regions_free(&regions, small[0]) == 0);
    assert(vp_texture_regions_free(&regions, small[1]) == 0);
    void *merged = vp_texture_regions_alloc(&regions, 8192u,
                                            VP_TEXTURE_REGION_CLASS_MAPPED);
    assert(merged == blocks[0]);
    assert(vp_texture_regions_free(&regions, merged) == 0);
    assert(vp_texture_regions_free(&regions, small[2]) == 0);
    for (unsigned i = 1; i < count; ++i)
        assert(vp_texture_regions_free(&regions, blocks[i]) == 0);
    void *large = vp_texture_regions_alloc(&regions,
                                           VP_TEXTURE_REGION_CHUNK * 2u,
                                           VP_TEXTURE_REGION_CLASS_MAPPED);
    assert(large && regions.physical_bytes <= VP_TEXTURE_REGION_BUDGET);
    assert(vp_texture_regions_free(&regions, large) == 0);
    assert(vp_texture_regions_reset(&regions) == 0 && !provider_bytes);
    puts("PASS physical budget, merged pages and larger chunk replacement");
}

static void test_kernel_pressure_and_slots(void) {
    void *blocks[256];
    init();
    provider_limit = 8u * 1024u * 1024u;
    void *old = vp_texture_regions_alloc(&regions, 4096u,
                                         VP_TEXTURE_REGION_CLASS_MAPPED);
    assert(old);
    assert(vp_texture_regions_free(&regions, old) == 0);
    /* The provider cannot hold the old 4 MiB plus the new 8 MiB. */
    void *large = vp_texture_regions_alloc(&regions, provider_limit,
                                           VP_TEXTURE_REGION_CLASS_MAPPED);
    assert(large && regions.kernel_frees == 1u);
    assert(vp_texture_regions_free(&regions, large) == 0);
    for (unsigned i = 0; i < 256u; ++i) {
        blocks[i] = vp_texture_regions_alloc(&regions, 4096u,
                                             VP_TEXTURE_REGION_CLASS_MAPPED);
        assert(blocks[i]);
    }
    assert(!vp_texture_regions_alloc(&regions, 4096u,
                                     VP_TEXTURE_REGION_CLASS_MAPPED));
    for (unsigned i = 0; i < 256u; ++i)
        assert(vp_texture_regions_free(&regions, blocks[i]) == 0);
    assert(vp_texture_regions_reset(&regions) == 0 && !provider_bytes);
    puts("PASS provider pressure and 256 live suballocation limit");
}

static void test_partial_bitmap_words(void) {
    init();
    void *prefix = vp_texture_regions_alloc(&regions, 31u * 4096u,
                                            VP_TEXTURE_REGION_CLASS_MAPPED);
    void *middle = vp_texture_regions_alloc(&regions, 65u * 4096u,
                                            VP_TEXTURE_REGION_CLASS_MAPPED);
    void *suffix = vp_texture_regions_alloc(&regions, 4096u,
                                            VP_TEXTURE_REGION_CLASS_MAPPED);
    assert(prefix && middle && suffix);
    memset(prefix, 0x73, 31u * 4096u); memset(suffix, 0x27, 4096u);
    assert(vp_texture_regions_free(&regions, middle) == 0);
    void *reused = vp_texture_regions_alloc(&regions, 65u * 4096u,
                                            VP_TEXTURE_REGION_CLASS_MAPPED);
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
        blocks[i] = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK,
                                             VP_TEXTURE_REGION_CLASS_MAPPED);
        assert(blocks[i]);
    }
    provider_max_block = 1024u * 1024u;
    for (unsigned i = 12u; i < 28u; ++i) {
        blocks[i] = vp_texture_regions_alloc(&regions, 1024u * 1024u,
                                             VP_TEXTURE_REGION_CLASS_MAPPED);
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
        assert(!vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK,
                                         VP_TEXTURE_REGION_CLASS_MAPPED));
    assert(provider_calls == calls_before);
    assert(regions.kernel_allocs == 28u && regions.kernel_frees == 0u);
    assert(vp_texture_regions_free(&regions, blocks[0]) == 0);
    void *small = vp_texture_regions_alloc(&regions, 4096u,
                                           VP_TEXTURE_REGION_CLASS_MAPPED);
    assert(small && small != blocks[0]);
    void *large = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK,
                                           VP_TEXTURE_REGION_CLASS_MAPPED);
    assert(large == blocks[0]);
    assert(vp_texture_regions_free(&regions, small) == 0);
    assert(vp_texture_regions_free(&regions, large) == 0);
    for (unsigned i = 1; i < 12u; ++i)
        assert(vp_texture_regions_free(&regions, blocks[i]) == 0);
    assert(vp_texture_regions_reset(&regions) == 0 && !provider_bytes);
    puts("PASS smaller physical holes fill the same bounded byte budget");
}

static void test_dynamic_128_mib_budget(void) {
    enum {
        count = VP_TEXTURE_REGION_MAX_BUDGET / VP_TEXTURE_REGION_CHUNK,
        beyond_default = VP_TEXTURE_REGION_BUDGET / VP_TEXTURE_REGION_CHUNK + 1u
    };
    void *blocks[count];
    init_with_budget(VP_TEXTURE_REGION_MAX_BUDGET);
    assert(regions.budget == VP_TEXTURE_REGION_MAX_BUDGET);
    for (unsigned i = 0; i < beyond_default; ++i) {
        blocks[i] = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK,
                                             VP_TEXTURE_REGION_CLASS_MAPPED);
        assert(blocks[i]);
    }
    /* More than the default 64 MiB is physically resident and still inside
     * the 128 MiB experiment budget. */
    assert(regions.physical_bytes > VP_TEXTURE_REGION_BUDGET);
    assert(regions.physical_bytes ==
           (size_t)beyond_default * VP_TEXTURE_REGION_CHUNK);
    for (unsigned i = beyond_default; i < count; ++i) {
        blocks[i] = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK,
                                             VP_TEXTURE_REGION_CLASS_MAPPED);
        assert(blocks[i]);
    }
    assert(regions.physical_bytes == VP_TEXTURE_REGION_MAX_BUDGET);
    assert(regions.peak_bytes == VP_TEXTURE_REGION_MAX_BUDGET);
    assert(!vp_texture_regions_alloc(&regions, 4096u,
                                     VP_TEXTURE_REGION_CLASS_MAPPED));
    for (unsigned i = 0; i < count; ++i)
        assert(vp_texture_regions_free(&regions, blocks[i]) == 0);
    assert(vp_texture_regions_reset(&regions) == 0 && !provider_bytes);
    puts("PASS 128 MiB budget holds more than 64 MiB without exceeding 128 MiB");
}

static void test_split_classes_share_one_budget(void) {
    enum { per_class = 8u };
    void *mapped[per_class], *cached[per_class];
    init();
    for (unsigned i = 0; i < per_class; ++i) {
        mapped[i] = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK,
                                             VP_TEXTURE_REGION_CLASS_MAPPED);
        cached[i] = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK,
                                             VP_TEXTURE_REGION_CLASS_CPU_CACHED);
        assert(mapped[i] && cached[i]);
        memset(mapped[i], 0x11, VP_TEXTURE_REGION_CHUNK);
        memset(cached[i], 0x22, VP_TEXTURE_REGION_CHUNK);
    }
    assert(regions.physical_bytes == VP_TEXTURE_REGION_BUDGET);
    assert(regions.class_physical[VP_TEXTURE_REGION_CLASS_MAPPED] ==
           VP_TEXTURE_REGION_BUDGET / 2u);
    assert(regions.class_physical[VP_TEXTURE_REGION_CLASS_CPU_CACHED] ==
           VP_TEXTURE_REGION_BUDGET / 2u);
    assert(regions.class_allocated[VP_TEXTURE_REGION_CLASS_MAPPED] ==
           VP_TEXTURE_REGION_BUDGET / 2u);
    assert(regions.class_allocated[VP_TEXTURE_REGION_CLASS_CPU_CACHED] ==
           VP_TEXTURE_REGION_BUDGET / 2u);
    check_class_integrity();
    /* Both classes share the same 64 MiB physical limit. */
    assert(!vp_texture_regions_alloc(&regions, 4096u,
                                     VP_TEXTURE_REGION_CLASS_MAPPED));
    assert(!vp_texture_regions_alloc(&regions, 4096u,
                                     VP_TEXTURE_REGION_CLASS_CPU_CACHED));
    assert(regions.physical_bytes == VP_TEXTURE_REGION_BUDGET);

    /* A live page pins its chunk: an 8 MiB request cannot rebuild any chunk
     * because every class-0 chunk is only 4 MiB and all chunks have live pages. */
    unsigned calls_before = provider_calls;
    void *large = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK * 2u,
                                           VP_TEXTURE_REGION_CLASS_MAPPED);
    assert(!large);
    assert(provider_calls == calls_before);
    assert(regions.physical_bytes == VP_TEXTURE_REGION_BUDGET);
    for (unsigned i = 0; i < VP_TEXTURE_REGION_CHUNK; ++i)
        assert(((uint8_t *)cached[0])[i] == 0x22);

    /* Freeing every cached chunk makes them empty. An 8 MiB mapped request may
     * now drop those empty chunks and recreate them as class 0; the combined
     * physical total stays inside 64 MiB. */
    for (unsigned i = 0; i < per_class; ++i)
        assert(vp_texture_regions_free(&regions, cached[i]) == 0);
    large = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK * 2u,
                                     VP_TEXTURE_REGION_CLASS_MAPPED);
    assert(large);
    /* Only enough empty cached chunks are dropped to admit the 8 MiB chunk;
     * the remaining empty cached chunks stay available for class 1. */
    assert(regions.class_physical[VP_TEXTURE_REGION_CLASS_CPU_CACHED] ==
           6u * VP_TEXTURE_REGION_CHUNK);
    assert(regions.class_allocated[VP_TEXTURE_REGION_CLASS_CPU_CACHED] == 0u);
    assert(regions.class_physical[VP_TEXTURE_REGION_CLASS_MAPPED] ==
           (per_class + 2u) * VP_TEXTURE_REGION_CHUNK);
    assert(regions.physical_bytes <= VP_TEXTURE_REGION_BUDGET);
    assert(regions.physical_bytes ==
           (per_class + 2u + 6u) * VP_TEXTURE_REGION_CHUNK);
    check_class_integrity();
    for (unsigned i = 0; i < per_class; ++i)
        assert(vp_texture_regions_free(&regions, mapped[i]) == 0);
    assert(vp_texture_regions_free(&regions, large) == 0);
    assert(vp_texture_regions_reset(&regions) == 0 && !provider_bytes);
    puts("PASS split classes never mix and share the single 64 MiB budget");
}

static void test_empty_chunk_class_change_and_coalesce(void) {
    init_with_budget(VP_TEXTURE_REGION_CHUNK);
    void *cached = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK,
                                            VP_TEXTURE_REGION_CLASS_CPU_CACHED);
    assert(cached);
    assert(vp_texture_regions_free(&regions, cached) == 0);
    /* The only chunk is empty, so it may be released and recreated for the
     * mapped class even though the byte budget is already full. */
    void *mapped = vp_texture_regions_alloc(&regions, 4096u,
                                            VP_TEXTURE_REGION_CLASS_MAPPED);
    assert(mapped);
    assert(regions.kernel_frees == 1u && regions.kernel_allocs == 2u);
    assert(regions.class_physical[VP_TEXTURE_REGION_CLASS_CPU_CACHED] == 0u);
    assert(regions.class_physical[VP_TEXTURE_REGION_CLASS_MAPPED] ==
           VP_TEXTURE_REGION_CHUNK);
    assert(vp_texture_regions_free(&regions, mapped) == 0);
    void *left = vp_texture_regions_alloc(&regions, 4096u,
                                          VP_TEXTURE_REGION_CLASS_MAPPED);
    void *right = vp_texture_regions_alloc(&regions, 4096u,
                                           VP_TEXTURE_REGION_CLASS_MAPPED);
    assert(left && right && right == (uint8_t *)left + 4096u);
    assert(vp_texture_regions_free(&regions, left) == 0);
    assert(vp_texture_regions_free(&regions, right) == 0);
    void *merged = vp_texture_regions_alloc(&regions, 8192u,
                                            VP_TEXTURE_REGION_CLASS_MAPPED);
    assert(merged == left);
    assert(vp_texture_regions_free(&regions, merged) == 0);
    assert(vp_texture_regions_reset(&regions) == 0 && !provider_bytes);
    puts("PASS empty chunks change class and adjacent pages coalesce");
}

static void test_failed_block_is_remembered_per_class(void) {
    init();
    void *live_raw = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK,
                                              VP_TEXTURE_REGION_CLASS_CPU_CACHED);
    assert(live_raw);
    /* The cached class cannot grow, but the mapped class must still allocate
     * the same size. The remembered failure is per class. */
    provider_fail_class = (int)VP_TEXTURE_REGION_CLASS_CPU_CACHED;
    provider_fail_bytes = VP_TEXTURE_REGION_CHUNK;
    assert(!vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK,
                                     VP_TEXTURE_REGION_CLASS_CPU_CACHED));
    assert(regions.failed_block_bytes[VP_TEXTURE_REGION_CLASS_CPU_CACHED] ==
           VP_TEXTURE_REGION_CHUNK);
    void *mapped = vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK,
                                            VP_TEXTURE_REGION_CLASS_MAPPED);
    assert(mapped);
    assert(regions.failed_block_bytes[VP_TEXTURE_REGION_CLASS_MAPPED] == 0u);
    unsigned calls = provider_calls;
    for (unsigned i = 0; i < 64u; ++i)
        assert(!vp_texture_regions_alloc(&regions, VP_TEXTURE_REGION_CHUNK,
                                         VP_TEXTURE_REGION_CLASS_CPU_CACHED));
    assert(provider_calls == calls);
    /* A newly encountered larger raw resource may still retry the provider. */
    provider_fail_class = -1;
    provider_fail_bytes = 0u;
    void *larger_raw = vp_texture_regions_alloc(&regions,
        VP_TEXTURE_REGION_CHUNK * 2u, VP_TEXTURE_REGION_CLASS_CPU_CACHED);
    assert(larger_raw);
    assert(vp_texture_regions_free(&regions, live_raw) == 0);
    assert(vp_texture_regions_free(&regions, mapped) == 0);
    assert(vp_texture_regions_free(&regions, larger_raw) == 0);
    assert(vp_texture_regions_reset(&regions) == 0 && !provider_bytes);
    puts("PASS failed block sizes are remembered per allocation class");
}

int main(void) {
    test_mixed_size_reuse();
    test_budget_and_coalescing();
    test_kernel_pressure_and_slots();
    test_partial_bitmap_words();
    test_fragmented_provider();
    test_dynamic_128_mib_budget();
    test_split_classes_share_one_budget();
    test_empty_chunk_class_change_and_coalesce();
    test_failed_block_is_remembered_per_class();
    return 0;
}

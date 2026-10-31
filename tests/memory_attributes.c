#include "memory_attributes.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define PAGE 0x1000u
#define REF_BASE 0x00100000u
#define REF_PAGES 256u
#define CAP VP_MEMORY_ATTRIBUTE_LIMIT

static uint32_t rng_state = 0x9e3779b9u;

static uint32_t next_random(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static void check_canonical(const VPMemoryAttributes *table) {
    uint64_t previous_end = 0;
    uint32_t previous_attr = 0;
    assert(table->count <= CAP);
    for (unsigned i = 0; i < table->count; ++i) {
        const VPMemoryAttributeRange *r = &table->ranges[i];
        const uint64_t end = (uint64_t)r->base + r->size;
        assert(r->size != 0);
        assert((r->base & 0xFFFu) == 0 && (r->size & 0xFFFu) == 0);
        assert(r->attr != 0 && (r->attr & ~VP_MEMORY_ATTRIBUTE_BITS) == 0);
        assert(end <= VP_MEMORY_ATTRIBUTE_END);
        assert((uint64_t)r->base >= previous_end);
        /* Canonical state cannot contain two touching equal-attribute
         * intervals; that is exactly what a merge must prevent. */
        if ((uint64_t)r->base == previous_end)
            assert(r->attr != previous_attr);
        previous_end = end;
        previous_attr = r->attr;
    }
}

static void check_equal(const VPMemoryAttributes *a,
                        const VPMemoryAttributes *b) {
    assert(a->count == b->count);
    assert(memcmp(a->ranges, b->ranges, a->count * sizeof(a->ranges[0])) == 0);
}

/* Independent reference: one byte of attribute per page, updated with the
 * same masked-write rule the kernel applies. The test window is small enough
 * for a flat array and is disjoint from the top-address cases. */
static void apply_reference(uint8_t *pages, uint32_t page,
                            uint32_t count, uint32_t mask, uint32_t attr) {
    for (uint32_t i = 0; i < count; ++i)
        pages[page + i] = (uint8_t)((pages[page + i] & ~mask) | (attr & mask));
}

static void compare_reference(const VPMemoryAttributes *table,
                              const uint8_t *pages) {
    check_canonical(table);
    for (uint32_t p = 0; p < REF_PAGES; ++p) {
        const uint32_t address = REF_BASE + p * PAGE;
        assert(vp_memory_attributes_at(table, address) == pages[p]);
        assert(vp_memory_attributes_at(table, address + 0x800u) == pages[p]);

        uint64_t expected_start;
        if (pages[p] != 0) {
            uint32_t q = p;
            while (q > 0 && pages[q - 1] == pages[p]) --q;
            expected_start = REF_BASE + q * PAGE;
        } else {
            uint32_t q = p;
            while (q > 0 && pages[q - 1] == 0) --q;
            expected_start = q == 0 ? 0 : REF_BASE + q * PAGE;
        }
        uint64_t expected_end;
        uint32_t q = p;
        while (q < REF_PAGES && pages[q] == pages[p]) ++q;
        if (q == REF_PAGES && pages[p] == 0)
            expected_end = VP_MEMORY_ATTRIBUTE_END;
        else
            expected_end = REF_BASE + q * PAGE;
        const uint64_t actual_start =
            vp_memory_attributes_start(table, address);
        const uint64_t actual_end = vp_memory_attributes_end(table, address);
        if (actual_start != expected_start || actual_end != expected_end) {
            fprintf(stderr,
                    "boundary mismatch page=%u address=0x%x attr=%u "
                    "start=%llx expected=%llx end=%llx expected=%llx\n",
                    p, address, pages[p],
                    (unsigned long long)actual_start,
                    (unsigned long long)expected_start,
                    (unsigned long long)actual_end,
                    (unsigned long long)expected_end);
            for (unsigned i = 0; i < table->count; ++i)
                fprintf(stderr, "  [0x%x,0x%x) attr=%x\n",
                        table->ranges[i].base,
                        table->ranges[i].base + table->ranges[i].size,
                        table->ranges[i].attr);
        }
        assert(actual_start == expected_start);
        assert(actual_end == expected_end);
    }
}

static void test_mask_update_and_merge(void) {
    VPMemoryAttributes table;
    memset(&table, 0, sizeof(table));

    assert(vp_memory_attributes_set(&table, 0x200000, PAGE, 0xF, 1)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == 1 && table.ranges[0].base == 0x200000
           && table.ranges[0].size == PAGE && table.ranges[0].attr == 1);
    /* Touching equal attribute merges. */
    assert(vp_memory_attributes_set(&table, 0x201000, PAGE, 0xF, 1)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == 1 && table.ranges[0].size == 2 * PAGE);
    /* Touching different attribute stays a second interval. */
    assert(vp_memory_attributes_set(&table, 0x202000, PAGE, 0xF, 2)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == 2 && table.ranges[1].attr == 2);
    assert(vp_memory_attributes_set(&table, 0x203000, PAGE, 0xF, 1)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == 3);

    /* Masked update in the middle: only bits 1..3 change, bit 0 survives.
     * The first covered page keeps its old bit 0 (1|6 = 7) while the second
     * has none (2&~E = 0), so the two updated pieces stay separate. */
    assert(vp_memory_attributes_set(&table, 0x201000, 2 * PAGE, 0xE, 6)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == 4);
    assert(table.ranges[0].base == 0x200000 && table.ranges[0].size == PAGE
           && table.ranges[0].attr == 1);
    assert(table.ranges[1].base == 0x201000 && table.ranges[1].size == PAGE
           && table.ranges[1].attr == 7);
    assert(table.ranges[2].base == 0x202000 && table.ranges[2].size == PAGE
           && table.ranges[2].attr == 6);
    assert(table.ranges[3].base == 0x203000 && table.ranges[3].size == PAGE
           && table.ranges[3].attr == 1);
    /* The masked write must not claim bits outside the mask. */
    assert(vp_memory_attributes_set(&table, 0x201000, 2 * PAGE, 0xE, 8)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(vp_memory_attributes_at(&table, 0x201000) == 9);
    assert(vp_memory_attributes_at(&table, 0x202000) == 8);

    /* Clearing the middle removes it; the surviving sides do not merge
     * across the cleared pages. */
    assert(vp_memory_attributes_set(&table, 0x201000, 2 * PAGE, 0xF, 0)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == 2);
    assert(table.ranges[0].base == 0x200000);
    assert(table.ranges[1].base == 0x203000);
    /* Refilling the gap merges all three pages-runs back into one. */
    assert(vp_memory_attributes_set(&table, 0x201000, 2 * PAGE, 0xF, 1)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == 1 && table.ranges[0].base == 0x200000
           && table.ranges[0].size == 4 * PAGE && table.ranges[0].attr == 1);
    assert(vp_memory_attributes_set(&table, 0x200000, 4 * PAGE, 0xF, 0)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == 0);
    puts("PASS masked updates, touching merge, clear and refill");
}

static void test_all_bits(void) {
    VPMemoryAttributes table;
    memset(&table, 0, sizeof(table));
    assert(vp_memory_attributes_set(&table, 0x400000, PAGE, 0xF, 0xF)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(vp_memory_attributes_at(&table, 0x400800) == 0xF);
    uint32_t expected = 0xFu;
    for (unsigned bit = 0; bit < 4; ++bit) {
        assert(vp_memory_attributes_set(&table, 0x400000, PAGE, 1u << bit, 0)
               == VP_MEMORY_ATTRIBUTE_OK);
        expected &= ~(1u << bit);
        assert(vp_memory_attributes_at(&table, 0x400000) == expected);
    }
    assert(table.count == 0);
    for (unsigned bit = 0; bit < 4; ++bit) {
        assert(vp_memory_attributes_set(&table, 0x400000, PAGE, 1u << bit,
                                        1u << bit)
               == VP_MEMORY_ATTRIBUTE_OK);
        assert(vp_memory_attributes_at(&table, 0x400000) == (1u << bit));
        assert(vp_memory_attributes_set(&table, 0x400000, PAGE, 1u << bit, 0)
               == VP_MEMORY_ATTRIBUTE_OK);
        assert(table.count == 0);
    }
    puts("PASS every attribute bit set and cleared independently");
}

static void test_repeated_cycles(void) {
    VPMemoryAttributes table;
    memset(&table, 0, sizeof(table));
    for (unsigned i = 0; i < 700; ++i) {
        const uint32_t attr = 1u + (i % 15u);
        assert(vp_memory_attributes_set(&table, 0x600000, 0x100000, 0xF, attr)
               == VP_MEMORY_ATTRIBUTE_OK);
        assert(table.count == 1);
        assert(vp_memory_attributes_at(&table, 0x6fffff) == attr);
        assert(vp_memory_attributes_set(&table, 0x600000, 0x100000, 0xF, 0)
               == VP_MEMORY_ATTRIBUTE_OK);
        assert(table.count == 0);
    }
    puts("PASS 700 set/clear cycles stay at one interval");
}

static void test_random_against_page_array(void) {
    VPMemoryAttributes table;
    uint8_t pages[REF_PAGES];
    memset(&table, 0, sizeof(table));
    memset(pages, 0, sizeof(pages));
    rng_state = 0x12345678u;
    for (unsigned iteration = 0; iteration < 3000; ++iteration) {
        const uint32_t page = next_random() % REF_PAGES;
        const uint32_t length = 1u + next_random() % (REF_PAGES - page);
        const uint32_t address = REF_BASE + page * PAGE;
        const uint32_t size = length * PAGE;
        if ((next_random() & 7u) == 0) {
            assert(vp_memory_attributes_forget(&table, address, size)
                   == VP_MEMORY_ATTRIBUTE_OK);
            apply_reference(pages, page, length, 0xF, 0);
        } else {
            const uint32_t mask = next_random() & 0xFu;
            const uint32_t attr = next_random() & mask;
            assert(vp_memory_attributes_set(&table, address, size, mask, attr)
                   == VP_MEMORY_ATTRIBUTE_OK);
            apply_reference(pages, page, length, mask, attr);
        }
        if ((iteration & 7u) == 0) compare_reference(&table, pages);
    }
    compare_reference(&table, pages);
    puts("PASS 3000 deterministic random masked ops match page-array reference");
}

static void fill_256_4k(VPMemoryAttributes *table) {
    memset(table, 0, sizeof(*table));
    for (unsigned i = 0; i < CAP; ++i) {
        const uint32_t base = 0x100000u + i * 0x2000u;
        /* Attributes 1..7 keep the chosen probe attribute 8 distinct from
         * both neighbours of the gap used for the overflow attempt. */
        assert(vp_memory_attributes_set(table, base, PAGE, 0xF, 1u + (i % 7u))
               == VP_MEMORY_ATTRIBUTE_OK);
    }
    assert(table->count == CAP);
    check_canonical(table);
}

static void fill_256_with_12k(VPMemoryAttributes *table) {
    memset(table, 0, sizeof(*table));
    for (unsigned i = 0; i < CAP - 1; ++i) {
        const uint32_t base = 0x100000u + i * 0x2000u;
        assert(vp_memory_attributes_set(table, base, PAGE, 0xF, 1u + (i % 7u))
               == VP_MEMORY_ATTRIBUTE_OK);
    }
    assert(vp_memory_attributes_set(table, 0x800000u, 3 * PAGE, 0xF, 8)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table->count == CAP);
    check_canonical(table);
}

static void test_capacity_and_transactional_failure(void) {
    VPMemoryAttributes table, snapshot;
    fill_256_4k(&table);
    snapshot = table;
    /* A new interval in the gap would need 257. */
    assert(vp_memory_attributes_set(&table, 0x100000u + PAGE, PAGE, 0xF, 8)
           == VP_MEMORY_ATTRIBUTE_FULL);
    check_equal(&table, &snapshot);
    /* Clearing an unrelated range is not a new interval and must succeed. */
    assert(vp_memory_attributes_set(&table, 0x100000u, PAGE, 0xF, 0)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == CAP - 1);

    /* A masked update of the middle page of a 12 KiB interval splits one
     * interval into three, taking the table from 256 to 258. */
    fill_256_with_12k(&table);
    snapshot = table;
    assert(vp_memory_attributes_set(&table, 0x801000u, PAGE, 0xF, 9)
           == VP_MEMORY_ATTRIBUTE_FULL);
    check_equal(&table, &snapshot);

    /* Forget needs a split of a 12 KiB interval into two, taking the table
     * from 256 to 257; the original state must survive the rejection. */
    assert(vp_memory_attributes_forget(&table, 0x801000u, PAGE)
           == VP_MEMORY_ATTRIBUTE_FULL);
    check_equal(&table, &snapshot);
    /* The same forget on a table with room succeeds and leaves the two
     * surviving sides. */
    assert(vp_memory_attributes_set(&table, 0x100000u, PAGE, 0xF, 0)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == CAP - 1);
    assert(vp_memory_attributes_forget(&table, 0x801000u, PAGE)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == CAP);
    assert(vp_memory_attributes_at(&table, 0x800000u) == 8);
    assert(vp_memory_attributes_at(&table, 0x801000u) == 0);
    assert(vp_memory_attributes_at(&table, 0x802000u) == 8);
    puts("PASS 256 live intervals; set/forget capacity failures leave state unchanged");
}

static void test_forget_splits(void) {
    VPMemoryAttributes table;
    memset(&table, 0, sizeof(table));
    assert(vp_memory_attributes_set(&table, 0x500000, 5 * PAGE, 0xF, 1)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(vp_memory_attributes_forget(&table, 0x501000, PAGE)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == 2);
    assert(table.ranges[0].base == 0x500000 && table.ranges[0].size == PAGE);
    assert(table.ranges[1].base == 0x502000 && table.ranges[1].size == 3 * PAGE);
    assert(vp_memory_attributes_forget(&table, 0x502000, 3 * PAGE)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == 1 && table.ranges[0].base == 0x500000);
    assert(vp_memory_attributes_forget(&table, 0x500000, PAGE)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == 0);
    /* A range that covers nothing is accepted and changes nothing. */
    assert(vp_memory_attributes_forget(&table, 0x900000, PAGE)
           == VP_MEMORY_ATTRIBUTE_OK);
    puts("PASS forget splits partially covered intervals");
}

static void test_top_address_and_invalid_ranges(void) {
    VPMemoryAttributes table, snapshot;
    memset(&table, 0, sizeof(table));
    /* 0xFFFFF000 + 0x1000 is exactly 0x100000000 and is representable. */
    assert(vp_memory_attributes_set(&table, 0xFFFFF000u, PAGE, 0xF, 8)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(vp_memory_attributes_at(&table, 0xFFFFF000u) == 8);
    assert(vp_memory_attributes_at(&table, 0xFFFFFFFFu) == 8);
    assert(vp_memory_attributes_start(&table, 0xFFFFFFFFu) == 0xFFFFF000u);
    assert(vp_memory_attributes_end(&table, 0xFFFFFFFFu)
           == VP_MEMORY_ATTRIBUTE_END);
    snapshot = table;
    /* address + size above 2^32 is a real wrap. */
    assert(vp_memory_attributes_set(&table, 0xFFFFF000u, 2 * PAGE, 0xF, 8)
           == VP_MEMORY_ATTRIBUTE_INVALID);
    check_equal(&table, &snapshot);
    assert(vp_memory_attributes_forget(&table, 0xFFFFF000u, 2 * PAGE)
           == VP_MEMORY_ATTRIBUTE_INVALID);
    check_equal(&table, &snapshot);
    /* Unaligned, zero-sized, out-of-mask and non-subset attributes fail
     * without changing the table. */
    assert(vp_memory_attributes_set(&table, 0xFFFFF800u, PAGE, 0xF, 8)
           == VP_MEMORY_ATTRIBUTE_INVALID);
    assert(vp_memory_attributes_set(&table, 0xFFFFF000u, 0, 0xF, 8)
           == VP_MEMORY_ATTRIBUTE_INVALID);
    assert(vp_memory_attributes_set(&table, 0xFFFFF000u, PAGE, 0x10, 0)
           == VP_MEMORY_ATTRIBUTE_INVALID);
    assert(vp_memory_attributes_set(&table, 0xFFFFF000u, PAGE, 0x1, 0x2)
           == VP_MEMORY_ATTRIBUTE_INVALID);
    check_equal(&table, &snapshot);
    /* Forgetting the top page clears it. */
    assert(vp_memory_attributes_forget(&table, 0xFFFFF000u, PAGE)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(table.count == 0);
    assert(vp_memory_attributes_end(&table, 0xFFFFFFFFu)
           == VP_MEMORY_ATTRIBUTE_END);
    puts("PASS top page endpoint 0x100000000 and invalid wrap rejection");
}

static void test_zero_gap_boundaries(void) {
    VPMemoryAttributes table;
    memset(&table, 0, sizeof(table));
    assert(vp_memory_attributes_set(&table, 0x300000, PAGE, 0xF, 4)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(vp_memory_attributes_set(&table, 0x302000, PAGE, 0xF, 4)
           == VP_MEMORY_ATTRIBUTE_OK);
    assert(vp_memory_attributes_at(&table, 0x301000) == 0);
    assert(vp_memory_attributes_start(&table, 0x301000) == 0x301000);
    assert(vp_memory_attributes_end(&table, 0x301000) == 0x302000);
    assert(vp_memory_attributes_start(&table, 0x200000) == 0);
    assert(vp_memory_attributes_end(&table, 0x200000) == 0x300000);
    assert(vp_memory_attributes_end(&table, 0x400000)
           == VP_MEMORY_ATTRIBUTE_END);
    puts("PASS zero-attribute gap boundaries");
}

int main(void) {
    test_mask_update_and_merge();
    test_all_bits();
    test_repeated_cycles();
    test_random_against_page_array();
    test_capacity_and_transactional_failure();
    test_forget_splits();
    test_top_address_and_invalid_ranges();
    test_zero_gap_boundaries();
    puts("ALL MEMORY ATTRIBUTE TESTS PASSED");
    return 0;
}

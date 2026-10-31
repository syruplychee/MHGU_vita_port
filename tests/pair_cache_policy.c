/* Host regression for the static pair-cache selection policy.
 *
 * Covers the two historical defects: a full cache always evicted slot 0
 * instead of the least-recently-used slot, and a free hole stopped the scan
 * before a later live match could be found. Build with -fsanitize=address,
 * undefined to keep the policy free of out-of-range slot reads.
 */
#include <stdint.h>
#include <stdio.h>

#include "pair_cache_policy.h"

static unsigned failures;

#define CHECK(condition, name)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("PAIR_CACHE_POLICY FAIL case=%s line=%d\n", (name),         \
                   __LINE__);                                                 \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* Mirrors the production scan loop: every slot is visited, slot predicates are
 * supplied by the caller, and a hit ends the scan only when the policy says so. */
static unsigned scan_cache(unsigned limit, const uint8_t *used,
                           const uint8_t *match, const uint32_t *last_use,
                           int *hit_seen) {
    VPPairCacheScan scan;
    unsigned i;
    vp_pair_cache_scan_begin(&scan);
    *hit_seen = 0;
    for (i = 0; i < limit; i++) {
        int is_free = !used[i];
        int is_hit = !is_free && match[i];
        if (vp_pair_cache_scan_slot(&scan, i, is_free, is_hit, last_use[i]) ==
            VP_PAIR_CACHE_SCAN_HIT) {
            *hit_seen = 1;
            break;
        }
    }
    return vp_pair_cache_scan_victim(&scan);
}

int main(void) {
    /* Full cache: the oldest slot is 1, not the first scanned slot 0. */
    {
        const uint8_t used[4] = {1, 1, 1, 1};
        const uint8_t match[4] = {0, 0, 0, 0};
        const uint32_t last_use[4] = {50u, 3u, 20u, 7u};
        int hit = 0;
        unsigned victim = scan_cache(4, used, match, last_use, &hit);
        CHECK(!hit, "full_miss_hit");
        CHECK(victim == 1u, "full_lru_non_first");
        CHECK(victim != 0u, "full_lru_not_slot_zero");
    }

    /* Failed-build hole at slot 1 hides a later match at slot 3. */
    {
        const uint8_t used[4] = {1, 0, 1, 1};
        const uint8_t match[4] = {0, 0, 0, 1};
        const uint32_t last_use[4] = {9u, 0u, 4u, 2u};
        int hit = 0;
        unsigned victim = scan_cache(4, used, match, last_use, &hit);
        CHECK(hit, "hole_later_match_found");
        /* No hit continuation: victim would still be the hole, not the LRU. */
        CHECK(victim == 1u, "hole_tracked_separately");
    }

    /* A hit after a hole wins even though the hole was recorded first. */
    {
        const uint8_t used[4] = {0, 1, 0, 1};
        const uint8_t match[4] = {0, 0, 0, 1};
        const uint32_t last_use[4] = {0u, 80u, 0u, 1u};
        int hit = 0;
        unsigned victim = scan_cache(4, used, match, last_use, &hit);
        CHECK(hit, "hole_hit_priority");
        CHECK(victim == 0u, "hole_hit_free_first");
    }

    /* Full cache with equal last_use keeps the first scanned as LRU. */
    {
        const uint8_t used[3] = {1, 1, 1};
        const uint8_t match[3] = {0, 0, 0};
        const uint32_t last_use[3] = {5u, 5u, 9u};
        int hit = 0;
        unsigned victim = scan_cache(3, used, match, last_use, &hit);
        CHECK(victim == 0u, "equal_last_use_first");
    }

    /* Empty cache: the first hole is the victim and no hit is reported. */
    {
        const uint8_t used[2] = {0, 0};
        const uint8_t match[2] = {0, 0};
        const uint32_t last_use[2] = {0u, 0u};
        int hit = 0;
        unsigned victim = scan_cache(2, used, match, last_use, &hit);
        CHECK(!hit, "empty_no_hit");
        CHECK(victim == 0u, "empty_first_hole");
    }

    /* Last_use rollover: 0xffffffff is a valid age, not a "no slot" marker. */
    {
        const uint8_t used[3] = {1, 1, 1};
        const uint8_t match[3] = {0, 0, 0};
        const uint32_t last_use[3] = {0xffffffffu, 2u, 1u};
        int hit = 0;
        unsigned victim = scan_cache(3, used, match, last_use, &hit);
        CHECK(victim == 2u, "large_age_oldest");
    }

    /* Limit 0 must not fabricate a slot. */
    {
        VPPairCacheScan scan;
        vp_pair_cache_scan_begin(&scan);
        CHECK(vp_pair_cache_scan_victim(&scan) == VP_PAIR_CACHE_NO_SLOT,
              "no_slot_sentinel");
    }

    if (failures != 0) {
        printf("PAIR_CACHE_POLICY failures=%u\n", failures);
        return 1;
    }
    printf("PAIR_CACHE_POLICY failures=0 cases=7\n");
    return 0;
}

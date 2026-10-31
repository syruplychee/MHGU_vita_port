/*
 * Pure selection policy for the static program-pair cache.
 *
 * The scan must visit every slot: failed pair builds leave free holes, and a
 * hole must not hide a later live match. A free slot is tracked separately
 * from the LRU victim so a full cache never freezes its victim on the first
 * scanned slot. Keep this file free of Vita SDK dependencies so the policy is
 * host-testable.
 */
#ifndef VP_PAIR_CACHE_POLICY_H
#define VP_PAIR_CACHE_POLICY_H

#include <stdint.h>

#define VP_PAIR_CACHE_NO_SLOT 0xffffffffu

typedef struct {
    unsigned free_slot;
    unsigned lru_slot;
    uint32_t oldest;
} VPPairCacheScan;

enum {
    VP_PAIR_CACHE_SCAN_CONTINUE = 0,
    VP_PAIR_CACHE_SCAN_HIT = 1
};

static inline void vp_pair_cache_scan_begin(VPPairCacheScan *scan) {
    scan->free_slot = VP_PAIR_CACHE_NO_SLOT;
    scan->lru_slot = VP_PAIR_CACHE_NO_SLOT;
    scan->oldest = 0xffffffffu;
}

/* Note one slot. `is_hit` wins immediately; `is_free` records the first hole
 * but never ends the scan. Used non-matching slots update the LRU victim on
 * every strictly older last_use, regardless of the current free slot. */
static inline int vp_pair_cache_scan_slot(VPPairCacheScan *scan, unsigned index,
                                          int is_free, int is_hit,
                                          uint32_t last_use) {
    if (is_hit) return VP_PAIR_CACHE_SCAN_HIT;
    if (is_free) {
        if (scan->free_slot == VP_PAIR_CACHE_NO_SLOT) scan->free_slot = index;
        return VP_PAIR_CACHE_SCAN_CONTINUE;
    }
    if (scan->lru_slot == VP_PAIR_CACHE_NO_SLOT || last_use < scan->oldest) {
        scan->lru_slot = index;
        scan->oldest = last_use;
    }
    return VP_PAIR_CACHE_SCAN_CONTINUE;
}

/* Prefer reusing a hole over evicting live content; evict the true LRU slot
 * only when the cache has no free slot. */
static inline unsigned vp_pair_cache_scan_victim(const VPPairCacheScan *scan) {
    return scan->free_slot != VP_PAIR_CACHE_NO_SLOT ? scan->free_slot
                                                    : scan->lru_slot;
}

#endif /* VP_PAIR_CACHE_POLICY_H */

#ifndef VP_GUEST_HEAP_CONFIG_H
#define VP_GUEST_HEAP_CONFIG_H
#include <stddef.h>
#include <stdint.h>

#define VP_GUEST_HEAP_DEFAULT_BYTES 0x76700000u
#define VP_GUEST_ALIAS_BYTES (8u << 20)
#define VP_GUEST_STACK_BYTES (16u << 20)

/* A startup-only experiment. The reported heap and its real backing must use
 * the same capacity; invalid controls must never select a silent fallback. */
static inline int vp_guest_heap_parse(const char *text, size_t length,
                                      uint32_t *heap_bytes) {
    unsigned mib = 0u;
    size_t at = 0u;
    while (at < length && text[at] >= '0' && text[at] <= '9') {
        mib = mib * 10u + (unsigned)(text[at++] - '0');
        if (mib > 1895u) return -1;
    }
    if (!at || mib < 128u) return -1;
    for (; at < length; ++at) {
        char ch = text[at];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') return -1;
    }
    *heap_bytes = mib << 20;
    return 0;
}

static inline uint32_t vp_guest_pool_bytes(uint32_t heap_bytes) {
    return heap_bytes + VP_GUEST_ALIAS_BYTES + VP_GUEST_STACK_BYTES;
}
#endif

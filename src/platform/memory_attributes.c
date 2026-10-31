#include "memory_attributes.h"

#include <string.h>

#define VP_MEMORY_PAGE_MASK 0xFFFu

static int valid_range(uint32_t base, uint32_t size) {
    return size != 0
        && (base & VP_MEMORY_PAGE_MASK) == 0
        && (size & VP_MEMORY_PAGE_MASK) == 0
        && (uint64_t)base + size <= VP_MEMORY_ATTRIBUTE_END;
}

/* Append [base, end) with one effective attribute. Adjacent intervals that
 * carry the same nonzero attribute are merged so the canonical table never
 * depends on the order in which equivalent updates arrived. */
static int push_range(VPMemoryAttributeRange *out, unsigned *count,
                      uint64_t base, uint64_t end, uint32_t attr) {
    if (attr == 0 || end <= base) return VP_MEMORY_ATTRIBUTE_OK;
    if (base > 0xFFFFFFFFull || end > VP_MEMORY_ATTRIBUTE_END
        || end - base > 0xFFFFFFFFull)
        return VP_MEMORY_ATTRIBUTE_INVALID;
    if (*count) {
        VPMemoryAttributeRange *previous = &out[*count - 1];
        if ((uint64_t)previous->base + previous->size == base
            && previous->attr == attr) {
            if (end - previous->base > 0xFFFFFFFFull)
                return VP_MEMORY_ATTRIBUTE_INVALID;
            previous->size = (uint32_t)(end - previous->base);
            return VP_MEMORY_ATTRIBUTE_OK;
        }
    }
    if (*count >= VP_MEMORY_ATTRIBUTE_LIMIT) return VP_MEMORY_ATTRIBUTE_FULL;
    out[*count].base = (uint32_t)base;
    out[*count].size = (uint32_t)(end - base);
    out[*count].attr = attr;
    ++*count;
    return VP_MEMORY_ATTRIBUTE_OK;
}

int vp_memory_attributes_set(VPMemoryAttributes *table, uint32_t address,
                             uint32_t size, uint32_t mask, uint32_t attr) {
    VPMemoryAttributeRange out[VP_MEMORY_ATTRIBUTE_LIMIT];
    unsigned count = 0;
    int status = VP_MEMORY_ATTRIBUTE_OK;

    if (!table || !valid_range(address, size)
        || (mask & ~VP_MEMORY_ATTRIBUTE_BITS)
        || (attr & ~VP_MEMORY_ATTRIBUTE_BITS)
        || (attr & ~mask))
        return VP_MEMORY_ATTRIBUTE_INVALID;

    const uint64_t start = address;
    const uint64_t end = (uint64_t)address + size;
    const uint32_t set_attr = attr & mask;
    uint64_t cursor = start;

    /* The output is a canonical merge of three pieces per overlapping
     * interval: the unchanged left part, the masked intersection, and the
     * unchanged right part. Attributed-zero gaps inside the update range get
     * exactly the requested bits. Building into a temporary array keeps the
     * operation transactional when a split would exceed the bound. */
    for (unsigned i = 0; i < table->count; ++i) {
        const VPMemoryAttributeRange *r = &table->ranges[i];
        const uint64_t a = r->base;
        const uint64_t b = a + r->size;
        if (b <= start) {
            status = push_range(out, &count, a, b, r->attr);
        } else if (a >= end) {
            if (cursor < end) {
                status = push_range(out, &count, cursor, end, set_attr);
                cursor = end;
            }
            if (status == VP_MEMORY_ATTRIBUTE_OK)
                status = push_range(out, &count, a, b, r->attr);
        } else {
            if (a < start)
                status = push_range(out, &count, a, start, r->attr);
            if (status == VP_MEMORY_ATTRIBUTE_OK && a > cursor) {
                const uint64_t gap_end = a < end ? a : end;
                status = push_range(out, &count, cursor, gap_end, set_attr);
            }
            if (status == VP_MEMORY_ATTRIBUTE_OK) {
                const uint64_t hit_start = a > start ? a : start;
                const uint64_t hit_end = b < end ? b : end;
                status = push_range(out, &count, hit_start, hit_end,
                                    (r->attr & ~mask) | set_attr);
            }
            cursor = b < end ? b : end;
            if (status == VP_MEMORY_ATTRIBUTE_OK && b > end)
                status = push_range(out, &count, end, b, r->attr);
        }
        if (status != VP_MEMORY_ATTRIBUTE_OK) return status;
    }
    if (cursor < end) {
        status = push_range(out, &count, cursor, end, set_attr);
        if (status != VP_MEMORY_ATTRIBUTE_OK) return status;
    }

    memcpy(table->ranges, out, count * sizeof(out[0]));
    table->count = count;
    return VP_MEMORY_ATTRIBUTE_OK;
}

int vp_memory_attributes_forget(VPMemoryAttributes *table, uint32_t base,
                                uint32_t size) {
    VPMemoryAttributeRange out[VP_MEMORY_ATTRIBUTE_LIMIT];
    unsigned count = 0;
    int status = VP_MEMORY_ATTRIBUTE_OK;

    if (!table || !valid_range(base, size))
        return VP_MEMORY_ATTRIBUTE_INVALID;

    const uint64_t start = base;
    const uint64_t end = (uint64_t)base + size;

    for (unsigned i = 0; i < table->count; ++i) {
        const VPMemoryAttributeRange *r = &table->ranges[i];
        const uint64_t a = r->base;
        const uint64_t b = a + r->size;
        if (b <= start || a >= end) {
            status = push_range(out, &count, a, b, r->attr);
        } else {
            /* Only one interval can contain the whole range; an interval that
             * merely straddles one edge contributes its surviving side. */
            if (a < start) status = push_range(out, &count, a, start, r->attr);
            if (status == VP_MEMORY_ATTRIBUTE_OK && b > end)
                status = push_range(out, &count, end, b, r->attr);
        }
        if (status != VP_MEMORY_ATTRIBUTE_OK) return status;
    }

    memcpy(table->ranges, out, count * sizeof(out[0]));
    table->count = count;
    return VP_MEMORY_ATTRIBUTE_OK;
}

uint32_t vp_memory_attributes_at(const VPMemoryAttributes *table,
                                 uint32_t address) {
    if (!table) return 0;
    unsigned low = 0, high = table->count;
    while (low < high) {
        const unsigned middle = low + (high - low) / 2;
        const VPMemoryAttributeRange *r = &table->ranges[middle];
        if (address < r->base) {
            high = middle;
        } else if (address - r->base < r->size) {
            return r->attr;
        } else {
            low = middle + 1;
        }
    }
    return 0;
}

uint64_t vp_memory_attributes_end(const VPMemoryAttributes *table,
                                  uint32_t address) {
    if (!table) return VP_MEMORY_ATTRIBUTE_END;
    unsigned low = 0, high = table->count;
    while (low < high) {
        const unsigned middle = low + (high - low) / 2;
        const VPMemoryAttributeRange *r = &table->ranges[middle];
        if (address < r->base) {
            high = middle;
        } else if (address - r->base < r->size) {
            return (uint64_t)r->base + r->size;
        } else {
            low = middle + 1;
        }
    }
    if (low < table->count) return table->ranges[low].base;
    return VP_MEMORY_ATTRIBUTE_END;
}

uint64_t vp_memory_attributes_start(const VPMemoryAttributes *table,
                                    uint32_t address) {
    unsigned low = 0, high = table->count;
    uint64_t previous_end = 0;
    while (low < high) {
        const unsigned middle = low + (high - low) / 2;
        const VPMemoryAttributeRange *r = &table->ranges[middle];
        if (address < r->base) {
            high = middle;
        } else if (address - r->base < r->size) {
            return r->base;
        } else {
            previous_end = (uint64_t)r->base + r->size;
            low = middle + 1;
        }
    }
    return previous_end;
}

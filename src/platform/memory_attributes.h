#ifndef VP_MEMORY_ATTRIBUTES_H
#define VP_MEMORY_ATTRIBUTES_H

#include <stdint.h>

/* Effective svcSetMemoryAttribute state.
 *
 * The kernel's authoritative representation is the attribute of each mapped
 * page: a later masked set or clear overwrites the earlier value, and an
 * svcUnmapMemory drops the attributes of the unmapped pages. Recording the
 * cumulative event history instead is both semantically wrong for QueryMemory
 * and unbounded across resource reloads. This table keeps the resulting state
 * as sorted, disjoint intervals that each carry one nonzero attribute.
 *
 * Endpoints are computed in 64 bits so the page at 0xFFFFF000 can end at
 * 0x100000000 without wrapping to zero. Any range whose end exceeds
 * 0x100000000 is rejected as a real wrap. The table has fixed capacity and no
 * heap backing; an operation that would need more intervals than that fails
 * without changing the existing state. */
#define VP_MEMORY_ATTRIBUTE_LIMIT 256u
#define VP_MEMORY_ATTRIBUTE_BITS 0xFu
#define VP_MEMORY_ATTRIBUTE_END 0x100000000ull

typedef struct {
    uint32_t base;
    uint32_t size;
    uint32_t attr;
} VPMemoryAttributeRange;

typedef struct {
    VPMemoryAttributeRange ranges[VP_MEMORY_ATTRIBUTE_LIMIT];
    unsigned count;
} VPMemoryAttributes;

enum {
    VP_MEMORY_ATTRIBUTE_OK = 0,
    VP_MEMORY_ATTRIBUTE_INVALID = -1,
    VP_MEMORY_ATTRIBUTE_FULL = -2
};

/* Apply svcSetMemoryAttribute(address, size, mask, attr) to the effective
 * state. Only the bits selected by mask change; pages outside the range keep
 * their attributes. attr must be a subset of mask and both are limited to the
 * four defined attribute bits. Returns INVALID for malformed input, FULL if
 * the canonical result needs more than VP_MEMORY_ATTRIBUTE_LIMIT intervals,
 * and leaves the table untouched on any failure. */
int vp_memory_attributes_set(VPMemoryAttributes *table, uint32_t address,
                             uint32_t size, uint32_t mask, uint32_t attr);

/* Drop every attribute inside [base, base + size), splitting intervals that
 * extend outside the range. Same transactional failure and capacity rules as
 * vp_memory_attributes_set. */
int vp_memory_attributes_forget(VPMemoryAttributes *table, uint32_t base,
                                uint32_t size);

/* Effective attribute at address, or 0 when the address is outside every
 * nonzero interval. */
uint32_t vp_memory_attributes_at(const VPMemoryAttributes *table,
                                 uint32_t address);

/* First address after the effective unit that contains address: the end of a
 * nonzero interval, the start of the next interval for an attributed-zero
 * gap, or 0x100000000 after the last interval. QueryMemory uses this to avoid
 * reporting a block with more than one attribute as homogeneous. */
uint64_t vp_memory_attributes_end(const VPMemoryAttributes *table,
                                  uint32_t address);

/* First address of the effective unit that contains address: the start of a
 * nonzero interval, or the end of the previous interval (zero when none) for
 * an attributed-zero gap. */
uint64_t vp_memory_attributes_start(const VPMemoryAttributes *table,
                                    uint32_t address);

#endif

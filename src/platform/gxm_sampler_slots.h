/* Bounded fragment sampler layout shared by the guest descriptor resolver and
 * the GXM worker.
 *
 * The translated fragment ABI names its samplers tex0..tex4 and exposes their
 * guest texture handles as (bank 0, count 1, shift 0, size_shift 3) records at
 * byte offsets 32, 40, 48, 56 and 64. Five is the supported limit; a program
 * that declares a sixth sampler or a sampler with any other name is refused
 * instead of leaving a stale or unbound texture unit behind.
 *
 * B119: the B118 package's quest stage set uses tex0..tex4 (five samplers).
 * The resource sidecar already carried the slot-4 handle at offset 64, but this
 * limit refused those programs at resident registration, so every quest draw
 * using them failed and retried the register/reflect/unregister cycle.
 *
 * Vertex programs keep their existing single tex0 resource. This header is
 * deliberately free of VitaSDK types so the host tests can compile it. */
#ifndef VP_GXM_SAMPLER_SLOTS_H
#define VP_GXM_SAMPLER_SLOTS_H

#include <stddef.h>

#define VP_GXM_FRAGMENT_SAMPLER_LIMIT 5u

/* Byte offset in the bound constant bank of the TIC handle for a fragment
 * sampler. The bank is always bank 0 with one descriptor per slot. Returns -1
 * for slot >= VP_GXM_FRAGMENT_SAMPLER_LIMIT. */
static inline int vp_gxm_fragment_sampler_offset(unsigned sampler_index,
                                                 unsigned *offset) {
    if (offset == NULL || sampler_index >= VP_GXM_FRAGMENT_SAMPLER_LIMIT)
        return -1;
    *offset = 32u + 8u * sampler_index;
    return 0;
}

/* Parse an exact "texN" name with N in [0, VP_GXM_FRAGMENT_SAMPLER_LIMIT).
 * Returns -1 for any other name, including a too-large index. */
static inline int vp_gxm_fragment_sampler_slot(const char *name,
                                               unsigned *slot) {
    unsigned value;
    if (name == NULL || slot == NULL) return -1;
    if (name[0] != 't' || name[1] != 'e' || name[2] != 'x' ||
        name[3] < '0' || name[3] > '9' || name[4] != '\0')
        return -1;
    value = (unsigned)(name[3] - '0');
    if (value >= VP_GXM_FRAGMENT_SAMPLER_LIMIT) return -1;
    *slot = value;
    return 0;
}

/* Parse an exact "texN" name for a fragment or vertex stage. Fragment samplers
 * accept tex0..tex4; the vertex stage keeps its existing single tex0 resource.
 * Returns -1 for an unknown name, an out-of-range index or a vertex tex1..tex4.
 */
static inline int vp_gxm_sampler_name_slot(const char *name, unsigned vertex,
                                           unsigned *slot) {
    unsigned value;
    if (slot == NULL) return -1;
    if (vp_gxm_fragment_sampler_slot(name, &value) < 0) return -1;
    if (vertex && value != 0u) return -1;
    *slot = value;
    return 0;
}

#endif

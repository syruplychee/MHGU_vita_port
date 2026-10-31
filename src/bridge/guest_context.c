#include "guest_context.h"
#include <string.h>

#define LEAF_LIMIT 4u
VPGuestContext **vp_guest_stack_directory[256];
static VPGuestContext *leaves[LEAF_LIMIT][256];
static uint32_t used, sealed;

void vp_guest_registry_reset(void) {
    /* Caller guarantees that no guest execution or lookup can still be in flight. */
    memset(vp_guest_stack_directory,0,sizeof(vp_guest_stack_directory));
    memset(leaves,0,sizeof(leaves)); used=0; sealed=0;
}
int vp_guest_stack_register(uint32_t base,uint32_t size,VPGuestContext *context) {
    if (sealed) return VP_GUEST_SEALED;
    if (!context || !base || (base&65535u) || !size || (size&65535u)
        || size>4u*1024u*1024u || base>UINT32_MAX-size) return VP_GUEST_ARGUMENT;
    unsigned char missing[256]={0}; uint32_t needed=0;
    for (uint32_t offset=0;offset<size;offset+=65536u) {
        uint32_t address=base+offset, hi=address>>24, lo=(address>>16)&255u;
        VPGuestContext **leaf=vp_guest_stack_directory[hi];
        if (leaf && leaf[lo]) return VP_GUEST_CONFLICT;
        if (!leaf && !missing[hi]) { missing[hi]=1; ++needed; }
    }
    if (needed>LEAF_LIMIT-used) return VP_GUEST_CAPACITY;
    for (uint32_t hi=0;hi<256;++hi) if (missing[hi]) vp_guest_stack_directory[hi]=leaves[used++];
    for (uint32_t offset=0;offset<size;offset+=65536u) {
        uint32_t address=base+offset;
        vp_guest_stack_directory[address>>24][(address>>16)&255u]=context;
    }
    return 0;
}
void vp_guest_registry_seal(void) { sealed=1; }
VPGuestContext *vp_guest_context_lookup(uint32_t sp) {
    if (!sealed) return NULL;
    VPGuestContext **leaf=vp_guest_stack_directory[sp>>24];
    return leaf ? leaf[(sp>>16)&255u] : NULL;
}

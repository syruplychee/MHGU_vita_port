#ifndef VP_NATIVE_ARENA_H
#define VP_NATIVE_ARENA_H
#include "native_link.h"
typedef struct {
    uint32_t image_bytes, island_offset, island_capacity, capacity;
} VPArenaLayout;
/* Islands are module-relative; capacity covers every byte and page touched.
 * Zero island bytes does not reserve an unused megabyte or executable page. */
int vp_native_arena_layout(uint32_t span,uint32_t island_bytes,VPArenaLayout *);
/* Check the entire reserved arenas, not just the NSO spans. native_addresses
 * requires image == base, as it must for direct ARM load/store execution. */
int vp_native_arenas_check(const VPNativeModule *,uint32_t,int native_addresses);
#endif

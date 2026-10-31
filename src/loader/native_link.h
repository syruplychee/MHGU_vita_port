#ifndef VP_NATIVE_LINK_H
#define VP_NATIVE_LINK_H
#include "nso_loader.h"
enum { VP_LINK_METADATA=-60, VP_LINK_SYMBOL=-61, VP_LINK_UNRESOLVED=-62,
       VP_LINK_RELOCATION=-63, VP_LINK_STATE=-64, VP_LINK_PREIMAGE=-65 };
typedef struct {
    VPNSO nso;
    unsigned char *image;
    uint32_t base, state, dynamic, dynamic_bytes, mod0;
    uint32_t hash, buckets, symbol_count, rel, rel_bytes, plt, plt_bytes, got;
    uint32_t init, fini, init_array, init_bytes, fini_array, fini_bytes;
    uint32_t capacity, island_offset, island_bytes;
} VPNativeModule;
typedef struct { uint32_t module, offset, before, after, type; } VPNativeWrite;
typedef struct {
    VPNativeWrite *writes;
    uint32_t count, modules, ready, committed, capacity;
    uint32_t bases[4];
    unsigned char *images[4];
    unsigned char before_sha256[4][32];
    unsigned char writes_sha256[32];
    VPNativeModule snapshot[4];
} VPNativePlan;
int vp_native_parse(VPNativeModule *);
int vp_native_plan(VPNativeModule *,uint32_t,const uint32_t *,VPNativePlan *);
int vp_native_commit(VPNativeModule *,uint32_t,VPNativePlan *);
void vp_native_discard(VPNativePlan *);
int vp_native_export(const VPNativeModule *,const char *,uint32_t *);
#endif

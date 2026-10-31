#ifndef VP_NATIVE_STARTUP_H
#define VP_NATIVE_STARTUP_H
#include "native_link.h"
enum { VP_STARTUP_ARGUMENT=-80, VP_STARTUP_STATE=-81, VP_STARTUP_NOT_READY=-82,
       VP_STARTUP_BACKEND=-83, VP_STARTUP_EXECUTION=-84 };
/* These are supplied by verified platform/patch integration, never inferred
 * from the fact that a symbol resolved. Both callbacks propagate real failure. */
enum { VP_READY_PERMISSIONS=1, VP_READY_CACHE=2, VP_READY_INSTRUCTIONS=4,
       VP_READY_SERVICES=8, VP_READY_SDK_CONNECTIONS=16, VP_READY_GUEST_CONTEXT=32,
       VP_READY_UNWIND=64, VP_READY_ALL=127 };
typedef int (*VPProtect)(void *,uint32_t,uint32_t,unsigned); /* R=1 W=2 X=4 */
typedef int (*VPFlush)(void *,uint32_t,uint32_t);
typedef int (*VPGuestCall)(void *,uint32_t,uint32_t);
typedef struct {
    uint32_t phase, ready, initialized_mask, init_order[4], init_count;
    uint32_t bases[4], count, scope[4], entry_module, entry_offset;
    VPNativeModule *modules;
    VPNativeModule snapshot[4];
    unsigned char arrays_sha256[4][2][32];
    uint32_t failed_module, failed_address, failed_phase, fini_failed_mask;
    int failure;
} VPStartup;
/* Checks every DT_INIT/DT_FINI and every relocated array target before any
 * constructor can have side effects. Arrays remain owned by original wrappers. */
int vp_native_lifecycle_check(const VPNativeModule *,uint32_t);
/* Protection is closed on failure where possible; original failure is returned.
 * Arena offsets are relative to the module; gaps get no permissions. */
int vp_native_protect(VPNativeModule *,uint32_t,VPProtect,VPFlush,void *);
int vp_startup_prepare(VPStartup *,VPNativeModule *,uint32_t,const uint32_t *,uint32_t,uint32_t,uint32_t);
/* Reverse semantic order, excludes rtld (index 0); call DT_INIT wrapper only.
 * Never separately calls INIT_ARRAY: that would double initialize this build. */
int vp_startup_initialize(VPStartup *,VPGuestCall,void *);
int vp_startup_enter(VPStartup *,VPGuestCall,void *);
/* After guest workers stop, reverse successful initialization; DT_FINI wrappers
 * own their arrays. Partial failures are not retried after side effects. */
int vp_startup_finalize(VPStartup *,VPGuestCall,void *);
#endif

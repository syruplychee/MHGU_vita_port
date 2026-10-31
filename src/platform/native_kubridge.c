#include "native_kubridge.h"
#include <psp2/types.h>
/* Public API facts from bythos14/kubridge exceptions_mprotect/kubridge.h.
 * No kernel/plugin implementation is copied into this project. */
extern int kuKernelMemProtect(void *,SceSize,SceUInt32);
extern void kuKernelFlushCaches(const void *,SceSize);
static int protect(void *ctx,uint32_t address,uint32_t length,unsigned flags) {
    (void)ctx;
    uint32_t protection=(flags&1?0x40u:0)|(flags&2?0x20u:0)|(flags&4?0x10u:0);
    return kuKernelMemProtect((void *)(uintptr_t)address,length,protection);
}
static int flush(void *ctx,uint32_t address,uint32_t length) {
    (void)ctx;kuKernelFlushCaches((void *)(uintptr_t)address,length);return 0;
}
int vp_vita_module_permissions(VPNativeModule *modules,uint32_t count) {
    return vp_native_protect(modules,count,protect,flush,NULL);
}

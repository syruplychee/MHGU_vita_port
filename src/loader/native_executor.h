#ifndef VP_NATIVE_EXECUTOR_H
#define VP_NATIVE_EXECUTOR_H
#include "native_startup.h"
#include "host_bridge.h"
typedef struct VPExecutor VPExecutor;
typedef int (*VPExecutorHandler)(VPExecutor *,VPBridgeFrame *);
struct VPExecutor {
    VPHostBridge bridge;
    VPNativeModule *modules;
    uint32_t count,stack_base,stack_bytes,stack_top,calls,active;
    uint32_t module,address;
    int failure;
    VPBridgeFrame fault_frame;
    VPExecutorHandler handler;
    void *user;
};
enum { VP_EXECUTOR_ARGUMENT=-100,VP_EXECUTOR_STATE=-101,VP_EXECUTOR_GUARD=-102 };
/* Owner-thread only. Storage/stack/TLS must outlive all guest workers. Adds the
 * aligned stack to the registry; caller seals it after registering every worker.
 * Stack canaries diagnose damage on return; they are not MMU guard pages. */
int vp_executor_prepare(VPExecutor *,VPNativeModule *,uint32_t,void *,uint32_t,
                        uint32_t,uint32_t,VPExecutorHandler,void *);
int vp_executor_invoke(VPExecutor *,uint32_t,uint32_t,const uint32_t[4],uint32_t *);
int vp_executor_invoke6(VPExecutor *,uint32_t,uint32_t,const uint32_t[6],uint32_t *);
/* Integer/pointer AAPCS arguments, up to 24 words. No variadic/VFP ABI claim. */
int vp_executor_invoke_args(VPExecutor *,uint32_t,uint32_t,const uint32_t *,uint32_t,uint32_t *);
int vp_executor_guest_call(void *,uint32_t,uint32_t);
int vp_executor_quiescent(const VPExecutor *);
#endif

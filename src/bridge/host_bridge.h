#ifndef VP_HOST_BRIDGE_H
#define VP_HOST_BRIDGE_H
#include "guest_context.h"
#include <stddef.h>

typedef struct {
    uint32_t service, reserved;
    uint64_t vfp[32];
    uint32_t apsr, fpscr, regs[13], lr, sp, pc;
} VPBridgeFrame;
typedef struct VPHostBridge VPHostBridge;
typedef int (*VPBridgeHandler)(VPHostBridge *, VPBridgeFrame *);
struct VPHostBridge {
    VPGuestContext guest;
    uint32_t host_sp, host_fpscr, guest_frame, state;
    VPBridgeHandler handler;
    void *user;
    uint32_t guest_fpscr;
    int32_t last_error;
};
enum { VP_BRIDGE_ARGUMENT=-1, VP_BRIDGE_BUSY=-2, VP_BRIDGE_STACK=-3, VP_BRIDGE_UNSUPPORTED=-4,
       VP_BRIDGE_SESSION_STOP=-5,
       /* The application's own applet exit request was acted on: the platform
        * model terminates the process, as ISelfController::Exit/UnlockExit does
        * when an exit is pending. Distinct from the loader's evidence stop. */
       VP_BRIDGE_PROCESS_EXIT=-6 };
_Static_assert(sizeof(VPBridgeFrame)==336 && offsetof(VPBridgeFrame,regs)==272,"saved guest frame ABI");
_Static_assert(offsetof(VPBridgeFrame,pc)==332 && offsetof(VPBridgeFrame,sp)==328,"continuation ABI");
_Static_assert(sizeof(VPHostBridge)==40 && offsetof(VPHostBridge,handler)==24,"bridge context ABI");
/* Owner-thread only. Registry must be sealed; guest must keep sufficient stack
 * headroom at every bridge site. Callback reentry into this context is rejected.
 * Handler may alter only its documented frame outputs and must restore host SP.
 */
int vp_bridge_invoke(VPHostBridge *,uint32_t,uint32_t,const uint32_t[4],uint32_t *);
uint32_t vp_bridge_enter(VPHostBridge *,uint32_t,uint32_t,const uint32_t[4]);
#endif

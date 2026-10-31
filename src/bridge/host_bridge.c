#include "host_bridge.h"

int vp_bridge_invoke(VPHostBridge *context,uint32_t sp,uint32_t pc,const uint32_t args[4],uint32_t *result) {
    if (!context || !args || !result || !context->handler || !pc || (!(pc&1u) && (pc&3u))) return VP_BRIDGE_ARGUMENT;
    if (context->state) return VP_BRIDGE_BUSY;
    if ((sp&7u) || sp<sizeof(VPBridgeFrame)
        || vp_guest_context_lookup(sp)!=&context->guest
        || vp_guest_context_lookup(sp-sizeof(VPBridgeFrame))!=&context->guest) return VP_BRIDGE_STACK;
    context->last_error=0; context->state=1;
    *result=vp_bridge_enter(context,sp,pc,args);
    context->state=0; context->guest_frame=0;
    if (context->last_error) *result=0;
    return context->last_error;
}

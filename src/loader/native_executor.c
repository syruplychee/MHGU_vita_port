#include "native_executor.h"
#include <string.h>
#define GUARD 0x715a3ce9u
static int guards(const VPExecutor *e) {
    const uint32_t *p=(const void *)(uintptr_t)e->stack_base;
    for (unsigned i=0;i<1024;++i) if (p[i]!=GUARD) return 0;
    return p[e->stack_bytes/4-1]==GUARD;
}
static int dispatch(VPHostBridge *bridge,VPBridgeFrame *frame) {
    VPExecutor *e=bridge->user;
    int rc=e->handler?e->handler(e,frame):VP_BRIDGE_UNSUPPORTED;
    if (rc) { e->failure=rc;memcpy(&e->fault_frame,frame,sizeof(*frame)); }
    return rc;
}
int vp_executor_prepare(VPExecutor *e,VPNativeModule *m,uint32_t count,void *stack,uint32_t bytes,
                        uint32_t tls_ro,uint32_t tls_rw,VPExecutorHandler handler,void *user) {
    uintptr_t base=(uintptr_t)stack;
    if (!e || !m || !count || count>4 || !stack || (base&65535) || bytes<0x20000
        || (bytes&65535) || base>UINT32_MAX-bytes) return VP_EXECUTOR_ARGUMENT;
    if (e->active || e->stack_bytes) return VP_EXECUTOR_STATE;
    VPExecutor next={0};next.modules=m;next.count=count;next.stack_base=(uint32_t)base;
    next.stack_bytes=bytes;next.stack_top=(uint32_t)base+bytes-128;next.handler=handler;next.user=user;
    next.bridge.guest=(VPGuestContext){tls_ro,tls_rw};next.bridge.handler=dispatch;next.bridge.user=e;
    /* Registration refers to the final context address, never this local copy. */
    int rc=vp_guest_stack_register((uint32_t)base,bytes,&e->bridge.guest);
    if (rc) return rc;
    *e=next;
    uint32_t *p=stack;for (unsigned i=0;i<1024;++i) p[i]=GUARD;p[bytes/4-1]=GUARD;
    return 0;
}
int vp_executor_invoke(VPExecutor *e,uint32_t module,uint32_t address,const uint32_t args[4],uint32_t *result) {
    if (!e || !args || !result || module>=e->count || !e->stack_bytes) return VP_EXECUTOR_ARGUMENT;
    if (e->active || e->bridge.state) return VP_EXECUTOR_STATE;
    const VPNativeModule *m=&e->modules[module];const VPNSOSegment *text=&m->nso.segments[0];
    uint32_t pc=address&~1u,bytes=(address&1)?2:4;
    if (m->state!=3 || (uintptr_t)m->image!=m->base || text->bytes>UINT32_MAX-text->offset
        || m->base>UINT32_MAX-text->offset-text->bytes || pc<m->base+text->offset
        || pc-(m->base+text->offset)>text->bytes || bytes>text->bytes-(pc-m->base-text->offset)
        || (!(address&1) && (address&3))) return VP_EXECUTOR_STATE;
    if (!guards(e)) return VP_EXECUTOR_GUARD;
    e->module=module;e->address=address;e->failure=0;memset(&e->fault_frame,0,sizeof(e->fault_frame));
    e->active=1;++e->calls;
    int rc=vp_bridge_invoke(&e->bridge,e->stack_top,address,args,result);
    e->active=0;
    if (!guards(e) && !rc) rc=VP_EXECUTOR_GUARD;
    e->failure=rc;return rc;
}
int vp_executor_guest_call(void *context,uint32_t module,uint32_t address) {
    uint32_t args[4]={0},result=0;
    return vp_executor_invoke(context,module,address,args,&result);
}
int vp_executor_invoke6(VPExecutor *e,uint32_t module,uint32_t address,const uint32_t args[6],uint32_t *result) {
    return vp_executor_invoke_args(e,module,address,args,6,result);
}
int vp_executor_invoke_args(VPExecutor *e,uint32_t module,uint32_t address,const uint32_t *args,uint32_t count,uint32_t *result) {
    if (!e || !args || count>24 || !e->stack_bytes || e->active || e->bridge.state) return VP_EXECUTOR_ARGUMENT;
    uintptr_t source=(uintptr_t)args;
    if (source>UINTPTR_MAX-count*4 || (source<e->stack_top+24*4 && source+count*4>e->stack_top)) return VP_EXECUTOR_ARGUMENT;
    uint32_t regs[4]={0},*stack=(void *)(uintptr_t)e->stack_top;
    for (unsigned i=0;i<4 && i<count;++i) regs[i]=args[i];
    memset(stack,0,24*4);
    if (count>4) memcpy(stack,args+4,(count-4)*4);
    int rc=vp_executor_invoke(e,module,address,regs,result);
    memset(stack,0,24*4);return rc;
}
int vp_executor_quiescent(const VPExecutor *e) { return e && !e->active && !e->bridge.state; }

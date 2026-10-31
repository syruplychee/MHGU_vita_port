/* Self-authored ARM fixtures; no original game routine is replaced by these. */
#include "native_lifecycle.h"
#include "native_executor.h"
#include <psp2/kernel/sysmem.h>
#include <string.h>
extern void vp_loader_fixture_init(void),vp_loader_fixture_fini(void),vp_loader_fixture_entry(void);
typedef struct { VPStartup startup;unsigned calls,events[8],fail_call,expected_fp; } Run;
static int handler(VPExecutor *executor,VPBridgeFrame *frame) {
    Run *run=executor->user;unsigned module=frame->regs[0];
    if (module>=4 || run->calls>=8 || frame->service!=(run->startup.phase==2?0x227u:run->startup.phase==7?0x228u:0x229u)
        || vp_guest_context_lookup(frame->sp)!=&executor->bridge.guest || frame->fpscr!=run->expected_fp) return -130;
    frame->fpscr^=0x00400000u;run->expected_fp=frame->fpscr;
    run->events[run->calls++]=module;
    if (vp_startup_initialize(&run->startup,vp_executor_guest_call,executor)!=VP_STARTUP_STATE
        || vp_startup_finalize(&run->startup,vp_executor_guest_call,executor)!=VP_STARTUP_STATE) return -131;
    return run->calls==run->fail_call?-132:0;
}
static void function(unsigned char *image,unsigned offset,unsigned module,void (*entry)(void)) {
    uint32_t words[4]={0xe3a00000u|module,0xe59fc000u,0xe12fff1cu,(uint32_t)(uintptr_t)entry};
    memcpy(image+offset,words,sizeof(words));
}
int vp_native_lifecycle_probe(FILE *log) {
    SceUID code=-1,stack=-1;void *image=NULL,*storage=NULL;VPNativeModule modules[4]={0};
    VPExecutor executor={0};Run run={0};uint32_t scope[4]={0,1,2,3},tls[128]={0};int rc=0,opened=0;
    code=sceKernelAllocMemBlockForVM("vp-lifecycle-fixtures",16384);if (code<0) rc=code;
    if (!rc) rc=sceKernelGetMemBlockBase(code,&image);
    SceKernelAllocMemBlockOpt options;memset(&options,0,sizeof(options));options.size=sizeof(options);
    options.attr=SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;options.alignment=0x10000;
    if (!rc) { stack=sceKernelAllocMemBlock("vp-lifecycle-stack",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,0x20000,&options);if (stack<0) rc=stack; }
    if (!rc) rc=sceKernelGetMemBlockBase(stack,&storage);
    if (!rc) {
        memset(image,0,16384);
        for (unsigned i=0;i<4;++i) {
            VPNativeModule *m=&modules[i];m->image=(unsigned char *)image+i*4096;m->base=(uint32_t)(uintptr_t)m->image;
            m->capacity=m->nso.span=m->nso.segments[0].bytes=4096;m->state=3;m->init=0x20;m->fini=0x40;
            function(m->image,0x20,i,vp_loader_fixture_init);function(m->image,0x40,i,vp_loader_fixture_fini);
            function(m->image,0x60,i,vp_loader_fixture_entry);
        }
        rc=vp_executor_prepare(&executor,modules,4,storage,0x20000,(uint32_t)(uintptr_t)tls,0,handler,&run);
    }
    if (!rc) { vp_guest_registry_seal();rc=sceKernelSyncVMDomain(code,image,16384); }
    if (!rc) { rc=sceKernelOpenVMDomain();if (!rc) opened=1; }
    unsigned cases=0;
    if (!rc) {
        static const unsigned expected[]={3,2,1,1,1,2,3};
        rc=vp_startup_prepare(&run.startup,modules,4,scope,VP_READY_ALL,1,0x60);
        if (!rc) rc=vp_startup_initialize(&run.startup,vp_executor_guest_call,&executor);
        if (!rc) rc=vp_startup_enter(&run.startup,vp_executor_guest_call,&executor);
        if (!rc) rc=vp_startup_finalize(&run.startup,vp_executor_guest_call,&executor);
        if (!rc && (run.calls!=7 || memcmp(expected,run.events,sizeof(expected)) || run.startup.phase!=8
            || vp_startup_finalize(&run.startup,vp_executor_guest_call,&executor)!=VP_STARTUP_STATE)) rc=-133;
        if (!rc) ++cases;
    }
    if (!rc) {
        memset(&run,0,sizeof(run));run.fail_call=2;run.expected_fp=executor.bridge.guest_fpscr;
        rc=vp_startup_prepare(&run.startup,modules,4,scope,VP_READY_ALL,1,0x60);
        if (!rc && (vp_startup_initialize(&run.startup,vp_executor_guest_call,&executor)!=-132
            || run.startup.initialized_mask!=8 || run.startup.failed_module!=2
            || vp_startup_initialize(&run.startup,vp_executor_guest_call,&executor)!=VP_STARTUP_STATE)) rc=-134;
        if (!rc) rc=vp_startup_finalize(&run.startup,vp_executor_guest_call,&executor);
        if (!rc && (run.calls!=3 || run.events[2]!=3 || run.startup.initialized_mask)) rc=-135;
        if (!rc) ++cases;
    }
    if (!rc) {
        memset(&run,0,sizeof(run));run.fail_call=4;run.expected_fp=executor.bridge.guest_fpscr;
        rc=vp_startup_prepare(&run.startup,modules,4,scope,VP_READY_ALL,1,0x60);
        if (!rc) rc=vp_startup_initialize(&run.startup,vp_executor_guest_call,&executor);
        if (!rc && (vp_startup_finalize(&run.startup,vp_executor_guest_call,&executor)!=-132
            || run.calls!=6 || run.startup.phase!=9 || run.startup.fini_failed_mask!=2
            || run.startup.initialized_mask || run.events[5]!=3
            || vp_startup_finalize(&run.startup,vp_executor_guest_call,&executor)!=VP_STARTUP_STATE)) rc=-136;
        if (!rc) ++cases;
    }
    if (opened) { int close=sceKernelCloseVMDomain();if (!rc) rc=close; }
    if (!vp_executor_quiescent(&executor)) rc=VP_EXECUTOR_STATE;
    if (!rc && executor.bridge.guest_fpscr!=run.expected_fp) rc=-137;
    vp_guest_registry_reset();
    if (stack>=0) { int release=sceKernelFreeMemBlock(stack);if (!rc) rc=release; }
    if (code>=0) { int release=sceKernelFreeMemBlock(code);if (!rc) rc=release; }
    fprintf(log,"LIFECYCLE_EXECUTION self_authored=1 cases=%u init_order=3,2,1 reverse_fini=1 partial_failure=1 fini_failure_continues=1 status=%d\n",cases,rc);fflush(log);
    return rc;
}

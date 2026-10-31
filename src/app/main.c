#include "native_link.h"
#include "target_identity.h"
#include "target_tls.h"
#include "patch_loader.h"
#include "guest_context.h"
#include "native_startup.h"
#include "rtld_connections.h"
#include "native_arena.h"
#include "native_unwind.h"
#include "native_executor.h"
#ifdef VP_NATIVE_SDK_DSO
#include "sdk_dso.h"
#else
#include "target_startup.h"
#endif
#include "native_lifecycle.h"
#include "gxm_runtime.h"
#ifdef VP_HAVE_KUBRIDGE
#include "native_kubridge.h"
#endif
#include <psp2/kernel/sysmem.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

unsigned int _newlib_heap_size_user=32*1024*1024;
#define MIB 0x100000u
extern const unsigned char vp_tls_island_read_ro_begin[],vp_tls_island_read_ro_r0_begin[],vp_tls_island_read_ro_r2_begin[];
extern const unsigned char vp_tls_island_write_rw_begin[],vp_tls_island_write_rw_r0_begin[];
extern const unsigned char vp_udiv_template_begin[],vp_udiv_template_end[];
extern void vp_loader_dso_unavailable(void);
static VPUnwindRegistry unwind_registry;
static unsigned startup_attempted;
static VPNativeModule modules[4];
static SceUID blocks[4]={-1,-1,-1,-1};
static VPRtldObject runtime_objects[4];
static void hex(FILE *f,const unsigned char *p,unsigned bytes) { for (unsigned i=0;i<bytes;++i) fprintf(f,"%02x",p[i]); }
static void image_hash(FILE *log,const char *stage,unsigned index) {
    unsigned char sha[32];vp_sha256(modules[index].image,modules[index].nso.span,sha);
    fprintf(log,"HASH stage=%s module=%u sha256=",stage,index);hex(log,sha,32);fputc('\n',log);fflush(log);
}
static void memory(FILE *log,const char *stage) {
    SceKernelFreeMemorySizeInfo info;memset(&info,0,sizeof(info));info.size=sizeof(info);
    int rc=sceKernelGetFreeMemorySize(&info);
    fprintf(log,"MEMORY stage=%s status=%d user=%u cdram=%u\n",stage,rc,(unsigned)info.size_user,(unsigned)info.size_cdram);fflush(log);
}
static int stage(FILE *log,unsigned i) {
    const VPIdentity *id=&vp_identities[i];char path[80];snprintf(path,sizeof(path),"app0:game/%s",id->name);
    FILE *f=fopen(path,"rb");if (!f) return VP_NSO_INPUT;
    unsigned char *raw=malloc(id->bytes);if (!raw) { fclose(f);return VP_NSO_MEMORY; }
    size_t got=fread(raw,1,id->bytes,f);int extra=fgetc(f);fclose(f);int rc=0;
    if (got!=id->bytes || extra!=EOF) rc=VP_NSO_INPUT;
    VPNativeModule *m=&modules[i];
    if (!rc) rc=vp_nso_inspect(&m->nso,raw,id->bytes,id->sha,id->build);
    uint32_t allocated=0;
    if (!rc) {
        VPArenaLayout layout;
        uint32_t reserve=i==3?sizeof(vp_tls_sites)/sizeof(vp_tls_sites[0])*80+8:i==0?224:0;
#ifdef VP_NATIVE_SDK_DSO
        reserve+=0x80000; /* Private sorted EHABI view, never source-table edits. */
#endif
        rc=vp_native_arena_layout(m->nso.span,reserve,&layout);
        if (rc) { free(raw);return rc; }
        allocated=layout.capacity;
        m->capacity=allocated;m->island_offset=layout.island_offset;
        /* main/subsdk0 remain RW. Small rtld/SDK VM blocks execute only the
         * bounded entries below; this does not prove per-page W^X on hardware. */
#ifdef VP_NATIVE_SDK_DSO
        /* Emulator-only experiment: Vita3K accepts VM arenas above Vita's
         * documented 16 MiB ceiling. This mode is not a hardware mapping. */
        blocks[i]=sceKernelAllocMemBlockForVM(id->name,allocated);
#else
        blocks[i]=(i==0 || i==3)?sceKernelAllocMemBlockForVM(id->name,allocated)
            :sceKernelAllocMemBlock(id->name,SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,allocated,NULL);
#endif
        if (blocks[i]<0) rc=blocks[i];
    }
    if (!rc) rc=sceKernelGetMemBlockBase(blocks[i],(void **)&m->image);
    if (!rc) {
        m->base=(uint32_t)(uintptr_t)m->image;
        rc=vp_nso_decode(&m->nso,raw,id->bytes,m->image,allocated);
    }
    free(raw);
    if (!rc) { image_hash(log,"decoded",i);rc=vp_native_parse(m); }
    fprintf(log,"MODULE index=%u name=%s base=0x%08x span=%u allocated=%u symbols=%u status=%d\n",i,id->name,m->base,m->nso.span,allocated,m->symbol_count,rc);
    fflush(log);return rc;
}
static int patch_tls(FILE *log) {
    VPNativeModule *m=&modules[3];uint32_t island_offset=m->island_offset;
    const unsigned char *templates[]={vp_tls_island_read_ro_begin,vp_tls_island_read_ro_r0_begin,
        vp_tls_island_read_ro_r2_begin,vp_tls_island_write_rw_begin,vp_tls_island_write_rw_r0_begin};
    VPPatchTarget target;int rc=vp_patch_open(&target,m->image,m->nso.span,m->base);
    unsigned count=sizeof(vp_tls_sites)/sizeof(vp_tls_sites[0]);
    if (island_offset>m->capacity || count*80>m->capacity-island_offset) return VP_PATCH_RANGE;
    /* Validate every site before touching this module's instructions. */
    for (unsigned i=0;i<count && !rc;++i) {
        uint32_t word=0;
        if (vp_tls_sites[i].offset>m->nso.segments[0].bytes-4 || vp_tls_sites[i].kind>=5) return VP_PATCH_RANGE;
        memcpy(&word,m->image+vp_tls_sites[i].offset,4);
        if (word!=vp_tls_sites[i].original) return VP_PATCH_BINDING;
    }
    if (!rc) memset(m->image+island_offset,0,count*80);
    for (unsigned i=0;i<count && !rc;++i) {
#ifdef VP_NATIVE_SDK_DSO
        /* These complete functions receive native TLS implementations below;
         * no intermediate TLS rewrite may own the same entry word. */
        if (vp_sdk_dso_replaces_tls(vp_tls_sites[i].offset)) continue;
#endif
        uint32_t destination=m->base+island_offset+i*80;
        rc=vp_patch_island(destination,templates[vp_tls_sites[i].kind],80,76,
            m->base+vp_tls_sites[i].offset+4,16,(uint32_t)(uintptr_t)vp_guest_stack_directory);
        if (!rc) rc=vp_patch_site(&target,vp_tls_sites[i].offset,vp_tls_sites[i].original,destination);
    }
    m->island_bytes=count*80;
    fprintf(log,"TLS sites=%u words=%u ledger=%u status=%d directory=0x%08x island=0x%08x sha256=",count,target.words,target.ledger_count,rc,
        (unsigned)(uintptr_t)vp_guest_stack_directory,m->base+island_offset);
    unsigned char sha[32];vp_sha256(m->image+island_offset,count*80,sha);hex(log,sha,32);fputc('\n',log);
    image_hash(log,"tls-patched",3);return rc;
}
static int patch_rtld(FILE *log) {
    VPNativeModule *m=&modules[0];VPPatchTarget sites,island;
    uint32_t positions[2]={0x9a8,0xf90};uint32_t bytes=(uint32_t)(vp_udiv_template_end-vp_udiv_template_begin);
    if (bytes!=104) return VP_PATCH_TEMPLATE;
    int rc=vp_patch_open(&sites,m->image,m->nso.span,m->base);
    for (unsigned i=0;i<2 && !rc;++i) {
        uint32_t off=m->island_offset+i*112,destination=m->base+off,original;
        memcpy(&original,m->image+positions[i],4);
        if (original!=0xe733f211u) return VP_PATCH_BINDING;
        memcpy(m->image+off,vp_udiv_template_begin,bytes);
        rc=vp_patch_open(&island,m->image+off,bytes,destination);
        uint32_t before;memcpy(&before,m->image+off+100,4);
        if (!rc) rc=vp_patch_site(&island,100,before,m->base+positions[i]+4);
        if (!rc) rc=vp_patch_site(&sites,positions[i],original,destination);
    }
    m->island_bytes=224;
    fprintf(log,"UDIV sites=2 words=%u status=%d scope=owned_stack_lookup_entries_only\n",sites.words,rc);fflush(log);return rc;
}
static int patch_dso_boundary(FILE *log) {
#ifdef VP_NATIVE_SDK_DSO
    return vp_sdk_dso_patch(log,modules);
#else
    VPNativeModule *m=&modules[3];unsigned char hash[32];uint32_t actual=0,address=0;
    vp_sha256(m->image+VP_DSO_OFFSET,VP_DSO_BYTES,hash);
    memcpy(&actual,m->image+VP_DSO_OFFSET,4);
    if (memcmp(hash,vp_dso_sha256,32) || actual!=VP_DSO_WORD
        || vp_native_export(m,"__nnmusl_init_dso",&address) || address!=m->base+VP_DSO_OFFSET) return VP_PATCH_BINDING;
    uint32_t guard,plt;memcpy(&guard,m->image+0x862754,4);memcpy(&plt,m->image+0x852678,4);
    if (guard || plt!=address) return VP_PATCH_BINDING;
    uint32_t off=m->island_offset+m->island_bytes;
    if (off>m->capacity || m->capacity-off<8) return VP_PATCH_RANGE;
    uint32_t trampoline[2]={0xe51ff004u,(uint32_t)(uintptr_t)vp_loader_dso_unavailable};
    memcpy(m->image+off,trampoline,8);
    VPPatchTarget site;int rc=vp_patch_open(&site,m->image,m->nso.span,m->base);
    if (!rc) rc=vp_patch_site(&site,VP_DSO_OFFSET,VP_DSO_WORD,m->base+off);
    if (!rc) m->island_bytes+=8;
    fprintf(log,"DSO_BOUNDARY module=3 offset=0x%x island=0x%08x status=%d implementation=unavailable\n",VP_DSO_OFFSET,m->base+off,rc);fflush(log);
    if (!rc) image_hash(log,"dso-patched",3);
    return rc;
#endif
}
static int unavailable(VPExecutor *e,VPBridgeFrame *frame) {
    (void)e;(void)frame;return VP_BRIDGE_UNSUPPORTED;
}
#ifndef VP_NATIVE_SDK_DSO
static uint32_t host_tls(void) { uint32_t v;__asm__ volatile("mrc p15,0,%0,c13,c0,3":"=r"(v));return v; }
static uint32_t host_fp(void) { uint32_t v;__asm__ volatile("vmrs %0,FPSCR":"=r"(v));return v; }
static int original_init_diagnostic(FILE *log,VPExecutor *executor) {
    uint32_t tls=host_tls(),fp=host_fp(),args[4]={0},result=0;
    startup_attempted=1;
    int rc=vp_executor_invoke(executor,3,modules[3].base+modules[3].init,args,&result);
    VPBridgeFrame *f=&executor->fault_frame;
    int checked=rc==VP_BRIDGE_UNSUPPORTED && f->service==0x226 && f->pc==modules[3].base+0x14c
        && f->lr==f->pc && host_tls()==tls && host_fp()==fp && vp_executor_quiescent(executor)
        && f->sp>=executor->stack_base+4096 && f->sp<executor->stack_top;
    fprintf(log,"ORIGINAL_INIT module=3 entry=0x24 status=%d service=0x%x continuation=0x%08x sp=0x%08x checked=%d constructors=0\n",
        rc,f->service,f->pc,f->sp,checked);
    for (unsigned i=0;i<13;++i) fprintf(log,"FAULT_REG r%u=0x%08x\n",i,f->regs[i]);
    /* After the rejected invocation, host stack/TLS/FP and an independent safe
     * guest entry must still work. This never retries a failed constructor. */
    args[0]=(uint32_t)(uintptr_t)"after abort";args[1]=args[0];
    int reused=vp_executor_invoke(executor,0,modules[0].base+0x1880,args,&result);
    fprintf(log,"EXECUTOR_RECOVERY status=%d result=%u host_context=%d quiescent=%d\n",reused,result,host_tls()==tls && host_fp()==fp,vp_executor_quiescent(executor));fflush(log);
    return checked && !reused && !result && host_tls()==tls && host_fp()==fp?0:1;
}
#endif
static int invoke_lookup(VPExecutor *e,const uint32_t *old,uint32_t *result) {
    return vp_executor_invoke6(e,0,old[0],old+1,result);
}
static int lookup_tests(FILE *log) {
    /* The locked main NPDM declares a 1 MiB main-thread stack; the generic
     * loader scope keeps the smaller probe stack. */
#ifdef VP_NATIVE_SDK_DSO
    const uint32_t stack_bytes=0x100000;
#else
    const uint32_t stack_bytes=0x20000;
#endif
    SceKernelAllocMemBlockOpt options;memset(&options,0,sizeof(options));options.size=sizeof(options);
    options.attr=SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;options.alignment=0x10000;
    SceUID stack=sceKernelAllocMemBlock("vp-real-lookup-stack",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,stack_bytes,&options);
    if (stack<0) return stack;
    void *base=NULL;int rc=sceKernelGetMemBlockBase(stack,&base);
    unsigned checks=0,failures=0;VPExecutor executor={0};uint32_t tls_ro[128]={0},tls_rw[128]={0};
    if (!rc) rc=vp_executor_prepare(&executor,modules,4,base,stack_bytes,(uint32_t)(uintptr_t)tls_ro,(uint32_t)(uintptr_t)tls_rw,unavailable,NULL);
    if (!rc) {
#ifdef VP_NATIVE_SDK_DSO
        /* Worker stacks and their TLS contexts must be in the registry before
         * it is sealed; guest svcCreateThread only hands out these slots. */
        rc=vp_sdk_thread_pool_prepare(modules,4,log);
        if (!rc) vp_guest_registry_seal();
#else
        vp_guest_registry_seal();
#endif
        VPNativeModule *sdk=&modules[3];uint32_t object[24]={0};
        object[0x10/4]=sdk->base;object[0x28/4]=sdk->base+sdk->hash+8;
        object[0x2c/4]=object[0x28/4]+sdk->buckets*4;object[0x30/4]=sdk->base+sdk->nso.strings;
        object[0x34/4]=sdk->base+sdk->nso.symbols;object[0x54/4]=sdk->buckets;
        const char *names[]={"__set_tp","__get_tp","__nnmusl_init_dso"};
        for (unsigned i=0;i<3;++i) {
            uint32_t name_offset=0,symbol_pointer=0,expected=0;
            rc=vp_native_export(sdk,names[i],&expected);if (rc) break;
            for (uint32_t j=1;j<sdk->symbol_count;++j) {
                unsigned char *s=sdk->image+sdk->nso.symbols+j*16;uint32_t n;memcpy(&n,s,4);
                if (!strcmp((char *)sdk->image+sdk->nso.strings+n,names[i]) && (s[14] || s[15])) {
                    name_offset=n;symbol_pointer=(uint32_t)(uintptr_t)s;break;
                }
            }
            uint32_t args[8]={modules[0].base+0x958,(uint32_t)(uintptr_t)object,
                sdk->base+sdk->nso.strings+name_offset,0,0,0,0,(uint32_t)(uintptr_t)base+0x1ff00};
            uint32_t result=0;rc=invoke_lookup(&executor,args,&result);++checks;failures+=rc || result!=symbol_pointer;
            uint32_t output=0;args[0]=modules[0].base+0xeec;args[2]=(uint32_t)(uintptr_t)&output;
            args[3]=name_offset;args[6]=0x300;
            rc=invoke_lookup(&executor,args,&result);++checks;failures+=rc || result!=1 || output!=expected;
        }
        uint32_t args[8]={modules[0].base+0x958,(uint32_t)(uintptr_t)object,
            (uint32_t)(uintptr_t)"vp_definitely_missing_symbol",0,0,0,0,(uint32_t)(uintptr_t)base+0x1ff00};
        uint32_t missing=1;rc=invoke_lookup(&executor,args,&missing);++checks;failures+=rc || missing!=0;
        /* Original rtld global lookup follows the native-installed module list. */
        for (unsigned i=0;i<3;++i) {
            uint32_t expected=0;rc=vp_native_export(sdk,names[i],&expected);if (rc) break;
            args[0]=modules[0].base+0x884;args[1]=(uint32_t)(uintptr_t)names[i];
            uint32_t result=0;rc=invoke_lookup(&executor,args,&result);++checks;failures+=rc || result!=expected;
        }
        if (!rc && !failures) {
#ifdef VP_NATIVE_SDK_DSO
            startup_attempted=1;rc=vp_sdk_dso_execute(log,&executor);
#else
            rc=original_init_diagnostic(log,&executor);
#endif
        }
    }
    if (!vp_executor_quiescent(&executor)) return VP_EXECUTOR_STATE;
    vp_guest_registry_reset();int released=sceKernelFreeMemBlock(stack);
    fprintf(log,"ORIGINAL_LOOKUP sites=0x9a8,0xf90 entries=0x958,0xeec checks=%u failures=%u stack_bytes=%u low_guard=4096 release=%d\n",checks,failures,stack_bytes,released);fflush(log);
    return rc?rc:failures?1:released;
}
int main(void) {
    if (mkdir("ux0:data/mhgu-vita-port",0777) && errno!=EEXIST) return 1;
#ifdef VP_NATIVE_SDK_DSO
    FILE *log=fopen("ux0:data/mhgu-vita-port/sdk-dso.log","wb");if (!log) return 2;
    setvbuf(log,NULL,_IONBF,0);
#else
    FILE *log=fopen("ux0:data/mhgu-vita-port/native-loader.log","wb");if (!log) return 2;
#endif
    fputs("MHGU_NATIVE_LOADER_V2\nREAL_1_4_0_MODULES\n",log);fflush(log);
    /* GXM worker bring-up. Evidence runs opt in with gxm-test.enable or
     * m2-frame.enable; normal play opts in with display-bridge.enable. Without
     * any of those markers the worker is not started, preserving the exact
     * behavior of every existing non-GXM run. */
    {
        FILE *gxm_enable=fopen("ux0:data/mhgu-vita-port/gxm-test.enable","rb");
        FILE *frame_enable=fopen("ux0:data/mhgu-vita-port/m2-frame.enable","rb");
        FILE *bridge_on=fopen("ux0:data/mhgu-vita-port/display-bridge.enable","rb");
        FILE *bridge_off=fopen("ux0:data/mhgu-vita-port/display-bridge.disable","rb");
        const char *reason=gxm_enable?"gxm-test":frame_enable?"m2-frame":
                            bridge_on?"display-bridge":bridge_off?"disabled":"default";
        int grc=-1;
        if (gxm_enable || frame_enable || bridge_on) {
            if (gxm_enable) fclose(gxm_enable);
            if (frame_enable) fclose(frame_enable);
            if (bridge_on) fclose(bridge_on);
            if (bridge_off) fclose(bridge_off);
            grc=vp_gxm_worker_start();
        } else {
            if (bridge_off) fclose(bridge_off);
        }
        fprintf(log,"VP_GXM_WORKER started=%d reason=%s\n",grc,reason);
        fflush(log);
    }
    int rc=0,opened=0;unsigned checks=0,failures=0;VPNativePlan plan={0};memory(log,"before");
    for (unsigned i=0;i<4 && !rc;++i) rc=stage(log,i);
    uint32_t scope[4]={0,1,2,3};
    if (!rc) rc=vp_native_plan(modules,4,scope,&plan);
    fprintf(log,"PLAN rows=%u status=%d\n",plan.count,rc);fflush(log);
    if (!rc) rc=vp_native_commit(modules,4,&plan);
    fprintf(log,"COMMIT rows=%u status=%d\n",plan.count,rc);fflush(log);
    if (!rc) for (unsigned i=0;i<4;++i) image_hash(log,"linked",i);
    vp_native_discard(&plan);
    if (!rc) rc=patch_tls(log);
    if (!rc) rc=patch_rtld(log);
    if (!rc) rc=vp_native_arenas_check(modules,4,1);
    if (!rc) rc=vp_native_lifecycle_check(modules,4);
    if (!rc) rc=vp_unwind_register(&unwind_registry,modules,4);
    fprintf(log,"LIFECYCLE_UNWIND modules=4 status=%d\n",rc);
    if (!rc) for (unsigned i=0;i<4;++i) fprintf(log,"UNWIND module=%u entries=%u order_breaks=%u init_array=%u fini_array=%u\n",
        i,unwind_registry.modules[i].entries,unwind_registry.modules[i].order_breaks,modules[i].init_bytes/4,modules[i].fini_bytes/4);
    if (!rc) rc=patch_dso_boundary(log);
    if (!rc) {
        rc=vp_rtld_connect(modules,runtime_objects,scope);
        fprintf(log,"RTLD_CONNECTIONS hooks=5 modules=4 status=%d\n",rc);fflush(log);
    }
    memory(log,"loaded");
    if (!rc) {
        /* Source identity and the 312-byte leaf are independently bound in the
         * collector; it has no out-of-range branch, import or privileged access. */
        rc=sceKernelSyncVMDomain(blocks[0],modules[0].image,modules[0].capacity);
        if (!rc) rc=sceKernelSyncVMDomain(blocks[3],modules[3].image,modules[3].capacity);
#ifdef VP_NATIVE_SDK_DSO
        if (!rc) rc=sceKernelSyncVMDomain(blocks[1],modules[1].image,modules[1].capacity);
        if (!rc) rc=sceKernelSyncVMDomain(blocks[2],modules[2].image,modules[2].capacity);
#endif
        fprintf(log,"CACHE rtld_sync=%d\n",rc);fflush(log);
    }
    if (!rc) { rc=sceKernelOpenVMDomain();if (!rc) {
        opened=1;modules[0].state=3;modules[3].state=3;
#ifdef VP_NATIVE_SDK_DSO
        modules[1].state=3;modules[2].state=3;
#endif
    } }
    if (!rc) {
        int (*compare)(const char *,const char *)=(void *)(uintptr_t)(modules[0].base+0x1880);
        unsigned char left[128] __attribute__((aligned(16)))={0},right[128] __attribute__((aligned(16)))={0};
        static const char *cases[][2]={{"",""},{"same","same"},{"abc","abd"},{"abd","abc"},{"a","ab"},{"ab","a"},{"long equal string 1234567890","long equal string 1234567890"}};
        for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) for (unsigned unaligned=0;unaligned<2;++unaligned) {
            memset(left,0,sizeof(left));memset(right,0,sizeof(right));
            strcpy((char *)left,cases[i][0]);strcpy((char *)right+unaligned,cases[i][1]);
            int expected=strcmp((char *)left,(char *)right+unaligned),got=compare((char *)left,(char *)right+unaligned);
            ++checks;failures+=((expected>0)-(expected<0))!=((got>0)-(got<0));
        }
        fprintf(log,"ORIGINAL_LEAF module=rtld offset=0x1880 bytes=312 checks=%u failures=%u\n",checks,failures);fflush(log);
        rc=lookup_tests(log);
    }
    if (opened) { int close=sceKernelCloseVMDomain();fprintf(log,"VM_CLOSE status=%d\n",close);if (!rc) rc=close;opened=0; }
    if (!rc) rc=vp_native_lifecycle_probe(log);
    if (!rc) { rc=vp_unwind_verify(&unwind_registry);fprintf(log,"UNWIND_VERIFY status=%d\n",rc); }
#ifdef VP_HAVE_KUBRIDGE
    if (!rc) {
        /* Optional hardware backend: entire ordinary RW arenas, including main
         * larger than 16 MiB, are finalized page-wise. No VM size assumption. */
        for (unsigned i=0;i<4;++i) modules[i].state=2;
        rc=vp_vita_module_permissions(modules,4);
        fprintf(log,"HARDWARE_BACKEND kubridge_protect=%d measured_on_hardware=0\n",rc);fflush(log);
    }
#endif
    if (opened) { int close=sceKernelCloseVMDomain();fprintf(log,"VM_CLOSE status=%d\n",close);if (!rc) rc=close; }
    vp_guest_registry_reset();
    for (unsigned i=0;i<4;++i) if (blocks[i]>=0) {
        int released=sceKernelFreeMemBlock(blocks[i]);fprintf(log,"RELEASE module=%u status=%d\n",i,released);if (!rc) rc=released;
    }
    vp_unwind_reset(&unwind_registry);
    memory(log,"after");
#ifdef VP_NATIVE_SDK_DSO
    unsigned nnos_returned=vp_sdk_dso_nnos_returned();
    unsigned nnmain_entered=vp_sdk_dso_nnmain_entered();
    unsigned session_stopped=vp_sdk_dso_session_stopped();
    unsigned title_exited=vp_sdk_dso_title_exited();
#else
    unsigned nnos_returned=0,nnmain_entered=0,session_stopped=0,title_exited=0;
#endif
    fprintf(log,"RESULT status=%d leaf_checks=%u failures=%u startup_executed=%u nnos_returned=%u constructors_completed=%u nnmain_entered=%u session_stopped=%u hardware_permissions_proven=0 title_exit=%u\n",
        rc,checks,failures,startup_attempted,nnos_returned,
        nnmain_entered?1u:0u, /* Start runs every DT_INIT callback before nnMain */
        nnmain_entered,session_stopped,title_exited);
    fprintf(log,"RESULT %s\n",rc || failures?"FAIL":"COMPLETE");
    fclose(log);return rc || failures?1:0;
}

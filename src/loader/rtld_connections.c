#include "rtld_connections.h"
#include <string.h>
static void put(void *p,uint32_t v) { memcpy(p,&v,4); }
static uint32_t get(const void *p) { uint32_t v;memcpy(&v,p,4);return v; }
int vp_rtld_connect(VPNativeModule m[4],VPRtldObject objects[4],const uint32_t scope[4]) {
    if (!m || !objects || !scope || scope[0]!=0 || (uintptr_t)objects>UINT32_MAX-sizeof(VPRtldObject)*4) return VP_LINK_STATE;
    uint32_t seen=0;
    for (unsigned i=0;i<4;++i) {
        if (m[i].state!=2 || scope[i]>=4 || (seen&(1u<<scope[i]))) return VP_LINK_STATE;
        seen|=1u<<scope[i];
    }
    /* Destinations are independently verified against exact named exports. */
    static const char *names[]={"_ZN2nn2ro6detail15g_pAutoLoadListE","_ZN2nn2ro6detail17g_pManualLoadListE",
        "_ZN2nn2ro6detail14g_pRoDebugFlagE","_ZN2nn2ro6detail34g_pLookupGlobalAutoFunctionPointerE",
        "_ZN2nn2ro6detail36g_pLookupGlobalManualFunctionPointerE"};
    static const uint32_t offsets[]={8817044,8817048,8817088,8817092,8817096};
    static const uint32_t rtld_offsets[]={12616,12608,12624,2180,12628};
    for (unsigned i=0;i<5;++i) {
        uint32_t address=0;
        if (vp_native_export(&m[3],names[i],&address) || address!=m[3].base+offsets[i]
            || offsets[i]>m[3].nso.span-4 || get(m[3].image+offsets[i])!=0) return VP_LINK_PREIMAGE;
    }
    if (m[0].nso.span<12632 || get(m[0].image+12608) || get(m[0].image+12612)
        || get(m[0].image+12616) || get(m[0].image+12620)) return VP_LINK_PREIMAGE;
    memset(objects,0,sizeof(VPRtldObject)*4);
    uint32_t auto_head=m[0].base+12616,manual_head=m[0].base+12608;
    for (unsigned i=0;i<4;++i) {
        unsigned index=scope[i];VPRtldObject *o=&objects[index];VPNativeModule *x=&m[index];
        o->words[0]=i?(uint32_t)(uintptr_t)&objects[scope[i-1]]:auto_head;
        o->words[1]=i==3?auto_head:(uint32_t)(uintptr_t)&objects[scope[i+1]];
        o->words[2]=x->base+x->plt;o->words[3]=x->base+x->rel;o->words[4]=x->base;
        o->words[5]=x->base+x->dynamic;o->words[7]=x->plt_bytes;
        o->words[8]=x->init?x->base+x->init:0;o->words[9]=x->fini?x->base+x->fini:0;
        o->words[10]=x->base+x->hash+8;o->words[11]=o->words[10]+x->buckets*4;
        o->words[12]=x->base+x->nso.strings;o->words[13]=x->base+x->nso.symbols;
        o->words[14]=x->nso.string_bytes;o->words[15]=x->base+x->got;o->words[17]=x->rel_bytes;
        o->words[20]=x->symbol_count;o->words[21]=x->buckets;
        /* The relative prefix counters are cleared: native linker owns every
         * relocation. Runtime discovery/relink is unsupported in this session. */
    }
    put(m[0].image+12608,manual_head);put(m[0].image+12612,manual_head);
    put(m[0].image+12616,(uint32_t)(uintptr_t)&objects[scope[3]]);
    put(m[0].image+12620,(uint32_t)(uintptr_t)&objects[scope[0]]);
    for (unsigned i=0;i<5;++i) put(m[3].image+offsets[i],m[0].base+rtld_offsets[i]);
    return 0;
}

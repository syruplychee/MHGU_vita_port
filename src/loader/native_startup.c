#include "native_startup.h"
#include "native_arena.h"
#include <string.h>
static uint32_t page(uint32_t n) { return (n+4095u)&~4095u; }
static int backed(const VPNativeModule *m,uint32_t off,uint32_t bytes) {
    for (unsigned i=0;i<3;++i) {
        const VPNSOSegment *seg=&m->nso.segments[i];
        if (off>=seg->offset && off-seg->offset<=seg->bytes && bytes<=seg->bytes-(off-seg->offset)) return 1;
    }
    return 0;
}
static int executable(const VPNativeModule *m,uint32_t off) {
    const VPNSOSegment *text=&m->nso.segments[0];uint32_t begin=off&~1u,bytes=(off&1)?2:4;
    return ((off&1) || !(off&3)) && begin>=text->offset && begin-text->offset<=text->bytes
        && bytes<=text->bytes-(begin-text->offset);
}
int vp_native_lifecycle_check(const VPNativeModule *m,uint32_t count) {
    if (!m || !count || count>4) return VP_STARTUP_ARGUMENT;
    for (uint32_t i=0;i<count;++i) {
        if (!m[i].image || m[i].nso.span>UINT32_MAX-m[i].base) return VP_STARTUP_ARGUMENT;
        if ((m[i].init_bytes && !m[i].init) || (m[i].fini_bytes && !m[i].fini)) return VP_STARTUP_ARGUMENT;
        if ((m[i].init && !executable(&m[i],m[i].init)) || (m[i].fini && !executable(&m[i],m[i].fini))) return VP_STARTUP_ARGUMENT;
        uint32_t offsets[2]={m[i].init_array,m[i].fini_array},sizes[2]={m[i].init_bytes,m[i].fini_bytes};
        for (unsigned a=0;a<2;++a) {
            if (!sizes[a]) continue;
            if ((offsets[a]&3) || (sizes[a]&3) || !backed(&m[i],offsets[a],sizes[a])) return VP_STARTUP_ARGUMENT;
            for (uint32_t off=0;off<sizes[a];off+=4) {
                uint32_t address;memcpy(&address,m[i].image+offsets[a]+off,4);int found=0;
                for (uint32_t j=0;j<count;++j) if (address>=m[j].base && executable(&m[j],address-m[j].base)) { found=1;break; }
                if (!found) return VP_STARTUP_ARGUMENT;
            }
        }
    }
    return 0;
}
int vp_native_protect(VPNativeModule *m,uint32_t count,VPProtect protect,VPFlush flush,void *ctx) {
    if (!m || !count || count>4 || !protect || !flush) return VP_STARTUP_ARGUMENT;
    if (vp_native_arenas_check(m,count,0)) return VP_STARTUP_STATE;
    for (uint32_t i=0;i<count;++i) {
        if (m[i].state!=2) return VP_STARTUP_STATE;
        uint32_t end=0;
        for (unsigned j=0;j<3;++j) {
            const VPNSOSegment *seg=&m[i].nso.segments[j];
            if ((seg->offset&4095) || seg->offset<end || seg->offset>m[i].nso.span
                || seg->bytes>m[i].nso.span-seg->offset) return VP_STARTUP_STATE;
            end=seg->offset+page(seg->bytes);
            if (end>page(m[i].nso.span)) return VP_STARTUP_STATE;
        }
    }
    int status=0;
    for (uint32_t i=0;i<count && !status;++i) {
        status=flush(ctx,m[i].base,m[i].capacity);
        if (status) break;
        status=protect(ctx,m[i].base,m[i].capacity,0);
        for (unsigned segment=0;segment<3 && !status;++segment) {
            const VPNSOSegment *seg=&m[i].nso.segments[segment];
            uint32_t length=segment==2?m[i].nso.span-seg->offset:seg->bytes;
            if (length) status=protect(ctx,m[i].base+seg->offset,page(length),segment==0?5:segment==1?1:3);
        }
        if (!status && m[i].island_bytes) status=protect(ctx,m[i].base+m[i].island_offset,page(m[i].island_bytes),5);
    }
    if (status) {
        for (uint32_t i=0;i<count;++i) {
            protect(ctx,m[i].base,m[i].capacity,0);
            m[i].state=4; /* Any partially protected arena must be discarded. */
        }
        return status;
    }
    for (uint32_t i=0;i<count;++i) m[i].state=3;
    return 0;
}
static void arrays_hash(const VPNativeModule *m,unsigned char hashes[2][32]) {
    vp_sha256(m->image+(m->init_bytes?m->init_array:0),m->init_bytes,hashes[0]);
    vp_sha256(m->image+(m->fini_bytes?m->fini_array:0),m->fini_bytes,hashes[1]);
}
int vp_startup_prepare(VPStartup *s,VPNativeModule *m,uint32_t count,const uint32_t *scope,
                       uint32_t ready,uint32_t entry_module,uint32_t entry_offset) {
    if (!s || !m || !scope || !count || count>4 || entry_module>=count || (ready&~VP_READY_ALL)) return VP_STARTUP_ARGUMENT;
    if (s->phase) return VP_STARTUP_STATE;
    uint32_t seen=0;
    for (uint32_t i=0;i<count;++i) {
        if (scope[i]>=count || (seen&(1u<<scope[i])) || m[i].state!=3) return VP_STARTUP_STATE;
        seen|=1u<<scope[i];
    }
    if ((ready&VP_READY_ALL)!=VP_READY_ALL) return VP_STARTUP_NOT_READY;
    if (vp_native_arenas_check(m,count,0) || !executable(&m[entry_module],entry_offset)
        || vp_native_lifecycle_check(m,count)) return VP_STARTUP_ARGUMENT;
    memset(s,0,sizeof(*s));s->modules=m;s->count=count;s->ready=ready;s->entry_module=entry_module;s->entry_offset=entry_offset;
    for (uint32_t i=0;i<count;++i) {
        s->scope[i]=scope[i];s->bases[i]=m[i].base;
        memcpy(&s->snapshot[i],&m[i],sizeof(m[i]));arrays_hash(&m[i],s->arrays_sha256[i]);
    }
    s->phase=1;return 0;
}
static int bound(const VPStartup *s) {
    if (!s->modules || !s->count || s->count>4) return 0;
    for (uint32_t i=0;i<s->count;++i) {
        unsigned char hashes[2][32];
        if (memcmp(&s->modules[i],&s->snapshot[i],sizeof(s->snapshot[i]))) return 0;
        arrays_hash(&s->modules[i],hashes);
        if (memcmp(hashes,s->arrays_sha256[i],sizeof(hashes))) return 0;
    }
    return 1;
}
static void failure(VPStartup *s,uint32_t phase,uint32_t module,uint32_t address,int rc) {
    if (!s->failure) { s->failure=rc;s->failed_phase=phase;s->failed_module=module;s->failed_address=address; }
}
int vp_startup_initialize(VPStartup *s,VPGuestCall call,void *ctx) {
    if (!s || !call || s->phase!=1 || !bound(s)) return VP_STARTUP_STATE;
    s->phase=2;
    for (uint32_t pos=s->count;pos;--pos) {
        uint32_t i=s->scope[pos-1];if (!i || !s->modules[i].init) continue;
        uint32_t address=s->bases[i]+s->modules[i].init;
        int rc=call(ctx,i,address);
        if (rc) { failure(s,2,i,address,rc);s->phase=5;return rc; }
        s->initialized_mask|=1u<<i;s->init_order[s->init_count++]=i;
    }
    s->phase=3;return 0;
}
int vp_startup_enter(VPStartup *s,VPGuestCall call,void *ctx) {
    if (!s || !call || s->phase!=3 || !bound(s)) return VP_STARTUP_STATE;
    s->phase=6;
    uint32_t address=s->bases[s->entry_module]+s->entry_offset;
    int rc=call(ctx,s->entry_module,address);
    if (rc) failure(s,6,s->entry_module,address,rc);
    s->phase=rc?5:4;return rc;
}
int vp_startup_finalize(VPStartup *s,VPGuestCall call,void *ctx) {
    if (!s || !call || !(s->phase==3 || s->phase==4 || s->phase==5) || !bound(s)) return VP_STARTUP_STATE;
    s->phase=7;int first=0;
    for (uint32_t pos=s->init_count;pos;--pos) {
        uint32_t i=s->init_order[pos-1];s->initialized_mask&=~(1u<<i);
        if (s->modules[i].fini) {
            uint32_t address=s->bases[i]+s->modules[i].fini;
            int rc=call(ctx,i,address);
            if (rc) { if (!first) first=rc;s->fini_failed_mask|=1u<<i;failure(s,7,i,address,rc); }
        }
    }
    s->phase=first?9:8;return first;
}

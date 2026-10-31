#include "native_unwind.h"
#include <string.h>
static uint32_t word(const unsigned char *p) { uint32_t v;memcpy(&v,p,4);return v; }
static int backed(const VPNativeModule *m,uint32_t off,uint32_t bytes) {
    for (unsigned i=0;i<3;++i) {
        const VPNSOSegment *s=&m->nso.segments[i];
        if (off>=s->offset && off-s->offset<=s->bytes && bytes<=s->bytes-(off-s->offset)) return 1;
    }
    return 0;
}
static int relative(uint32_t place,uint32_t v,uint32_t *target) {
    int64_t displacement=(v&0x40000000u)?(int64_t)(v&0x7fffffffu)-0x80000000ll:(v&0x7fffffffu);
    int64_t result=(int64_t)place+displacement;
    if (result<0 || result>UINT32_MAX) return VP_UNWIND_METADATA;
    *target=(uint32_t)result;return 0;
}
static int mod_relative(uint32_t place,uint32_t v,uint32_t *target) {
    int64_t result=(int64_t)place+(int32_t)v;
    if (result<0 || result>UINT32_MAX) return VP_UNWIND_METADATA;
    *target=(uint32_t)result;return 0;
}
static int text_offset(const VPNativeModule *m,uint32_t off,int sentinel) {
    const VPNSOSegment *s=&m->nso.segments[0];
    return !(off&1) && off>=s->offset && off-s->offset<=s->bytes
        && (sentinel || off-s->offset<s->bytes);
}
static int descriptor(const VPNativeModule *m,uint32_t place,uint32_t value) {
    if (value==1) return 0; /* EXIDX_CANTUNWIND */
    if (value&0x80000000u) return (value>>24)==0x80?0:VP_UNWIND_METADATA;
    uint32_t extab;
    if (relative(place,value,&extab) || (extab&3) || !backed(m,extab,4)) return VP_UNWIND_METADATA;
    uint32_t first=word(m->image+extab),words;
    if (first&0x80000000u) {
        unsigned personality=(first>>24)&0x7f;
        if (personality>2) return VP_UNWIND_METADATA;
        words=personality?1+((first>>16)&255):1;
    } else {
        uint32_t personality;
        if (relative(extab,first,&personality) || !text_offset(m,personality&~1u,0)
            || !backed(m,extab,8)) return VP_UNWIND_METADATA;
        /* Generic personality data is opaque to the loader. Its own routine
         * interprets it; the compact model's word-count byte does not apply. */
        words=2;
    }
    return backed(m,extab,words*4)?0:VP_UNWIND_METADATA;
}
int vp_unwind_register(VPUnwindRegistry *r,const VPNativeModule *m,uint32_t count) {
    if (!r || !m || !count || count>4) return VP_UNWIND_ARGUMENT;
    if (r->sealed || r->count) return VP_UNWIND_STATE;
    VPUnwindRegistry next={0};next.count=count;
    for (uint32_t i=0;i<count;++i) {
        if (!m[i].image || !m[i].nso.span || m[i].nso.span>0x8000000 || m[i].state<2 || m[i].state>3 || m[i].base>UINT32_MAX-m[i].nso.span
            || !backed(&m[i],m[i].mod0,28)) return VP_UNWIND_STATE;
        for (unsigned j=0;j<3;++j) if (m[i].nso.segments[j].offset>m[i].nso.span
            || m[i].nso.segments[j].bytes>m[i].nso.span-m[i].nso.segments[j].offset) return VP_UNWIND_METADATA;
        VPUnwindModule *u=&next.modules[i];uint32_t end;
        if (mod_relative(m[i].mod0,word(m[i].image+m[i].mod0+16),&u->offset)
            || mod_relative(m[i].mod0,word(m[i].image+m[i].mod0+20),&end)
            || (u->offset&3) || end<u->offset || (end-u->offset)%8
            || !backed(&m[i],u->offset,end-u->offset)) return VP_UNWIND_METADATA;
        u->module=&m[i];u->image=m[i].image;u->nso=m[i].nso;u->mod0=m[i].mod0;
        u->base=m[i].base;u->entries=(end-u->offset)/8;
        u->text_begin=m[i].base+m[i].nso.segments[0].offset;
        u->text_end=u->text_begin+m[i].nso.segments[0].bytes;
        for (uint32_t j=0;j<i;++j) if (u->text_begin<next.modules[j].text_end && next.modules[j].text_begin<u->text_end) return VP_UNWIND_METADATA;
        uint32_t previous=0;
        for (uint32_t j=0;j<u->entries;++j) {
            uint32_t place=u->offset+j*8,raw=word(m[i].image+place),function;
            if ((raw&0x80000000u) || relative(place,raw,&function)
                || !text_offset(&m[i],function,1)
                || descriptor(&m[i],place+4,word(m[i].image+place+4))) return VP_UNWIND_METADATA;
            if (j && function<=previous) ++u->order_breaks;
            previous=function;
        }
        vp_sha256(m[i].image+u->offset,u->entries*8,u->digest);
    }
    next.sealed=1;*r=next;return 0;
}
int vp_unwind_verify(const VPUnwindRegistry *r) {
    if (!r || !r->sealed || !r->count || r->count>4) return VP_UNWIND_STATE;
    for (uint32_t i=0;i<r->count;++i) {
        const VPUnwindModule *u=&r->modules[i];unsigned char digest[32];
        if (!u->module || u->module->base!=u->base || u->module->state<2 || u->module->state>3
            || u->module->image!=u->image || u->module->mod0!=u->mod0 || memcmp(&u->module->nso,&u->nso,sizeof(u->nso))
            || !backed(u->module,u->offset,u->entries*8)) return VP_UNWIND_STATE;
        vp_sha256(u->module->image+u->offset,u->entries*8,digest);
        if (memcmp(digest,u->digest,32)) return VP_UNWIND_METADATA;
    }
    return 0;
}
int vp_unwind_lookup(const VPUnwindRegistry *r,uint32_t pc,VPUnwindLookup *out) {
    if (!r || !out || !r->sealed || r->count>4) return VP_UNWIND_ARGUMENT;
    int valid=vp_unwind_verify(r);if (valid) return valid;
    pc&=~1u;
    for (uint32_t i=0;i<r->count;++i) {
        const VPUnwindModule *u=&r->modules[i];
        if (pc<u->text_begin || pc>=u->text_end) continue;
        if (!u->module || u->module->base!=u->base || u->module->state<2 || u->module->state>3) return VP_UNWIND_STATE;
        uint32_t offset=pc-u->base,index=UINT32_MAX,best=0,duplicate=0;
        /* Original tables have concatenated runs. Do not reorder PREL31 words
         * or pretend that the original table is globally binary-searchable. */
        for (uint32_t j=0;j<u->entries;++j) {
            uint32_t place=u->offset+j*8,function;
            if (relative(place,word(u->module->image+place),&function)) return VP_UNWIND_METADATA;
            if (function<=offset && (index==UINT32_MAX || function>best)) { index=j;best=function;duplicate=0; }
            else if (index!=UINT32_MAX && function==best) duplicate=1;
        }
        if (index==UINT32_MAX) return VP_UNWIND_NOT_FOUND;
        if (duplicate) return VP_UNWIND_AMBIGUOUS;
        uint32_t place=u->offset+index*8;
        *out=(VPUnwindLookup){i,u->base+u->offset,u->entries,index,u->base+best,
                             u->base+place+4,word(u->module->image+place+4)};return 0;
    }
    return VP_UNWIND_NOT_FOUND;
}
void vp_unwind_reset(VPUnwindRegistry *r) { if (r) memset(r,0,sizeof(*r)); }

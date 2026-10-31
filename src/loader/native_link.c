#include "native_link.h"
#include <stdlib.h>
#include <string.h>
static uint32_t rd(const unsigned char *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static int within(uint32_t a,uint32_t n,uint32_t start,uint32_t bytes) { return a>=start && a-start<=bytes && n<=bytes-(a-start); }
static int backed(const VPNativeModule *m,uint32_t p,uint32_t n) {
    for (unsigned i=0;i<3;++i) if (within(p,n,m->nso.segments[i].offset,m->nso.segments[i].bytes)) return 1;
    return 0;
}
static int region(const VPNativeModule *m,uint32_t value,uint32_t bytes) {
    return backed(m,value,bytes) || within(value,bytes,m->nso.bss_begin,m->nso.span-m->nso.bss_begin);
}
static int metadata(const VPNativeModule *m,uint32_t offset) {
    uint32_t pairs[][2]={{m->mod0,28},{m->dynamic,m->dynamic_bytes},{m->hash,(2+m->buckets+m->symbol_count)*4},
        {m->nso.strings,m->nso.string_bytes},{m->nso.symbols,m->nso.symbol_bytes},{m->rel,m->rel_bytes},{m->plt,m->plt_bytes}};
    for (unsigned i=0;i<sizeof(pairs)/sizeof(pairs[0]);++i)
        if (pairs[i][1] && offset<pairs[i][0]+pairs[i][1] && pairs[i][0]<offset+4) return 1;
    return 0;
}
int vp_native_parse(VPNativeModule *m) {
    if (!m || !m->image || m->state || m->base>UINT32_MAX-m->nso.span || m->nso.span<8) return VP_LINK_STATE;
    m->mod0=rd(m->image+4);
    if ((m->mod0&3) || !backed(m,m->mod0,28) || memcmp(m->image+m->mod0,"MOD0",4)) return VP_LINK_METADATA;
    int64_t dyn=(int64_t)m->mod0+(int32_t)rd(m->image+m->mod0+4);
    if (dyn<0 || dyn>UINT32_MAX || ((uint32_t)dyn&3)) return VP_LINK_METADATA;
    m->dynamic=(uint32_t)dyn;
    int64_t bss=(int64_t)m->mod0+(int32_t)rd(m->image+m->mod0+8);
    int64_t bss_end=(int64_t)m->mod0+(int32_t)rd(m->image+m->mod0+12);
    if (bss<(int64_t)m->nso.segments[2].offset || bss_end<bss || bss_end>m->nso.span) return VP_LINK_METADATA;
    for (uint32_t i=(uint32_t)bss;i<(uint32_t)bss_end;++i) if (m->image[i]) return VP_LINK_METADATA;
    uint32_t tags[31]={0},seen=0;int terminated=0;
    for (uint32_t i=0;i<8192;++i) {
        uint32_t p=m->dynamic+i*8;
        if (p<m->dynamic || !backed(m,p,8)) return VP_LINK_METADATA;
        uint32_t tag=rd(m->image+p),value=rd(m->image+p+4);m->dynamic_bytes=(i+1)*8;
        if (!tag) { terminated=1;break; }
        if (tag==0x6ffffef5u || tag==0x6ffffffau) continue;
        if (tag==1) {
            /* Discovery is supplied as an explicit module set/scope by the caller. */
            if (value>=m->nso.string_bytes || !memchr(m->image+m->nso.strings+value,0,m->nso.string_bytes-value)) return VP_LINK_METADATA;
            continue;
        }
        if (tag>30 || (seen&(1u<<tag))) return VP_LINK_METADATA;
        seen|=1u<<tag;tags[tag]=value;
    }
    if (!terminated || tags[5]!=m->nso.strings || tags[10]!=m->nso.string_bytes || tags[6]!=m->nso.symbols
        || tags[11]!=16 || tags[19]!=8 || (tags[4]&3) || !backed(m,tags[4],8)) return VP_LINK_METADATA;
    m->hash=tags[4];m->buckets=rd(m->image+m->hash);m->symbol_count=rd(m->image+m->hash+4);
    if (!m->buckets || m->buckets>m->symbol_count || m->symbol_count!=m->nso.symbol_bytes/16
        || m->symbol_count>1000000 || !backed(m,m->hash,(2+m->buckets+m->symbol_count)*4)) return VP_LINK_METADATA;
    for (uint32_t i=0;i<m->buckets+m->symbol_count;++i)
        if (rd(m->image+m->hash+8+i*4)>=m->symbol_count) return VP_LINK_METADATA;
    for (uint32_t i=0;i<m->symbol_count;++i) {
        const unsigned char *s=m->image+m->nso.symbols+i*16;uint32_t n=rd(s);
        if (n>=m->nso.string_bytes || !memchr(m->image+m->nso.strings+n,0,m->nso.string_bytes-n) || (s[13]&~3u)) return VP_LINK_SYMBOL;
    }
    static const unsigned char null_symbol[16]={0};
    if (memcmp(m->image+m->nso.symbols,null_symbol,16)) return VP_LINK_SYMBOL;
    m->rel=tags[17];m->rel_bytes=tags[18];m->plt=tags[23];m->plt_bytes=tags[2];m->got=tags[3];
    if ((m->rel&3) || (m->plt&3) || m->rel_bytes%8 || m->plt_bytes%8 || (m->plt_bytes && tags[20]!=17)
        || (m->rel_bytes && !backed(m,m->rel,m->rel_bytes)) || (m->plt_bytes && !backed(m,m->plt,m->plt_bytes))) return VP_LINK_METADATA;
    m->init=tags[12];m->fini=tags[13];m->init_array=tags[25];m->init_bytes=tags[27];m->fini_array=tags[26];m->fini_bytes=tags[28];
    if ((m->init && !within(m->init&~1u,2,0,m->nso.segments[0].bytes)) || (m->fini && !within(m->fini&~1u,2,0,m->nso.segments[0].bytes))
        || (m->init_bytes%4) || (m->fini_bytes%4) || (m->init_array&3) || (m->fini_array&3)
        || (m->init_bytes && !backed(m,m->init_array,m->init_bytes)) || (m->fini_bytes && !backed(m,m->fini_array,m->fini_bytes))) return VP_LINK_METADATA;
    m->state=1;return 0;
}
static uint32_t hash_name(const char *name) {
    uint32_t hash=0;
    for (const unsigned char *p=(const unsigned char *)name;*p;++p) {
        hash=(hash<<4)+*p;uint32_t high=hash&0xf0000000u;
        if (high) hash^=high>>24;
        hash&=0x0fffffffu;
    }
    return hash;
}
static int lookup(const VPNativeModule *m,const char *name,const unsigned char **out) {
    uint32_t i=rd(m->image+m->hash+8+(hash_name(name)%m->buckets)*4);
    for (uint32_t steps=0;i && steps<m->symbol_count;++steps) {
        if (i>=m->symbol_count) return VP_LINK_SYMBOL;
        const unsigned char *s=m->image+m->nso.symbols+i*16;
        unsigned section=s[14]|((unsigned)s[15]<<8);
        if ((s[12]&15)!=4 && section && section!=0xfff2u && !strcmp((const char *)m->image+m->nso.strings+rd(s),name)) { *out=s;return 0; }
        i=rd(m->image+m->hash+8+(m->buckets+i)*4);
    }
    return i?VP_LINK_SYMBOL:VP_LINK_UNRESOLVED;
}
static int symbol_address(const VPNativeModule *m,const unsigned char *s,uint32_t *address,uint32_t *thumb) {
    uint32_t v=rd(s+4),type=s[12]&15u,section=s[14]|((uint32_t)s[15]<<8);
    *thumb=type==2?(v&1u):0;
    if (section==0xfff1) { *address=v;return 0; }
    if (!section || section>=0xff00 || type>3 || !region(m,v&~*thumb,0)
        || (rd(s+8) && !region(m,v&~*thumb,rd(s+8)))
        || (type==2 && (!within(v&~1u,2,0,m->nso.segments[0].bytes) || (!(v&1) && (v&3))))) return VP_LINK_SYMBOL;
    if (v>UINT32_MAX-m->base) return VP_LINK_SYMBOL;
    *address=m->base+v;return 0;
}
int vp_native_export(const VPNativeModule *m,const char *name,uint32_t *address) {
    if (!m || !m->state || !name || !address) return VP_LINK_STATE;
    const unsigned char *s=NULL;uint32_t thumb;
    int rc=lookup(m,name,&s);return rc?rc:symbol_address(m,s,address,&thumb);
}
typedef struct { uint32_t ready,value,thumb; } Cached;
static int resolve(VPNativeModule *m,uint32_t count,const uint32_t *scope,uint32_t owner,uint32_t index,uint32_t *value,uint32_t *thumb) {
    if (!index) { *value=0;*thumb=0;return 0; }
    const unsigned char *s=m[owner].image+m[owner].nso.symbols+index*16;
    uint32_t own=s[13]&3u;const char *name=(const char *)m[owner].image+m[owner].nso.strings+rd(s);
    for (uint32_t j=0;j<(own?1:count);++j) {
        uint32_t provider=own?owner:scope[j];const unsigned char *found=NULL;
        int rc=lookup(&m[provider],name,&found);
        if (rc==VP_LINK_UNRESOLVED) continue;
        if (rc) return rc;
        if (!own && found[12]<0x10) continue;
        return symbol_address(&m[provider],found,value,thumb);
    }
    if ((s[12]>>4)!=2) return VP_LINK_UNRESOLVED;
    *value=0;*thumb=0;return 0;
}
static int compare_write(const void *a,const void *b) {
    const VPNativeWrite *x=a,*y=b;
    if (x->module!=y->module) return x->module<y->module?-1:1;
    return x->offset<y->offset?-1:x->offset>y->offset;
}
void vp_native_discard(VPNativePlan *p) { if (p) { free(p->writes);memset(p,0,sizeof(*p)); } }
int vp_native_plan(VPNativeModule *m,uint32_t count,const uint32_t *scope,VPNativePlan *p) {
    if (!p || p->writes || !m || !scope || !count || count>4) return VP_LINK_STATE;
    uint32_t seen=0,total=0;
    for (uint32_t i=0;i<count;++i) {
        if (scope[i]>=count || (seen&(1u<<scope[i])) || m[i].state!=1) return VP_LINK_STATE;
        seen|=1u<<scope[i];total+=m[i].rel_bytes/8+m[i].plt_bytes/8;
        for (uint32_t j=0;j<i;++j)
            if (m[i].base<m[j].base+m[j].nso.span && m[j].base<m[i].base+m[i].nso.span) return VP_LINK_STATE;
    }
    if (total>1000000) return VP_LINK_RELOCATION;
    p->writes=calloc(total?total:1,sizeof(*p->writes));
    if (!p->writes) return VP_NSO_MEMORY;
    int rc=0;p->modules=count;p->capacity=total;
    for (uint32_t i=0;i<count && !rc;++i) {
        VPNativeModule *x=&m[i];p->bases[i]=x->base;p->images[i]=x->image;
        memcpy(&p->snapshot[i],x,sizeof(*x));
        vp_sha256(x->image,x->nso.span,p->before_sha256[i]);
        Cached *cache=calloc(x->symbol_count,sizeof(*cache));
        if (!cache) { rc=VP_NSO_MEMORY;break; }
        for (unsigned table=0;table<2 && !rc;++table) {
            uint32_t start=table?x->plt:x->rel,length=table?x->plt_bytes:x->rel_bytes;
            for (uint32_t j=0;j<length && !rc;j+=8) {
                uint32_t offset=rd(x->image+start+j),info=rd(x->image+start+j+4),type=info&255u,index=info>>8;
                if (index>=x->symbol_count || (offset&3) || !within(offset,4,x->nso.segments[2].offset,x->nso.span-x->nso.segments[2].offset)
                    || metadata(x,offset) || !(type==2 || type==21 || type==22 || type==23) || (table && type!=22)) { rc=VP_LINK_RELOCATION;break; }
                uint32_t before=rd(x->image+offset),value=0,thumb=0;
                if (type==23) {
                    if (index || !region(x,before,0) || before>UINT32_MAX-x->base) { rc=VP_LINK_RELOCATION;break; }
                    value=x->base+before;
                } else {
                    Cached *entry=&cache[index];
                    if (!entry->ready) {
                        rc=resolve(m,count,scope,i,index,&entry->value,&entry->thumb);
                        if (rc) break;
                        entry->ready=1;
                    }
                    thumb=entry->thumb;value=entry->value;
                    /* ELF32 arithmetic is modulo 2^32; st_value's Thumb bit is restored after addition. */
                    value=((value&~thumb)+(type==22?0:before))|thumb;
                }
                p->writes[p->count++]=(VPNativeWrite){i,offset,before,value,type};
            }
        }
        free(cache);
    }
    if (!rc) {
        qsort(p->writes,p->count,sizeof(*p->writes),compare_write);
        for (uint32_t i=1;i<p->count;++i)
            if (p->writes[i].module==p->writes[i-1].module && p->writes[i].offset==p->writes[i-1].offset) { rc=VP_LINK_RELOCATION;break; }
    }
    if (rc) { vp_native_discard(p);return rc; }
    vp_sha256(p->writes,p->count*sizeof(*p->writes),p->writes_sha256);
    p->ready=1;return 0;
}
int vp_native_commit(VPNativeModule *m,uint32_t count,VPNativePlan *p) {
    if (!m || !p || !p->ready || p->committed || count!=p->modules || count>4 || p->count!=p->capacity) return VP_LINK_STATE;
    unsigned char plan_sha[32];vp_sha256(p->writes,p->count*sizeof(*p->writes),plan_sha);
    if (memcmp(plan_sha,p->writes_sha256,32)) return VP_LINK_PREIMAGE;
    for (uint32_t i=0;i<count;++i) {
        unsigned char sha[32];
        if (m[i].state!=1 || memcmp(&m[i],&p->snapshot[i],sizeof(m[i]))) return VP_LINK_STATE;
        vp_sha256(m[i].image,m[i].nso.span,sha);
        if (memcmp(sha,p->before_sha256[i],32)) return VP_LINK_PREIMAGE;
    }
    for (uint32_t i=0;i<p->count;++i) {
        const VPNativeWrite *w=&p->writes[i];
        if (w->module>=count || (w->offset&3) || !within(w->offset,4,m[w->module].nso.segments[2].offset,
            m[w->module].nso.span-m[w->module].nso.segments[2].offset) || metadata(&m[w->module],w->offset)
            || rd(m[w->module].image+w->offset)!=w->before
            || (i && compare_write(&p->writes[i-1],w)>=0)) return VP_LINK_PREIMAGE;
    }
    for (uint32_t i=0;i<p->count;++i) {
        const VPNativeWrite *w=&p->writes[i];unsigned char *out=m[w->module].image+w->offset;
        for (unsigned j=0;j<4;++j) out[j]=(unsigned char)(w->after>>(j*8));
    }
    for (uint32_t i=0;i<count;++i) m[i].state=2;
    p->committed=1;return 0;
}

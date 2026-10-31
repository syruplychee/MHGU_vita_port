#include "sdk_exception_index.h"
#include <stdlib.h>
#include <string.h>
static uint32_t word(const void *p) { uint32_t v;memcpy(&v,p,4);return v; }
static int64_t displacement(uint32_t v) { return (v&0x40000000u)?(int64_t)(v&0x7fffffffu)-0x80000000ll:v&0x7fffffffu; }
static int compare(const void *a,const void *b) {
    uint32_t x=word(a),y=word(b);if (x!=y) return x<y?-1:1;
    x=word((const unsigned char *)a+4);y=word((const unsigned char *)b+4);return x<y?-1:x>y;
}
static int encode(int64_t target,uint32_t place,uint32_t *out) {
    int64_t delta=target-place;
    if (delta<-(int64_t)0x40000000 || delta>=0x40000000ll) return VP_UNWIND_METADATA;
    *out=(uint32_t)delta&0x7fffffffu;return 0;
}
int vp_sdk_exception_index(const VPUnwindModule *u,uint32_t offset,VPSdkExceptionIndex *out) {
    if (!u || !u->module || !out || (offset&7)) return VP_UNWIND_ARGUMENT;
    const VPNativeModule *m=u->module;
    if (offset<m->nso.span || offset<m->island_offset+m->island_bytes || offset>m->capacity
        || u->entries>(m->capacity-offset)/8 || u->offset>m->nso.span || u->entries>(m->nso.span-u->offset)/8) return VP_UNWIND_STATE;
    unsigned char digest[32];vp_sha256(m->image+u->offset,u->entries*8,digest);
    if (memcmp(digest,u->digest,32)) return VP_UNWIND_METADATA;
    unsigned char *dest=m->image+offset;
    /* Sort temporary {module-relative function, original record offset}. The
     * secondary key makes equal function starts deterministic without qsort
     * stability assumptions and retains the SDK's observed last-record rule. */
    for (uint32_t i=0;i<u->entries;++i) {
        uint32_t place=u->offset+i*8;
        int64_t target=(int64_t)place+displacement(word(m->image+place));
        if (target<0 || target>UINT32_MAX) return VP_UNWIND_METADATA;
        uint32_t pair[2]={(uint32_t)target,place};memcpy(dest+i*8,pair,8);
    }
    qsort(dest,u->entries,8,compare);
    for (uint32_t i=0;i<u->entries;++i) {
        uint32_t function=word(dest+i*8),original=word(dest+i*8+4),values[2],place=offset+i*8;
        int rc=encode(function,place,&values[0]);if (rc) return rc;
        values[1]=word(m->image+original+4);
        if (values[1]!=1 && !(values[1]&0x80000000u)) {
            rc=encode((int64_t)original+4+displacement(values[1]),place+4,&values[1]);if (rc) return rc;
        }
        memcpy(dest+i*8,values,8);
    }
    VPSdkExceptionIndex next={offset,u->entries,{0}};vp_sha256(dest,u->entries*8,next.digest);*out=next;return 0;
}
int vp_sdk_exception_index_verify(const VPUnwindModule *u,const VPSdkExceptionIndex *view) {
    if (!u || !u->module || !view || view->entries!=u->entries || view->offset<u->module->nso.span
        || view->offset>u->module->capacity || view->entries>(u->module->capacity-view->offset)/8) return VP_UNWIND_STATE;
    unsigned char digest[32];vp_sha256(u->module->image+view->offset,view->entries*8,digest);
    return memcmp(digest,view->digest,32)?VP_UNWIND_METADATA:0;
}

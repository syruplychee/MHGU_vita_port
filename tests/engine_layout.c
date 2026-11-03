#include "engine_layout.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    VPEngineLayoutPool *p=calloc(1,sizeof(*p));
    vp_gpu_state *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b));
    vp_mme_shared *sa=calloc(1,sizeof(*sa)),*sb=calloc(1,sizeof(*sb));
    uint32_t input[24]={0};
    /* A real decoder packet, with immediate and incrementing commands. */
    const uint32_t packet[]={0x20020458u,0x12345678u,0x3a400040u,
        0x90000700u,0x80010620u,0x20010703u,4u};
    vp_gpu_write writes[64];
    vp_gpu_decode_stats stats;
    assert(p && a && b && sa && sb);
    assert(!vp_engine_layout_owner(p,0u));
    assert(vp_engine_layout_owner(p,11u));
    assert(vp_engine_layout_owner(p,11u));
    assert(!vp_engine_layout_owner(p,12u));
    assert(p->owner==11u);
    a->sub[0].value[0x701u]=b->sub[0].value[0x701u]=0x8899aabbu;
    a->sub[0].valid[0x701u]=b->sub[0].valid[0x701u]=1u;
    a->sub[0].value[0x70bu]=b->sub[0].value[0x70bu]=99u;
    assert(vp_gpu_decode_collect(packet,7u,writes,64u,&stats)==0 && !stats.unparsed);
    unsigned first=vp_engine_layout_publish(p,0x1000u,input,writes,(unsigned)stats.writes);
    assert(first==1u && vp_engine_layout_find(p,0x1000u,input)==first);
    /* Distinct engine objects sharing identical layout contents reuse the
     * same result; a signature is only a lookup hint, never the identity. */
    assert(vp_engine_layout_find(p,0x2000u,input)==first);
    uint32_t collision[24]={1u,32u};
    assert(!vp_engine_layout_find(p,0x2000u,collision));
    assert(vp_engine_layout_apply(p,VP_ENGINE_LAYOUT_TOKEN|first,0,a,sa)==0);
    for (unsigned i=0;i<stats.writes;i++) {
        vp_gpu_state_apply(b,writes[i].sub,writes[i].method,writes[i].value);
        vp_mme_shared_write(sb,writes[i].method,writes[i].value);
    }
    assert(memcmp(a,b,sizeof(*a))==0 && memcmp(sa,sb,sizeof(*sa))==0);
    assert(a->sub[0].value[0x701u]==0x8899aabbu && a->sub[0].value[0x70bu]==99u);
    /* A reused address with new contents receives another immutable record.
     * A command buffer may replay the first record after this replacement. */
    input[5]=0xdeadbeefu;
    assert(!vp_engine_layout_find(p,0x1000u,input));
    writes[0].value=7u;
    unsigned second=vp_engine_layout_publish(p,0x1000u,input,writes,(unsigned)stats.writes);
    assert(second==2u);
    assert(vp_engine_layout_apply(p,VP_ENGINE_LAYOUT_TOKEN|second,0,a,sa)==0);
    assert(a->sub[0].value[0x458u]==7u);
    assert(vp_engine_layout_apply(p,VP_ENGINE_LAYOUT_TOKEN|first,0,a,sa)==0);
    assert(memcmp(a,b,sizeof(*a))==0 && memcmp(sa,sb,sizeof(*sa))==0);
    assert(vp_engine_layout_apply(p,VP_ENGINE_LAYOUT_TOKEN,0,a,sa)<0);
    assert(vp_engine_layout_apply(p,VP_ENGINE_LAYOUT_TOKEN|first,1,a,sa)<0);
    assert(vp_engine_layout_apply(p,0x12340001u,0,a,sa)<0);
    vp_gpu_write invalid={.method=0x585u,.sub=0,.value=0};
    assert(!vp_engine_layout_publish(p,0x3000u,input,&invalid,1u));
    for (unsigned i=p->used;i<64u;i++) {
        input[0]=i;
        assert(vp_engine_layout_publish(p,0x4000u,input,writes,(unsigned)stats.writes)==i+1u);
    }
    input[0]=99u;
    assert(!vp_engine_layout_publish(p,0x4000u,input,writes,(unsigned)stats.writes));
    assert(vp_engine_layout_apply(p,VP_ENGINE_LAYOUT_TOKEN|first,0,a,sa)==0);
    free(p);free(a);free(b);free(sa);free(sb);
    return 0;
}

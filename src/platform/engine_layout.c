#include "engine_layout.h"
#include <stdlib.h>

#if defined(_MSC_VER)
#include <intrin.h>
#define VP_LAYOUT_LOAD(p) ((unsigned)_InterlockedCompareExchange((volatile long *)(p),0,0))
#define VP_LAYOUT_STORE(p,v) ((void)_InterlockedExchange((volatile long *)(p),(long)(v)))
#else
#define VP_LAYOUT_LOAD(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define VP_LAYOUT_STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#endif
#ifndef VP_ENGINE_LAYOUT_CORE_ONLY
static VPEngineLayoutPool *pool;
typedef void (*VPLayoutBind)(uint32_t *,int,const uint32_t *);
static VPLayoutBind bind_attrib,bind_stream;
#endif

static int same_input(const uint32_t *a,const uint32_t *b) {
    for (unsigned i=0;i<VP_ENGINE_LAYOUT_WORDS;i++) if (a[i]!=b[i]) return 0;
    return 1;
}
int vp_engine_layout_owner(VPEngineLayoutPool *p,uint32_t identity) {
    if (!identity) return 0;
#if defined(_MSC_VER)
    unsigned owner=VP_LAYOUT_LOAD(&p->owner);
    if (!owner) owner=(unsigned)_InterlockedCompareExchange((volatile long *)&p->owner,(long)identity,0);
    return !owner || owner==identity;
#else
    unsigned owner=__atomic_load_n(&p->owner,__ATOMIC_RELAXED);
    if (!owner) {
        unsigned empty=0u;
        if (__atomic_compare_exchange_n(&p->owner,&empty,identity,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED))
            owner=identity;
        else owner=empty;
    }
    return owner==identity;
#endif
}
static uint32_t input_signature(const uint32_t *input) {
    uint32_t result=0u;
    for (unsigned i=0;i<VP_ENGINE_LAYOUT_WORDS;i++)
        result=((result<<5)|(result>>27))^input[i];
    return result;
}
static unsigned source_bucket(uintptr_t source) {
    return (unsigned)((source>>4u)^(source>>11u))&127u;
}
unsigned vp_engine_layout_find(VPEngineLayoutPool *p,uintptr_t source,
    const uint32_t *input) {
    unsigned source_slot=source_bucket(source),previous=p->source_hint[source_slot];
    /* The pointer only selects a candidate. All 96 bytes still establish
     * identity, including after in-place edits and bucket collisions. */
    if (previous && same_input(p->entries[previous-1u].input,input)) return previous;
    uint32_t signature=input_signature(input);
    unsigned bucket=signature&127u,hint=p->hint[bucket];
    if (hint && p->entries[hint-1u].signature==signature &&
        same_input(p->entries[hint-1u].input,input)) {
        p->source_hint[source_slot]=hint;
        return hint;
    }
    for (unsigned i=0;i<p->used;i++)
        if (p->entries[i].signature==signature && same_input(p->entries[i].input,input)) {
            p->hint[bucket]=i+1u;
            p->source_hint[source_slot]=i+1u;
            return i+1u;
        }
    return 0;
}
static int layout_method(uint16_t m) {
    return (m>=0x458u && m<0x468u) || (m>=0x620u && m<0x630u) ||
        (m>=0x700u && m<0x740u && ((m&3u)==0u || (m&3u)==3u));
}
unsigned vp_engine_layout_publish(VPEngineLayoutPool *p,uintptr_t source,
    const uint32_t *input,const vp_gpu_write *writes,unsigned count) {
    if (!count || count>VP_ENGINE_LAYOUT_WRITES) return 0;
    for (unsigned i=0;i<count;i++)
        if (writes[i].sub!=0u || !layout_method(writes[i].method)) return 0;
    unsigned existing=vp_engine_layout_find(p,source,input);
    if (existing) return existing;
    if (p->used==VP_ENGINE_LAYOUT_SLOTS) return 0;
    unsigned index=p->used;
    VPEngineLayoutEntry *e=&p->entries[index];
    e->source=source;
    e->signature=input_signature(input);
    for (unsigned i=0;i<VP_ENGINE_LAYOUT_WORDS;i++) e->input[i]=input[i];
    e->count=count;
    for (unsigned i=0;i<count;i++) e->writes[i]=writes[i];
    VP_LAYOUT_STORE(&p->used,index+1u);
    p->hint[e->signature&127u]=index+1u;
    p->source_hint[source_bucket(source)]=index+1u;
    return index+1u;
}
int vp_engine_layout_apply(const VPEngineLayoutPool *p,uint32_t token,uint8_t sub,
    vp_gpu_state *state,vp_mme_shared *shared) {
    unsigned index=token&0xffffu;
    if (!p || sub!=0u || (token&0xffff0000u)!=VP_ENGINE_LAYOUT_TOKEN ||
        !index || index>VP_LAYOUT_LOAD(&p->used)) return -1;
    const VPEngineLayoutEntry *e=&p->entries[index-1u];
    for (unsigned i=0;i<e->count;i++) {
        const vp_gpu_write *w=&e->writes[i];
        /* Admission excludes CB latches, draw methods and every method with
         * side effects. Apply this prepared subset without generic routing. */
        state->sub[0].value[w->method]=w->value;
        state->sub[0].valid[w->method]=1u;
        shared->regs[w->method]=w->value;
        shared->valid[w->method]=1u;
    }
    return 0;
}
#ifndef VP_ENGINE_LAYOUT_CORE_ONLY
int vp_engine_layout_init(uint32_t attrib_function,uint32_t stream_function) {
    if (pool) return -1;
    pool=calloc(1,sizeof(*pool));
    if (!pool) return -1;
    bind_attrib=(VPLayoutBind)(uintptr_t)attrib_function;
    bind_stream=(VPLayoutBind)(uintptr_t)stream_function;
    return 0;
}

/* This is called with guest TLS. No allocation, stdio or thread/runtime API is
 * permitted here. Only integer operations, pure decode and the original two
 * integer-argument SDK functions are used. One guest TLS identity owns the
 * producer; other threads and recursive reserve callbacks use the originals. */
static void own_increment(uint32_t *value) {
    __atomic_store_n(value,__atomic_load_n(value,__ATOMIC_RELAXED)+1u,__ATOMIC_RELAXED);
}
__attribute__((optimize("no-tree-loop-distribute-patterns")))
int vp_engine_layout_prepare(uint32_t *command,const uint32_t *layout) {
    uint32_t identity;
    __asm__ volatile("mrc p15,0,%0,c13,c0,3":"=r"(identity));
    if (!vp_engine_layout_owner(pool,identity)) {
        __atomic_fetch_add(&pool->other_calls,1u,__ATOMIC_RELAXED);
        return 0;
    }
    own_increment(&pool->calls);
    if (pool->busy) {
        own_increment(&pool->original);
        return 0;
    }
    const uint32_t *source=(const uint32_t *)(uintptr_t)layout[0x3cu/4u];
    unsigned index=vp_engine_layout_find(pool,(uintptr_t)source,source);
    uint32_t before=command[3];
    if (index && command[4]>=before && command[4]-before>=8u) {
        uint32_t *dst=(uint32_t *)(uintptr_t)before;
        dst[0]=0x20010000u|VP_ENGINE_LAYOUT_METHOD;
        dst[1]=VP_ENGINE_LAYOUT_TOKEN|index;
        command[3]=before+8u;
        own_increment(&pool->hits);
        return 1;
    }
    /* Full records stay alive for old/reusable command buffers. Do not pay
     * cold capture/decode costs on each uncached layout once the pool fills. */
    if (!index && VP_LAYOUT_LOAD(&pool->used)==VP_ENGINE_LAYOUT_SLOTS) {
        own_increment(&pool->original);
        own_increment(&pool->full);
        return 0;
    }
    int no_growth=command[4]>=before && command[4]-before>=0x1c4u;
    if (!no_growth) {
        own_increment(&pool->original);
        return 0;
    }
    uint32_t input[VP_ENGINE_LAYOUT_WORDS];
    for (unsigned i=0;i<VP_ENGINE_LAYOUT_WORDS;i++) input[i]=source[i];
    pool->busy=1u;
    bind_attrib(command,16,source);
    const uint32_t *stream_source=(const uint32_t *)(uintptr_t)layout[0x3cu/4u];
    bind_stream(command,4,stream_source+16u);
    pool->busy=0u;
    own_increment(&pool->original);
    uint32_t after=command[3];
    if (!no_growth || stream_source!=source || !same_input(input,source) || after<before ||
        after-before>0x400u || ((after-before)&3u)) return 1;
    vp_gpu_write writes[VP_ENGINE_LAYOUT_WRITES];
    vp_gpu_decode_stats stats;
    if (vp_gpu_decode_collect((const uint32_t *)(uintptr_t)before,(after-before)/4u,
            writes,VP_ENGINE_LAYOUT_WRITES,&stats) || stats.unparsed) return 1;
    if (!vp_engine_layout_publish(pool,(uintptr_t)source,input,writes,(unsigned)stats.writes))
        own_increment(&pool->full);
    return 1;
}
int vp_engine_layout_consume(uint32_t token,uint8_t sub,vp_gpu_state *state,vp_mme_shared *shared) {
    int rc=vp_engine_layout_apply(pool,token,sub,state,shared);
    if (pool && rc) __atomic_fetch_add(&pool->rejected,1u,__ATOMIC_RELAXED);
    return rc;
}
void vp_engine_layout_note_consumed(unsigned count) {
    if (pool && count) __atomic_fetch_add(&pool->consumed,count,__ATOMIC_RELAXED);
}
void vp_engine_layout_report(FILE *log) {
    if (!pool) return;
    fprintf(log,"ENGINE_LAYOUT_STATS calls=%u hits=%u original=%u consumed=%u rejected=%u "
        "entries=%u full=%u other_calls=%u pool_bytes=%u\n",
        VP_LAYOUT_LOAD(&pool->calls)+VP_LAYOUT_LOAD(&pool->other_calls),
        VP_LAYOUT_LOAD(&pool->hits),VP_LAYOUT_LOAD(&pool->original)+VP_LAYOUT_LOAD(&pool->other_calls),VP_LAYOUT_LOAD(&pool->consumed),
        VP_LAYOUT_LOAD(&pool->rejected),VP_LAYOUT_LOAD(&pool->used),VP_LAYOUT_LOAD(&pool->full),
        VP_LAYOUT_LOAD(&pool->other_calls),(unsigned)sizeof(*pool));
}
#endif

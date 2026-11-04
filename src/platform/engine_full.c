#include "engine_full.h"
#include "engine_queue_native.h"
#include <stddef.h>
#ifdef VP_ENGINE_CAPTURE
#include "engine_capture.h"
#endif

volatile uint32_t vp_engine_full_profile_enabled;
volatile uint32_t vp_engine_full_fast_enabled;
volatile uint32_t vp_engine_shared_prepare_enabled;
volatile uint32_t vp_engine_texture_pipeline_enabled;
volatile uint32_t vp_engine_texture_idle_resize_enabled;
volatile uint32_t vp_engine_clear_defer_enabled;
typedef struct {
    uint32_t fast,calls,draws,last_site,last_slot,buffers,handles;
} PrepareFrame;
static uint32_t direct_buffers,direct_handles;
static uint32_t image_base;
static uint32_t calls, failures, nvn_calls, draws, last_site, last_slot, last_context;
extern void vp_engine_nvn_call(uint32_t, const VPEngineNvnCall *, VPEngineNvnResult *);

_Static_assert(offsetof(VPEngineNvnCall, r) == 8, "NVN core-register ABI");
_Static_assert(offsetof(VPEngineNvnCall, stack) == 24, "NVN stack ABI");
_Static_assert(offsetof(VPEngineNvnCall, s) == 60, "NVN float-register ABI");
_Static_assert(sizeof(VPEngineNvnCall) == 80, "NVN call record ABI");
_Static_assert(offsetof(VPEngineNvnResult, has_value) == 8, "NVN return ABI");

/* These helpers also serve the reconstructed functions while guest TLS is
 * installed. Never enter Vita libc with the original game's TLS. */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *vp_engine_copy(void *dst, const void *src, size_t size) {
    unsigned char *d=dst;
    const unsigned char *s=src;
    typedef uint32_t AliasWord __attribute__((may_alias));
    while (size>=4u && !(((uintptr_t)d|(uintptr_t)s)&3u)) {
        *(AliasWord *)d=*(const AliasWord *)s;
        d+=4;s+=4;size-=4;
    }
    for (size_t i=0;i<size;i++) d[i]=s[i];
    return dst;
}
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *vp_engine_clear(void *dst, int value, size_t size) {
    unsigned char *d=dst;
    typedef uint32_t AliasWord __attribute__((may_alias));
    uint32_t byte=(unsigned char)value,word=byte*0x01010101u;
    while (size>=4u && !((uintptr_t)d&3u)) {
        *(AliasWord *)d=word; d+=4;size-=4;
    }
    for (size_t i=0;i<size;i++) d[i]=(unsigned char)value;
    return dst;
}
static uint32_t load(uint32_t address) { return *(const uint32_t *)(uintptr_t)address; }
static void *resolve(void *opaque, uint32_t address) {
    (void)opaque;
#ifdef VP_ENGINE_CAPTURE
    vp_engine_capture_range(address,0xd0u);
#endif
    return (void *)(uintptr_t)address;
}
static void emit(void *opaque, const VPEngineNvnCall *call, VPEngineNvnResult *out) {
    PrepareFrame *frame=opaque;
    frame->last_site=call->site;frame->last_slot=call->slot;frame->calls++;
    if (call->slot>=VP_SLOT_DRAW_ARRAYS && call->slot<=VP_SLOT_DRAW_ELEMENTS_INSTANCED)
        frame->draws++;
    uint32_t function=load(image_base+call->slot);
#ifdef VP_ENGINE_CAPTURE
    vp_engine_capture_before(call,function);
#endif
    vp_engine_nvn_call(function,call,out);
#ifdef VP_ENGINE_CAPTURE
    vp_engine_capture_after(out);
#endif
}
static void memclr(void *opaque, uint32_t site, uint32_t pointer, uint32_t size) {
    (void)opaque;
    uint32_t offset=site==0xbc1134u ? 0x13efd7cu : 0x13efdc4u;
    ((void (*)(uint32_t,uint32_t))(uintptr_t)(image_base+offset))(pointer,size);
}
static void resource(void *opaque, uint32_t site, uint32_t a, uint32_t b) {
    (void)opaque;
    /* The call site chooses the same original resource operation. */
    uint32_t offset=site==0xbbf58cu ? 0xb03d98u:0xb03d2cu;
    ((void (*)(uint32_t,uint32_t))(uintptr_t)(image_base+offset))(a,b);
}
static double numeric(void *opaque, uint32_t site, double a, double b) {
    (void)opaque; (void)site;
    return ((double (*)(double,double))(uintptr_t)(image_base+0x13f0ea4u))(a,b);
}
static void simple(void *opaque,uint32_t site,uint32_t slot,uint32_t a,uint32_t b,uint32_t c,uint32_t d) {
    VPEngineNvnCall call;
    vp_engine_clear(&call,0,sizeof(call));
    call.site=site; call.slot=slot;
    call.r[0]=a;call.r[1]=b;call.r[2]=c;call.r[3]=d;
    emit(opaque,&call,NULL);
}
#define BIND3(name,slot) \
static void name(void *opaque,uint32_t site,uint32_t cmd,unsigned n,uint32_t object) { \
    simple(opaque,site,slot,cmd,n,object,0); }
BIND3(attrib,VP_SLOT_BIND_VERTEX_ATTRIB_STATE)
BIND3(stream,VP_SLOT_BIND_VERTEX_STREAM_STATE)
#undef BIND3
/* Program callback has (cmd, program, flags), unlike the count/object binds. */
static void bind_program(void *opaque,uint32_t site,uint32_t cmd,uint32_t p,uint32_t flags) {
    simple(opaque,site,VP_SLOT_BIND_PROGRAM,cmd,p,flags,0);
}
static uint64_t buffer(void *opaque,uint32_t site,uint32_t object) {
    PrepareFrame *frame=opaque;
    if (frame->fast) {
        /* SDK+30CDE4 invokes pool->vtable[7], then adds the unsigned buffer
         * offset. Only SDK+32708C is folded: it reads the live pool+0x28 u64.
         * No cached address survives a pool edit, relocation or resource call. */
        uint32_t pool=load(object+12u),vtable=load(pool);
        uint32_t getter=load(vtable+0x1cu);
        uint32_t expected=load(image_base+VP_SLOT_BUFFER_GET_ADDRESS)-0x30cde4u+0x32708cu;
        if (getter==expected) {
#ifdef VP_ENGINE_CAPTURE
            vp_engine_capture_range(object,24u);
            vp_engine_capture_range(pool,0x40u);
            vp_engine_capture_range(vtable,0x40u);
#endif
            uint64_t base=(uint64_t)load(pool+0x28u)|((uint64_t)load(pool+0x2cu)<<32);
            frame->buffers++;
            return base+load(object+0x10u);
        }
    }
    VPEngineNvnCall call;
    VPEngineNvnResult out;
    vp_engine_clear(&call,0,sizeof(call));call.site=site;
    call.slot=VP_SLOT_BUFFER_GET_ADDRESS;call.r[0]=object;
    emit(opaque,&call,&out);
    return out.value;
}
static uint64_t handle(void *opaque,uint32_t site,uint32_t device,uint32_t texture,uint32_t sampler) {
    PrepareFrame *frame=opaque;
    if (frame->fast) {
        /* Exact SDK+318888..318898 expression, including the signed low word. */
        uint32_t low=texture|(sampler<<20);
        frame->handles++;
        return ((uint64_t)((low&0x80000000u)?UINT32_MAX:1u)<<32)|low;
    }
    VPEngineNvnCall call;
    VPEngineNvnResult out;
    vp_engine_clear(&call,0,sizeof(call));call.site=site;
    call.slot=VP_SLOT_DEVICE_GET_TEXTURE_HANDLE;
    call.r[0]=device;call.r[1]=texture;call.r[2]=sampler;
    emit(opaque,&call,&out);
    return out.value;
}
static void uniform(void *opaque,uint32_t site,uint32_t cmd,unsigned stage,unsigned index,
    uint64_t address,uint32_t size) {
    (void)opaque;
    VPEngineNvnCall call;
    vp_engine_clear(&call,0,sizeof(call));call.site=site;call.slot=VP_SLOT_BIND_UNIFORM_BUFFER;
    call.r[0]=cmd;call.r[1]=stage;call.r[2]=index;
    call.stack[0]=(uint32_t)address;call.stack[1]=(uint32_t)(address>>32);
    call.stack[2]=size;call.stack_words=3u;emit(opaque,&call,NULL);
}
static void texture(void *opaque,uint32_t site,uint32_t cmd,unsigned stage,unsigned index,uint64_t h) {
    (void)opaque;
    VPEngineNvnCall call;
    vp_engine_clear(&call,0,sizeof(call));call.site=site;call.slot=VP_SLOT_BIND_TEXTURE;
    call.r[0]=cmd;call.r[1]=stage;call.r[2]=index;
    call.stack[0]=(uint32_t)h;call.stack[1]=(uint32_t)(h>>32);
    call.stack_words=2u;emit(opaque,&call,NULL);
}
#define BIND2(name,slot) \
static void name(void *opaque,uint32_t site,uint32_t cmd,uint32_t object) { \
    simple(opaque,site,slot,cmd,object,0,0); }
BIND2(barrier,VP_SLOT_BARRIER)
BIND2(color,VP_SLOT_BIND_COLOR_STATE)
BIND2(multisample,VP_SLOT_BIND_MULTISAMPLE_STATE)
#undef BIND2
static void alpha(void *opaque,uint32_t site,uint32_t cmd,float value) {
    (void)opaque;
    VPEngineNvnCall call;
    vp_engine_clear(&call,0,sizeof(call));call.site=site;call.slot=VP_SLOT_SET_ALPHA_REF;
    call.r[0]=cmd;vp_engine_copy(&call.s[0],&value,4u);call.s_words=1u;
    emit(opaque,&call,NULL);
}
int vp_engine_full_init(uint32_t base) {
    if (image_base) return -1;
    image_base=base;
#ifdef VP_ENGINE_CAPTURE
    if (vp_engine_capture_init(base)) return -1;
#endif
    return 0;
}
static int run(uint32_t context,uint32_t fast) {
    PrepareFrame frame={.fast=fast};
    uint32_t state[VP_ENGINE_STATE_BYTES/4u],scratch[0x100u/4u];
    VPEngineNvnEnv env;
    VPEngineMaterialEnv mat;
    vp_engine_clear(&env,0,sizeof(env));vp_engine_clear(&mat,0,sizeof(mat));
    env.opaque=&frame;mat.opaque=&frame;
    env.resolve=resolve;env.emit=emit;env.memclr=memclr;
    if (fast) env.buffer_address=buffer;
    env.external_resource=resource;env.numeric_pow=numeric;
    env.gpu_root=load(image_base+0x183944cu);
    env.default_viewport=load(image_base+0x183f2b0u);
    env.srgb_config=load(image_base+0x1838db0u);
    env.scratch=(uint32_t)(uintptr_t)scratch;env.scratch_size=sizeof(scratch);
    mat.resolve=resolve;mat.gpu_root=env.gpu_root;
    mat.bind_program=bind_program;mat.bind_vertex_attrib_state=attrib;
    mat.bind_vertex_stream_state=stream;mat.buffer_get_address=buffer;
    mat.bind_uniform_buffer=uniform;mat.device_get_texture_handle=handle;
    mat.bind_texture=texture;mat.barrier=barrier;mat.set_alpha_ref=alpha;
    mat.bind_color_state=color;mat.bind_multisample_state=multisample;
    __atomic_fetch_add(&calls,1u,__ATOMIC_RELAXED);
    __atomic_store_n(&last_context,context,__ATOMIC_RELAXED);
#ifdef VP_ENGINE_CAPTURE
    vp_engine_capture_begin(calls,context,(uint32_t)(uintptr_t)state,
        env.scratch,env.gpu_root);
#endif
    int rc=vp_engine_queue_native_run(&env,&mat,context,(uint32_t)(uintptr_t)state);
#ifdef VP_ENGINE_CAPTURE
    int capture_rc=vp_engine_capture_end();
    if (capture_rc) return capture_rc;
#endif
    __atomic_fetch_add(&nvn_calls,frame.calls,__ATOMIC_RELAXED);
    __atomic_fetch_add(&draws,frame.draws,__ATOMIC_RELAXED);
    __atomic_fetch_add(&direct_buffers,frame.buffers,__ATOMIC_RELAXED);
    __atomic_fetch_add(&direct_handles,frame.handles,__ATOMIC_RELAXED);
    __atomic_store_n(&last_site,frame.last_site,__ATOMIC_RELAXED);
    __atomic_store_n(&last_slot,frame.last_slot,__ATOMIC_RELAXED);
    if (rc) __atomic_fetch_add(&failures,1u,__ATOMIC_RELAXED);
    return rc;
}
int vp_engine_full_run(uint32_t context) { return run(context,0u); }
int vp_engine_full_run_fast(uint32_t context) { return run(context,1u); }
void vp_engine_full_report(FILE *log) {
    if (!image_base) return;
#ifdef VP_ENGINE_CAPTURE
    vp_engine_capture_report(log);
#endif
    fprintf(log,"ENGINE_FULL_STATS calls=%u failures=%u nvn_calls=%u draws=%u "
        "last_site=%08x last_slot=%08x last_context=%08x direct_buffers=%u direct_handles=%u\n",
        __atomic_load_n(&calls,__ATOMIC_RELAXED),
        __atomic_load_n(&failures,__ATOMIC_RELAXED),
        __atomic_load_n(&nvn_calls,__ATOMIC_RELAXED),
        __atomic_load_n(&draws,__ATOMIC_RELAXED),
        __atomic_load_n(&last_site,__ATOMIC_RELAXED),
        __atomic_load_n(&last_slot,__ATOMIC_RELAXED),
        __atomic_load_n(&last_context,__ATOMIC_RELAXED),
        __atomic_load_n(&direct_buffers,__ATOMIC_RELAXED),
        __atomic_load_n(&direct_handles,__ATOMIC_RELAXED));
}

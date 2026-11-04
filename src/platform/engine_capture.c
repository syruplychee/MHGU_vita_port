#include "engine_capture.h"
#include <stddef.h>
#include <stdlib.h>

/* Diagnostic only. Allocation and file I/O run under host TLS; recording uses
 * guest-safe copies only. Pages preserve their first preimage, not a global
 * atomic snapshot. Unknown resource formats are never synthesized. */
extern void *vp_engine_copy(void *, const void *, size_t);
#define PAGE_MAX 4096u
#define CALL_MAX 8192u
#define BLOB_MAX (4u * 1024u * 1024u)
typedef struct { uint32_t start, offset, size; } Blob;
typedef struct {
    VPEngineNvnCall call;
    uint32_t function, has_result;
    uint64_t value;
    uint32_t before[20], after[20];
    Blob gpu, control;
} Record;
typedef struct {
    uint32_t magic, version, header_size, record_size;
    uint32_t count, pages, blob_bytes, base, ctx, state, scratch;
} Header;
_Static_assert(sizeof(Header) == 44, "Capture header ABI");
_Static_assert(sizeof(Record) == 280, "Capture call ABI");
_Static_assert(offsetof(Record, before) == 96, "Capture command ABI");
_Static_assert(offsetof(Record, gpu) == 256, "Capture blob ABI");
static Header header;
static uint32_t *page_addresses;
static unsigned char *page_bytes, *blob_bytes;
static Record *records;
static unsigned active, done, error;
static uint32_t error_address, error_size;

static uint32_t load(uint32_t a) { return *(const uint32_t *)(uintptr_t)a; }
static void fail(unsigned code, uint32_t address, uint32_t size) {
    if (!error) { error=code; error_address=address; error_size=size; }
}
int vp_engine_capture_init(uint32_t base) {
    page_addresses=malloc(PAGE_MAX*sizeof(*page_addresses));
    page_bytes=malloc(PAGE_MAX*4096u);
    records=calloc(CALL_MAX,sizeof(*records));
    blob_bytes=malloc(BLOB_MAX);
    if (!page_addresses || !page_bytes || !records || !blob_bytes) return -1;
    header=(Header){0x31435056u,1u,sizeof(Header),sizeof(Record),0,0,0,base,0,0,0};
    return 0;
}
void vp_engine_capture_range(uint32_t address, uint32_t size) {
    if (!active || error || !size) return;
    if (!address || size>UINT32_MAX-address) { fail(1,address,size); return; }
    uint32_t end=address+size;
    for (uint32_t p=address&~4095u; p<end; p+=4096u) {
        unsigned i;
        for (i=0;i<header.pages;i++) if (page_addresses[i]==p) break;
        if (i<header.pages) continue;
        if (header.pages==PAGE_MAX) { fail(2,p,4096); return; }
        page_addresses[header.pages]=p;
        vp_engine_copy(page_bytes+header.pages*4096u,(void *)(uintptr_t)p,4096u);
        header.pages++;
    }
}
void vp_engine_capture_begin(uint32_t count, uint32_t ctx, uint32_t state,
    uint32_t scratch, uint32_t gpu_root) {
    if (count!=3000u || __atomic_load_n(&done,__ATOMIC_RELAXED)) return;
    active=1; header.ctx=ctx; header.state=state; header.scratch=scratch;
    vp_engine_capture_range(ctx,0x43bb80u);
    vp_engine_capture_range(state,0x1180u);
    vp_engine_capture_range(scratch,0x100u);
    vp_engine_capture_range(gpu_root,0x20u);
    vp_engine_capture_range(load(gpu_root+0x14u),0x5c0u);
}
static void object(uint32_t a) { if (a) vp_engine_capture_range(a,0x100u); }
void vp_engine_capture_before(const VPEngineNvnCall *call, uint32_t function) {
    if (!active || error) return;
    if (header.count==CALL_MAX) { fail(3,call->site,header.count); return; }
    Record *r=&records[header.count];
    vp_engine_copy(&r->call,call,sizeof(*call)); r->function=function;
    uint32_t slot=call->slot, cmd=call->r[0];
    if (slot==VP_SLOT_BUFFER_GET_ADDRESS) {
        vp_engine_capture_range(cmd,0x20u);
        uint32_t pool=load(cmd+0xcu);
        vp_engine_capture_range(pool,0x40u);
        vp_engine_capture_range(load(pool),0x40u);
        return;
    }
    if (slot==VP_SLOT_DEVICE_GET_TEXTURE_HANDLE) return;
    vp_engine_copy(r->before,(void *)(uintptr_t)cmd,sizeof(r->before));
    vp_engine_capture_range(cmd,0x50u);
    vp_engine_capture_range(load(cmd),0x2000u);
    /* Preserve the already open GPU span, which control records may reference. */
    if (r->before[3]>=r->before[5])
        vp_engine_capture_range(r->before[5],r->before[3]-r->before[5]);
    switch (slot) {
    case VP_SLOT_BIND_PROGRAM: {
        uint32_t p=call->r[1]; object(p);
        vp_engine_capture_range(load(p+8u),load(p+0x10u)*4u); break;
    }
    case VP_SLOT_BIND_VERTEX_ATTRIB_STATE:
        vp_engine_capture_range(call->r[2],call->r[1]*4u); break;
    case VP_SLOT_BIND_VERTEX_STREAM_STATE:
        vp_engine_capture_range(call->r[2],call->r[1]*8u); break;
    case VP_SLOT_BIND_BLEND_STATE: case VP_SLOT_BIND_CHANNEL_MASK_STATE:
    case VP_SLOT_BIND_COLOR_STATE: case VP_SLOT_BIND_MULTISAMPLE_STATE:
    case VP_SLOT_BIND_POLYGON_STATE: case VP_SLOT_BIND_DEPTH_STENCIL_STATE:
        object(call->r[1]); break;
    case VP_SLOT_SET_RENDER_TARGETS:
        vp_engine_capture_range(call->r[2],call->r[1]*4u);
        if (call->r[3]) vp_engine_capture_range(call->r[3],call->r[1]*4u);
        for (unsigned i=0;i<call->r[1];i++) {
            object(load(call->r[2]+i*4u));
            if (call->r[3]) object(load(call->r[3]+i*4u));
        }
        object(call->stack[0]); object(call->stack[1]); break;
    case VP_SLOT_SET_BLEND_COLOR: vp_engine_capture_range(call->r[1],16u); break;
    case VP_SLOT_CLEAR_COLOR: vp_engine_capture_range(call->r[2],16u); break;
    case VP_SLOT_COPY_TEXTURE_TO_TEXTURE:
        object(call->r[1]); object(call->r[2]);
        object(call->stack[0]); object(call->stack[1]);
        vp_engine_capture_range(call->stack[2],24u);
        vp_engine_capture_range(call->r[3],24u); break;
    case VP_SLOT_COPY_TEXTURE_TO_BUFFER:
        object(call->r[1]); object(call->r[2]);
        vp_engine_capture_range(call->r[3],24u); break;
    case VP_SLOT_DOWNSAMPLE: object(call->r[1]); object(call->r[2]); break;
    default: break;
    }
}
static void append(Blob *b, uint32_t start, uint32_t end, uint32_t limit) {
    b->start=start; b->offset=header.blob_bytes;
    if (end<start || end>limit) { fail(4,start,end); return; }
    b->size=end-start;
    if (b->size>BLOB_MAX-header.blob_bytes) { fail(5,start,b->size); return; }
    vp_engine_copy(blob_bytes+header.blob_bytes,(void *)(uintptr_t)start,b->size);
    header.blob_bytes+=b->size;
}
void vp_engine_capture_after(const VPEngineNvnResult *out) {
    if (!active || error) return;
    Record *r=&records[header.count];
    if (out) { r->has_result=out->has_value; r->value=out->value; }
    if (r->call.slot!=VP_SLOT_BUFFER_GET_ADDRESS &&
        r->call.slot!=VP_SLOT_DEVICE_GET_TEXTURE_HANDLE) {
        vp_engine_copy(r->after,(void *)(uintptr_t)r->call.r[0],sizeof(r->after));
        append(&r->gpu,r->before[3],r->after[3],r->before[4]);
        append(&r->control,r->before[6],r->after[6],r->before[7]);
    }
    header.count++;
}
int vp_engine_capture_end(void) {
    if (!active) return 0;
    active=0;
    __atomic_store_n(&done,1u,__ATOMIC_RELEASE);
    return -100;
}
static int write_file(const char *path, const void *data, size_t size) {
    FILE *f=fopen(path,"wb");
    if (!f) return -1;
    int bad=fwrite(data,1,size,f)!=size;
    if (fclose(f)) bad=1;
    return bad?-1:0;
}
void vp_engine_capture_report(FILE *log) {
    unsigned expected=1u;
    if (!__atomic_compare_exchange_n(&done,&expected,2u,0,
            __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return;
    if (!error) {
        FILE *f=fopen("ux0:data/mhgu-vita-port/engine-full-pages.bin","wb");
        if (!f) error=6;
        else {
            for (unsigned i=0;i<header.pages;i++) {
                if (fwrite(page_addresses+i,4u,1u,f)!=1u ||
                    fwrite(page_bytes+i*4096u,4096u,1u,f)!=1u) { error=6; break; }
            }
            if (fclose(f)) error=6;
        }
        f=fopen("ux0:data/mhgu-vita-port/engine-full-calls.bin","wb");
        if (!f) error=6;
        else {
            if (fwrite(&header,sizeof(header),1,f)!=1 ||
                fwrite(records,sizeof(*records),header.count,f)!=header.count) error=6;
            if (fclose(f)) error=6;
        }
        if (write_file("ux0:data/mhgu-vita-port/engine-full-encoded.bin",
                blob_bytes,header.blob_bytes)) error=6;
    }
    fprintf(log,"ENGINE_SDK_CAPTURE status=%s error=%u address=%08x size=%u "
        "base=%08x ctx=%08x state=%08x scratch=%08x pages=%u calls=%u bytes=%u\n",
        error?"failed":"ok",error,error_address,error_size,header.base,
        header.ctx,header.state,header.scratch,header.pages,header.count,header.blob_bytes);
    fflush(log);
}

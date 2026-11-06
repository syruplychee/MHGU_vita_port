#ifndef VP_ENGINE_LAYOUT_H
#define VP_ENGINE_LAYOUT_H
#include "gpu_stream.h"
#include <stdint.h>
#include <stdio.h>

#define VP_ENGINE_LAYOUT_SLOTS 64u
#define VP_ENGINE_LAYOUT_WORDS 24u
#define VP_ENGINE_LAYOUT_WRITES 64u
#define VP_ENGINE_LAYOUT_METHOD 0xdffu
#define VP_ENGINE_LAYOUT_TOKEN 0x564c0000u
typedef struct {
    uintptr_t source;
    uint32_t signature;
    uint32_t input[VP_ENGINE_LAYOUT_WORDS];
    unsigned count;
    vp_gpu_write writes[VP_ENGINE_LAYOUT_WRITES];
} VPEngineLayoutEntry;
typedef struct {
    unsigned used,owner,busy;
    unsigned hint[128];
    unsigned source_hint[128];
    uint32_t calls,hits,original,consumed,rejected,full,other_calls;
    VPEngineLayoutEntry entries[VP_ENGINE_LAYOUT_SLOTS];
} VPEngineLayoutPool;

/* Entries are immutable and never evicted: a recorded command buffer can be
 * submitted more than once, long after the originating source has changed. */
unsigned vp_engine_layout_find(VPEngineLayoutPool *,uintptr_t,const uint32_t *);
int vp_engine_layout_owner(VPEngineLayoutPool *,uint32_t);
unsigned vp_engine_layout_publish(VPEngineLayoutPool *,uintptr_t,const uint32_t *,
    const vp_gpu_write *,unsigned);
int vp_engine_layout_apply(const VPEngineLayoutPool *,uint32_t,uint8_t,
    vp_gpu_state *,vp_mme_shared *);
int vp_engine_layout_init(uint32_t attrib_function,uint32_t stream_function);
int vp_engine_layout_prepare(uint32_t *command,const uint32_t *layout);
int vp_engine_layout_consume(uint32_t,uint8_t,vp_gpu_state *,vp_mme_shared *);
void vp_engine_layout_note_consumed(unsigned);
void vp_engine_layout_report(FILE *);
#endif

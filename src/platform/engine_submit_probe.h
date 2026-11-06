#ifndef VP_ENGINE_SUBMIT_PROBE_H
#define VP_ENGINE_SUBMIT_PROBE_H
#include <stdint.h>

/* Return addresses, not call addresses, from the locked main module. */
static inline unsigned vp_engine_submit_family(uint32_t offset) {
    static const uint32_t returns[8]={0xbbee3cu,0xbbefa4u,0xbbf2bcu,0xbbf3f0u,
        0xbbf840u,0xbbfd08u,0xbbffdcu,0xbc0124u};
    for (unsigned i=0;i<8u;i++) if (offset==returns[i]) return i;
    return 8u;
}
typedef struct {
    uint64_t calls,same_object,same_layout,elapsed_us,encoded_bytes;
    uint64_t families[9];
    uint32_t max_us;
} VPEngineSubmitStats;
static inline void vp_engine_submit_account(VPEngineSubmitStats *s,unsigned family,
    int same_object,int same_layout,uint32_t elapsed,uint32_t bytes) {
    s->calls++;
    s->same_object+=same_object!=0;
    s->same_layout+=same_layout!=0;
    s->elapsed_us+=elapsed;
    s->encoded_bytes+=bytes;
    s->families[family<8u?family:8u]++;
    if (elapsed>s->max_us) s->max_us=elapsed;
}
#endif

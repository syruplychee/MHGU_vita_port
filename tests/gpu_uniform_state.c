#include "gpu_stream.h"
#include <assert.h>
#include <stdio.h>

static vp_gpu_state state;

static void select_buffer(unsigned sub, uint64_t address, uint32_t size) {
    vp_gpu_state_apply(&state, sub, 0x8e0, size);
    vp_gpu_state_apply(&state, sub, 0x8e1, (uint32_t)(address >> 32));
    vp_gpu_state_apply(&state, sub, 0x8e2, (uint32_t)address);
}

int main(void) {
    vp_gpu_cbuf_binding b;
    vp_gpu_state_init(&state);
    select_buffer(0, UINT64_C(0x403633200), 256);
    vp_gpu_state_apply(&state, 0, 0x904, 0x31);
    select_buffer(0, UINT64_C(0x403630500), 512);
    vp_gpu_state_apply(&state, 0, 0x904, 0x51);
    b = state.sub[0].cbuf[0][3];
    assert(b.enabled && b.address == UINT64_C(0x403633200) && b.size == 256);
    assert(state.sub[0].cbuf[0][5].address == UINT64_C(0x403630500));
    vp_gpu_state_apply(&state, 0, 0x924, 0x31);
    assert(state.sub[0].cbuf[4][3].address == UINT64_C(0x403630500));
    select_buffer(1, UINT64_C(0x500000000), 64);
    vp_gpu_state_apply(&state, 1, 0x904, 0x31);
    assert(state.sub[0].cbuf[0][3].address == b.address);
    assert(state.sub[1].cbuf[0][3].size == 64);
    vp_gpu_state_apply(&state, 0, 0x904, 0x30);
    assert(!state.sub[0].cbuf[0][3].enabled);
    assert(state.sub[0].cbuf[4][3].enabled);
    vp_gpu_state_apply(&state, 0, 0x904, 0x1f1); /* invalid slot: reject */
    assert(state.sub[0].cbuf[0][5].size == 512);
    vp_gpu_state_apply(&state, 0, 0x8e3, 32);
    vp_gpu_state_apply(&state, 0, 0x8e4, 123);
    vp_gpu_state_apply(&state, 0, 0x8f3, 456);
    assert(state.sub[0].value[0x8e3] == 40);
    puts("PASS uniform binding latch, stages, slots, channels, disable, upload cursor");
    return 0;
}

/* Host regression: exercise the actual mixer across a streaming ring boundary,
 * including silent data, same-address refills, pause and EOS. Linux MAP_32BIT
 * supplies real readable AArch32 addresses; no emulated guest memory. */
#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
/* The host test does not invoke the ARM bridge. Its pointer-size ABI assertion
 * is deliberately unavailable in a 64-bit mixer-only harness. */
#define VP_HOST_BRIDGE_H
#include <stdint.h>
typedef struct VPBridgeFrame VPBridgeFrame;
#include "../src/platform/audio_render.c"

int vp_guest_svc_readable(const VPGuestSvc *s, uint32_t a, uint32_t n) {
    (void)s; return a && n;
}
void vp_guest_svc_log(VPGuestSvc *s, const char *fmt, ...) { (void)s; (void)fmt; }
static void w32(uint32_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static void w64(uint32_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }
static void wave(uint32_t b, unsigned slot, uint32_t data, unsigned frames) {
    uint32_t at = b + 0x60 + slot * 0x38;
    w64(at, data); w64(at + 8, frames * 2); w32(at + 0x14, frames);
}
int main(void) {
    void *mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(mem != MAP_FAILED && (uintptr_t)mem <= UINT32_MAX);
    uint32_t base = (uint32_t)(uintptr_t)mem, data = base + 1024;
    for (unsigned i = 0; i < 300; ++i) ((int16_t *)(uintptr_t)data)[i] = 8192;
    VPAudioRenderer *r = vp_audio_renderer_create(48000, 240);
    ArVoice v = {.in_use=1, .format=2, .sample_rate=48000, .channels=1,
                 .pitch=1, .volume=1, .mix_id=0};
    ArMix mix = {.in_use=1, .buffer_count=1};
    ArResource res = {.in_use=1, .volume={1}};
    float samples[480] = {0};
    r->samples=samples; r->mixes=&mix; r->mix_cap=1;
    r->resources=&res; r->resource_cap=1;
    w32(base + 0x3c, 2); wave(base, 0, data, 100); wave(base, 1, data+200, 200);
    VPAudioRenderStats st = {0}; VPGuestSvc svc = {0};
    render_voice(r, &svc, &v, (const uint8_t *)(uintptr_t)base, base, &st);
    assert(v.wave_consumed==1 && v.played_samples==240 && v.wave_index==1);
    for (unsigned i=0; i<240; ++i) assert(samples[i]==0.25f);
    v.play_state=2; uint64_t cursor=v.cursor_q16;
    render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    assert(v.wave_consumed==1 && v.played_samples==240 && v.cursor_q16==cursor);
    v.play_state=0;
    memset(samples,0,sizeof(samples)); render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    assert(v.wave_consumed==2 && v.played_samples==300);
    for (unsigned i=0; i<240; ++i) assert(samples[i]==(i<60?0.25f:0));
    render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    assert(v.wave_consumed==2 && v.played_samples==300);
    /* Advance to the next slots, then refill slot 0 with identical backing. */
    wave(base,2,data,100); wave(base,3,data,100);
    render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    assert(v.wave_consumed==4 && v.played_samples==500 && v.wave_index==0);
    *(uint8_t *)(uintptr_t)(base+0x60+0x1a)=1;
    render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    *(uint8_t *)(uintptr_t)(base+0x60+0x1a)=0;
    memset((void *)(uintptr_t)data,0,200);
    render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    assert(v.wave_consumed==5 && v.played_samples==600);
    r->voices=&v; r->voice_cap=1;
    uint64_t played=0; uint32_t consumed=0;
    assert(!vp_audio_renderer_voice_status(r,0,&played,&consumed));
    assert(played==600 && consumed==5);
    assert(vp_audio_renderer_voice_status(r,1,&played,&consumed)<0);
    /* EOS retires the final buffer and resets the stream's sample clock. */
    *(uint8_t *)(uintptr_t)(base+0x60+0x38+0x1a)=1;
    render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    *(uint8_t *)(uintptr_t)(base+0x60+0x38+0x1a)=0;
    *(uint8_t *)(uintptr_t)(base+0x60+0x38+0x19)=1;
    render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    assert(v.wave_consumed==6 && v.played_samples==0);
    /* NuPlayer can enqueue pre-roll while its new voice is still Stopped.
     * Those slots must survive until Started; an actual stop after playback
     * flushes the outstanding slots and advances the server ring head. */
    memset(&v,0,sizeof(v)); v.in_use=1; v.format=2; v.sample_rate=48000;
    v.channels=1; v.pitch=1; v.volume=1; v.play_state=1;
    memset((void *)(uintptr_t)base,0,512); w32(base+0x3c,3);
    for (unsigned i=0;i<300;++i) ((int16_t *)(uintptr_t)data)[i]=8192;
    wave(base,0,data,100); wave(base,1,data,200); wave(base,2,data,100);
    render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    assert(v.wave_consumed==0 && v.played_samples==0 && v.wave_index==0);
    assert(v.wave_valid[0] && v.wave_valid[1] && v.wave_valid[2]);
    /* The client marks pre-roll sent and appends its fourth slot at Start. */
    for (unsigned i=0;i<3;++i) *(uint8_t *)(uintptr_t)(base+0x60+i*0x38+0x1a)=1;
    w32(base+0x3c,4); wave(base,3,data,100); v.play_state=0;
    memset(samples,0,sizeof(samples)); render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    assert(v.wave_consumed==1 && v.played_samples==240 && v.wave_index==1);
    for (unsigned i=0;i<240;++i) assert(samples[i]==0.25f);
    v.play_state=1; render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    assert(v.wave_consumed==4 && v.played_samples==0 && v.wave_index==0);
    render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    assert(v.wave_consumed==4); /* repeated Stopped cannot retire twice */
    /* A subsequent stopped pre-roll is also retained for the next Start. */
    w32(base+0x3c,1); memset((void *)(uintptr_t)(base+0x98),0,3*0x38);
    *(uint8_t *)(uintptr_t)(base+0x60+0x1a)=0;
    render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    assert(v.wave_consumed==4 && v.wave_valid[0]);
    v.play_state=0; render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    assert(v.wave_consumed==5 && v.played_samples==100);
    /* Stereo advances the stream clock once, independent of channel count. */
    memset(&v,0,sizeof(v)); v.in_use=1; v.format=2; v.sample_rate=48000;
    v.channels=2; v.pitch=1; v.volume=1; mix.buffer_count=2;
    ArResource stereo[2]={{.in_use=1,.volume={1,0}}, {.in_use=1,.volume={0,1}}};
    r->resources=stereo; r->resource_cap=2;
    memset((void *)(uintptr_t)base,0,512); w32(base+0x3c,2);
    w32(base+0x140,0); w32(base+0x144,1);
    for (unsigned i=0;i<300;++i) {
        ((int16_t *)(uintptr_t)data)[i*2]=8192;
        ((int16_t *)(uintptr_t)data)[i*2+1]=16384;
    }
    wave(base,0,data,100); wave(base,1,data+400,200);
    w64(base+0x60+8,400); w64(base+0x60+0x38+8,800);
    memset(samples,0,sizeof(samples)); render_voice(r,&svc,&v,(const uint8_t *)(uintptr_t)base,base,&st);
    assert(v.wave_consumed==1 && v.played_samples==240);
    for (unsigned i=0;i<240;++i) { assert(samples[i]==0.25f); assert(samples[240+i]==0.5f); }
    r->voices=NULL; r->voice_cap=0;
    r->samples=NULL; r->mixes=NULL; r->resources=NULL;
    vp_audio_renderer_destroy(r); munmap(mem,4096);
    puts("audio wave progress PASS");
}

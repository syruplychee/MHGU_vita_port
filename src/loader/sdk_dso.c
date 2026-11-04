#include "sdk_dso.h"
#include "sdk_dso_contract.h"
#include "sdk_svc_contract.h"
#include "sdk_timer_contract.h"
#include "sdk_barrier_contract.h"
#include "patch_loader.h"
#include "thread_local_allocator.h"
#include "loader_exception_info.h"
#include "sdk_tls_slots.h"
#include "sdk_exception_index.h"
#include "guest_svc.h"
#include "audio_probe.h"
#include "audio_sink.h"
#include "media_video.h"
#include "movie_control.h"
#include "session_gate.h"
#include "engine_submit_probe.h"
#include "engine_layout.h"
#include "engine_full.h"
#include "gxm_runtime.h"
#include <stddef.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/sysmem.h>
#include <string.h>
#include <stdlib.h>
#if defined(__GNUC__) && (defined(__arm__) || defined(__thumb__))
#define VP_SDK_INTEGER_ONLY __attribute__((target("general-regs-only")))
#else
#define VP_SDK_INTEGER_ONLY
#endif
extern void vp_sdk_alloc(void),vp_sdk_free(void),vp_sdk_ctor_gate(void),vp_sdk_dso_enter(void),vp_sdk_lock(void),vp_sdk_unlock(void),vp_sdk_exception_info(void);
extern void vp_sdk_malloc(void),vp_sdk_heap_free(void),vp_sdk_cpp_fixture_end(void);
extern void vp_sdk_tls_normal(void),vp_sdk_tls_private(void),vp_sdk_tls_free_bridge(void),vp_sdk_tls_get_bridge(void),vp_sdk_tls_set_bridge(void);
extern void vp_sdk_try_lock(void),vp_sdk_is_locked(void);
extern void vp_sdk_video_readback(void);
extern void vp_movie_color_rgb_enter(void),vp_movie_color_rgb_leave(void);
extern void vp_movie_color_nv12_enter(void),vp_movie_color_nv12_leave(void);
extern void vp_engine_submit_enter(void),vp_engine_submit_leave(void);
extern void vp_engine_layout_entry(void);
extern void vp_engine_full_entry(void);
uint32_t vp_engine_layout_resume;
uint32_t vp_engine_layout_fallback;
extern const unsigned char vp_sdk_svc_site_template[],vp_sdk_svc_site_end[];
extern const unsigned char vp_sdk_svc_site_service_value[],vp_sdk_svc_site_continuation_value[],vp_sdk_svc_site_branch[];
extern const unsigned char vp_sdk_timer_site_template[],vp_sdk_timer_site_end[];
extern const unsigned char vp_sdk_timer_site_service_value[],vp_sdk_timer_site_rt_value[],
    vp_sdk_timer_site_continuation_value[],vp_sdk_timer_site_branch[];
extern void vp_bridge_saved_entry(void);
extern int vp_sdk_cpp_fixture(int,volatile uint32_t *);
extern uint32_t vp_sdk_cpp_functions[7],vp_cpp_typeinfo[2] __asm__("_ZTIi");
extern const unsigned char vp_fixture_exidx_start[],vp_fixture_exidx_end[];
extern const unsigned char vp_sdk_argument_fixture[],vp_sdk_argument_fixture_end[];
extern const unsigned char vp_sdk_unwind_query[],vp_sdk_unwind_query_end[];
#define VP_SDK_LOCK_LIMIT 512
/* nn::os::detail::InternalCriticalSection is a recursive lock keyed by its
 * guest word. Each address maps to one recursive host mutex; owner is the
 * guest thread handle (start_handle for the main executor) and depth is the
 * guest recursion count. Only the outermost acquisition owns the host mutex;
 * WaitProcessWideKeyAtomic releases it once and restores the guest depth. */
typedef struct { uint32_t address;SceUID id;unsigned held,depth;uint32_t owner;
    unsigned host_depth;
    /* Enter increments pending before it releases the session lock to block
     * on the host mutex, and clears it once the mutex is held. A slot with
     * pending set is mid-acquisition: its host mutex may already be owned
     * even though depth has not been updated yet, so it must not be
     * recycled. */
    unsigned pending;
    /* B126: diagnostic hold timestamp (0 when the play-perf marker is off). */
    uint64_t held_since_us; } Lock;
#define VP_SDK_LOCK_BLOCK_LIMIT_US 5000000u
#define VP_SDK_LOCK_DEADLOCK (-143)
/* B126: one long host-mutex wait per acquisition attempt. The kernel's queued
 * wake-up decides who gets the lock when it is released; short retry slices
 * re-queued the waiter and let rapid acquire/release loops overtake it. */
#define VP_SDK_LOCK_SLICE_US 1000000u
/* B125: bounded diagnostics for the movie-pacing investigation. The session
 * mutex bookkeeping below and the arrival trace further down share these
 * switches; both only emit while the play-perf marker is present. */
static unsigned g_sdk_lock_wait_lines;
static unsigned g_sdk_hold_lines;
static int svc_arrival_enabled(void);
/* Horizon process-wide keys: one host event flag per blocked guest thread.
 * The kernel hands the lock word between waiters; this platform model clears
 * the word on entry and wakes through the host event flag instead, so the
 * handoff chain is not reproduced (logged as model=event-flag). The critical
 * section registered on the wait address is released before parking and
 * reacquired when the waiter is signalled, which is the observable effect of
 * the kernel's atomic release and next-owner handoff. */
#define VP_KEY_WAIT_LIMIT 32u
typedef struct { uint32_t key,address,tag;SceUID event;unsigned used,signaled; } KeyWait;
typedef struct {
    uint64_t started_us;
    uint32_t mode,width,height,destination,source_y,source_uv;
} VPMovieColorTiming;
static int movie_color_probe_enabled;
static int engine_submit_probe_enabled;
static int engine_full_ab_enabled;
static uint32_t engine_full_ab_trampoline;
static uint32_t engine_submit_replay;
static uint32_t engine_submit_leave_replay;
#define VP_GAME_COST_SITES 4u
#define VP_GAME_COST_DEPTH 16u
extern void vp_game_cost_frame(void), vp_game_cost_submit(void);
extern void vp_game_cost_queue(void), vp_game_cost_end_frame(void);
static int game_cost_enabled;
static uint32_t game_cost_trampolines[VP_GAME_COST_SITES];
typedef struct {
    uint64_t started, child_us, bridge_us, bridge_calls;
    uint32_t site, context, caller, presents;
} VPGameCostSpan;
typedef struct {
    VPGameCostSpan stack[VP_GAME_COST_DEPTH];
    unsigned depth;
} VPGameCostThread;
/* Same-process A/B accounting. entry_us/t_us use the process clock on the host
 * side; command is the root context's command buffer and before/after are its
 * write offsets. mode is the mode selected when this call entered, so a control
 * file change applies only to later root calls. */
#define VP_ENGINE_AB_CONTROL_PATH "ux0:data/mhgu-vita-port/engine-full-mode.txt"
#define VP_ENGINE_AB_POLL_US 500000ull
typedef struct {
    uint64_t entry_us;
    uint32_t context,command,before,block_before,mode,active;
} VPEnginePrepareTiming;
typedef struct {
    uint64_t started;
    uint32_t active,object,state,command,before,family,same_object,same_layout;
} VPEngineSubmitTiming;
typedef struct {
    VPThreadLocalAllocator allocator;
    Lock locks[VP_SDK_LOCK_LIMIT];
    unsigned lock_hint[64]; /* Slot plus one; verify the address after reuse. */
    KeyWait key_waits[VP_KEY_WAIT_LIMIT];
    unsigned key_waits_live,key_signals,key_wakes,key_timeouts;
    uint32_t original_args[4][21],resume,observed,observed_mask,gated,lock_calls,unlock_calls;
    unsigned lock_recycles,lock_cursor;
    unsigned fast_enter,fast_leave,fast_try,fast_islocked,fast_fallback;
    unsigned fast_miss[6]; /* Context, session busy, owner, slot, permission, contention. */
    int fast_enabled;
    VPExecutor *main_executor;
    VPMovieColorTiming movie_color[64]; /* Main plus the 63 guest worker slots. */
    unsigned movie_color_samples;
    VPEngineSubmitTiming engine_submit[64];
    VPEnginePrepareTiming engine_prepare[64];
    VPGameCostThread game_cost[64];
    uint64_t engine_ab_poll_us;
    uint32_t engine_ab_mode;
    unsigned engine_ab_mode_valid;
    VPEngineSubmitStats engine_stats;
    unsigned movie_readback_stacks;
    VPUnwindRegistry exceptions;
    VPSdkExceptionIndex indexes[4];
    uint32_t exception_output[4],exception_queries;
    volatile uint32_t destructors;
    VPSdkTlsPool tls_pool;VPSdkTlsValues tls_values;
    unsigned heap_allocs,heap_frees,tls_destructors;
    VPGuestSvc svc;
    SceUID pool_block,launch_block;
    uint32_t pool_base,launch_address,start_handle,start_result,start_flag;
    unsigned ctor_dispatches;
    SceUID state_lock;          /* Kernel backend, also retained for rollback. */
    VPSessionGate state_gate;
    int session_kernel;
    volatile uint32_t stopping;
    /* Temporary diagnosis: if the thread that entered nnMain stops issuing
     * SVCs while other guest threads keep running, its guest stack is dumped
     * once so the user-mode loop it is parked in can be resolved offline. */
    uint32_t main_sp;
    uint64_t main_tick;
    int stall_dumped;
    uint64_t main_bridge_tick;
    unsigned main_bridge_burst;
    /* Temporary diagnosis: a main-thread busy loop that only reads the
     * generic timer parks no SVC, so the stall check above never fires. The
     * first sustained timer-only burst dumps the stack once. */
    uint64_t main_timer_tick;
    uint64_t main_late_tick;
    uint64_t main_heart_tick;
    int timer_dumped;
    int late_dumped;
    int wait118_dumped;
    int nnmain_scanned; /* temporary: live-frame scans for nnMain */
    int nnmain_found;
    unsigned nnmain_attempts;
    /* Evidence-bounded session stop: once nnMain has been observed live, let
     * the title's frame loop run for a grace period and then end the session
     * from the next main-thread bridge call. This is a loader-side stop, not
     * a title exit or a simulated success. */
    uint64_t nnmain_found_tick;
    int session_stopped;
    /* Set only when the title acted on a platform exit request and the
     * ISelfController exit model terminated the process. Distinct from the
     * loader-side evidence stop below. */
    int title_exited;
    /* Frame-loop observation for the evidence-bounded stop: the vi:obj
     * BufferQueue producer must have completed at least one full ring cycle
     * (the consumer model released a presented slot) before the stop may
     * claim a frame loop. */
    int frame_observed;
    uint64_t frame_tick;
    unsigned frame_queues, frame_presents;
    int heart_diag;
    /* Explicit termination test: when the marker file exists, deliver a
     * reviewed applet Exit message to the title's own message loop after the
     * frame loop starts. The title decides whether to exit; the loader does
     * not synthesize nnMain's return. */
    int termination_test;
    /* Interactive play must not take the probe's two-second evidence stop or
     * inject an applet Exit. Actual title-requested process exit still works. */
    int continuous_play;
    int applet_exit_sent;
    uint64_t applet_exit_tick;
    /* Diagnostic knob for the explicit termination test: how long the title
     * runs after the frame loop starts before the reviewed applet Exit message
     * is delivered. Default 30 s; applet-exit-delay-ms overrides it so a long
     * observation does not change the test's semantics or the guest code. */
    uint64_t termination_delay_us;
    /* M5 audio probe. The SDK client entry points are resolved once after
     * the module bases are known; the probe thread is created only when the
     * service observes the title's first successful OpenAudioRenderer and
     * the marker file is present. */
    struct {
        uint32_t init_param, get_work_size, get_cfg_size, init_cfg;
        uint32_t open, start, stop, update;
        uint32_t acquire_voice, set_voice_dest, set_play_state;
        uint32_t set_voice_mix_volume;
        uint32_t acquire_final_mix, add_sink;
        uint32_t create_thread, start_thread;
        int status;
    } audio_probe_sdk;
    void *audio_probe_area;
    uint32_t audio_probe_area_addr;
    uint32_t audio_probe_args_addr;
    uint32_t audio_probe_thread_addr;
    uint32_t audio_probe_stack_addr;
    SceUID audio_probe_monitor;
    int audio_probe_launched;
    int audio_probe_marker;
    FILE *log;
} Session;
static Session session;
static int vp_sdk_guest_call_args(void *user,uint32_t module,uint32_t address,
                                  const uint32_t *args,uint32_t count,uint32_t *result);
static void vp_sdk_audio_probe_request(void *user);
static void vp_sdk_audio_probe_resolve(Session *s,VPNativeModule *sdk);
unsigned vp_sdk_dso_nnos_returned(void) { return session.start_flag==1u?1u:0u; }
unsigned vp_sdk_dso_nnmain_entered(void) { return session.nnmain_found?1u:0u; }
unsigned vp_sdk_dso_session_stopped(void) { return session.session_stopped?1u:0u; }
unsigned vp_sdk_dso_title_exited(void) { return session.title_exited?1u:0u; }
static int termination_test_enabled(void) {
    FILE *f = fopen("ux0:data/mhgu-vita-port/applet-exit", "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}
static int vp_sdk_guest_call(void *user,uint32_t module,uint32_t address,
                             uint32_t arg,uint32_t *result);

/* Guest threads started through svcCreateThread run the original SDK entry
 * trampoline on their own executor stack. The stack and its VPGuestContext
 * must exist before vp_guest_registry_seal(), so the pool is provisioned by
 * lookup_tests and only handed to the session at execution time. */
/* The locked title's movie stack starts its own decoder/player thread group
 * while the SDK pool is already occupied by the boot, audio and rendering
 * threads: the nvdec bring-up observed 32 live threads and a 33rd create
 * before the stream was even submitted. The pool is a loader resource, not a
 * game-logic limit, so it is sized for the observed peak with headroom; the
 * 128 KiB stack per thread is unchanged.
 *
 * 63 is deliberate. The stack memblock is 63*0x20000 = 0x7E0000 bytes, which
 * is not a power of two. Vita3K's ksceKernelAllocMemBlock raises the alignment
 * to `size & -size` (0x20000 here) and alloc_aligned reserves size+alignment,
 * so the block costs exactly 8 MiB of the 4 GiB user address space. A 64th
 * thread would make the size 0x800000 and the alignment 8 MiB, reserving
 * 16 MiB and pushing vp-guest-pool past 4 GiB (SCE_KERNEL_ERROR_NO_MEMORY
 * from the pool allocation before the SDK allocator even runs). */
#define VP_GUEST_THREAD_LIMIT 63
#define VP_GUEST_THREAD_STACK_BYTES 0x20000u
#define VP_GUEST_THREAD_TLS_BYTES 0x200u
#define VP_GUEST_THREAD_RW_BYTES 0x400u
#define VP_BRIDGE_THREAD_EXIT (-200)
#define VP_NOT_HANDLED (-999)
typedef struct {
    volatile int state;         /* 0 free, 1 created, 2 running, 3 exited */
    uint32_t handle,type,entry,arg,priority,core;
    SceUID host;
    /* svcCancelSynchronization targets a thread, not the object it waits on.
     * These flags implement the kernel's thread-level wait cancellation:
     * wait_cancel_requested may arrive before the thread parks (the reference
     * kernel keeps it pending and the next cancellable wait returns
     * ResultCancelled), and a parked empty wait polls it while the session
     * lock is dropped. */
    volatile unsigned wait_cancel_requested;
    volatile unsigned wait_cancellable;
    uint32_t tls_ro_storage[VP_GUEST_THREAD_TLS_BYTES/4];
    uint32_t tls_rw_storage[VP_GUEST_THREAD_RW_BYTES/4];
    VPSdkTlsValues tls_values;
    VPExecutor executor;
    /* Alias-stack snapshot. The SDK maps the stack with svcMapMemory and
     * writes the thread-entry thunk at dst+size-16. The mapping is associated
     * with the next created thread; its slot word is checked at start and at
     * worker entry so a clobber is reported instead of a blx to 0. */
    uint32_t alias_dst, alias_size, entry_slot_word, alias_valid;
} VPGuestThread;
/* The thread stacks are one memblock. Splitting it into chunks was tried
 * while the pool-allocation failure was attributed to this block; probe2's
 * log showed the block itself succeeding and the SDK_SET_ALLOCATOR failure
 * coming from the pool instead, so the chunking only fragmented the address
 * space. The block size is chosen so its alignment charge stays 8 MiB (see
 * VP_GUEST_THREAD_LIMIT). */
typedef struct {
    VPGuestThread threads[VP_GUEST_THREAD_LIMIT];
    SceUID stack_block;
    uint32_t stack_base;
    unsigned prepared;
} VPGuestThreadPool;
static VPGuestThreadPool thread_pool;
static int thread_probe_enabled=-1;
volatile uint32_t vp_movie_umovie_thread;
volatile uint32_t vp_movie_umovie_state;
static uint32_t resume;
VP_SDK_INTEGER_ONLY
static uint32_t host_tls(void) { uint32_t v;__asm__ volatile("mrc p15,0,%0,c13,c0,3":"=r"(v));return v; }
static uint32_t host_fp(void) { uint32_t v;__asm__ volatile("vmrs %0,FPSCR":"=r"(v));return v; }
int vp_sdk_dso_replaces_tls(uint32_t offset) {
    for (unsigned i=1;i<sizeof(vp_sdk_dso_patches)/sizeof(vp_sdk_dso_patches[0]);++i)
        if (offset>=vp_sdk_dso_patches[i].offset && offset-vp_sdk_dso_patches[i].offset<vp_sdk_dso_patches[i].bytes) return 1;
    return 0;
}
static uint32_t word(const void *p) { uint32_t v;memcpy(&v,p,4);return v; }
static uint32_t init_flag(const VPNativeModule *m) {
    int64_t offset=0x158ll+(int32_t)word(m->image+0x204);
    return offset>=0 && offset<=m->nso.span-4?word(m->image+(uint32_t)offset):UINT32_MAX;
}
/* Diagnostic-only input-path probe. The game's FullKey and Handheld pad
 * readers (main module 0x86d34c / 0x86e1b4) are the only callers of the
 * GetNpadState stubs in this title. With the marker present, the word pair
 * after each reader's SDK call is replaced by an absolute veneer into a copied
 * SVC island: the island replays those two displaced words, enters the host
 * bridge with a probe service, and resumes at the original continuation. The
 * handler only records the npad id and the state the SDK returned. Every word
 * is checked against the locked image before it is written; the marker absent
 * means no site is touched. */
static int sdk_hid_helper_state_probe(FILE *log,VPNativeModule modules[4]) {
    static const struct { uint32_t offset,word0,word1,svc; const char *name; } sites[]={
        {0x86d37cu,0xe59d0020u,0xe3a01000u,0x7du,"fullkey"},
        {0x86e1e4u,0xe3a05001u,0xe3a01000u,0x7cu,"handheld"},
    };
    const unsigned site_count=sizeof(sites)/sizeof(sites[0]);
    const uint32_t template_bytes=(uint32_t)(vp_sdk_svc_site_end-vp_sdk_svc_site_template);
    const uint32_t tp_service=(uint32_t)(vp_sdk_svc_site_service_value-vp_sdk_svc_site_template);
    const uint32_t tp_continuation=(uint32_t)(vp_sdk_svc_site_continuation_value-vp_sdk_svc_site_template);
    const uint32_t tp_branch=(uint32_t)(vp_sdk_svc_site_branch-vp_sdk_svc_site_template);
    const uint32_t stride=(8u+template_bytes+15u)&~15u;
    FILE *marker=fopen("ux0:data/mhgu-vita-port/hid-helper-probe.enable","rb");
    VPNativeModule *game=&modules[1],*sdk=&modules[3];
    uint32_t base;
    VPPatchTarget site_target;
    int rc;
    if (!marker) return VP_PATCH_OK;
    fclose(marker);
    if (8u+template_bytes>stride || tp_service+8u>template_bytes
        || tp_continuation+4u>template_bytes || tp_branch+4u>template_bytes
        || (tp_branch&3u)) return VP_PATCH_TEMPLATE;
    base=sdk->island_offset+sdk->island_bytes;
    if (sdk->island_offset>sdk->capacity || base>sdk->capacity
        || site_count*stride>sdk->capacity-base) return VP_PATCH_RANGE;
    rc=vp_patch_open(&site_target,game->image,game->nso.span,game->base);
    for (unsigned i=0;i<site_count && !rc;++i) {
        uint32_t original0=0,original1=0;
        uint32_t place=base+i*stride,island=sdk->base+place;
        uint32_t service=VP_GUEST_SVC_BASE+sites[i].svc;
        uint32_t continuation=game->base+sites[i].offset+8u;
        int64_t delta;
        uint32_t branch;
        if (sites[i].offset>game->nso.segments[0].bytes-8u) { rc=VP_PATCH_RANGE; break; }
        memcpy(&original0,game->image+sites[i].offset,4);
        memcpy(&original1,game->image+sites[i].offset+4,4);
        if (original0!=sites[i].word0 || original1!=sites[i].word1) { rc=VP_PATCH_BINDING; break; }
        memcpy(sdk->image+place,&original0,4);
        memcpy(sdk->image+place+4,&original1,4);
        memcpy(sdk->image+place+8,vp_sdk_svc_site_template,template_bytes);
        memset(sdk->image+place+8+template_bytes,0,stride-8-template_bytes);
        memcpy(sdk->image+place+8+tp_service,&service,4);
        memcpy(sdk->image+place+8+tp_continuation,&continuation,4);
        /* The template is copied away from its link-time address, so its
         * relative branch to the host bridge is re-encoded for the copy. */
        delta=(int64_t)(uint32_t)(uintptr_t)vp_bridge_saved_entry
            -((int64_t)(island+8)+(int64_t)tp_branch+8);
        if ((delta&3)||delta<-(int64_t)VP_PATCH_BRANCH_REACH
            ||delta>(int64_t)VP_PATCH_BRANCH_REACH-4) { rc=VP_PATCH_REACH; break; }
        branch=0xea000000u|(((uint32_t)(delta>>2))&0xffffffu);
        memcpy(sdk->image+place+8+tp_branch,&branch,4);
        /* Absolute veneer at the site: ldr pc,[pc,#-4]; .word island. The
         * island replays both displaced words, so the reader resumes at the
         * second word's following instruction (site+8). */
        rc=vp_patch_word(&site_target,sites[i].offset,sites[i].word0,0xe51ff004u);
        if (!rc) rc=vp_patch_word(&site_target,sites[i].offset+4,sites[i].word1,island);
        fprintf(log,"HID_HELPER_PATCH name=%s site=0x%x island=0x%08x service=0x%x "
            "continuation=0x%08x status=%d\n",
            sites[i].name,sites[i].offset,island,service,continuation,rc);
    }
    if (!rc) sdk->island_bytes=base+site_count*stride-sdk->island_offset;
    fflush(log);
    return rc;
}
static int movie_color_probe_patch(FILE *log,VPNativeModule *m) {
    FILE *marker=fopen("ux0:data/mhgu-vita-port/media-color-probe.enable","rb");
    movie_color_probe_enabled=marker!=NULL;
    if (marker) fclose(marker);
    fprintf(log,"MOVIE_COLOR_PROBE_MODE enabled=%d module=subsdk0\n",movie_color_probe_enabled);
    if (!movie_color_probe_enabled) return 0;
    static const unsigned char module_sha[32]={
        0x75,0xfb,0x81,0x27,0x05,0x61,0x2e,0xc6,0x8c,0xd0,0x76,0x06,0x40,0x18,0x3a,0x14,
        0x0a,0x32,0xd4,0xd0,0x71,0x81,0xa4,0x03,0x55,0xcc,0x00,0x2c,0x52,0x09,0x62,0x45};
    static const unsigned char build_id[32]={
        0xc2,0x77,0x4f,0xa4,0xca,0xce,0x9e,0xcc,0x6a,0x71,0x2b,0xfc,0x83,0x92,0x14,0xc1,
        0x95,0x15,0xf3,0x6e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    static const struct { uint32_t offset,bytes;unsigned char sha[32]; } functions[]={
        {0x33c4e8u,0x1acu,{
            0xfd,0x83,0xcf,0x27,0x7f,0xeb,0x8d,0x26,0xb9,0x99,0x0d,0x17,0xbd,0xb7,0x5c,0xc0,
            0xe6,0xd9,0x18,0x31,0x04,0x5c,0xce,0x14,0x5c,0xc0,0x54,0x27,0xcf,0x8a,0x98,0xcf}},
        {0x33c694u,0x1ecu,{
            0x5f,0x85,0xc2,0xe7,0x81,0xd5,0xad,0xd8,0x39,0xd2,0xcd,0xe7,0x32,0xda,0xe7,0x70,
            0xc7,0xb6,0x2b,0x99,0xf9,0x8c,0x32,0x58,0x2e,0xdb,0x7c,0xbf,0x59,0x5c,0xc6,0x5c}}};
    static const struct { uint32_t offset,word;void (*entry)(void); } sites[]={
        {0x33c4f4u,0xe590e040u,vp_movie_color_rgb_enter},
        {0x33c678u,0xe5810000u,vp_movie_color_rgb_leave},
        {0x33c6a0u,0xe58d100cu,vp_movie_color_nv12_enter},
        {0x33c868u,0xe5890000u,vp_movie_color_nv12_leave}};
    if (memcmp(m->nso.file_sha256,module_sha,32) || memcmp(m->nso.build_id,build_id,32))
        return VP_PATCH_BINDING;
    /* Check both whole functions and all four original words before the first
     * movie probe write. A different linked body is rejected, never sampled. */
    for (unsigned i=0;i<2u;++i) {
        unsigned char sha[32];
        if (functions[i].offset>m->nso.segments[0].bytes
            || functions[i].bytes>m->nso.segments[0].bytes-functions[i].offset) return VP_PATCH_RANGE;
        vp_sha256(m->image+functions[i].offset,functions[i].bytes,sha);
        if (memcmp(sha,functions[i].sha,32)) return VP_PATCH_BINDING;
    }
    for (unsigned i=0;i<4u;++i)
        if (word(m->image+sites[i].offset)!=sites[i].word) return VP_PATCH_BINDING;
    if (m->island_offset>m->capacity || m->island_bytes>m->capacity-m->island_offset)
        return VP_PATCH_RANGE;
    uint32_t off=m->island_offset+m->island_bytes;
    if ((off&3u) || 32u>m->capacity-off) return VP_PATCH_RANGE;
    VPPatchTarget target;
    int rc=vp_patch_open(&target,m->image,m->nso.span,m->base);
    for (unsigned i=0;i<4u && !rc;++i) {
        uint32_t island=m->base+off+i*8u;
        int64_t delta=(int64_t)island-((int64_t)m->base+sites[i].offset+8);
        if ((delta&3) || delta<-(int64_t)VP_PATCH_BRANCH_REACH || delta>=VP_PATCH_BRANCH_REACH)
            return VP_PATCH_REACH;
        uint32_t veneer[2]={0xe51ff004u,(uint32_t)(uintptr_t)sites[i].entry};
        memcpy(m->image+off+i*8u,veneer,sizeof(veneer));
        rc=vp_patch_site(&target,sites[i].offset,sites[i].word,island);
        fprintf(log,"MOVIE_COLOR_PROBE_PATCH site=0x%x word=0x%08x service=0x%x island=0x%08x status=%d\n",
            sites[i].offset,sites[i].word,0x24cu+i,island,rc);
    }
    if (!rc) m->island_bytes+=32u;
    return rc;
}

static int engine_submit_probe_patch(FILE *log,VPNativeModule *m) {
    FILE *marker=fopen("ux0:data/mhgu-vita-port/engine-submit-probe.enable","rb");
    engine_submit_probe_enabled=marker!=NULL;
    if (marker) fclose(marker);
    fprintf(log,"ENGINE_SUBMIT_PROBE_MODE enabled=%d\n",engine_submit_probe_enabled);
    if (!engine_submit_probe_enabled) return 0;
    static const unsigned char file_sha[32]={
        0xb3,0x93,0x54,0x15,0x49,0x95,0x49,0xad,0xaf,0x4c,0x0e,0x89,0x82,0x28,0x31,0xda,
        0xa1,0x75,0xcf,0xd4,0x56,0xa7,0x99,0x07,0x91,0xc7,0x9b,0xe3,0x99,0x76,0x45,0xef};
    static const unsigned char body_sha[32]={
        0xe7,0x1e,0x66,0xbb,0xa8,0x8c,0x89,0xc1,0x8a,0xd8,0xd2,0xe4,0x65,0x1d,0xd3,0x18,
        0x8c,0xcb,0x3a,0xed,0xf5,0x34,0x84,0xaf,0x9b,0x30,0xd2,0x10,0xc8,0xb4,0x65,0x7e};
    static const struct {uint32_t site,word;void (*entry)(void);} sites[]={
        {0xbc1764u,0xe92d4ff0u,vp_engine_submit_enter},
        {0xbc23d4u,0xe28dd04cu,vp_engine_submit_leave}};
    unsigned char sha[32];
    if (memcmp(m->nso.file_sha256,file_sha,32)) return VP_PATCH_BINDING;
    if (m->nso.segments[0].bytes<0xbc23dcu) return VP_PATCH_RANGE;
    vp_sha256(m->image+0xbc1764u,0xc78u,sha);
    if (memcmp(sha,body_sha,32)) return VP_PATCH_BINDING;
    for (unsigned i=0;i<2u;i++)
        if (word(m->image+sites[i].site)!=sites[i].word) return VP_PATCH_BINDING;
    uint32_t off=m->island_offset+m->island_bytes;
    if ((off&3u) || off>m->capacity || 32u>m->capacity-off) return VP_PATCH_RANGE;
    for (unsigned i=0;i<2u;i++) {
        int64_t delta=(int64_t)off+i*8u-sites[i].site-8;
        if (delta<-(int64_t)VP_PATCH_BRANCH_REACH || delta>=VP_PATCH_BRANCH_REACH)
            return VP_PATCH_REACH;
    }
    VPPatchTarget target;
    int rc=vp_patch_open(&target,m->image,m->nso.span,m->base);
    for (unsigned i=0;i<2u && !rc;i++) {
        uint32_t island=m->base+off+i*8u;
        uint32_t veneer[2]={0xe51ff004u,(uint32_t)(uintptr_t)sites[i].entry};
        memcpy(m->image+off+i*8u,veneer,sizeof(veneer));
        rc=vp_patch_site(&target,sites[i].site,sites[i].word,island);
        fprintf(log,"ENGINE_SUBMIT_PATCH site=0x%x original=%08x island=%08x status=%d\n",
            sites[i].site,sites[i].word,island,rc);
    }
    if (!rc) {
        uint32_t replay[4]={0xe92d4ff0u,0xea000000u|
            (((0xbc1768u-(off+20u+8u))>>2)&0xffffffu),
            0xe28dd04cu,0xea000000u|(((0xbc23d8u-(off+28u+8u))>>2)&0xffffffu)};
        memcpy(m->image+off+16u,replay,sizeof(replay));
        engine_submit_replay=m->base+off+16u;
        engine_submit_leave_replay=m->base+off+24u;
        m->island_bytes+=32u;
    }
    return rc;
}

static int engine_layout_patch(FILE *log,VPNativeModule modules[4]) {
    FILE *marker=fopen("ux0:data/mhgu-vita-port/engine-layout.enable","rb");
    int enabled=marker!=NULL;
    if (marker) fclose(marker);
    fprintf(log,"ENGINE_LAYOUT_MODE enabled=%d\n",enabled);
    if (!enabled) return 0;
    if (engine_submit_probe_enabled) return VP_PATCH_BINDING;
    VPNativeModule *m=&modules[1],*sdk=&modules[3];
    static const unsigned char main_sha[32]={
        0xe7,0x1e,0x66,0xbb,0xa8,0x8c,0x89,0xc1,0x8a,0xd8,0xd2,0xe4,0x65,0x1d,0xd3,0x18,
        0x8c,0xcb,0x3a,0xed,0xf5,0x34,0x84,0xaf,0x9b,0x30,0xd2,0x10,0xc8,0xb4,0x65,0x7e};
    static const struct {unsigned offset,bytes;unsigned char sha[32];} checks[]={
        {0x33f8b0u,0x1b4u,{0x4c,0x22,0x37,0xd6,0xaa,0x39,0xc8,0x9a,
            0x07,0x5d,0xe1,0xc8,0x0d,0x91,0x9c,0xdb,0xae,0xbb,0xe8,0x14,0xf0,0x4a,0x9f,0x4d,
            0x96,0x9e,0xdb,0xb6,0x61,0xc2,0xc6,0x72}},
        {0x33fa64u,0x154u,{0x81,0x70,0x47,0x6f,0xe0,0x5b,0x44,0x83,
            0xc7,0x95,0x30,0xbe,0x82,0x7b,0xa1,0x59,0xf6,0x00,0x65,0xe2,0x72,0x31,0xde,0x98,
            0xce,0xdf,0x4d,0xba,0xc9,0xe4,0xcb,0x53}}};
    unsigned char sha[32];
    if (m->nso.segments[0].bytes<0xbc23e4u || sdk->nso.segments[0].bytes<0x33fbb8u)
        return VP_PATCH_RANGE;
    if (word(m->image+0xbc23e0u)!=0x00c7ffbcu) return VP_PATCH_BINDING;
    vp_sha256(m->image+0xbc1764u,0xc78u,sha);
    if (memcmp(sha,main_sha,32)) return VP_PATCH_BINDING;
    for (unsigned i=0;i<2u;i++) {
        vp_sha256(sdk->image+checks[i].offset,checks[i].bytes,sha);
        if (memcmp(sha,checks[i].sha,32)) return VP_PATCH_BINDING;
    }
    uint32_t off=m->island_offset+m->island_bytes;
    if ((off&3u) || off>m->capacity || 20u>m->capacity-off) return VP_PATCH_RANGE;
    int64_t delta=(int64_t)off-0xbc17d8u-8;
    if (delta<-(int64_t)VP_PATCH_BRANCH_REACH || delta>=VP_PATCH_BRANCH_REACH) return VP_PATCH_REACH;
    if (vp_engine_layout_init(sdk->base+0x33f8b0u,sdk->base+0x33fa64u)) return VP_PATCH_ARGUMENT;
    vp_engine_layout_resume=m->base+0xbc1814u;
    vp_engine_layout_fallback=m->base+off+8u;
    uint32_t veneer[5]={0xe51ff004u,(uint32_t)(uintptr_t)vp_engine_layout_entry,
        0xe59f0000u,0xea000000u|(((0xbc17dcu-(off+20u))>>2)&0xffffffu),
        word(m->image+0xbc23e0u)};
    memcpy(m->image+off,veneer,sizeof(veneer));
    VPPatchTarget target;
    int rc=vp_patch_open(&target,m->image,m->nso.span,m->base);
    if (!rc) rc=vp_patch_site(&target,0xbc17d8u,0xe59f0c00u,m->base+off);
    if (!rc) m->island_bytes+=20u;
    fprintf(log,"ENGINE_LAYOUT_PATCH site=0xbc17d8 original=e59f0c00 continuation=%08x status=%d\n",
        vp_engine_layout_resume,rc);
    return rc;
}

/* The A/B control file is host state, never guest memory. Exactly one '0',
 * '1', '2', '3', '4', '5' or '6' is required; trailing whitespace is accepted
 * and any other byte is invalid. Missing and malformed files are distinct so
 * the caller can report the actual failure instead of falling back to a path. */
static int engine_ab_read_control(uint32_t *mode) {
    FILE *f=fopen(VP_ENGINE_AB_CONTROL_PATH,"rb");
    if (!f) return 1;
    int first=fgetc(f),rc=0;
    if (!vp_engine_ab_control_digit_valid(first)) rc=2;
    if (!rc) {
        int c;
        while ((c=fgetc(f))!=EOF) {
            if (c!=' ' && c!='\t' && c!='\n' && c!='\v' && c!='\f' && c!='\r') {
                rc=2;break;
            }
        }
    }
    if (ferror(f)) rc=2;
    fclose(f);
    if (!rc) *mode=(uint32_t)(first-'0');
    return rc;
}
static int engine_texture_pipeline_marker(void) {
    FILE *marker=fopen("ux0:data/mhgu-vita-port/texture-pipeline.disable","rb");
    int enabled=marker==NULL;
    if (marker) fclose(marker);
    return enabled;
}

static int engine_full_patch(FILE *log,VPNativeModule *m) {
    FILE *marker=fopen("ux0:data/mhgu-vita-port/engine-full.enable","rb");
    int full=marker!=NULL;
    if (marker) fclose(marker);
    FILE *ab_marker=fopen("ux0:data/mhgu-vita-port/engine-full-ab.enable","rb");
    int ab=ab_marker!=NULL;
    if (ab_marker) fclose(ab_marker);
    FILE *fast_marker=fopen("ux0:data/mhgu-vita-port/engine-full-fast.enable","rb");
    int fast=fast_marker!=NULL;
    if (fast_marker) fclose(fast_marker);
    int enabled=full||ab||fast;
    vp_engine_full_fast_enabled=fast?1u:0u;
    /* No downstream shared preparation unless mode 3 is selected below. The
     * texture pipeline is a separate switch: A/B mode owns it when present;
     * otherwise texture-pipeline.disable opts out of the default path. */
    __atomic_store_n(&vp_engine_shared_prepare_enabled,0u,__ATOMIC_RELEASE);
    __atomic_store_n(&vp_engine_texture_idle_resize_enabled,0u,__ATOMIC_RELEASE);
    __atomic_store_n(&vp_engine_clear_defer_enabled,0u,__ATOMIC_RELEASE);
    if (!ab) {
        int texture_pipeline=engine_texture_pipeline_marker();
        __atomic_store_n(&vp_engine_texture_pipeline_enabled,
            texture_pipeline?1u:0u,__ATOMIC_RELEASE);
    }
    fprintf(log,"ENGINE_FULL_MODE enabled=%d ab=%d\n",enabled,ab);
    if (!enabled) {
        engine_full_ab_enabled=0;
        vp_engine_full_profile_enabled=0;
        return 0;
    }
#ifdef VP_ENGINE_CAPTURE
    /* The capture build stops by design on a fixed call count. Running it in
     * the A/B profile would inject that stop into the measured interval, so
     * the combination is refused instead of producing a contaminated sample. */
    if (ab || fast) {
        fprintf(log,"ENGINE_FULL_AB_REJECT reason=engine-capture-build ab=%d fast=%d\n",ab,fast);
        fflush(log);
        return VP_PATCH_BINDING;
    }
#endif
    if (engine_submit_probe_enabled || vp_engine_layout_resume) {
        fprintf(log,"ENGINE_FULL_AB_REJECT reason=exclusive-marker layout=%d probe=%d\n",
            vp_engine_layout_resume!=0,engine_submit_probe_enabled!=0);
        fflush(log);
        return VP_PATCH_BINDING;
    }
    static const struct {uint32_t offset,bytes;const char *sha;} checks[]={
        {0xbbe9c8u,7004u,"32a31fb8ec825cfab9c184d0864fab8411c12b41c93767fd26e9a8f5399b3b67"},
        {0xbc0e98u,1328u,"57a40e584db356275bd74acc66213efda539a6f1891225dfcd0046083a158478"},
        {0xbc13e0u,856u,"05ff02d5f4b4628c3d72728eb4a2047a15aaf002e5d1672a7098f33140ed6df9"},
        {0xbc1764u,3192u,"e71e66bba88c89c18ad8d2e4651dd3188ccb3aedf53484af9b30d210c8b4657e"}};
    static const char digits[]="0123456789abcdef";
    for (unsigned i=0;i<4u;i++) {
        const uint32_t offset=checks[i].offset,bytes=checks[i].bytes;
        if (offset>m->nso.segments[0].bytes || bytes>m->nso.segments[0].bytes-offset)
            return VP_PATCH_RANGE;
        unsigned char sha[32];
        vp_sha256(m->image+offset,bytes,sha);
        for (unsigned j=0;j<32u;j++)
            if (digits[sha[j]>>4]!=checks[i].sha[j*2u] ||
                digits[sha[j]&15u]!=checks[i].sha[j*2u+1u]) return VP_PATCH_BINDING;
    }
    if (ab) {
        uint32_t initial=0;
        int control=engine_ab_read_control(&initial);
        if (control) {
            fprintf(log,"ENGINE_PREPARE_CONTROL_ERROR path=%s reason=%s status=%d\n",
                VP_ENGINE_AB_CONTROL_PATH,control==1?"missing":"invalid",control);
            fflush(log);
            return VP_PATCH_ARGUMENT;
        }
        fprintf(log,"ENGINE_FULL_AB_MODE control=%s initial=%s\n",
            VP_ENGINE_AB_CONTROL_PATH,vp_engine_ab_mode_name(initial));
        __atomic_store_n(&vp_engine_shared_prepare_enabled,
            initial==3u?1u:0u,__ATOMIC_RELEASE);
        __atomic_store_n(&vp_engine_texture_pipeline_enabled,
            vp_engine_ab_mode_uses_deep_texture(initial)?1u:0u,__ATOMIC_RELEASE);
        __atomic_store_n(&vp_engine_texture_idle_resize_enabled,
            vp_engine_ab_mode_uses_idle_resize(initial)?1u:0u,__ATOMIC_RELEASE);
        __atomic_store_n(&vp_engine_clear_defer_enabled,
            vp_engine_ab_mode_uses_clear_defer(initial)?1u:0u,__ATOMIC_RELEASE);
    }
    if (m->island_offset>m->capacity || m->island_bytes>m->capacity-m->island_offset)
        return VP_PATCH_RANGE;
    uint32_t off=m->island_offset+m->island_bytes;
    uint32_t island_bytes=ab?20u:8u;
    if ((off&3u) || off>m->capacity || island_bytes>m->capacity-off) return VP_PATCH_RANGE;
    if (ab && ((off+8u)&3u)) return VP_PATCH_RANGE;
    int64_t delta=(int64_t)off-0xbbe9c8u-8;
    if (delta<-(int64_t)VP_PATCH_BRANCH_REACH || delta>=VP_PATCH_BRANCH_REACH) return VP_PATCH_REACH;
    if (vp_engine_full_init(m->base)) return VP_PATCH_ARGUMENT;
    engine_full_ab_enabled=ab;
    vp_engine_full_profile_enabled=ab?1u:0u;
    uint32_t original[4]={0,0,0,0};
    memcpy(original,m->image+0xbbe9c8u,sizeof(original));
    if (ab)
        fprintf(log,"ENGINE_FULL_AB_ORIGINAL site=0xbbe9c8 bytes=%08x,%08x,%08x,%08x\n",
            original[0],original[1],original[2],original[3]);
    uint32_t veneer[2]={0xe51ff004u,(uint32_t)(uintptr_t)vp_engine_full_entry};
    memcpy(m->image+off,veneer,sizeof(veneer));
    if (ab) {
        /* Replay the displaced PUSH, then jump absolutely into the untouched
         * original body at main+0xBBE9CC. PC-relative instructions inside the
         * body stay at their linked addresses; no function bytes are copied. */
        uint32_t trampoline[3]={0xe92d4ff0u,0xe51ff004u,m->base+0xbbe9ccu};
        memcpy(m->image+off+8u,trampoline,sizeof(trampoline));
        engine_full_ab_trampoline=m->base+off+8u;
        fprintf(log,"ENGINE_FULL_AB_TRAMPOLINE address=0x%08x range=[0x%x,0x%x) "
            "bytes=%08x,%08x,%08x target=0x%08x\n",
            engine_full_ab_trampoline,m->base+off+8u,m->base+off+20u,
            trampoline[0],trampoline[1],trampoline[2],m->base+0xbbe9ccu);
    }
    VPPatchTarget target;
    int rc=vp_patch_open(&target,m->image,m->nso.span,m->base);
    if (!rc) rc=vp_patch_site(&target,0xbbe9c8u,0xe92d4ff0u,m->base+off);
    if (!rc) m->island_bytes+=island_bytes;
    fprintf(log,"ENGINE_FULL_PATCH site=0xbbe9c8 original=e92d4ff0 veneer=0x%08x "
        "trampoline=0x%08x island=[0x%x,0x%x) island_bytes=%u capacity=%u "
        "branch_delta=%lld ab=%d status=%d\n",
        m->base+off,ab?engine_full_ab_trampoline:0u,m->base+off,m->base+off+island_bytes,
        island_bytes,m->capacity,(long long)delta,ab,rc);
    return rc;
}

static int game_cost_patch(FILE *log, VPNativeModule *m) {
    static const struct {
        uint32_t offset, bytes, first;
        const char *sha;
        void (*entry)(void);
    } sites[VP_GAME_COST_SITES] = {
        {0x3da8fcu, 0x688u, 0xe92d4ff0u,
         "8b5f4df7baf5038cbce56497b87b71c8de40779fdea3c64179ee573ec8fdd749", vp_game_cost_frame},
        {0xbbe154u, 0x504u, 0xe92d4ff0u,
         "36d52f82c0048a26998537d64597b65c91d9a01dbfad47afb391c1c0ea1f01fd", vp_game_cost_submit},
        {0xbbe66cu, 0x184u, 0xe92d4ff0u,
         "73113068eaf1b2c985cb7a4d8f8c97ad1e5cda4c1c8b482ba7fbbe41b2e5ca86", vp_game_cost_queue},
        {0xbc0874u, 0x36cu, 0xe92d41f0u,
         "a94873db81179626ef67cc814069fb4831934d590558c9f952952e01a213cac6", vp_game_cost_end_frame}
    };
    FILE *marker = fopen("ux0:data/mhgu-vita-port/game-cost.enable", "rb");
    game_cost_enabled = marker != NULL;
    if (marker) fclose(marker);
    fprintf(log, "GAME_COST_MODE enabled=%d sites=%u\n", game_cost_enabled, VP_GAME_COST_SITES);
    if (!game_cost_enabled) return 0;
    /* Validate every immutable range before writing any site. The module
     * identity has already passed the loader's locked input checks. */
    const char *digits = "0123456789abcdef";
    for (unsigned i = 0; i < VP_GAME_COST_SITES; ++i) {
        unsigned char sha[32];
        if (sites[i].offset > m->nso.span ||
            sites[i].bytes > m->nso.span - sites[i].offset) return VP_PATCH_RANGE;
        if (word(m->image + sites[i].offset) != sites[i].first) return VP_PATCH_BINDING;
        vp_sha256(m->image + sites[i].offset, sites[i].bytes, sha);
        for (unsigned j = 0; j < 32u; ++j)
            if (digits[sha[j] >> 4] != sites[i].sha[j * 2u] ||
                digits[sha[j] & 15u] != sites[i].sha[j * 2u + 1u]) return VP_PATCH_BINDING;
    }
    if (m->island_offset > m->capacity ||
        m->island_bytes > m->capacity - m->island_offset) return VP_PATCH_RANGE;
    uint32_t off = m->island_offset + m->island_bytes;
    if ((off & 3u) || 20u * VP_GAME_COST_SITES > m->capacity - off) return VP_PATCH_RANGE;
    for (unsigned i = 0; i < VP_GAME_COST_SITES; ++i) {
        int64_t delta = (int64_t)(off + 20u * i) - sites[i].offset - 8;
        if (delta < -(int64_t)VP_PATCH_BRANCH_REACH || delta >= VP_PATCH_BRANCH_REACH)
            return VP_PATCH_REACH;
    }
    VPPatchTarget target;
    int rc = vp_patch_open(&target, m->image, m->nso.span, m->base);
    if (rc) return rc;
    for (unsigned i = 0; i < VP_GAME_COST_SITES; ++i) {
        uint32_t at = off + 20u * i;
        uint32_t code[5] = {0xe51ff004u, (uint32_t)(uintptr_t)sites[i].entry,
            sites[i].first, 0xe51ff004u, m->base + sites[i].offset + 4u};
        memcpy(m->image + at, code, sizeof(code));
        game_cost_trampolines[i] = m->base + at + 8u;
        rc = vp_patch_site(&target, sites[i].offset, sites[i].first, m->base + at);
        fprintf(log, "GAME_COST_PATCH id=%u site=0x%x bytes=4 original=%08x "
            "island=[0x%x,0x%x) trampoline=0x%x return=0x%x branch_delta=%lld status=%d\n",
            i, sites[i].offset, sites[i].first, m->base + at, m->base + at + 20u,
            game_cost_trampolines[i], code[4], (long long)at - sites[i].offset - 8, rc);
        if (rc) return rc;
    }
    m->island_bytes += 20u * VP_GAME_COST_SITES;
    return 0;
}

int vp_sdk_dso_patch(FILE *log,VPNativeModule modules[4]) {
    int engine_rc=engine_submit_probe_patch(log,&modules[1]);
    if (engine_rc) return engine_rc;
    engine_rc=engine_layout_patch(log,modules);
    if (engine_rc) return engine_rc;
    engine_rc=engine_full_patch(log,&modules[1]);
    if (engine_rc) return engine_rc;
    engine_rc=game_cost_patch(log,&modules[1]);
    if (engine_rc) return engine_rc;
    int color_rc=movie_color_probe_patch(log,&modules[2]);
    if (color_rc) {
        fprintf(log,"MOVIE_COLOR_PROBE_REJECT status=%d\n",color_rc);fflush(log);
        return color_rc;
    }
    VPNativeModule *m=&modules[3];
    void (*entries[])(void)={vp_sdk_dso_enter,vp_sdk_ctor_gate,vp_sdk_lock,vp_sdk_unlock,vp_sdk_exception_info,
        vp_sdk_tls_normal,vp_sdk_tls_private,vp_sdk_tls_free_bridge,vp_sdk_tls_get_bridge,vp_sdk_tls_set_bridge,
        vp_sdk_lock,vp_sdk_unlock,vp_sdk_try_lock,vp_sdk_is_locked,
        vp_sdk_video_readback};
    const unsigned count=sizeof(entries)/sizeof(entries[0]);
    _Static_assert(sizeof(entries)/sizeof(entries[0])==sizeof(vp_sdk_dso_patches)/sizeof(vp_sdk_dso_patches[0]),"patch contract count");
    for (unsigned i=0;i<count;++i) {
        const VPSDKDSOPatch *p=&vp_sdk_dso_patches[i];unsigned char digest[32];
        if (p->offset>m->nso.span || p->bytes>m->nso.span-p->offset) return VP_PATCH_RANGE;
        vp_sha256(m->image+p->offset,p->bytes,digest);
        if (word(m->image+p->offset)!=p->word || memcmp(digest,p->sha,32)) return VP_PATCH_BINDING;
    }
    if (word(m->image+0x862754) || word(m->image+0x852678)!=m->base+0x439854) return VP_PATCH_BINDING;
    uint32_t off=m->island_offset+m->island_bytes;
    if (off>m->capacity || m->capacity-off<count*8+8) return VP_PATCH_RANGE;
    VPPatchTarget sites;int rc=vp_patch_open(&sites,m->image,m->nso.span,m->base);
    for (unsigned i=0;i<count && !rc;++i) {
        const VPSDKDSOPatch *p=&vp_sdk_dso_patches[i];uint32_t address=m->base+off;
        uint32_t code[4]={0xe51ff004u,(uint32_t)(uintptr_t)entries[i],p->word,0};
        if (!i) {
            /* Observation resumes this displaced original PUSH, then the
             * untouched DSO body. The original caller LR is preserved. */
            resume=address+8;
            int64_t delta=(int64_t)m->base+p->offset+4-((int64_t)address+12+8);
            if ((delta&3) || delta<-(int64_t)VP_PATCH_BRANCH_REACH || delta>=VP_PATCH_BRANCH_REACH) return VP_PATCH_REACH;
            code[3]=0xea000000u|(((uint32_t)(delta/4))&0xffffffu);
        }
        memcpy(m->image+off,code,i?8:16);
        rc=vp_patch_site(&sites,p->offset,p->word,address);
        fprintf(log,"SDK_PATCH index=%u offset=0x%x island=0x%08x status=%d\n",i,p->offset,address,rc);
        off+=i?8:16;
    }
    if (!rc) m->island_bytes=off-m->island_offset;
    /* Explicit native provider bindings after the original static link. No
     * relocation arithmetic is repeated; every slot checks its linked value. */
    static const uint32_t slots[]={0x852934,0x852938,0x85cc44};
    static const char *names[]={"malloc","free","free"};
    for (unsigned i=0;i<3 && !rc;++i) {
        uint32_t original=0;rc=vp_native_export(&modules[1],names[i],&original);
        if (!rc) rc=vp_patch_slot(&sites,slots[i],original,(uint32_t)(uintptr_t)(i?vp_sdk_heap_free:vp_sdk_malloc));
        fprintf(log,"SDK_PROVIDER slot=0x%x name=%s before=0x%08x after=0x%08x status=%d\n",slots[i],names[i],original,(unsigned)(uintptr_t)(i?vp_sdk_heap_free:vp_sdk_malloc),rc);
    }
    /* Kernel interface: replace each reviewed ARM SVC word with a branch to a
     * per-site bridge island. The original veneer body resumes at site+4 and
     * performs its own out-parameter stores; only the kernel side is native. */
    const uint32_t tp_service_offset=(uint32_t)(vp_sdk_svc_site_service_value-vp_sdk_svc_site_template);
    const uint32_t tp_continuation_offset=(uint32_t)(vp_sdk_svc_site_continuation_value-vp_sdk_svc_site_template);
    const uint32_t tp_branch_offset=(uint32_t)(vp_sdk_svc_site_branch-vp_sdk_svc_site_template);
    const uint32_t template_bytes=(uint32_t)(vp_sdk_svc_site_end-vp_sdk_svc_site_template);
    const uint32_t svc_count=sizeof(vp_sdk_svc_sites)/sizeof(vp_sdk_svc_sites[0]);
    const uint32_t stride=80;
    if (!rc && (template_bytes>stride || tp_service_offset>=template_bytes || tp_continuation_offset+4>template_bytes
        || tp_branch_offset>=template_bytes || (tp_branch_offset&3))) rc=VP_PATCH_TEMPLATE;
    if (!rc && (off>m->capacity || svc_count*stride>m->capacity-off)) rc=VP_PATCH_RANGE;
    for (unsigned i=0;i<svc_count && !rc;++i) {
        const VPSDKSvcSite *site=&vp_sdk_svc_sites[i];uint32_t original=0;
        if (site->offset>m->nso.segments[0].bytes-4) { rc=VP_PATCH_RANGE;break; }
        memcpy(&original,m->image+site->offset,4);
        if (original!=site->word) { rc=VP_PATCH_BINDING;break; }
        uint32_t place=off+i*stride,island=m->base+place;
        memcpy(m->image+place,vp_sdk_svc_site_template,template_bytes);
        memset(m->image+place+template_bytes,0,stride-template_bytes);
        uint32_t service=VP_GUEST_SVC_BASE+site->svc,continuation=m->base+site->offset+4;
        memcpy(m->image+place+tp_service_offset,&service,4);
        memcpy(m->image+place+tp_continuation_offset,&continuation,4);
        /* The template is copied away from its link-time address, so its
         * relative branch to the host bridge must be re-encoded for the copy. */
        int64_t delta=(int64_t)(uint32_t)(uintptr_t)vp_bridge_saved_entry
            -((int64_t)island+(int64_t)tp_branch_offset+8);
        if ((delta&3) || delta<-(int64_t)VP_PATCH_BRANCH_REACH || delta>(int64_t)VP_PATCH_BRANCH_REACH-4) { rc=VP_PATCH_REACH;break; }
        uint32_t branch=0xea000000u|(((uint32_t)(delta>>2))&0xffffffu);
        memcpy(m->image+place+tp_branch_offset,&branch,4);
        rc=vp_patch_site(&sites,site->offset,site->word,island);
        fprintf(log,"SDK_SVC site=0x%x svc=0x%x island=0x%08x status=%d\n",site->offset,site->svc,island,rc);
    }
    if (!rc) { off+=svc_count*stride;m->island_bytes=off-m->island_offset; }
    /* Generic-timer reads: replace each reviewed MRRC p15,c14 with an island
     * whose service returns a host monotonic 64-bit tick in the saved Rt:Rt2.
     * The original instruction is 4 bytes and has no memory side effect. */
    const uint32_t tp_timer_service_offset=(uint32_t)(vp_sdk_timer_site_service_value-vp_sdk_timer_site_template);
    const uint32_t tp_timer_rt_offset=(uint32_t)(vp_sdk_timer_site_rt_value-vp_sdk_timer_site_template);
    const uint32_t tp_timer_continuation_offset=(uint32_t)(vp_sdk_timer_site_continuation_value-vp_sdk_timer_site_template);
    const uint32_t tp_timer_branch_offset=(uint32_t)(vp_sdk_timer_site_branch-vp_sdk_timer_site_template);
    const uint32_t timer_template_bytes=(uint32_t)(vp_sdk_timer_site_end-vp_sdk_timer_site_template);
    const uint32_t timer_count=sizeof(vp_sdk_timer_sites)/sizeof(vp_sdk_timer_sites[0]);
    if (!rc && (timer_template_bytes>stride || tp_timer_service_offset>=timer_template_bytes
        || tp_timer_rt_offset+4>timer_template_bytes || tp_timer_continuation_offset+4>timer_template_bytes
        || tp_timer_branch_offset>=timer_template_bytes || (tp_timer_branch_offset&3))) rc=VP_PATCH_TEMPLATE;
    if (!rc && (off>m->capacity || timer_count*stride>m->capacity-off)) rc=VP_PATCH_RANGE;
    for (unsigned i=0;i<timer_count && !rc;++i) {
        const VPSDKTimerSite *site=&vp_sdk_timer_sites[i];uint32_t original=0;
        if (site->offset>m->nso.segments[0].bytes-4) { rc=VP_PATCH_RANGE;break; }
        memcpy(&original,m->image+site->offset,4);
        if (original!=site->word) { rc=VP_PATCH_BINDING;break; }
        uint32_t place=off+i*stride,island=m->base+place;
        memcpy(m->image+place,vp_sdk_timer_site_template,timer_template_bytes);
        memset(m->image+place+timer_template_bytes,0,stride-timer_template_bytes);
        uint32_t service=VP_GUEST_TIMER_SERVICE,rt=site->rt,continuation=m->base+site->offset+4;
        memcpy(m->image+place+tp_timer_service_offset,&service,4);
        memcpy(m->image+place+tp_timer_rt_offset,&rt,4);
        memcpy(m->image+place+tp_timer_continuation_offset,&continuation,4);
        int64_t delta=(int64_t)(uint32_t)(uintptr_t)vp_bridge_saved_entry
            -((int64_t)island+(int64_t)tp_timer_branch_offset+8);
        if ((delta&3) || delta<-(int64_t)VP_PATCH_BRANCH_REACH || delta>(int64_t)VP_PATCH_BRANCH_REACH-4) { rc=VP_PATCH_REACH;break; }
        uint32_t branch=0xea000000u|(((uint32_t)(delta>>2))&0xffffffu);
        memcpy(m->image+place+tp_timer_branch_offset,&branch,4);
        rc=vp_patch_site(&sites,site->offset,site->word,island);
        fprintf(log,"SDK_TIMER site=0x%x word=0x%08x rt=%u island=0x%08x status=%d\n",
            site->offset,site->word,site->rt,island,rc);
    }
    if (!rc) { off+=timer_count*stride;m->island_bytes=off-m->island_offset; }
    /* Legacy ARMv6 write-buffer drains around ldrex/strex: rewrite each bound
     * site in place to architectural DSB sy.  Dynarmic decodes that encoding
     * natively; the legacy one falls into Vita3K's unhandled CP15 path.  The
     * replacement is a full-system barrier with no register or memory effect. */
    const uint32_t barrier_count=sizeof(vp_sdk_barrier_sites)/sizeof(vp_sdk_barrier_sites[0]);
    for (unsigned i=0;i<barrier_count && !rc;++i) {
        const VPSDKBarrierSite *site=&vp_sdk_barrier_sites[i];
        rc=vp_patch_word(&sites,site->offset,site->word,VP_SDK_BARRIER_WORD);
        fprintf(log,"SDK_BARRIER site=0x%x word=0x%08x replacement=0x%08x status=%d\n",
            site->offset,site->word,VP_SDK_BARRIER_WORD,rc);
    }
    if (!rc) rc=sdk_hid_helper_state_probe(log,modules);
    if (!rc) {
        unsigned char digest[32];vp_sha256(m->image,m->nso.span,digest);
        fputs("SDK_PATCHED_IMAGE sha256=",log);for (unsigned i=0;i<32;++i) fprintf(log,"%02x",digest[i]);
        vp_sha256(m->image+m->island_offset,m->island_bytes,digest);
        fprintf(log," island_bytes=%u island_sha256=",m->island_bytes);for (unsigned i=0;i<32;++i) fprintf(log,"%02x",digest[i]);fputc('\n',log);
    }
    fflush(log);return rc;
}
VP_SDK_INTEGER_ONLY
static int writable(const VPExecutor *e,const Session *s,uint32_t p,uint32_t bytes) {
    if (p>=e->stack_base+4096 && p<=e->stack_base+e->stack_bytes-bytes) return 1;
    for (unsigned i=0;i<4;++i) {
        const VPNativeModule *m=&e->modules[i];uint32_t start=m->base+m->nso.segments[2].offset;
        if (m->nso.segments[2].offset && p>=start && p<=m->base+m->nso.span-bytes) return 1;
    }
    for (unsigned i=0;i<VP_THREAD_LOCAL_ALLOCATION_LIMIT;++i) {
        uint32_t begin=(uint32_t)(uintptr_t)s->allocator.allocations[i].pointer,size=s->allocator.allocations[i].size;
        if (begin && p>=begin && p-begin<=size && bytes<=size-(p-begin)) return 1;
    }
    /* Guest heap/alias/stack regions are backed host memory too. The SDK now
     * allocates its own lock and mutex objects from the heap it created via
     * svcSetMemoryHeapSize, so bridge-side memory checks must accept them. */
    return vp_guest_svc_writable(&s->svc, p, bytes);
}
VP_SDK_INTEGER_ONLY
static int session_enter(Session *s) {
    if (s->session_kernel) return sceKernelLockMutex(s->state_lock,1,NULL);
    return vp_session_gate_enter(&s->state_gate,host_tls());
}
VP_SDK_INTEGER_ONLY
static int session_leave(Session *s) {
    if (s->session_kernel) return sceKernelUnlockMutex(s->state_lock,1);
    return vp_session_gate_leave(&s->state_gate,host_tls());
}
static _Noreturn void session_failure(Session *s,const char *operation,int rc) {
    fprintf(s->log,"SDK_SESSION_LOCK_FATAL operation=%s backend=%s status=%d action=process-exit\n",
        operation,s->session_kernel?"kernel-mutex":"user-gate",rc);
    fflush(s->log);
    sceKernelExitProcess(rc);
    __builtin_trap(); /* A failed exit must never resume without the lock. */
}
static void state_lock(Session *s) {
    int rc=session_enter(s);
    if (rc) session_failure(s,"enter",rc);
}
static void state_unlock(Session *s) {
    int rc=session_leave(s);
    if (rc) session_failure(s,"leave",rc);
}
/* A service that parks must drop the session lock so the signaller can run.
 * While it is parked another dispatch may rewrite the ambient thread identity
 * and IPC buffer, so the parked thread restores its own values before it
 * resumes inside the service. Without this, the code after a wait (logging,
 * thread checks, savedata/storage ownership) could see another guest thread's
 * handle. device_wait_begin/end already carry the same token for the device
 * path; these helpers give the other park sites that discipline. */
typedef struct { uint32_t thread, tls; } VPPark;
static void park_begin(Session *s, VPPark *park) {
    park->thread = s->svc.current_thread_handle;
    park->tls = s->svc.tls_buffer;
    state_unlock(s);
}
static void park_end(Session *s, const VPPark *park) {
    state_lock(s);
    s->svc.current_thread_handle = park->thread;
    s->svc.tls_buffer = park->tls;
}
static void device_wait_begin(void *user,uint32_t token[2]) {
    Session *s=user;
    token[0]=s->svc.current_thread_handle;
    token[1]=s->svc.tls_buffer;
    state_unlock(s);
}
static void device_wait_end(void *user,const uint32_t token[2]) {
    Session *s=user;
    state_lock(s);
    s->svc.current_thread_handle=token[0];
    s->svc.tls_buffer=token[1];
}
VP_SDK_INTEGER_ONLY
static unsigned sdk_lock_index(Session *s,uint32_t address) {
    unsigned hash=(address>>2)&63u;
    unsigned hint=s->lock_hint[hash];
    if (hint && hint<=VP_SDK_LOCK_LIMIT && s->locks[hint-1u].address==address)
        return hint-1u;
    unsigned i=0;
    while (i<VP_SDK_LOCK_LIMIT && s->locks[i].address && s->locks[i].address!=address) ++i;
    if (i<VP_SDK_LOCK_LIMIT && s->locks[i].address==address)
        s->lock_hint[hash]=i+1u;
    return i;
}

/* The caller pins the slot with pending before any park. A nonblocking probe
 * under the session lock is safe only when no other acquisition is in flight. */
static int sdk_lock_acquire(Session *s,Lock *lock,unsigned *waited) {
    int rc=(int)SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN;
    *waited=0;
    if (!lock->depth && lock->pending==1u) {
        rc=sceKernelTryLockMutex(lock->id,1);
        if (rc!=(int)SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN) return rc;
    }
    /* This clock enforces the contention budget, not a performance probe.
     * The uncontended return above never reads it. */
    uint64_t spin_started=sceKernelGetProcessTimeWide();
    for (unsigned attempt=0;attempt<256u;++attempt) {
        VPPark park;
        park_begin(s,&park);
        rc=sceKernelTryLockMutex(lock->id,1);
        park_end(s,&park);
        if (rc!=(int)SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN) return rc;
        if (sceKernelGetProcessTimeWide()-spin_started>=1000u) break;
    }
    while (*waited<VP_SDK_LOCK_BLOCK_LIMIT_US) {
        unsigned slice=VP_SDK_LOCK_SLICE_US;
        VPPark park;
        park_begin(s,&park);
        rc=sceKernelLockMutex(lock->id,1,&slice);
        park_end(s,&park);
        if (rc!=(int)SCE_KERNEL_ERROR_WAIT_TIMEOUT) return rc;
        /* The kernel may overwrite slice with remaining time; count the
         * requested timeout, otherwise repeated zero remainders never expire. */
        *waited+=VP_SDK_LOCK_SLICE_US;
    }
    return VP_SDK_LOCK_DEADLOCK;
}

static int create_guest_thread(Session *,VPBridgeFrame *);
static int start_guest_thread(Session *,VPBridgeFrame *);
static int exit_guest_thread(Session *,VPExecutor *,VPBridgeFrame *);
static int sleep_guest_thread(Session *,VPBridgeFrame *);
static int thread_service(Session *,VPExecutor *,VPBridgeFrame *);
static VPSdkTlsValues *tls_values_for(Session *,VPExecutor *);
static VPGuestThread *thread_for_executor(VPExecutor *);

/* This AAPCS boundary runs on the guest stack without a saved VFP frame.
 * Keep it integer-only. Acquire Session in the same order as dispatch; no
 * guest mutex is acquired before this wait. Existing slots still require
 * current writable memory; first use and guest-lock contention use dispatch. */
_Static_assert(offsetof(VPHostBridge,user)==28,"fast lock bridge user ABI");
VP_SDK_INTEGER_ONLY
int vp_sdk_lock_fast(uint32_t address,VPExecutor *e,uint32_t service) {
    if (!e || e->user!=&session || session.state_lock<0 || !session.fast_enabled) {
        __sync_fetch_and_add(&session.fast_miss[0],1u);
        return 0;
    }
    Session *s=&session;
    int rc=session_enter(s);
    if (rc) {
        __sync_fetch_and_add(&s->fast_miss[1],1u);
        return rc;
    }
    int result=0;
    if (s->stopping || (address&3u)) goto done;
    VPGuestThread *thread=thread_for_executor(e);
    uint32_t owner;
    if (thread && thread->handle) owner=thread->handle;
    else if (e==s->main_executor) owner=s->start_handle?s->start_handle:1u;
    else { ++s->fast_miss[2];goto done; }
    unsigned i=sdk_lock_index(s,address);
    if (i>=VP_SDK_LOCK_LIMIT || s->locks[i].address!=address) {
        ++s->fast_miss[3];goto done;
    }
    if (!writable(e,s,address,4u)) { ++s->fast_miss[4];goto done; }
    Lock *lock=&s->locks[i];
    if (service==0x246u) {
        result=lock->depth && lock->owner==owner?1:2;
        ++s->fast_islocked;
    } else if (service==0x232u || service==0x245u) {
        if (lock->depth && lock->owner==owner) {
            ++lock->depth;
            result=1;
        } else if (!lock->depth && !lock->pending) {
            rc=sceKernelTryLockMutex(lock->id,1);
            if (!rc) {
                lock->owner=owner;lock->depth=1;lock->host_depth=1;lock->held=1;
                /* A fast acquisition has no hold-time probe. Diagnostics
                 * cover only holds whose enter and leave both use dispatch. */
                lock->held_since_us=0;
                result=1;
            } else if (rc!=(int)SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN) {
                result=rc;
            }
        }
        if (result>0) {
            if (result==1) ++s->lock_calls;
            if (service==0x232u) ++s->fast_enter;
            else ++s->fast_try;
        }
    } else if (service==0x233u && lock->depth && lock->owner==owner) {
        if (lock->depth>1u) {
            --lock->depth;result=1;
        } else {
            rc=sceKernelUnlockMutex(lock->id,1);
            if (!rc) {
                lock->owner=0;lock->depth=0;lock->host_depth=0;lock->held=0;
                lock->held_since_us=0;result=1;
            } else result=rc;
        }
        if (result==1) { ++s->unlock_calls;++s->fast_leave; }
    }
done:
    if (!result) ++s->fast_miss[5];
    rc=session_leave(s);
    return rc?rc:result;
}

static int movie_color_probe_service(Session *s,VPExecutor *e,VPBridgeFrame *f) {
    if (!movie_color_probe_enabled) return VP_BRIDGE_UNSUPPORTED;
    VPGuestThread *thread=thread_for_executor(e);
    unsigned slot;
    if (e==s->main_executor) slot=0;
    else if (thread && thread->handle) slot=1u+(unsigned)(thread-thread_pool.threads);
    else return VP_BRIDGE_ARGUMENT;
    VPMovieColorTiming *timing=&s->movie_color[slot];
    unsigned mode=f->service<0x24eu?1u:2u;
    if (!(f->service&1u)) {
        uint32_t object=f->regs[0];
        if (timing->mode || object>UINT32_MAX-0x44u
            || !vp_guest_svc_readable(&s->svc,object+0x3cu,8u)) return VP_BRIDGE_ARGUMENT;
        uint32_t width=word((void *)(uintptr_t)(object+0x3cu));
        uint32_t height=word((void *)(uintptr_t)(object+0x40u));
        if (mode==1u) {
            f->lr=height; /* Displaced LDR lr,[r0,#0x40]. */
            f->pc=e->modules[2].base+0x33c4f8u;
        } else {
            if (f->sp>UINT32_MAX-12u || !writable(e,s,f->sp+12u,4u)) return VP_BRIDGE_ARGUMENT;
            memcpy((void *)(uintptr_t)(f->sp+12u),&f->regs[1],4u);
            f->pc=e->modules[2].base+0x33c6a4u;
        }
        timing->mode=mode;timing->width=width;timing->height=height;
        timing->destination=f->regs[1];timing->source_y=f->regs[2];timing->source_uv=f->regs[3];
        timing->started_us=sceKernelGetProcessTimeWide();
    } else {
        uint64_t ended_us=sceKernelGetProcessTimeWide();
        uint32_t output=mode==1u?f->regs[1]:f->regs[9];
        if (timing->mode!=mode || !writable(e,s,output,4u)) return VP_BRIDGE_ARGUMENT;
        memcpy((void *)(uintptr_t)output,&f->regs[0],4u);
        f->pc=e->modules[2].base+(mode==1u?0x33c67cu:0x33c86cu);
        unsigned n=++s->movie_color_samples;
        if (n<=2000u || n%32u==0u) {
            fprintf(s->log,"VP_MOVIE_COLOR_TIME n=%u mode=%s thread=0x%x width=%u height=%u "
                "destination=0x%08x source_y=0x%08x source_uv=0x%08x bytes=%u "
                "elapsed_us=%llu entry_us=%llu t_us=%llu\n",
                n,mode==1u?"rgb":"nv12",s->svc.current_thread_handle,timing->width,timing->height,
                timing->destination,timing->source_y,timing->source_uv,f->regs[0],
                (unsigned long long)(ended_us-timing->started_us),
                (unsigned long long)timing->started_us,(unsigned long long)ended_us);
        }
        timing->mode=0;timing->started_us=0;
    }
    return 0;
}

static void engine_submit_report(Session *s) {
    const VPEngineSubmitStats *p=&s->engine_stats;
    fprintf(s->log,"ENGINE_SUBMIT_STATS calls=%llu same_object=%llu same_layout=%llu "
        "elapsed_us=%llu max_us=%u encoded_bytes=%llu families=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)p->calls,(unsigned long long)p->same_object,
        (unsigned long long)p->same_layout,(unsigned long long)p->elapsed_us,p->max_us,
        (unsigned long long)p->encoded_bytes,(unsigned long long)p->families[0],
        (unsigned long long)p->families[1],(unsigned long long)p->families[2],
        (unsigned long long)p->families[3],(unsigned long long)p->families[4],
        (unsigned long long)p->families[5],(unsigned long long)p->families[6],
        (unsigned long long)p->families[7],(unsigned long long)p->families[8]);
}

static int engine_submit_probe_service(Session *s,VPExecutor *e,VPBridgeFrame *f) {
    if (!engine_submit_probe_enabled) return VP_BRIDGE_UNSUPPORTED;
    VPGuestThread *thread=thread_for_executor(e);
    unsigned slot;
    if (e==s->main_executor) slot=0u;
    else if (thread && thread->handle) slot=1u+(unsigned)(thread-thread_pool.threads);
    else return VP_BRIDGE_ARGUMENT;
    VPEngineSubmitTiming *t=&s->engine_submit[slot];
    if (f->service==0x250u) {
        uint32_t state=f->regs[1],object=f->regs[2],old,layout,old_layout=0u;
        if (t->active || state>UINT32_MAX-0x14u || object>UINT32_MAX-12u ||
            !vp_guest_svc_readable(&s->svc,state,0x14u) ||
            !vp_guest_svc_readable(&s->svc,object,12u) ||
            f->sp<36u || !writable(e,s,f->sp-36u,36u)) return VP_BRIDGE_ARGUMENT;
        old=word((void *)(uintptr_t)(state+0x10u));
        layout=word((void *)(uintptr_t)(object+8u));
        if (old) {
            if (old>UINT32_MAX-12u || !vp_guest_svc_readable(&s->svc,old,12u))
                return VP_BRIDGE_ARGUMENT;
            old_layout=word((void *)(uintptr_t)(old+8u));
        }
        uint32_t command=word((void *)(uintptr_t)(state+4u));
        if (command>UINT32_MAX-0x2cu || !vp_guest_svc_readable(&s->svc,command,0x2cu))
            return VP_BRIDGE_ARGUMENT;
        *t=(VPEngineSubmitTiming){.active=1u,.object=object,.state=state,
            .command=command,.before=word((void *)(uintptr_t)(command+12u)),
            .family=vp_engine_submit_family(f->lr-e->modules[1].base),
            .same_object=old==object,.same_layout=old && old_layout==layout};
        if (s->engine_stats.calls<16u)
            fprintf(s->log,"ENGINE_SUBMIT_SAMPLE thread=%u caller=%08x ctx=%08x state=%08x "
                "object=%08x old=%08x layout=%08x old_layout=%08x command=%08x family=%u\n",
                slot,f->lr-e->modules[1].base,f->regs[0],state,object,old,layout,old_layout,
                command,t->family);
        /* PUSH must run after restoring the bridge frame: its guest stack
         * destination overlaps the tail of the currently saved frame. */
        f->pc=engine_submit_replay;
        t->started=sceKernelGetProcessTimeWide();
    } else {
        uint64_t elapsed=sceKernelGetProcessTimeWide()-t->started;
        if (!t->active || f->sp>UINT32_MAX-0x4cu) return VP_BRIDGE_ARGUMENT;
        uint32_t after=word((void *)(uintptr_t)(t->command+12u));
        /* A command-buffer growth is not a contiguous byte range. */
        uint32_t bytes=after>=t->before && after-t->before<0x100000u ? after-t->before:0u;
        vp_engine_submit_account(&s->engine_stats,t->family,t->same_object,t->same_layout,
            elapsed>UINT32_MAX?UINT32_MAX:(uint32_t)elapsed,bytes);
        t->active=0u;
        f->pc=engine_submit_leave_replay;
        if (!(s->engine_stats.calls&4095u)) engine_submit_report(s);
    }
    return 0;
}

/* The vi:obj BufferQueue model releases a queued slot when the producer asks
 * for a new buffer. That counter is cumulative; the analyzer uses deltas
 * between recorded samples, never the absolute value as an interval count. */
static uint32_t engine_prepare_presents(const Session *s) {
    uint32_t presents=0;
    for (unsigned i=0;i<VP_GUEST_HANDLE_LIMIT;++i) {
        const VPGuestHandle *h=&s->svc.handles[i];
        if (h->kind!=2u || strcmp(h->name,"vi:obj")) continue;
        if (h->session<1u || h->session>VP_GUEST_SESSION_LIMIT) continue;
        uint32_t add=s->svc.sessions[h->session-1u].binder_presents;
        presents=add>UINT32_MAX-presents?UINT32_MAX:presents+add;
    }
    return presents;
}

/* Poll at most twice per second, always before the root entry timestamp. The
 * poll and its file I/O are therefore part of the frame but not of the reported
 * root interval, and both paths pay the same cadence. */
static int engine_prepare_ab_poll(Session *s,uint64_t now) {
    if (s->engine_ab_mode_valid && now>=s->engine_ab_poll_us
        && now-s->engine_ab_poll_us<VP_ENGINE_AB_POLL_US) return 0;
    s->engine_ab_poll_us=now;
    uint32_t mode=0;
    int rc=engine_ab_read_control(&mode);
    if (rc) {
        __atomic_store_n(&vp_engine_shared_prepare_enabled,0u,__ATOMIC_RELEASE);
        __atomic_store_n(&vp_engine_texture_pipeline_enabled,0u,__ATOMIC_RELEASE);
        __atomic_store_n(&vp_engine_texture_idle_resize_enabled,0u,__ATOMIC_RELEASE);
        __atomic_store_n(&vp_engine_clear_defer_enabled,0u,__ATOMIC_RELEASE);
        fprintf(s->log,"ENGINE_PREPARE_CONTROL_ERROR path=%s reason=%s status=%d thread=0x%x\n",
            VP_ENGINE_AB_CONTROL_PATH,rc==1?"missing":"invalid",rc,
            s->svc.current_thread_handle);
        fflush(s->log);
        return VP_BRIDGE_ARGUMENT;
    }
    /* Publish before the root timestamp so this call's downstream draws and
     * the elapsed_us interval see the same mode decision. */
    __atomic_store_n(&vp_engine_shared_prepare_enabled,
        mode==3u?1u:0u,__ATOMIC_RELEASE);
    __atomic_store_n(&vp_engine_texture_pipeline_enabled,
        vp_engine_ab_mode_uses_deep_texture(mode)?1u:0u,__ATOMIC_RELEASE);
    __atomic_store_n(&vp_engine_texture_idle_resize_enabled,
        vp_engine_ab_mode_uses_idle_resize(mode)?1u:0u,__ATOMIC_RELEASE);
    __atomic_store_n(&vp_engine_clear_defer_enabled,
        vp_engine_ab_mode_uses_clear_defer(mode)?1u:0u,__ATOMIC_RELEASE);
    if (!s->engine_ab_mode_valid || mode!=s->engine_ab_mode) {
        fprintf(s->log,"ENGINE_PREPARE_PHASE mode=%s t_us=%llu previous=%s thread=0x%x\n",
            vp_engine_ab_mode_name(mode),(unsigned long long)now,
            s->engine_ab_mode_valid?vp_engine_ab_mode_name(s->engine_ab_mode):"none",
            s->svc.current_thread_handle);
        fflush(s->log);
    }
    s->engine_ab_mode=mode;
    s->engine_ab_mode_valid=1;
    return 0;
}

static int engine_prepare_ab_service(Session *s,VPExecutor *e,VPBridgeFrame *f) {
    if (!engine_full_ab_enabled) return VP_BRIDGE_UNSUPPORTED;
    VPGuestThread *thread=thread_for_executor(e);
    unsigned slot;
    if (e==s->main_executor) slot=0u;
    else if (thread && thread->handle && thread>=thread_pool.threads
        && (unsigned)(thread-thread_pool.threads)<VP_GUEST_THREAD_LIMIT)
        slot=1u+(unsigned)(thread-thread_pool.threads);
    else return VP_BRIDGE_ARGUMENT;
    VPEnginePrepareTiming *t=&s->engine_prepare[slot];
    if (f->service==0x253u) {
        if (t->active) {
            fprintf(s->log,"ENGINE_PREPARE_NESTED slot=%u thread=0x%x context=0x%08x mode=%s\n",
                slot,s->svc.current_thread_handle,f->regs[0],
                vp_engine_ab_mode_name(t->mode));
            fflush(s->log);
            return VP_BRIDGE_ARGUMENT;
        }
        /* Poll first, then take the start timestamp. The file access is
         * deliberate frame work on both paths; it is not charged to root time. */
        int rc=engine_prepare_ab_poll(s,sceKernelGetProcessTimeWide());
        if (rc) return rc;
        uint64_t started=sceKernelGetProcessTimeWide();
        uint32_t context=f->regs[0];
        if (context>UINT32_MAX-0x98u
            || !vp_guest_svc_readable(&s->svc,context+0x94u,4u)) {
            fprintf(s->log,"ENGINE_PREPARE_ENTER_ERROR slot=%u thread=0x%x reason=context context=0x%08x\n",
                slot,s->svc.current_thread_handle,context);
            fflush(s->log);
            return VP_BRIDGE_ARGUMENT;
        }
        uint32_t command=word((void *)(uintptr_t)(context+0x94u));
        if (command>UINT32_MAX-0x2cu || !vp_guest_svc_readable(&s->svc,command,0x2cu)) {
            fprintf(s->log,"ENGINE_PREPARE_ENTER_ERROR slot=%u thread=0x%x reason=command command=0x%08x\n",
                slot,s->svc.current_thread_handle,command);
            fflush(s->log);
            return VP_BRIDGE_ARGUMENT;
        }
        uint32_t mode=s->engine_ab_mode;
        /* Modes 0, 3, 4, 5 and 6 execute the original root trampoline. Mode 3
         * enables shared preparation; 4/5/6 enable the deep texture pipeline;
         * 5 and 6 enable idle page resizing before a GPU drain, and only 6
         * defers the full-colour clear boundary. */
        uint32_t target=mode==2u?(uint32_t)(uintptr_t)vp_engine_full_run_fast:
            mode==1u?(uint32_t)(uintptr_t)vp_engine_full_run:engine_full_ab_trampoline;
        if (!s->engine_ab_mode_valid || mode>6u || !target) {
            fprintf(s->log,"ENGINE_PREPARE_ENTER_ERROR slot=%u thread=0x%x reason=target mode=%u target=0x%08x\n",
                slot,s->svc.current_thread_handle,mode,target);
            fflush(s->log);
            return VP_BRIDGE_ARGUMENT;
        }
        t->entry_us=started;
        t->context=context;
        t->command=command;
        t->before=word((void *)(uintptr_t)(command+12u));
        t->block_before=word((void *)(uintptr_t)(command+0x28u));
        t->mode=mode;
        t->active=1u;
        /* r0 stays context; the bridge restores it and this write supplies ip. */
        f->regs[12]=target;
        return 0;
    }
    if (f->service!=0x254u) return VP_BRIDGE_UNSUPPORTED;
    uint64_t ended=sceKernelGetProcessTimeWide();
    if (!t->active) {
        fprintf(s->log,"ENGINE_PREPARE_LEAVE_ERROR slot=%u thread=0x%x reason=not-active\n",
            slot,s->svc.current_thread_handle);
        fflush(s->log);
        return VP_BRIDGE_ARGUMENT;
    }
    /* Only the native C bodies (modes 1 and 2) declare a result. The original
     * root trampoline is used by modes 0, 3, 4, 5 and 6; its callers do not
     * consume r0, so any value is valid there. */
    if ((t->mode==1u || t->mode==2u) && f->regs[0]) {
        fprintf(s->log,"ENGINE_FULL_FATAL rc=%d\n",(int32_t)f->regs[0]);
        vp_engine_full_report(s->log);
        fflush(s->log);
        t->active=0u;
        return VP_BRIDGE_ARGUMENT;
    }
    uint32_t bytes=0u,growth=1u;
    if (t->command<=UINT32_MAX-0x2cu
        && vp_guest_svc_readable(&s->svc,t->command,0x2cu)) {
        uint32_t after=word((void *)(uintptr_t)(t->command+12u));
        if (after>=t->before && after-t->before<0x100000u &&
            word((void *)(uintptr_t)(t->command+0x28u))==t->block_before) {
            bytes=after-t->before;
            growth=0u;
        }
    }
    uint64_t elapsed=ended>=t->entry_us?ended-t->entry_us:0u;
    fprintf(s->log,"ENGINE_PREPARE_TIME mode=%s thread=0x%x context=0x%08x "
        "entry_us=%llu t_us=%llu elapsed_us=%llu encoded_bytes=%u growth=%u presents=%u\n",
        vp_engine_ab_mode_name(t->mode),s->svc.current_thread_handle,t->context,
        (unsigned long long)t->entry_us,(unsigned long long)ended,
        (unsigned long long)elapsed,bytes,growth,engine_prepare_presents(s));
    t->active=0u;
    return 0;
}

static int game_cost_service(Session *s, VPExecutor *e, VPBridgeFrame *f) {
    if (!game_cost_enabled) return VP_BRIDGE_UNSUPPORTED;
    unsigned site = f->regs[12];
    if (site >= VP_GAME_COST_SITES) return VP_BRIDGE_ARGUMENT;
    VPGuestThread *thread = thread_for_executor(e);
    unsigned slot;
    if (e == s->main_executor) slot = 0u;
    else if (thread && thread->handle && thread >= thread_pool.threads &&
        (unsigned)(thread - thread_pool.threads) < VP_GUEST_THREAD_LIMIT)
        slot = 1u + (unsigned)(thread - thread_pool.threads);
    else return VP_BRIDGE_ARGUMENT;
    VPGameCostThread *cost = &s->game_cost[slot];
    uint64_t now = sceKernelGetProcessTimeWide();
    if (f->service == 0x255u) {
        if (cost->depth == VP_GAME_COST_DEPTH ||
            !vp_guest_svc_readable(&s->svc, f->sp, 8u)) {
            fprintf(s->log, "GAME_COST_ERROR reason=entry slot=%u site=%u depth=%u\n",
                slot, site, cost->depth);
            return VP_BRIDGE_ARGUMENT;
        }
        VPGameCostSpan *span = &cost->stack[cost->depth++];
        span->started = now;
        span->child_us = 0u;
        span->bridge_us = span->bridge_calls = 0u;
        span->site = site;
        span->context = f->regs[0];
        span->caller = word((void *)(uintptr_t)(f->sp + 4u));
        span->presents = engine_prepare_presents(s);
        f->regs[12] = game_cost_trampolines[site];
        if (!f->regs[12]) return VP_BRIDGE_ARGUMENT;
        return 0;
    }
    if (!cost->depth || cost->stack[cost->depth - 1u].site != site) {
        fprintf(s->log, "GAME_COST_ERROR reason=leave slot=%u site=%u depth=%u\n",
            slot, site, cost->depth);
        return VP_BRIDGE_ARGUMENT;
    }
    VPGameCostSpan span = cost->stack[--cost->depth];
    if (now < span.started || now - span.started < span.child_us + span.bridge_us) {
        fprintf(s->log, "GAME_COST_ERROR reason=clock slot=%u site=%u\n", slot, site);
        return VP_BRIDGE_ARGUMENT;
    }
    uint64_t elapsed = now - span.started;
    if (cost->depth) cost->stack[cost->depth - 1u].child_us += elapsed;
    fprintf(s->log, "GAME_COST_TIME site=%u slot=%u thread=0x%x depth=%u "
        "context=0x%x caller=0x%x entry_us=%llu t_us=%llu incl_us=%llu self_us=%llu "
        "bridge_us=%llu bridge_calls=%llu presents_before=%u presents_after=%u\n", site, slot, s->svc.current_thread_handle,
        cost->depth, span.context, span.caller, (unsigned long long)span.started,
        (unsigned long long)now, (unsigned long long)elapsed,
        (unsigned long long)(elapsed - span.child_us),
        (unsigned long long)span.bridge_us, (unsigned long long)span.bridge_calls,
        span.presents, engine_prepare_presents(s));
    return 0;
}

static int dispatch_locked_body(VPExecutor *e,VPBridgeFrame *f);

/* Original-function self wall includes calls into the platform bridge. Account
 * these separately on the active innermost original span. This is wall time,
 * including blocking/worker handoff; subtraction is not a CPU-cycle claim. */
static int dispatch_locked(VPExecutor *e, VPBridgeFrame *f) {
    Session *s = e->user;
    VPGameCostThread *cost = NULL;
    if (game_cost_enabled && f->service != 0x255u && f->service != 0x256u) {
        if (e == s->main_executor) cost = &s->game_cost[0];
        else {
            VPGuestThread *thread = thread_for_executor(e);
            if (thread && thread->handle && thread >= thread_pool.threads &&
                (unsigned)(thread - thread_pool.threads) < VP_GUEST_THREAD_LIMIT)
                cost = &s->game_cost[1u + (unsigned)(thread - thread_pool.threads)];
        }
    }
    unsigned depth = cost ? cost->depth : 0u;
    uint64_t started = depth ? sceKernelGetProcessTimeWide() : 0u;
    int rc = dispatch_locked_body(e, f);
    if (started) {
        uint64_t ended = sceKernelGetProcessTimeWide();
        if (cost->depth != depth || ended < started) {
            fprintf(s->log, "GAME_COST_ERROR reason=bridge-depth-or-clock depth=%u\n", depth);
            return VP_BRIDGE_ARGUMENT;
        }
        cost->stack[depth - 1u].bridge_us += ended - started;
        cost->stack[depth - 1u].bridge_calls++;
    }
    return rc;
}

static int dispatch_locked_body(VPExecutor *e,VPBridgeFrame *f) {
    Session *s=e->user;
    if (f->service==0x255u || f->service==0x256u) return game_cost_service(s,e,f);
    if (f->service==0x252u) {
        fprintf(s->log,"ENGINE_FULL_FATAL rc=%d\n",(int32_t)f->regs[0]);
        vp_engine_full_report(s->log);
        fflush(s->log);
        return VP_BRIDGE_ARGUMENT;
    }
    if (f->service==0x253u || f->service==0x254u) return engine_prepare_ab_service(s,e,f);
    if (f->service==0x250u || f->service==0x251u) return engine_submit_probe_service(s,e,f);
    if (f->service>=0x24cu && f->service<=0x24fu) return movie_color_probe_service(s,e,f);
    if (f->service==0x24bu) {
        int32_t error=(int32_t)f->regs[0];
        fprintf(s->log,"SDK_LOCK_FAST_ERROR status=0x%08x lr=0x%08x thread=0x%08x\n",
            (uint32_t)error,f->lr,s->svc.current_thread_handle);
        fflush(s->log);
        return error<0?error:VP_BRIDGE_ARGUMENT;
    }
    if (f->service==0x24a) {
        /* ReadStrided has already resolved its SDK object. At +0x108 r0 is
         * that object, r1 is the row count, r7 is row bytes, r6 the source
         * offset, r4 the destination; local sp+4/+8 hold the two strides.
         * Preserve the original failure and partial-copy paths by resuming
         * the displaced LDR and loop whenever the CPU-output copy declines. */
        const uint32_t object=f->regs[0];
        if (!vp_guest_svc_readable(&s->svc,object,0x40u)
            || !vp_guest_svc_readable(&s->svc,f->sp+4u,8u)) return VP_BRIDGE_ARGUMENT;
        const uint32_t bytes=word((const unsigned char *)(uintptr_t)object+0x3cu);
        const uint32_t source=word((const unsigned char *)(uintptr_t)object+0x38u);
        const uint32_t dst_stride=word((const unsigned char *)(uintptr_t)f->sp+4u);
        const uint32_t src_stride=word((const unsigned char *)(uintptr_t)f->sp+8u);
        const uint64_t span=f->regs[1]
            ? (uint64_t)(f->regs[1]-1u)*src_stride+f->regs[7] : 0u;
        const uint64_t start=(uint64_t)source+f->regs[6];
        if (f->regs[6]<=bytes && span<=bytes-f->regs[6] && start<=UINT32_MAX
            && vp_media_video_readback(&s->svc,(uint32_t)start,src_stride,
                f->regs[4],dst_stride,f->regs[7],f->regs[1])) {
            if (movie_color_probe_enabled && s->movie_readback_stacks<4u) {
                unsigned n=++s->movie_readback_stacks;
                fprintf(s->log,"VP_MOVIE_READBACK_STACK n=%u thread=0x%x sp=0x%08x lr=0x%08x "
                    "saved_fp_offset=48 saved_lr_offset=52\n",n,s->svc.current_thread_handle,f->sp,f->lr);
                fprintf(s->log,"VP_MOVIE_READBACK_REGS n=%u",n);
                for (unsigned reg=0;reg<13u;++reg) fprintf(s->log," r%u=0x%08x",reg,f->regs[reg]);
                fputc('\n',s->log);
                fprintf(s->log,"VP_MOVIE_READBACK_WORDS n=%u",n);
                for (unsigned offset=0;offset<512u;offset+=4u) {
                    if (f->sp<=UINT32_MAX-offset
                        && vp_guest_svc_readable(&s->svc,f->sp+offset,4u)) {
                        uint32_t value=word((void *)(uintptr_t)(f->sp+offset));
                        fprintf(s->log," +0x%03x=0x%08x",offset,value);
                    } else fprintf(s->log," +0x%03x=unreadable",offset);
                }
                fputc('\n',s->log);fflush(s->log);
            }
            f->pc=e->modules[3].base+0x2a1f50u;
        } else {
            f->regs[11]=bytes; /* displaced LDR fp,[r0,#0x3c] */
            f->pc=e->modules[3].base+0x2a1edcu;
        }
        return 0;
    }
    if (f->service==0x304) { /* MapMemory: alias-stack calls and arena state */
        static int reported;
        uint32_t base=e->modules[1].base;
        fprintf(s->log,"SVC_MAPMEMORY d0=0x%08x s1=0x%08x z2=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x\n",
            f->regs[0],f->regs[1],f->regs[2],f->regs[4],f->regs[5],f->regs[6],f->regs[7]);
        if (!reported) {
            /* Main's GOT slots for nn::os::AllocateMemoryBlock/FreeMemoryBlock
             * (veneers at main+0x13f0124/+0x13f0130) and the named arena
             * objects hanging off the game's global memory framework. */
            uint32_t slot=base+0x183944c;
            uint32_t obj=vp_guest_svc_readable(&s->svc,slot,4)
                ?*(uint32_t *)(uintptr_t)slot:0;
            uint32_t alloc_got=vp_guest_svc_readable(&s->svc,base+0x1838150,8)
                ?*(uint32_t *)(uintptr_t)(base+0x1838150):0;
            uint32_t free_got=vp_guest_svc_readable(&s->svc,base+0x1838154,4)
                ?*(uint32_t *)(uintptr_t)(base+0x1838154):0;
            fprintf(s->log,"PROBE_IMPORT alloc=0x%08x free=0x%08x global=0x%08x\n",
                alloc_got,free_got,obj);
            if (obj) for (unsigned n=0;n<6;++n) {
                static const unsigned fields[6]={4,0x14,0x18,0x1c,0x20,0x38};
                uint32_t o=vp_guest_svc_readable(&s->svc,obj+fields[n],4)
                    ?*(uint32_t *)(uintptr_t)(obj+fields[n]):0;
                fprintf(s->log,"PROBE_HEAP field=0x%02x obj=0x%08x",fields[n],o);
                if (o && vp_guest_svc_readable(&s->svc,o,0x60)) {
                    fprintf(s->log," name=%.12s +0x50=0x%08x +0x54=0x%08x +0x58=0x%08x +0x5c=0x%08x",
                        (const char *)(uintptr_t)(o+0x10),
                        *(uint32_t *)(uintptr_t)(o+0x50),*(uint32_t *)(uintptr_t)(o+0x54),
                        *(uint32_t *)(uintptr_t)(o+0x58),*(uint32_t *)(uintptr_t)(o+0x5c));
                }
                fprintf(s->log,"\n");
            }
            reported=1;
        }
        fflush(s->log);
    }
    if (f->service>=VP_GUEST_SVC_BASE && f->service<VP_GUEST_SVC_MAX) {
        int thread_rc=thread_service(s,e,f);
        if (thread_rc!=VP_NOT_HANDLED) return thread_rc;
        return vp_guest_svc_dispatch(&s->svc,f);
    }
    if (f->service==0x100 || f->service==0x101) {
        VPHostBridge bridge=e->bridge;bridge.user=&s->allocator;
        int rc=vp_thread_local_bridge_handler(&bridge,f);
        fprintf(s->log,"SDK_ALLOC service=0x%x r0=0x%08x r1=%u live=%u bytes=%u status=%d\n",f->service,f->regs[0],f->regs[1],s->allocator.live_allocations,s->allocator.live_bytes,rc);fflush(s->log);
        return rc;
    }
    if (f->service==0x231) {
        unsigned owner=0;while (owner<4 && e->modules[owner].base!=f->regs[0]) ++owner;
        if (owner==4 || (s->observed_mask&(1u<<owner)) || f->sp<e->stack_base+4096 || f->sp>e->stack_base+e->stack_bytes-17*4) return -140;
        memcpy(s->original_args[owner],f->regs,16);memcpy(s->original_args[owner]+4,(void *)(uintptr_t)f->sp,17*4);
        ++s->observed;s->observed_mask|=1u<<owner;f->pc=s->resume;
        fprintf(s->log,"SDK_DSO_ENTER module=%u ordinal=%u\n",owner,s->observed);fflush(s->log);
        return 0;
    }
    if (f->service==0x230) {
        /* BOOT.3: this gate replaced `_init`'s first constructor-loop word.
         * Execute that displaced instruction (ldr r0,[pc,#0x8c]) here and
         * resume at the next one, so the module's original init array runs
         * with its linked relocations and the real guest stack. */
        ++s->gated;++s->ctor_dispatches;
        if (s->ctor_dispatches==1)
            fprintf(s->log,"SDK_CTOR_DISPATCH entry=0x%x word=0x%08x sp=0x%08x lr=0x%08x\n",
                    0x17c,word(e->modules[3].image+0x210),f->sp,f->lr);
        if (s->ctor_dispatches<=8) fflush(s->log);
        f->regs[0]=word(e->modules[3].image+0x210);
        f->pc=e->modules[3].base+0x180;
        return 0;
    }
    if (f->service==0x249) return VP_BRIDGE_UNSUPPORTED;
    if (f->service==0x235) {
        void *pointer=vp_thread_local_allocate(&s->allocator,f->regs[0],8);
        if (!pointer) return VP_THREAD_LOCAL_MEMORY;
        ++s->heap_allocs;f->regs[0]=(uint32_t)(uintptr_t)pointer;return 0;
    }
    if (f->service==0x236) {
        void *pointer=(void *)(uintptr_t)f->regs[0];if (!pointer) return 0;
        for (unsigned i=0;i<VP_THREAD_LOCAL_ALLOCATION_LIMIT;++i) if (s->allocator.allocations[i].pointer==pointer) {
            int rc=vp_thread_local_free(&s->allocator,pointer,s->allocator.allocations[i].size);
            if (!rc) ++s->heap_frees;
            return rc;
        }
        return VP_THREAD_LOCAL_OWNERSHIP;
    }
    if (f->service==0x240 || f->service==0x241) {
        if (!writable(e,s,f->regs[0],4) || (f->regs[0]&3)) return -148;
        int rc=vp_sdk_tls_allocate(&s->tls_pool,(void *)(uintptr_t)f->regs[0],f->regs[1],f->service==0x241);
        fprintf(s->log,"SDK_TLS_ALLOC sdk=%u slot=%u result=%d\n",f->service==0x241,word((void *)(uintptr_t)f->regs[0]),rc);fflush(s->log);
        if (rc<0) return rc;
        f->regs[0]=(uint32_t)rc;return 0;
    }
    if (f->service==0x242) return vp_sdk_tls_free(&s->tls_pool,tls_values_for(s,e),f->regs[0]);
    if (f->service==0x243) return vp_sdk_tls_get(&s->tls_pool,tls_values_for(s,e),f->regs[0],&f->regs[0]);
    if (f->service==0x244) return vp_sdk_tls_set(&s->tls_pool,tls_values_for(s,e),f->regs[0],f->regs[1]);
    if (f->service==0x234) {
        uint32_t pointer=f->regs[0];
        if (pointer!=(uint32_t)(uintptr_t)s->exception_output &&
            (pointer<e->stack_base+4096 || pointer>e->stack_base+e->stack_bytes-16 || (pointer&3))) return -144;
        uint32_t pc=f->regs[1]&~1u;int rc;
        uint32_t begin=(uint32_t)(uintptr_t)vp_sdk_cpp_fixture,end=(uint32_t)(uintptr_t)vp_sdk_cpp_fixture_end;
        if (pc>=begin && pc<end) {
            uint32_t base=begin&~4095u;
            /* Only the two fixture entries are exposed. Other application
             * CRT entries do not belong to this bounded guest fixture. */
            uint32_t info[4]={base,((uint32_t)(uintptr_t)vp_fixture_exidx_end-base+4095)&~4095u,
                (uint32_t)(uintptr_t)vp_fixture_exidx_start-base,16};
            memcpy((void *)(uintptr_t)pointer,info,sizeof(info));rc=1;
        } else {
            uint32_t info[4];rc=vp_loader_exception_info(&s->exceptions,f->regs[1],info);
            if (rc==1) {
                unsigned owner=0;while (owner<s->exceptions.count && s->exceptions.modules[owner].base!=info[0]) ++owner;
                if (owner==s->exceptions.count) return VP_UNWIND_STATE;
                int valid=vp_sdk_exception_index_verify(&s->exceptions.modules[owner],&s->indexes[owner]);if (valid) return valid;
                info[1]=s->exceptions.modules[owner].module->capacity;info[2]=s->indexes[owner].offset;
                memcpy((void *)(uintptr_t)pointer,info,sizeof(info));
            }
        }
        ++s->exception_queries;
        fprintf(s->log,"SDK_EXCEPTION_QUERY pc=0x%08x result=%d\n",f->regs[1],rc);fflush(s->log);
        if (rc<0) return rc;
        f->regs[0]=(uint32_t)rc;return 0;
    }
    if (f->service==0x232 || f->service==0x233 || f->service==0x245 || f->service==0x246) {
        ++s->fast_fallback;
        uint32_t address=f->regs[0];
        if (!writable(e,s,address,4) || (address&3)) return -141;
        unsigned i=sdk_lock_index(s,address);
        if (f->service==0x246) {
            f->regs[0]=i<VP_SDK_LOCK_LIMIT && s->locks[i].address && s->locks[i].depth
                && s->locks[i].owner==s->svc.current_thread_handle;
            return 0;
        }
        /* The table is a bounded address-keyed cache of the guest lock's
         * state, not the lock itself. When it is full, a slot whose lock is
         * fully released carries exactly the state of a freshly initialized
         * mutex, so it is repurposed for the new address and its host mutex
         * (already unlocked) is reused. Entries that are held or are being
         * acquired (pending) are never recycled. An Unlock of an address the
         * table has never seen still fails instead of creating state. */
        if (i==VP_SDK_LOCK_LIMIT && f->service!=0x233) {
            for (unsigned scan=0;scan<VP_SDK_LOCK_LIMIT;++scan) {
                unsigned r=(s->lock_cursor+scan)%VP_SDK_LOCK_LIMIT;
                Lock *slot=&s->locks[r];
                if (!slot->address || slot->depth || slot->host_depth || slot->pending || slot->held || slot->owner) continue;
                unsigned previous=slot->address;
                slot->address=address;
                s->lock_cursor=(r+1)%VP_SDK_LOCK_LIMIT;
                ++s->lock_recycles;
                if (s->lock_recycles<=8)
                    fprintf(s->log,"SDK_LOCK_RECYCLE index=%u old=0x%08x new=0x%08x total=%u\n",
                        r,previous,address,s->lock_recycles);
                i=r;
                break;
            }
        }
        if (i==VP_SDK_LOCK_LIMIT || (!s->locks[i].address && f->service==0x233)) {
            fprintf(s->log,"SDK_LOCK_REJECT service=0x%x address=0x%08x index=%u table=%u lr=0x%08x sp=0x%08x tp_thread=0x%08x\n",
                    f->service,address,i,(unsigned)VP_SDK_LOCK_LIMIT,f->lr,f->sp,
                    word((void *)(uintptr_t)(e->bridge.guest.tls_ro+0x1fc)));
            fputs("SDK_LOCK_STACK",s->log);
            for (unsigned d=0;d<8;++d)
                if (vp_guest_svc_readable(&s->svc,f->sp+d*4,4))
                    fprintf(s->log," +0x%02x=0x%08x",d*4,word((void *)(uintptr_t)(f->sp+d*4)));
            fputc('\n',s->log);
            fflush(s->log);
            return -142;
        }
        Lock *lock=&s->locks[i];
        if (!lock->address) {
            /* InternalCriticalSection is recursive (the SDK re-enters it from
             * TimedWait's timeout path); a non-recursive host mutex reports
             * MUTEX_RECURSIVE and kills the call. */
            lock->id=sceKernelCreateMutex("vp-sdk-libc-lock",SCE_KERNEL_MUTEX_ATTR_RECURSIVE,0,NULL);
            if (lock->id<0) return lock->id;
            lock->address=address;
            fprintf(s->log,"SDK_LOCK_NEW index=%u address=0x%08x\n",i,address);fflush(s->log);
        }
        s->lock_hint[(address>>2)&63u]=i+1u;
        int rc;
        if (f->service==0x245) {
            uint32_t owner=s->svc.current_thread_handle;
            if (lock->depth && lock->owner==owner) {
                ++lock->depth;++s->lock_calls;f->regs[0]=1;return 0;
            }
            rc=sceKernelTryLockMutex(lock->id,1);
            if (!rc) {
                if (!lock->depth) {
                    lock->owner=owner;
                    lock->host_depth=1;
                    lock->held_since_us=svc_arrival_enabled()
                        ?sceKernelGetProcessTimeWide():0u;
                }
                ++lock->depth;lock->held=1;++s->lock_calls;f->regs[0]=1;
            }
            else if (rc==(int)SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN) { f->regs[0]=0;rc=0; }
        } else if (f->service==0x232) {
            /* A blocking Enter must not hold the session lock: the current
             * owner needs it to reach Leave. B126: a waiter stays queued on
             * the host mutex for a long slice. The former 100 ms retry
             * re-queued the waiter every timeout, and a thread that acquires
             * and releases the same lock thousands of times per second could
             * overtake it indefinitely (the movie decoder waited 0.5-1.0 s
             * behind such a loop). The long slice keeps the kernel's queued
             * wake-up authoritative; the total cap still reports a real
             * deadlock instead of hanging the session. */
            uint32_t owner=s->svc.current_thread_handle;
            if (lock->depth && lock->owner==owner) {
                ++lock->depth;++s->lock_calls;return 0;
            }
            unsigned waited=0;
            if (lock->depth && address>=e->modules[3].base+0x86ec00u && address<e->modules[3].base+0x86ee00u)
                fprintf(s->log,"SDK_APPLET_LOCK_WAIT address=0x%08x owner=0x%08x depth=%u thread=0x%08x\n",
                    address,lock->owner,lock->depth,owner);
            ++lock->pending;
            uint64_t lock_started=svc_arrival_enabled()?sceKernelGetProcessTimeWide():0u;
            uint32_t holder_before=lock->owner;
            rc=sdk_lock_acquire(s,lock,&waited);
            --lock->pending;
            if (rc) {
                if (rc==VP_SDK_LOCK_DEADLOCK) {
                    fprintf(s->log,"SDK_LOCK_ENTER_TIMEOUT index=%u address=0x%08x owner=0x%08x thread=0x%08x depth=%u waited_us=%u model=host-mutex\n",
                        i,address,lock->owner,owner,lock->depth,waited);
                } else {
                    fprintf(s->log,"SDK_LOCK_ENTER_FAIL index=%u address=0x%08x owner=0x%08x thread=0x%08x depth=%u status=0x%08x\n",
                        i,address,lock->owner,owner,lock->depth,(uint32_t)rc);
                }
                fflush(s->log);return rc;
            }
            /* B125: record real contention separately from ordinary immediate
             * acquisition; the wait runs under a host mutex timeout slice, so
             * the measured process-time delta is the blocking time. */
            if (svc_arrival_enabled() && g_sdk_lock_wait_lines<4000u) {
                uint64_t waited_us=sceKernelGetProcessTimeWide()-lock_started;
                if (waited_us>=20000u) {
                    ++g_sdk_lock_wait_lines;
                    fprintf(s->log,"VP_SDK_LOCK_TIME address=0x%08x thread=0x%x holder_at_wait=0x%x owner=0x%x waited_us=%llu depth=%u slots=%u\n",
                        address,owner,holder_before,lock->owner,
                        (unsigned long long)waited_us,lock->depth,
                        (unsigned)s->lock_calls);
                    fflush(s->log);
                }
            }
            if (!lock->depth) {
                lock->owner=owner;
                lock->host_depth=1;
                lock->held_since_us=svc_arrival_enabled()
                    ?sceKernelGetProcessTimeWide():0u;
            }
            ++lock->depth;lock->held=1;++s->lock_calls;
            if (address>=e->modules[3].base+0x86ec00u && address<e->modules[3].base+0x86ee00u)
                fprintf(s->log,"SDK_APPLET_LOCK_ENTER address=0x%08x thread=0x%08x waited_us=%u depth=%u\n",
                    address,owner,waited,lock->depth);
        } else {
            if (lock->depth>1u && lock->owner==s->svc.current_thread_handle) {
                --lock->depth;++s->unlock_calls;return 0;
            }
            rc=(!lock->depth || lock->owner!=s->svc.current_thread_handle)
                ?(int)SCE_KERNEL_ERROR_MUTEX_NOT_OWNED:sceKernelUnlockMutex(lock->id,1);
            if (!rc) {
                uint64_t held_us=lock->held_since_us&&svc_arrival_enabled()
                    ?sceKernelGetProcessTimeWide()-lock->held_since_us:0u;
                if (lock->depth) --lock->depth;
                if (!lock->depth) {
                    lock->owner=0;
                    lock->host_depth=0;
                    if (held_us>=50000u && g_sdk_hold_lines<4000u) {
                        ++g_sdk_hold_lines;
                        fprintf(s->log,"VP_SDK_LOCK_HOLD address=0x%08x thread=0x%x hold_us=%llu\n",
                            address,s->svc.current_thread_handle,
                            (unsigned long long)held_us);
                        fflush(s->log);
                    }
                    lock->held_since_us=0;
                }
                lock->held=lock->depth!=0;
                ++s->unlock_calls;
                if (address>=e->modules[3].base+0x86ec00u && address<e->modules[3].base+0x86ee00u)
                    fprintf(s->log,"SDK_APPLET_LOCK_LEAVE address=0x%08x thread=0x%08x depth=%u\n",
                        address,s->svc.current_thread_handle,lock->depth);
            } else {
                fprintf(s->log,"SDK_LOCK_LEAVE_FAIL index=%u address=0x%08x owner=0x%08x thread=0x%08x depth=%u status=0x%08x\n",
                    i,address,lock->owner,s->svc.current_thread_handle,lock->depth,(uint32_t)rc);
                fflush(s->log);
            }
        }
        return rc;
    }
    return VP_BRIDGE_UNSUPPORTED;
}
/* B124: bounded bridge-arrival trace for the movie pacing investigation.
 * The movie player submits a decode/composite only every ~352 ms while both
 * hooks measure a few milliseconds, so the remaining time is either guest CPU
 * between services, a parked condition-variable wait, or one slow service.
 * This table records each guest thread's last dispatch exit on the host
 * process clock and logs arrivals that follow a long gap (with the previous
 * service and a bounded stack sample), plus slow dispatches past the old
 * 128-line cap. Marker-gated by play-perf and bounded so a two-minute run
 * stays readable; without the marker nothing is measured or printed. */
typedef struct {
    uint32_t handle;
    uint64_t exit_us;
    uint32_t last_service;
    uint64_t last_elapsed_us;
    uint32_t last_lr;
    /* B125: per-thread activity in the current one-second window. The sum of
     * dispatch durations, split by the service that owns the time, tells a
     * parked wait from guest CPU and from service work without a line per
     * call. All counters reset when the window is printed. */
    unsigned calls;
    uint64_t busy_us, max_us;
    unsigned sleep_n, key_n, lock_n, wait_n, ipc_n, timer_n;
    uint64_t sleep_us, key_us, lock_us, wait_us, ipc_us, timer_us;
} VPSvcArrival;

#define VP_SVC_ARRIVAL_LIMIT 64u
static VPSvcArrival g_svc_arrivals[VP_SVC_ARRIVAL_LIMIT];
static unsigned g_svc_arrival_lines, g_svc_arrival_stacks;
static unsigned g_svc_slow_lines, g_svc_key_lines;
static uint64_t g_svc_summary_last;
static int g_svc_arrival_enabled = -1;

static void svc_activity_note(VPSvcArrival *a, uint32_t service, uint64_t elapsed) {
    ++a->calls;
    a->busy_us += elapsed;
    if (elapsed > a->max_us) a->max_us = elapsed;
    switch (service) {
    case 0x30bu: a->sleep_us += elapsed; ++a->sleep_n; break; /* svcSleepThread */
    case 0x31cu: a->key_us += elapsed; ++a->key_n; break;     /* WaitProcessWideKeyAtomic */
    case 0x232u: a->lock_us += elapsed; ++a->lock_n; break;   /* SDK critical-section Enter */
    case 0x318u: a->wait_us += elapsed; ++a->wait_n; break;   /* WaitSynchronization */
    case 0x321u: case 0x322u:
        a->ipc_us += elapsed; ++a->ipc_n; break;              /* SendSyncRequest */
    case VP_GUEST_TIMER_SERVICE:
        a->timer_us += elapsed; ++a->timer_n; break;          /* MRRC generic timer */
    }
}

/* Print one line per guest thread per second and start a new window. Called
 * under the session lock from the dispatch tail, after the counters of the
 * current call were recorded. */
static void svc_activity_flush(Session *s, uint64_t now) {
    if (!svc_arrival_enabled()) return;
    if (!g_svc_summary_last) { g_svc_summary_last = now; return; }
    if (now - g_svc_summary_last < 1000000u) return;
    g_svc_summary_last = now;
    for (unsigned i = 0; i < VP_SVC_ARRIVAL_LIMIT; ++i) {
        VPSvcArrival *a = &g_svc_arrivals[i];
        if (!a->handle || !a->calls) continue;
        fprintf(s->log,"VP_THREAD_ACT thread=0x%x calls=%u busy_us=%llu max_us=%llu "
            "sleep=%u/%llu key=%u/%llu lock=%u/%llu wait=%u/%llu ipc=%u/%llu timer=%u/%llu\n",
            a->handle,a->calls,(unsigned long long)a->busy_us,
            (unsigned long long)a->max_us,a->sleep_n,(unsigned long long)a->sleep_us,
            a->key_n,(unsigned long long)a->key_us,a->lock_n,(unsigned long long)a->lock_us,
            a->wait_n,(unsigned long long)a->wait_us,a->ipc_n,(unsigned long long)a->ipc_us,
            a->timer_n,(unsigned long long)a->timer_us);
        a->calls = 0;
        a->busy_us = a->max_us = 0;
        a->sleep_n = a->key_n = a->lock_n = a->wait_n = a->ipc_n = a->timer_n = 0;
        a->sleep_us = a->key_us = a->lock_us = a->wait_us = a->ipc_us = a->timer_us = 0;
    }
    fflush(s->log);
}

static int svc_arrival_enabled(void) {
    if (g_svc_arrival_enabled < 0) {
        FILE *marker=fopen("ux0:data/mhgu-vita-port/play-perf.enable","rb");
        g_svc_arrival_enabled=marker!=NULL;
        if (marker) fclose(marker);
    }
    return g_svc_arrival_enabled;
}

static VPSvcArrival *svc_arrival_slot(uint32_t handle) {
    unsigned spare=VP_SVC_ARRIVAL_LIMIT;
    for (unsigned i=0;i<VP_SVC_ARRIVAL_LIMIT;++i) {
        if (g_svc_arrivals[i].handle==handle) return &g_svc_arrivals[i];
        if (!g_svc_arrivals[i].handle && spare==VP_SVC_ARRIVAL_LIMIT) spare=i;
    }
    if (spare==VP_SVC_ARRIVAL_LIMIT) return NULL;
    g_svc_arrivals[spare].handle=handle;
    return &g_svc_arrivals[spare];
}

static void svc_arrival_stack(FILE *log,const VPGuestSvc *svc,uint32_t sp) {
    fputs("VP_SVC_GAP_STACK",log);
    for (unsigned i=0;i<24u;++i) {
        uint32_t at=sp+i*4u,v=0;
        if (!vp_guest_svc_readable(svc,at,4u)) break;
        memcpy(&v,(void *)(uintptr_t)at,4);
        fprintf(log," %08x",v);
    }
    fputc('\n',log);
}

static int dispatch(VPExecutor *e,VPBridgeFrame *f) {
    Session *s=e->user;
    uint64_t wait_started=sceKernelGetProcessTimeWide();
    state_lock(s);
    uint64_t lock_entered=sceKernelGetProcessTimeWide();
    int stop_now=0;
    VPGuestThread *caller=thread_for_executor(e);
    uint32_t saved=s->svc.current_thread_handle;
    uint32_t saved_tls=s->svc.tls_buffer;
    /* The ambient values are shared between executor threads, so they must be
     * rewritten on every dispatch: a worker that parks with the session lock
     * dropped must not leak its handle or IPC buffer into another service.
     * svcSendSyncRequest reads the calling thread's TLS buffer, so this is
     * the per-thread command buffer exactly like the kernel side. */
    s->svc.current_thread_handle=caller && caller->handle
        ? caller->handle : (s->start_handle ? s->start_handle : 1u);
    s->svc.tls_buffer=e->bridge.guest.tls_ro;
    /* B124: report a service arrival that follows a long gap on the same
     * guest thread, so guest CPU time between services and parked waits can
     * be told apart from service duration. */
    VPSvcArrival *arrival=NULL;
    if (svc_arrival_enabled()) {
        arrival=svc_arrival_slot(s->svc.current_thread_handle);
        if (arrival && arrival->exit_us && wait_started>arrival->exit_us
            && g_svc_arrival_lines<6000u) {
            uint64_t gap=wait_started-arrival->exit_us;
            if (gap>=50000u) {
                ++g_svc_arrival_lines;
                fprintf(s->log,"VP_SVC_GAP thread=0x%x gap_us=%llu service=0x%x "
                    "pc=0x%08x lr=0x%08x sp=0x%08x prev_service=0x%x "
                    "prev_elapsed_us=%llu prev_lr=0x%08x\n",
                    s->svc.current_thread_handle,(unsigned long long)gap,
                    f->service,f->pc,f->lr,f->sp,arrival->last_service,
                    (unsigned long long)arrival->last_elapsed_us,arrival->last_lr);
                if (gap>=150000u && g_svc_arrival_stacks<240u) {
                    ++g_svc_arrival_stacks;
                    svc_arrival_stack(s->log,&s->svc,f->sp);
                }
                fflush(s->log);
            }
        }
    }
    if (s->svc.audio_probe_thread_handle
        && s->svc.current_thread_handle==s->svc.audio_probe_thread_handle) {
        fprintf(s->log,"SVC_PROBE service=0x%x r0=0x%08x r1=0x%08x r2=0x%08x"
            " r3=0x%08x pc=0x%08x lr=0x%08x sp=0x%08x\n",
            f->service,f->regs[0],f->regs[1],f->regs[2],f->regs[3],
            f->pc,f->lr,f->sp);
        fflush(s->log);
        if (s->svc.audio_probe_trace_budget) {
            --s->svc.audio_probe_trace_budget;
            FILE *trace=fopen("ux0:data/mhgu-vita-port/audio-probe-svc.log","a");
            if (trace) {
                fprintf(trace,"SVC_PROBE service=0x%x r0=0x%08x r1=0x%08x"
                    " r2=0x%08x r3=0x%08x pc=0x%08x lr=0x%08x sp=0x%08x\n",
                    f->service,f->regs[0],f->regs[1],f->regs[2],f->regs[3],
                    f->pc,f->lr,f->sp);
                fclose(trace);
            }
        }
    }
    {
        uint32_t main_handle=s->start_handle ? s->start_handle : 1u;
        uint64_t now=sceKernelGetProcessTimeWide();
        if (s->svc.current_thread_handle == main_handle) {
            s->main_sp=f->sp;s->main_tick=now;
            if (f->service==VP_GUEST_TIMER_SERVICE && !s->timer_dumped) {
                if (!s->main_timer_tick) s->main_timer_tick=now;
                else if (now-s->main_timer_tick>300000ull) {
                    s->timer_dumped=1;
                    fprintf(s->log,"SVC_MAIN_TIMER_SPIN us=%llu sp=0x%08x pc=0x%08x lr=0x%08x\n",
                        (unsigned long long)(now-s->main_timer_tick),f->sp,f->pc,f->lr);
                    uint32_t from=s->main_sp-0x400u,to=s->main_sp+0x1400u;
                    for (uint32_t at=from;at<to;at+=32u) {
                        fprintf(s->log,"SVC_MAIN_TIMER_STACK +0x%04x",at-s->main_sp);
                        for (unsigned i=0;i<8;++i) {
                            uint32_t v=0;
                            if (vp_guest_svc_readable(&s->svc,at+i*4u,4))
                                memcpy(&v,(void *)(uintptr_t)(at+i*4u),4);
                            fprintf(s->log," %08x",v);
                        }
                        fputc('\n',s->log);
                    }
                    fflush(s->log);
                }
            }
            /* Temporary diagnosis: the main thread remains inside the SDK
             * time path long after nnMain starts. Dump its stack once during
             * that steady state so the polling caller chain is resolvable. */
            if (f->service==VP_GUEST_TIMER_SERVICE && !s->late_dumped && now>20000000ull) {
                s->late_dumped=1;
                s->main_late_tick=now;
                fprintf(s->log,"SVC_MAIN_TIMER_LATE us=%llu sp=0x%08x pc=0x%08x lr=0x%08x\n",
                    (unsigned long long)now,f->sp,f->pc,f->lr);
                uint32_t from=s->main_sp-0x400u,to=s->main_sp+0x1400u;
                for (uint32_t at=from;at<to;at+=32u) {
                    fprintf(s->log,"SVC_MAIN_TIMER_LATE_STACK +0x%04x",at-s->main_sp);
                    for (unsigned i=0;i<8;++i) {
                        uint32_t v=0;
                        if (vp_guest_svc_readable(&s->svc,at+i*4u,4))
                            memcpy(&v,(void *)(uintptr_t)(at+i*4u),4);
                        fprintf(s->log," %08x",v);
                    }
                    fputc('\n',s->log);
                }
                fflush(s->log);
            }
            /* Temporary diagnosis: nnMain (main+0x1a82e8, verified prologue
             * push {r4,r5,fp,lr}; sub sp,#0x48; sub sp,#0x2400) never returns
             * while the title's frame loop runs, so Start cannot report it.
             * Scan the main thread's live stack once for a saved return
             * address inside nnMain's body. A hit proves execution entered
             * the original nnMain and is still inside a call it made. */
            if (!s->nnmain_found && now>5000000ull
                && s->main_sp+0x400u<e->stack_top && s->nnmain_attempts<600u) {
                ++s->nnmain_attempts;
                uint32_t nnmain=e->modules[1].base+0x1a82e8u;
                uint32_t found=0,slot=0;
                for (uint32_t at=s->main_sp;at+4u<=e->stack_top;at+=4u) {
                    uint32_t v=0;
                    if (!vp_guest_svc_readable(&s->svc,at,4)) continue;
                    memcpy(&v,(void *)(uintptr_t)at,4);
                    if (v>=nnmain && v<nnmain+0x80u) { found=v;slot=at;break; }
                }
                if (found) { s->nnmain_found=1;s->nnmain_found_tick=now; }
                if (found || s->nnmain_attempts==1) {
                    s->nnmain_scanned=1;
                    fprintf(s->log,"SVC_MAIN_NNMAIN_SCAN nnmain=0x%08x found=%u return=0x%08x slot=0x%08x offset=0x%x sp=0x%08x top=0x%08x attempts=%u\n",
                        nnmain,found?1u:0u,found,slot,found?found-nnmain:0u,
                        s->main_sp,e->stack_top,s->nnmain_attempts);
                    fflush(s->log);
                }
            }
            /* One heartbeat per second records where the main thread is when
             * it does reach the bridge; comparing samples distinguishes a
             * long initialization path from a fixed loop. The same cadence
             * polls the vi:obj BufferQueue counters once. */
            if (now-s->main_heart_tick>=1000000ull) {
                s->main_heart_tick=now;
                fprintf(s->log,"SVC_MAIN_HEART us=%llu service=0x%x sp=0x%08x pc=0x%08x lr=0x%08x\n",
                    (unsigned long long)now,f->service,f->sp,f->pc,f->lr);
                fprintf(s->log,"VP_SDK_LOCK_FAST enter=%u leave=%u try=%u islocked=%u fallback=%u hold_trace=slow-enter-and-leave-only\n",
                    s->fast_enter,s->fast_leave,s->fast_try,s->fast_islocked,s->fast_fallback);
                fprintf(s->log,"VP_SDK_LOCK_FAST_MISS context=%u session_error=%u owner=%u slot=%u permission=%u contention=%u\n",
                    s->fast_miss[0],s->fast_miss[1],s->fast_miss[2],s->fast_miss[3],s->fast_miss[4],s->fast_miss[5]);
                unsigned queues=0,presents=0;
                for (unsigned i=0;i<VP_GUEST_HANDLE_LIMIT;++i) {
                    const VPGuestHandle *h=&s->svc.handles[i];
                    if (h->kind!=2u || strcmp(h->name,"vi:obj")) continue;
                    if (h->session<1u || h->session>VP_GUEST_SESSION_LIMIT) continue;
                    queues+=s->svc.sessions[h->session-1u].binder_queues;
                    presents+=s->svc.sessions[h->session-1u].binder_presents;
                }
                /* The M5 probe waits for the title's presentation timeline to
                 * be running before it starts its measured stream; the
                 * loader's once-per-second poll is its only view of that
                 * counter. */
                if (s->svc.audio_probe_args)
                    *(volatile uint32_t *)(uintptr_t)(s->svc.audio_probe_args
                        +(uint32_t)offsetof(VPAudioProbeArgs,title_presents))=presents;
                if (!s->heart_diag && now>15000000ull && !presents) {
                    s->heart_diag=1;
                    for (unsigned i=0;i<VP_GUEST_HANDLE_LIMIT;++i) {
                        const VPGuestHandle *h=&s->svc.handles[i];
                        if (h->kind)
                            fprintf(s->log,"SVC_MAIN_HANDLE index=%u handle=0x%x kind=%u name=%s session=%u\n",
                                i,VP_GUEST_HANDLE_FIRST+i,h->kind,h->name,h->session);
                    }
                    fprintf(s->log,"SVC_MAIN_FRAME_SCAN queues=%u presents=%u\n",queues,presents);
                    fflush(s->log);
                }
                if (presents) {
                    if (!s->frame_observed) {
                        s->frame_observed=1;s->frame_tick=now;
                        VPAudioSinkStats av;vp_audio_sink_stats(&av);
                        fprintf(s->log,"VP022_FRAME_LOOP us=%llu queues=%u presents=%u model=vi-bufferqueue-consumer-release first=1 audio_out=%llu audio_written=%llu audio_pending=%u audio_underruns=%llu audio_silence=%llu audio_peak=%u audio_zc=%llu\n",
                            (unsigned long long)now,queues,presents,
                            (unsigned long long)av.output_frames,
                            (unsigned long long)av.written_frames,av.pending_frames,
                            (unsigned long long)av.underruns,
                            (unsigned long long)av.silence_frames,av.peak,
                            (unsigned long long)av.zero_crossings);
                    } else if (queues!=s->frame_queues || presents!=s->frame_presents) {
                        /* One line carries both presentation clocks: the VI
                         * buffer-queue present counter and the SceAudioOut
                         * output frame counter, sampled on the same
                         * sceKernelGetProcessTimeWide timeline. This is the
                         * A/V sync record; neither clock is derived from the
                         * other. */
                        VPAudioSinkStats av;vp_audio_sink_stats(&av);
                        fprintf(s->log,"VP022_FRAME_LOOP us=%llu queues=%u presents=%u model=vi-bufferqueue-consumer-release audio_out=%llu audio_written=%llu audio_pending=%u audio_underruns=%llu audio_silence=%llu audio_peak=%u audio_zc=%llu\n",
                            (unsigned long long)now,queues,presents,
                            (unsigned long long)av.output_frames,
                            (unsigned long long)av.written_frames,av.pending_frames,
                            (unsigned long long)av.underruns,
                            (unsigned long long)av.silence_frames,av.peak,
                            (unsigned long long)av.zero_crossings);
                    }
                    s->frame_queues=queues;s->frame_presents=presents;
                }
                fflush(s->log);
            }
            /* Sample loader-service traffic from the main thread for one
             * second out of every second of silence: the watchdog otherwise
             * cannot tell a spin on an unlogged service from a pure guest
             * loop. Bounded so a long run stays readable. */
            if (now - s->main_bridge_tick >= 1000000ull) {
                s->main_bridge_tick=now;s->main_bridge_burst=8;
            }
            if (s->main_bridge_burst) {
                --s->main_bridge_burst;
                fprintf(s->log,"SVC_MAIN_BRIDGE service=0x%x pc=0x%08x sp=0x%08x lr=0x%08x\n",
                    f->service,f->pc,f->sp,f->lr);
                fflush(s->log);
            }
            /* Explicit termination test: hand the title's own applet message
             * loop a reviewed Exit message (0x4) after the frame loop starts.
             * The title decides what to do; the loader only delivers it. */
            if (!s->continuous_play && s->termination_test && !s->applet_exit_sent && s->frame_observed
                && now - s->frame_tick > s->termination_delay_us) {
                s->applet_exit_sent = 1;
                int queued = vp_guest_svc_queue_applet_message(&s->svc, 4u, "title-exit-test");
                s->applet_exit_tick = now;
                fprintf(s->log, "VP022_APPLET_EXIT_REQUEST message=0x4 result=%d us=%llu\n",
                        queued, (unsigned long long)now);
                fflush(s->log);
            }
            /* The platform requested applet exit and the title released its
             * exit lock (ISelfController UnlockExit/Exit). The reviewed
             * reference terminates the process there; end the session as a
             * process termination from the main thread's next service call.
             * This is not the loader's evidence stop below. */
            if (!s->title_exited && vp_guest_svc_process_exit_requested(&s->svc)) {
                s->title_exited=1;s->session_stopped=1;stop_now=1;
                fprintf(s->log,"VP022_PROCESS_EXIT us=%llu reason=title-applet-exit-unlocked nnmain=0x%08x frames=%u presents=%u svc_calls=%u termination_test=%d\n",
                    (unsigned long long)now,
                    e->modules[1].base+0x1a82e8u,s->frame_queues,s->frame_presents,
                    s->svc.calls,s->termination_test);
                fflush(s->log);
            }
            /* Evidence-bounded stop: the original nnMain has been observed on
             * this thread's live stack and the vi display ring has completed
             * at least one full consumer-release cycle, then ran for the grace
             * period. End the diagnostic session from this bridge call instead
             * of letting the harness kill the process without a report. The
             * stop is recorded and is not a title exit. In the explicit
             * termination test the loader waits longer for the title's own
             * exit request to take effect before falling back to this stop. */
            uint64_t stop_grace = s->termination_test ? 8000000ull : 2000000ull;
            uint64_t grace_tick = s->termination_test ? s->applet_exit_tick : s->frame_tick;
            if (!s->continuous_play && s->nnmain_found && s->frame_observed && !s->session_stopped
                && (!s->termination_test || s->applet_exit_sent)
                && now-grace_tick>stop_grace) {
                s->session_stopped=1;
                stop_now=1;
                fprintf(s->log,"VP022_SESSION_STOP us=%llu reason=nnmain-and-frame-loop-observed nnmain=0x%08x frames=%u presents=%u service=0x%x pc=0x%08x lr=0x%08x svc_calls=%u termination_test=%d\n",
                    (unsigned long long)now,
                    e->modules[1].base+0x1a82e8u,s->frame_queues,s->frame_presents,
                    f->service,f->pc,f->lr,s->svc.calls,s->termination_test);
                fflush(s->log);
            }
        } else if (!s->stall_dumped && s->main_tick
                   && now - s->main_tick > 3000000ull) {
            s->stall_dumped=1;
            fprintf(s->log,"SVC_MAIN_STALL silent_us=%llu last_sp=0x%08x thread=0x%08x\n",
                (unsigned long long)(now - s->main_tick),s->main_sp,
                s->svc.current_thread_handle);
            uint32_t from=s->main_sp-0x800u,to=s->main_sp+0x1800u;
            for (uint32_t at=from;at<to;at+=32u) {
                fprintf(s->log,"SVC_MAIN_STACK +0x%04x",at-s->main_sp);
                for (unsigned i=0;i<8;++i) {
                    uint32_t v=0;
                    if (vp_guest_svc_readable(&s->svc,at+i*4u,4))
                        memcpy(&v,(void *)(uintptr_t)(at+i*4u),4);
                    fprintf(s->log," %08x",v);
                }
                fputc('\n',s->log);
            }
            fflush(s->log);
        }
    }
    int rc=stop_now?(s->title_exited?VP_BRIDGE_PROCESS_EXIT:VP_BRIDGE_SESSION_STOP):dispatch_locked(e,f);
    {
        static unsigned timing_count;
        uint64_t elapsed=sceKernelGetProcessTimeWide()-lock_entered;
        uint64_t waited=lock_entered-wait_started;
        if ((elapsed>20000u || waited>20000u)
            && (timing_count<128u || (arrival && g_svc_slow_lines<4000u))) {
            if (timing_count<128u) ++timing_count;
            if (arrival) ++g_svc_slow_lines;
            fprintf(s->log,"VP_SVC_TIMING n=%u thread=0x%x service=0x%x start=%llu wait=%llu elapsed=%llu\n",
                timing_count,s->svc.current_thread_handle,f->service,
                (unsigned long long)lock_entered,(unsigned long long)waited,(unsigned long long)elapsed);
        }
        if (arrival) {
            svc_activity_note(arrival,f->service,elapsed);
            arrival->exit_us=lock_entered+elapsed;
            arrival->last_service=f->service;
            arrival->last_elapsed_us=elapsed;
            arrival->last_lr=f->lr;
            svc_activity_flush(s,lock_entered+elapsed);
        }
    }
    s->svc.tls_buffer=saved_tls;
    s->svc.current_thread_handle=saved;
    state_unlock(s);
    return rc;
}
/* The original startup code builds five named arenas through
 * nn::os::AllocateMemoryBlock before any thread exists: Resource 500 MiB,
 * Unit 120 MiB, NVNBuffer 200 MiB, NVNTexture 850 MiB and Shader 100 MiB,
 * 1770 MiB in total. The SDK service behind that call is
 * MemoryHeapManager::AllocateFromHeap over the process heap, whose size the
 * SDK derives from GetInfo(TotalMemorySize) - GetInfo(UsedMemorySize). The
 * loader therefore models a process budget large enough to let the original
 * memory framework run, and reports the exact size it backs. This is the
 * Vita3K boot-evidence profile: it deliberately exceeds Vita hardware DRAM
 * and does not claim hardware feasibility (ADR 0012 gate B stays open). If
 * the host kernel cannot back the block, setup fails loudly instead of
 * pretending the arenas exist. The alias and stack regions are additional
 * modeled GetInfo regions placed after the heap.
 *
 * The arenas alone need 0x6EA00000 bytes. The SDK heap manager also keeps
 * roughly 65 MiB of alignment/metadata between them, and the title has
 * 0x6FE00000 bytes of the heap in use when it asks for the 100 MiB Shader
 * arena, i.e. 0x76200000 in total. With the previous 0x70000000 profile the
 * request returned null and the title printed "Memory Pool Initialization
 * Error" and exited. The heap is therefore 0x76F00000; with the 8 MiB alias
 * and 8 MiB stack views the modeled space ends at 0xFFC00000, below the
 * 32-bit ceiling. The alias view also holds the SDK's thread stacks through
 * MapAliasStack, so it is not collapsed further.
 *
 * Vita3K's user memory is exactly 4 GiB (TOTAL_MEM_SIZE) and this pool ends
 * at 0xFFE00000. The loader's guest thread stacks are a separate memblock
 * allocated before the pool; ksceKernelAllocMemBlock reserves
 * size + alignment, where the alignment is raised to size & -size. The
 * 63-thread block is sized so that charge is 8 MiB, the same footprint the
 * original 32-thread block used, so the pool still starts at the same page.
 * Do not round the thread count up to 64 or add another stack memblock: the
 * heap below has no slack for it (the title's memory pools consume it), and
 * the pool allocation starts failing with SCE_KERNEL_ERROR_NO_MEMORY.
 *
 * Stack capacity: the play session issues 35 MapAliasStack copies totalling
 * 9.49 MiB before it reaches the movie, with no unmaps (the backup run issues
 * 9.61 MiB / 20 unmaps). An 8 MiB stack view cannot hold that working set,
 * so the allocator is forced to reuse live ranges - which is exactly how the
 * 0x13e alias stack was overwritten. The pool total stays unchanged: the
 * extra 8 MiB is borrowed from the heap (0x76F00000 -> 0x76700000). The
 * title's heap high-water mark is 0x76200000, leaving 5 MiB of slack; this is
 * a layout experiment that must be checked against the title's pool init. */
#define VP_GUEST_HEAP_BYTES  (0x76700000u)
#define VP_GUEST_ALIAS_BYTES (8u<<20)
#define VP_GUEST_STACK_BYTES (16u<<20)
#define VP_GUEST_POOL_BYTES  (VP_GUEST_HEAP_BYTES+VP_GUEST_ALIAS_BYTES+VP_GUEST_STACK_BYTES)
#define VP_GUEST_LAUNCH_BYTES 0x9000u
static int setup_guest_memory(Session *s,VPExecutor *e) {
    SceKernelAllocMemBlockOpt opt;memset(&opt,0,sizeof(opt));opt.size=sizeof(opt);
    opt.attr=SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;opt.alignment=0x10000;
    s->pool_block=sceKernelAllocMemBlock("vp-guest-pool",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,VP_GUEST_POOL_BYTES,&opt);
    if (s->pool_block<0) {
        fprintf(s->log,"SVC_MEMORY_ALLOC arena=pool bytes=%u status=%d\n",VP_GUEST_POOL_BYTES,s->pool_block);fflush(s->log);
        return s->pool_block;
    }
    s->launch_block=sceKernelAllocMemBlock("vp-launch-args",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,VP_GUEST_LAUNCH_BYTES,&opt);
    if (s->launch_block<0) {
        fprintf(s->log,"SVC_MEMORY_ALLOC arena=launch bytes=%u status=%d\n",VP_GUEST_LAUNCH_BYTES,s->launch_block);fflush(s->log);
        return s->launch_block;
    }
    void *pool=NULL,*launch=NULL;
    int rc=sceKernelGetMemBlockBase(s->pool_block,&pool);if(rc) return rc;
    rc=sceKernelGetMemBlockBase(s->launch_block,&launch);if(rc) return rc;
    s->pool_base=(uint32_t)(uintptr_t)pool;s->launch_address=(uint32_t)(uintptr_t)launch;
    memset(pool,0,VP_GUEST_POOL_BYTES);memset(launch,0,VP_GUEST_LAUNCH_BYTES);
    /* The original nnosInitialize parses this NSO argument block: allocated
     * size, command-line length, 0x18 reserved bytes, then the string. */
    static const char command_line[]="mhgu-vita-port";
    uint32_t header[8]={VP_GUEST_LAUNCH_BYTES,(uint32_t)(sizeof(command_line)-1),0,0,0,0,0,0};
    memcpy(launch,header,sizeof(header));memcpy((char *)launch+32,command_line,sizeof(command_line));
    VPGuestSvc *v=&s->svc;
    v->heap_base=s->pool_base;v->heap_size=VP_GUEST_HEAP_BYTES;
    v->alias_base=s->pool_base+VP_GUEST_HEAP_BYTES;v->alias_size=VP_GUEST_ALIAS_BYTES;
    v->stack_base=v->alias_base+VP_GUEST_ALIAS_BYTES;v->stack_size=VP_GUEST_STACK_BYTES;
    v->aslr_base=v->alias_base;v->aslr_size=v->alias_size;
    v->plr_base=s->launch_address+0x8000;v->plr_size=0x1000;
    v->current_thread_handle=s->start_handle=1;
    /* The audio service answers GetWorkBufferSize by executing the locked
     * SDK's own server-side calculation; resolve its exported symbol. */
    v->guest_call=vp_sdk_guest_call;v->guest_call_user=s;v->audio_work_size_fn=0;
    v->guest_call_args=vp_sdk_guest_call_args;
    v->audio_probe_request=vp_sdk_audio_probe_request;
    v->audio_probe_request_user=s;
    {
        FILE *probe_marker=fopen("ux0:data/mhgu-vita-port/audio-probe.enable","rb");
        s->audio_probe_marker=probe_marker!=NULL;
        if (probe_marker) fclose(probe_marker);
        fprintf(s->log,"AUDIO_PROBE_MARKER present=%d path=ux0:data/mhgu-vita-port/audio-probe.enable\n",
            s->audio_probe_marker);
    }
    int symbol_rc=vp_native_export(&e->modules[3],
        "_ZN2nn5audio6server17AudioRenderSystem17GetWorkBufferSizeERKNS0_6detail30AudioRendererParameterInternalE",
        &v->audio_work_size_fn);
    fprintf(s->log,"SDK_AUDIO_WORK_SIZE_FN address=0x%08x status=%d\n",
        v->audio_work_size_fn,symbol_rc);fflush(s->log);
    /* M5 probe client symbols are resolved once; the probe thread itself is
     * only created after the title opens its renderer. */
    vp_sdk_audio_probe_resolve(s,&e->modules[3]);
    rc=vp_guest_svc_region(v,v->heap_base,v->heap_size,5,3,"guest-heap");
    /* The alias block is also the process's ASLR (alias-code) region: it is
     * the pool range no other guest allocator hands out, so the SDK's
     * address-space allocator can place shared-memory mappings there. It
     * must report Free (state 0) for that search to succeed; QueryMemory
     * reports the pages of an object once it is mapped as Shared. */
    if(!rc) rc=vp_guest_svc_region(v,v->alias_base,v->alias_size,0,3,"guest-alias");
    /* No guest thread stack is placed here yet. The SDK's
     * AddressSpaceAllocator only accepts QueryMemory pages whose state is
     * Free, so an empty stack region must report state 0; permission stays
     * read/write because this loader backs the whole pool. */
    if(!rc) rc=vp_guest_svc_region(v,v->stack_base,v->stack_size,0,3,"guest-stack");
    if(!rc) rc=vp_guest_svc_region(v,s->launch_address,VP_GUEST_LAUNCH_BYTES,5,3,"launch-args");
    if(!rc) rc=vp_guest_svc_region(v,e->stack_base,e->stack_bytes,11,3,"executor-stack");
    /* The modeled TPIDRURO block is also the SDK's IPC command buffer. */
    if(!rc) rc=vp_guest_svc_region(v,e->bridge.guest.tls_ro,0x200,11,3,"executor-tls");
    /* SDK guest threads run on loader-owned pool stacks. Guest code keeps IPC
     * handle arrays and locals on those stacks, so the pool has to be part of
     * the readable/writable memory model exactly like the executor stack. */
    if(!rc && thread_pool.prepared)
        rc=vp_guest_svc_region(v,thread_pool.stack_base,
            VP_GUEST_THREAD_LIMIT*VP_GUEST_THREAD_STACK_BYTES,
            11,3,"guest-thread-stack");
    /* Each worker's TPIDRURO block is also its HIPC command buffer. The
     * per-thread TLS storages live inside the thread array, so expose that
     * array to the same memory model; the kernel reads a worker's request
     * from the worker's own TLS, never from the main executor's block. */
    if(!rc && thread_pool.prepared)
        rc=vp_guest_svc_region(v,(uint32_t)(uintptr_t)thread_pool.threads,
            (uint32_t)sizeof(thread_pool.threads),11,3,"guest-thread-tls");
    for (unsigned i=0;i<4 && !rc;++i) {
        const VPNativeModule *m=&e->modules[i];
        static const uint32_t states[3]={3,2,4},perms[3]={5,1,3};
        static const char *names[3]={"module-text","module-rodata","module-data"};
        for (unsigned j=0;j<3 && !rc;++j) {
            const VPNSOSegment *seg=&m->nso.segments[j];
            uint32_t size=j==2?m->nso.span-seg->offset:seg->bytes;
            if (size) rc=vp_guest_svc_region(v,m->base+seg->offset,size,states[j],perms[j],names[j]);
        }
    }
    v->used_memory=VP_GUEST_LAUNCH_BYTES;
    for (unsigned i=0;i<4;++i) {
        if (v->used_memory>UINT32_MAX-e->modules[i].nso.span) v->used_memory=UINT32_MAX;
        else v->used_memory+=e->modules[i].nso.span;
    }
    /* The SDK derives its heap request as align_down(TotalMemorySize -
     * UsedMemorySize, 1 MiB). total_memory is therefore the modeled process
     * budget (images + backed heap), not the sum of registered regions: if it
     * grew with the heap region, every enlargement would raise the request
     * again instead of converging on the capacity this loader backs. */
    v->total_memory=v->used_memory+(v->used_memory>UINT32_MAX-v->heap_size?0:v->heap_size);
    fprintf(s->log,"SVC_MEMORY profile=title-arena-evidence pool=0x%08x bytes=%u heap=0x%08x+%u alias=0x%08x+%u stack=0x%08x+%u launch=0x%08x+%u executor=0x%08x+%u total=%u used=%u regions=%u\n",
        s->pool_base,VP_GUEST_POOL_BYTES,v->heap_base,v->heap_size,v->alias_base,v->alias_size,
        v->stack_base,v->stack_size,s->launch_address,VP_GUEST_LAUNCH_BYTES,e->stack_base,e->stack_bytes,v->total_memory,v->used_memory,v->region_count);
    fflush(s->log);return rc;
}
int vp_sdk_thread_pool_prepare(VPNativeModule *modules,unsigned count,FILE *log) {
    if (thread_pool.prepared) return 0;
    if (!modules || count!=4 || !log) return VP_EXECUTOR_ARGUMENT;
    int rc=0;
    {
        SceKernelAllocMemBlockOpt opt;memset(&opt,0,sizeof(opt));opt.size=sizeof(opt);
        opt.attr=SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;opt.alignment=0x10000;
        /* 63 stacks: 0x7E0000 bytes, charged with a 0x20000 alignment, i.e.
         * the same 8 MiB reservation as the original 32-thread block. */
        SceUID block=sceKernelAllocMemBlock("vp-guest-thread-stacks",
            SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
            VP_GUEST_THREAD_LIMIT*VP_GUEST_THREAD_STACK_BYTES,&opt);
        if (block<0) {
            fprintf(log,"SDK_THREAD_POOL threads=%u status=%d\n",VP_GUEST_THREAD_LIMIT,block);
            fflush(log);return block;
        }
        void *base=NULL;rc=sceKernelGetMemBlockBase(block,&base);
        if (rc) {
            sceKernelFreeMemBlock(block);
            fprintf(log,"SDK_THREAD_POOL threads=%u status=%d\n",VP_GUEST_THREAD_LIMIT,rc);
            fflush(log);return rc;
        }
        memset(base,0,VP_GUEST_THREAD_LIMIT*VP_GUEST_THREAD_STACK_BYTES);
        thread_pool.stack_block=block;
        thread_pool.stack_base=(uint32_t)(uintptr_t)base;
    }
    for (unsigned i=0;i<VP_GUEST_THREAD_LIMIT;++i) {
        VPGuestThread *t=&thread_pool.threads[i];
        t->state=0;t->host=-1;
        rc=vp_executor_prepare(&t->executor,modules,count,
            (void *)(uintptr_t)(thread_pool.stack_base+i*VP_GUEST_THREAD_STACK_BYTES),
            VP_GUEST_THREAD_STACK_BYTES,
            (uint32_t)(uintptr_t)t->tls_ro_storage,(uint32_t)(uintptr_t)t->tls_rw_storage,
            NULL,NULL);
        if (rc) break;
    }
    if (rc) {
        fprintf(log,"SDK_THREAD_POOL threads=%u status=%d\n",VP_GUEST_THREAD_LIMIT,rc);fflush(log);
        return rc;
    }
    thread_pool.prepared=1;
    fprintf(log,"SDK_THREAD_POOL threads=%u stack_base=0x%08x stack_bytes=%u tls_bytes=%u status=0\n",
        VP_GUEST_THREAD_LIMIT,thread_pool.stack_base,
        VP_GUEST_THREAD_STACK_BYTES,VP_GUEST_THREAD_TLS_BYTES);
    fflush(log);
    return 0;
}
VP_SDK_INTEGER_ONLY
static VPGuestThread *thread_for_executor(VPExecutor *e) {
    if (!thread_pool.prepared || !e) return NULL;
    for (unsigned i=0;i<VP_GUEST_THREAD_LIMIT;++i)
        if (&thread_pool.threads[i].executor==e) return &thread_pool.threads[i];
    return NULL;
}
static VPSdkTlsValues *tls_values_for(Session *s,VPExecutor *e) {
    VPGuestThread *t=thread_for_executor(e);
    return t?&t->tls_values:&s->tls_values;
}
/* Execute a pure original-SDK calculation for the service layer on a spare
 * executor. The caller holds the session lock, so a free pool slot cannot be
 * claimed by svcCreateThread while it is borrowed. The function must not
 * reach a bridge service (the reviewed audio size calculation is arithmetic
 * over the parameter and BehaviorInfo); any fault is reported to the caller. */
static int vp_sdk_guest_call(void *user,uint32_t module,uint32_t address,
                             uint32_t arg,uint32_t *result) {
    Session *s=user;
    if (!s || !result || module>=4) return VP_BRIDGE_ARGUMENT;
    VPGuestThread *t=NULL;
    for (unsigned i=0;i<VP_GUEST_THREAD_LIMIT;++i)
        if (thread_pool.threads[i].state==0) { t=&thread_pool.threads[i];break; }
    if (!t) return VP_EXECUTOR_STATE;
    t->state=1;
    uint32_t args[1]={arg};
    int rc=vp_executor_invoke_args(&t->executor,module,address,args,1,result);
    t->state=0;
    fprintf(s->log,"SDK_GUEST_CALL module=%u address=0x%08x arg=0x%08x result=0x%08x status=%d\n",
        module,address,arg,*result,rc);
    fflush(s->log);
    return rc;
}
/* Multi-argument form for nn::os::CreateThread/StartThread. It uses the same
 * borrowed-executor rule as vp_sdk_guest_call; the created SDK thread starts
 * after the caller releases the session lock and sets up its own TLS. */
static int vp_sdk_guest_call_args(void *user,uint32_t module,uint32_t address,
                                  const uint32_t *args,uint32_t count,uint32_t *result) {
    Session *s=user;
    if (!s || !result || !args || module>=4 || !count || count>24)
        return VP_BRIDGE_ARGUMENT;
    VPGuestThread *t=NULL;
    for (unsigned i=0;i<VP_GUEST_THREAD_LIMIT;++i)
        if (thread_pool.threads[i].state==0) { t=&thread_pool.threads[i];break; }
    if (!t) return VP_EXECUTOR_STATE;
    t->state=1;
    int rc=vp_executor_invoke_args(&t->executor,module,address,args,count,result);
    t->state=0;
    fprintf(s->log,"SDK_GUEST_CALL_ARGS module=%u address=0x%08x count=%u"
        " a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x result=0x%08x status=%d\n",
        module,address,count,args[0],count>1?args[1]:0,count>2?args[2]:0,
        count>3?args[3]:0,*result,rc);
    fflush(s->log);
    return rc;
}
static VPGuestThread *thread_for_handle(VPGuestSvc *svc,uint32_t handle) {    if (!thread_pool.prepared || handle<VP_GUEST_HANDLE_FIRST
        || handle>=VP_GUEST_HANDLE_FIRST+VP_GUEST_HANDLE_LIMIT) return NULL;
    VPGuestHandle *slot=&svc->handles[handle-VP_GUEST_HANDLE_FIRST];
    if (slot->kind!=VP_GUEST_HANDLE_KIND_THREAD || slot->arg>=VP_GUEST_THREAD_LIMIT) return NULL;
    return &thread_pool.threads[slot->arg];
}
static uint32_t alloc_thread_handle(VPGuestSvc *svc,unsigned index) {
    for (unsigned i=0;i<VP_GUEST_HANDLE_LIMIT;++i) if (!svc->handles[i].kind) {
        memset(&svc->handles[i],0,sizeof(svc->handles[i]));
        svc->handles[i].kind=VP_GUEST_HANDLE_KIND_THREAD;svc->handles[i].arg=index;
        memcpy(svc->handles[i].name,"thread",7);
        return VP_GUEST_HANDLE_FIRST+i;
    }
    return 0;
}
static int create_guest_thread(Session *s,VPBridgeFrame *f) {
    if (!thread_pool.prepared) {
        f->regs[0]=0xE0000000u;
        fprintf(s->log,"SVC_CREATE_THREAD_UNSUPPORTED no pool\n");fflush(s->log);
        return 0;
    }
    VPGuestThread *t=NULL;unsigned index=0;
    for (unsigned i=0;i<VP_GUEST_THREAD_LIMIT;++i)
        if (thread_pool.threads[i].state==0) { t=&thread_pool.threads[i];index=i;break; }
    if (!t) {
        f->regs[0]=0xE0000000u;
        fprintf(s->log,"SVC_CREATE_THREAD_UNSUPPORTED pool exhausted threads=%u\n",VP_GUEST_THREAD_LIMIT);
        fflush(s->log);return 0;
    }
    uint32_t handle=alloc_thread_handle(&s->svc,index);
    if (!handle) {
        f->regs[0]=0xE0000000u;
        fprintf(s->log,"SVC_CREATE_THREAD_UNSUPPORTED handle table full limit=%u\n",
            VP_GUEST_HANDLE_LIMIT);
        unsigned live=0,threads=0,sessions=0,events=0,other=0;
        for (unsigned i=0;i<VP_GUEST_HANDLE_LIMIT;++i) {
            const VPGuestHandle *h=&s->svc.handles[i];
            if (!h->kind) continue;
            ++live;
            if (h->kind==VP_GUEST_HANDLE_KIND_THREAD) ++threads;
            else if (h->kind==2u) ++sessions;
            else if (h->kind==3u) ++events;
            else ++other;
        }
        fprintf(s->log,"SVC_HANDLE_TABLE_FULL live=%u threads=%u sessions=%u events=%u other=%u\n",
            live,threads,sessions,events,other);
        for (unsigned i=0;i<VP_GUEST_HANDLE_LIMIT;++i) {
            const VPGuestHandle *h=&s->svc.handles[i];
            if (!h->kind) continue;
            fprintf(s->log,"SVC_HANDLE_LIVE index=%u handle=0x%x kind=%u name=%s arg=%u session=%u\n",
                i,VP_GUEST_HANDLE_FIRST+i,h->kind,h->name,h->arg,h->session);
        }
        fflush(s->log);
        return 0;
    }
    t->state=1;t->handle=handle;t->host=-1;
    if (s->svc.audio_probe_expect_thread) {
        s->svc.audio_probe_expect_thread=0;
        s->svc.audio_probe_thread_handle=handle;
        s->svc.audio_probe_trace_budget=400u;
        fprintf(s->log,"AUDIO_PROBE_THREAD_HANDLE handle=0x%08x index=%u\n",handle,index);
        fflush(s->log);
    }
    t->wait_cancel_requested=0;t->wait_cancellable=0;
    t->entry=f->regs[1];t->arg=f->regs[2];t->type=f->regs[2];
    t->priority=f->regs[0];t->core=f->regs[4];
    /* ThreadType stores the title entry/argument at +0x38/+0x34 and the debug
     * name pointer at +0xe8. Logging them is the only way to relate the SVC
     * entry trampoline to the title's own thread functions. Read-only; the
     * fields are checked before use and never modified. */
    {
        if (thread_probe_enabled<0) {
            FILE *m=fopen("ux0:data/mhgu-vita-port/thread-probe.enable","rb");
            thread_probe_enabled=m!=NULL;
            if (m) fclose(m);
            fprintf(s->log,"THREAD_PROBE_MODE enabled=%d\n",thread_probe_enabled);
        }
        if (thread_probe_enabled) {
            uint32_t user_entry=0,user_arg=0,name_ptr=0;
            char name[33];
            name[0]=0;
            if (vp_guest_svc_readable(&s->svc,t->arg+0x38,4))
                user_entry=*(const uint32_t *)(uintptr_t)(t->arg+0x38);
            if (vp_guest_svc_readable(&s->svc,t->arg+0x34,4))
                user_arg=*(const uint32_t *)(uintptr_t)(t->arg+0x34);
            if (vp_guest_svc_readable(&s->svc,t->arg+0xe8,4))
                name_ptr=*(const uint32_t *)(uintptr_t)(t->arg+0xe8);
            if (name_ptr && vp_guest_svc_readable(&s->svc,name_ptr,1)) {
                for (unsigned i=0;i<32u;++i) {
                    if (!vp_guest_svc_readable(&s->svc,name_ptr+i,1)) break;
                    char c=*(const char *)(uintptr_t)(name_ptr+i);
                    name[i]=c;
                    if (!c) break;
                    name[i]=(c>=32&&c<127)?c:'?';
                    name[i+1]=0;
                }
                name[32]=0;
            }
            fprintf(s->log,"SDK_THREAD_USER handle=0x%08x sdk_entry=0x%08x user_entry=0x%08x user_arg=0x%08x name_ptr=0x%08x name=\"%s\"\n",
                handle,t->entry,user_entry,user_arg,name_ptr,name);
            fflush(s->log);
        }
    }
    t->alias_dst=0;t->alias_size=0;t->entry_slot_word=0;t->alias_valid=0;
    /* CreateThread maps the alias stack immediately before issuing SVC 8, and
     * the SDK writes the entry thunk before that call. Associate the newest
     * unclaimed mapping inside the stack region with this thread. */
    for (unsigned i=s->svc.map_copy_count;i>0;--i) {
        uint32_t adst=s->svc.map_copy_ranges[i-1].dst;
        uint32_t asize=s->svc.map_copy_ranges[i-1].size;
        if (s->svc.map_copy_ranges[i-1].owner) continue;
        if (adst<s->svc.stack_base || asize>(s->svc.stack_base+s->svc.stack_size)-adst) continue;
        s->svc.map_copy_ranges[i-1].owner=index+1;
        t->alias_dst=adst;t->alias_size=asize;t->alias_valid=1;
        if (asize>=16 && vp_guest_svc_readable(&s->svc,adst+asize-16,4))
            t->entry_slot_word=*(const uint32_t *)(uintptr_t)(adst+asize-16);
        break;
    }
    if (t->alias_valid)
        fprintf(s->log,"SDK_THREAD_ALIAS handle=0x%08x index=%u alias=0x%08x+0x%x slot=0x%08x slot_word=0x%08x\n",
            handle,index,t->alias_dst,t->alias_size,t->alias_dst+t->alias_size-16,
            t->entry_slot_word);
    else
        fprintf(s->log,"SDK_THREAD_ALIAS handle=0x%08x index=%u status=none\n",handle,index);
    VPGuestHandle *slot=&s->svc.handles[handle-VP_GUEST_HANDLE_FIRST];
    slot->thread_priority=t->priority;slot->thread_core=(int32_t)t->core;
    slot->thread_affinity=(t->core<4)?(1u<<t->core):0xFu;
    memset(t->tls_ro_storage,0,sizeof(t->tls_ro_storage));
    memset(t->tls_rw_storage,0,sizeof(t->tls_rw_storage));
    memset(&t->tls_values,0,sizeof(t->tls_values));
    f->regs[0]=0;f->regs[1]=handle;
    fprintf(s->log,"SVC_CREATE_THREAD handle=0x%08x index=%u entry=0x%08x arg=0x%08x priority=0x%x core=0x%08x\n",
        handle,index,t->entry,t->arg,t->priority,t->core);fflush(s->log);
    return 0;
}
static int worker_entry(SceSize args,void *argp) {
    (void)args;
    VPGuestThread *t=*(VPGuestThread **)argp;
    Session *s=&session;
    uint32_t user_entry=0,user_arg=0,name_ptr=0;
    char name[33];
    name[0]=0;
    if (vp_guest_svc_readable(&s->svc,t->arg+0x38,4))
        user_entry=*(const uint32_t *)(uintptr_t)(t->arg+0x38);
    if (vp_guest_svc_readable(&s->svc,t->arg+0x34,4))
        user_arg=*(const uint32_t *)(uintptr_t)(t->arg+0x34);
    if (vp_guest_svc_readable(&s->svc,t->arg+0xe8,4))
        name_ptr=*(const uint32_t *)(uintptr_t)(t->arg+0xe8);
    if (name_ptr && vp_guest_svc_readable(&s->svc,name_ptr,1)) {
        for (unsigned i=0;i<32u;++i) {
            if (!vp_guest_svc_readable(&s->svc,name_ptr+i,1)) break;
            char c=*(const char *)(uintptr_t)(name_ptr+i);
            if (!c) break;
            name[i]=(c>=32&&c<127)?c:'?';
            name[i+1]=0;
        }
        name[32]=0;
    }
    /* The movie controller must be known even when the verbose thread probe
     * is off: the native presentation uses it to request the title's own stop
     * command. These reads are bounds-checked and never modify guest state. */
    if (!strcmp(name,"uMovie::PlayerThread")
        && vp_guest_svc_readable(&s->svc,user_arg,4)) {
        vp_movie_umovie_thread=user_arg;
        if (vp_guest_svc_readable(&s->svc,user_arg+0x158,4))
            vp_movie_umovie_state=*(const uint32_t *)(uintptr_t)(user_arg+0x158);
        if (thread_probe_enabled>0) {
            uint32_t vt=*(const uint32_t *)(uintptr_t)user_arg;
            uint32_t m18=0,m1c=0,m20=0;
            if (vt && vp_guest_svc_readable(&s->svc,vt+0x18,4))
                m18=*(const uint32_t *)(uintptr_t)(vt+0x18);
            if (vt && vp_guest_svc_readable(&s->svc,vt+0x1c,4))
                m1c=*(const uint32_t *)(uintptr_t)(vt+0x1c);
            if (vt && vp_guest_svc_readable(&s->svc,vt+0x20,4))
                m20=*(const uint32_t *)(uintptr_t)(vt+0x20);
            fprintf(s->log,"SDK_WORKER_VTABLE handle=0x%08x object=0x%08x vtable=0x%08x m18=0x%08x m1c=0x%08x m20=0x%08x\n",
                t->handle,user_arg,vt,m18,m1c,m20);
        }
        fprintf(s->log,"VP_MOVIE_CONTROL thread=0x%08x state=0x%08x\n",
            vp_movie_umovie_thread,vp_movie_umovie_state);
    }
    if (thread_probe_enabled>0) {
        fprintf(s->log,"SDK_WORKER_USER handle=0x%08x user_entry=0x%08x user_arg=0x%08x name_ptr=0x%08x name=\"%s\"\n",
            t->handle,user_entry,user_arg,name_ptr,name);
    }
    if (thread_probe_enabled>0) fflush(s->log);

    if (t->alias_valid) {
        uint32_t slot=t->alias_dst+t->alias_size-16;
        uint32_t now=t->entry_slot_word;
        if (vp_guest_svc_readable(&s->svc,slot,4))
            now=*(const uint32_t *)(uintptr_t)slot;
        fprintf(s->log,"SDK_WORKER_ENTER index=%u handle=0x%08x slot=0x%08x word=0x%08x expected=0x%08x\n",
            (unsigned)(t-thread_pool.threads),t->handle,slot,now,t->entry_slot_word);
        if (t->entry_slot_word && (now==0 || now!=t->entry_slot_word)) {
            fprintf(s->log,"SDK_THREAD_ENTRY_INVALID index=%u handle=0x%08x slot=0x%08x expected=0x%08x actual=0x%08x action=skip\n",
                (unsigned)(t-thread_pool.threads),t->handle,slot,t->entry_slot_word,now);
            fflush(s->log);
            state_lock(s);
            t->state=3;
            state_unlock(s);
            return 0;
        }
    }
    uint32_t call[4]={t->arg,0,0,0},result=0,module=3;
    for (unsigned i=0;i<4;++i) {
        VPNativeModule *m=&t->executor.modules[i];
        if (t->entry>=m->base && t->entry-m->base<m->nso.segments[0].bytes) {module=i;break;}
    }
    int rc=vp_executor_invoke(&t->executor,module,t->entry,call,&result);
    state_lock(s);
    t->state=3;
    if (rc==VP_BRIDGE_THREAD_EXIT) {
        fprintf(s->log,"SDK_THREAD_EXIT index=%u handle=0x%08x\n",(unsigned)(t-thread_pool.threads),t->handle);
    } else {
        VPBridgeFrame *fault=&t->executor.fault_frame;
        fprintf(s->log,"SDK_THREAD_STOP index=%u handle=0x%08x entry=0x%08x status=%d service=0x%x pc=0x%08x lr=0x%08x sp=0x%08x\n",
            (unsigned)(t-thread_pool.threads),t->handle,t->entry,rc,fault->service,fault->pc,fault->lr,fault->sp);
        if (rc) for (unsigned i=0;i<13;++i) fprintf(s->log,"SDK_THREAD_REG r%u=0x%08x\n",i,fault->regs[i]);
    }
    fflush(s->log);
    state_unlock(s);
    return 0;
}
static int vita_cpu_mask(int32_t core) {
    if (core<0 || core>3) return 0;
    return core==3?SCE_KERNEL_CPU_MASK_SYSTEM:(SCE_KERNEL_CPU_MASK_USER_0<<core);
}
static int start_guest_thread(Session *s,VPBridgeFrame *f) {
    uint32_t handle=f->regs[1];
    VPGuestThread *t=thread_for_handle(&s->svc,handle);
    if (!t && f->regs[0]) { handle=f->regs[0];t=thread_for_handle(&s->svc,handle); }
    if (!t) {
        f->regs[0]=0xE0010000u;
        fprintf(s->log,"SVC_START_THREAD_UNSUPPORTED r0=0x%08x r1=0x%08x\n",f->regs[0],f->regs[1]);
        fflush(s->log);return 0;
    }
    if (t->state!=1) {
        f->regs[0]=0xE0010000u;
        fprintf(s->log,"SVC_START_THREAD_STATE handle=0x%08x state=%d\n",handle,t->state);
        fflush(s->log);return 0;
    }
    if (t->alias_valid) {
        uint32_t slot=t->alias_dst+t->alias_size-16;
        uint32_t now=t->entry_slot_word;
        if (vp_guest_svc_readable(&s->svc,slot,4))
            now=*(const uint32_t *)(uintptr_t)slot;
        if (t->entry_slot_word && now!=t->entry_slot_word) {
            f->regs[0]=0xE0010000u;
            fprintf(s->log,"SDK_THREAD_ENTRY_CLOBBERED handle=0x%08x index=%u alias=0x%08x slot=0x%08x expected=0x%08x actual=0x%08x\n",
                handle,(unsigned)(t-thread_pool.threads),t->alias_dst,slot,
                t->entry_slot_word,now);
            fflush(s->log);return 0;
        }
        fprintf(s->log,"SDK_THREAD_ENTRY_CHECK handle=0x%08x index=%u slot=0x%08x word=0x%08x status=%s\n",
            handle,(unsigned)(t-thread_pool.threads),slot,now,
            t->entry_slot_word?(now==t->entry_slot_word?"ok":"changed"):"unverified");
    }
    t->host=sceKernelCreateThread("vp-guest-thread",worker_entry,0x10000100,0x8000,0,
        SCE_KERNEL_CPU_MASK_USER_ALL,NULL);
    if (t->host<0) {
        f->regs[0]=(uint32_t)t->host;
        fprintf(s->log,"SVC_START_THREAD_CREATE handle=0x%08x status=%d\n",handle,(int)t->host);
        fflush(s->log);return 0;
    }
    VPGuestHandle *slot=&s->svc.handles[handle-VP_GUEST_HANDLE_FIRST];
    int core=slot->thread_core,affinity=vita_cpu_mask(core);
    int affinity_rc=affinity?(int)sceKernelChangeThreadCpuAffinityMask(t->host,affinity):0;
    fprintf(s->log,"SVC_THREAD_AFFINITY handle=0x%08x core=%d mask=0x%llx host=0x%x status=%d\n",
        handle,core,(unsigned long long)slot->thread_affinity,affinity,affinity_rc);
    t->state=2;
    int rc=sceKernelStartThread(t->host,sizeof(t),&t);
    f->regs[0]=rc<0?(uint32_t)rc:0;
    fprintf(s->log,"SVC_START_THREAD handle=0x%08x host=%d entry=0x%08x status=%d\n",
        handle,(int)t->host,t->entry,rc);fflush(s->log);
    return 0;
}
static int exit_guest_thread(Session *s,VPExecutor *e,VPBridgeFrame *f) {
    (void)f;
    VPGuestThread *t=thread_for_executor(e);
    if (!t) {
        fprintf(s->log,"SVC_EXIT_THREAD_UNSUPPORTED non-worker thread\n");fflush(s->log);
        return VP_BRIDGE_UNSUPPORTED;
    }
    fprintf(s->log,"SVC_EXIT_THREAD index=%u handle=0x%08x\n",
        (unsigned)(t-thread_pool.threads),t->handle);fflush(s->log);
    return VP_BRIDGE_THREAD_EXIT;
}
static int sleep_guest_thread(Session *s,VPBridgeFrame *f) {
    uint64_t ns=(uint64_t)f->regs[0]|((uint64_t)f->regs[1]<<32);
    uint64_t us=ns/1000;
    if (us>10000) us=10000;
    static unsigned logged;
    if (logged<8) {
        fprintf(s->log,"SVC_SLEEP ns=0x%llx delay_us=%llu\n",(unsigned long long)ns,(unsigned long long)us);
        fflush(s->log);++logged;
    }
    VPPark sleep_park;
    park_begin(s,&sleep_park);
    sceKernelDelayThread(us?(unsigned)us:0);
    park_end(s,&sleep_park);
    f->regs[0]=0;
    return 0;
}
static Lock *find_lock(Session *s,uint32_t address) {
    unsigned i=sdk_lock_index(s,address);
    if (i<VP_SDK_LOCK_LIMIT && s->locks[i].address==address) return &s->locks[i];
    return NULL;
}
static int wait_guest_key(Session *s,VPBridgeFrame *f) {
    uint32_t address=f->regs[0],key=f->regs[1]&~3u,tag=f->regs[2];
    uint64_t ns=(uint64_t)f->regs[3]|((uint64_t)f->regs[4]<<32);
    if ((address&3)||!vp_guest_svc_writable(&s->svc,address,4)
        || !vp_guest_svc_writable(&s->svc,key,4)) {
        f->regs[0]=0xD401u;
        fprintf(s->log,"SVC_KEY_WAIT_REJECT address=0x%08x key=0x%08x\n",address,key);
        fflush(s->log);return 0;
    }
    unsigned i=0;
    while (i<VP_KEY_WAIT_LIMIT && s->key_waits[i].used) ++i;
    if (i==VP_KEY_WAIT_LIMIT) {
        fprintf(s->log,"SVC_KEY_WAIT_REJECT table=%u live=%u\n",VP_KEY_WAIT_LIMIT,s->key_waits_live);
        fflush(s->log);return VP_BRIDGE_UNSUPPORTED;
    }
    KeyWait *w=&s->key_waits[i];
    w->event=sceKernelCreateEventFlag("vp-guest-key",0,0,NULL);
    if (w->event<0) {
        f->regs[0]=(uint32_t)w->event;
        fprintf(s->log,"SVC_KEY_WAIT_CREATE status=%d\n",(int)w->event);fflush(s->log);return 0;
    }
    w->key=key;w->address=address;w->tag=tag;w->used=1;w->signaled=0;++s->key_waits_live;
    /* svcWaitProcessWideKeyAtomic releases the arbitrated critical section
     * registered on [address] as part of the wait. Enter/Leave are native
     * host-mutex services here, so drop this thread's recursion depth before
     * parking; the waiter is registered first so a signal cannot be missed. */
    Lock *lock=find_lock(s,address);
    uint32_t owner=s->svc.current_thread_handle;
    unsigned released=0;
    /* The wait releases this thread's critical-section ownership and the
     * function still has to reacquire it afterwards, so the slot must not be
     * recycled while a reference to it is held here. */
    Lock *pinned=NULL;
    if (lock && lock->depth && lock->owner==owner) {
        pinned=lock;++pinned->pending;
        int urc=sceKernelUnlockMutex(lock->id,1);
        if (urc) {
            fprintf(s->log,"SVC_KEY_WAIT_UNLOCK_FAIL address=0x%08x depth=%u status=0x%08x\n",
                address,lock->depth,(uint32_t)urc);
            fflush(s->log);
            sceKernelDeleteEventFlag(w->event);
            w->used=0;w->signaled=0;--s->key_waits_live;
            --pinned->pending;
            return urc;
        }
        released=lock->depth;
        lock->depth=0;lock->host_depth=0;lock->held=0;lock->owner=0;
        lock->held_since_us=0;
    } else if (lock && lock->depth) {
        fprintf(s->log,"SVC_KEY_WAIT_OWNER address=0x%08x owner=0x%08x thread=0x%08x depth=%u model=host-mutex\n",
            address,lock->owner,owner,lock->depth);
        fflush(s->log);
    }
    /* The Horizon kernel writes the next owner tag (zero when the chain is
     * empty) before blocking. This platform has no tag handoff chain. */
    /* Registration, release and the key flag are serialized with Signal by
     * Session. Only the event of this registered waiter may retain a wake;
     * a signal before registration grants no credit to a future waiter. */
    uint32_t has_waiter=1,next=0;
    memcpy((void *)(uintptr_t)key,&has_waiter,4);
    memcpy((void *)(uintptr_t)address,&next,4);
    int rc=0;
    SceUInt us=0,*timeout=NULL;
    if ((int64_t)ns>0) {
        uint64_t v=ns/1000u;if (!v) v=1;if (v>0xFFFFFFFFu) v=0xFFFFFFFFu;
        us=(SceUInt)v;timeout=&us;
    } else if (ns==0) {
        rc=SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    }
    if (ns!=0) {
        /* A blocked key wait must not hold the session lock: the signalling
         * thread needs it to reach svcSignalProcessWideKey. */
        if ((int64_t)ns<0) {
            /* Infinite waits are the SDK's condition-variable parking state.
             * Record the key, tag, owner and call site once so a thread that
             * parks forever can be matched to the object it is waiting on. */
            fprintf(s->log,"SVC_KEY_WAIT_BEGIN address=0x%08x key=0x%08x tag=0x%08x thread=0x%08x lr=0x%08x pc=0x%08x sp=0x%08x model=infinite\n",
                address,key,tag,s->svc.current_thread_handle,f->lr,f->pc,f->sp);
            fputs("SVC_KEY_WAIT_BEGIN_STACK",s->log);
            for (unsigned d=0;d<24;++d) {
                uint32_t at=f->sp+d*4;
                if (!vp_guest_svc_readable(&s->svc,at,4)) break;
                uint32_t v; memcpy(&v,(void *)(uintptr_t)at,4);
                fprintf(s->log," %08x",v);
            }
            fputc('\n',s->log);
            fflush(s->log);
        }
        VPPark key_park;
        unsigned bits=0;
        uint64_t wait_t0=svc_arrival_enabled()?sceKernelGetProcessTimeWide():0u;
        park_begin(s,&key_park);
        rc=sceKernelWaitEventFlag(w->event,1,SCE_EVENT_WAITOR|SCE_EVENT_WAITCLEAR,&bits,timeout);
        uint64_t wait_t1=wait_t0?sceKernelGetProcessTimeWide():0u;
        park_end(s,&key_park);
        /* B124: the parked time itself, separate from the dispatch duration
         * that also carries service work and critical-section reacquisition. */
        if (wait_t0 && wait_t1>wait_t0 && wait_t1-wait_t0>=20000u
            && g_svc_key_lines<4000u) {
            ++g_svc_key_lines;
            fprintf(s->log,"VP_KEY_WAIT_TIME key=0x%08x thread=0x%x wait_us=%llu "
                "rc=0x%08x caller=0x%08x t_us=%llu\n",
                key,s->svc.current_thread_handle,
                (unsigned long long)(wait_t1-wait_t0),(uint32_t)rc,f->lr,
                (unsigned long long)wait_t1);
            fflush(s->log);
        }
        if (rc==(int)SCE_KERNEL_ERROR_WAIT_TIMEOUT && w->signaled) rc=0;
    }
    /* A successful wait returns with the critical section owned again; the
     * SDK does not re-enter on that path. A timeout leaves it released
     * because TimedWait explicitly re-enters (0xa3180 in the locked SDK). */
    int reacquire_error=0;
    unsigned reacquired=0;
    if (!rc && released) {
        unsigned waited=0;
        int arc=sdk_lock_acquire(s,lock,&waited);
        if (!arc) {
            lock->depth=released;lock->host_depth=1;lock->held=1;lock->owner=owner;
            lock->held_since_us=svc_arrival_enabled()?sceKernelGetProcessTimeWide():0u;
            reacquired=released;
        } else {
            reacquire_error=arc;
        }
        if (reacquired!=released)
            fprintf(s->log,"SVC_KEY_WAIT_RELOCK address=0x%08x released=%u reacquired=%u depth=%u status=0x%08x\n",
                address,released,reacquired,lock->depth,(uint32_t)reacquire_error);
    }
    uint32_t result=0;
    if (!rc) { ++s->key_wakes; }
    else if (rc==(int)SCE_KERNEL_ERROR_WAIT_TIMEOUT) { result=0xEA01u;++s->key_timeouts; }
    else result=(uint32_t)rc;
    sceKernelDeleteEventFlag(w->event);
    w->used=0;w->signaled=0;--s->key_waits_live;
    /* The SVC result is a return value, not an in/out register: the SDK's
     * InternalConditionVariableImplByHorizon::Wait calls
     * nn::diag::detail::AbortImpl whenever r0 is non-zero after the wait.
     * Writing only the error case left r0 holding the critical-section
     * address on success, which aborted every thread that woke up. */
    f->regs[0]=result;
    fprintf(s->log,"SVC_KEY_WAIT address=0x%08x key=0x%08x tag=0x%08x timeout=0x%llx released=%u result=0x%08x model=event-flag live=%u caller=0x%08x pc=0x%08x\n",
        address,key,tag,(unsigned long long)ns,released,result,s->key_waits_live,
        f->lr,f->pc);
    /* Temporary diagnosis: the repeated timed waits after a library-applet
     * launch identify the SDK condition-variable loop that keeps the main
     * thread from reaching the next IPC. Dump the waiter's stack so the
     * caller chain can be resolved offline. */
    if (s->key_timeouts % 1500 == 0 && s->key_timeouts <= 9000) {
        fprintf(s->log,"SVC_KEY_WAIT_STACK timeouts=%u sp=0x%08x",
            s->key_timeouts,f->sp);
        for (unsigned i=0;i<64;++i) {
            uint32_t at=f->sp+i*4;
            if (!vp_guest_svc_readable(&s->svc,at,4)) break;
            uint32_t v; memcpy(&v,(void *)(uintptr_t)at,4);
            fprintf(s->log," %08x",v);
        }
        fputc('\n',s->log);
    }
    fflush(s->log);
    if (pinned) --pinned->pending;
    if (reacquire_error) return reacquire_error;
    return rc<0 && rc!=(int)SCE_KERNEL_ERROR_WAIT_TIMEOUT ? rc : 0;
}
static int signal_guest_key(Session *s,VPBridgeFrame *f) {
    uint32_t key=f->regs[0]&~3u;int32_t count=(int32_t)f->regs[1];
    if (!vp_guest_svc_writable(&s->svc,key,4)) {
        f->regs[0]=0xD401u;
        fprintf(s->log,"SVC_KEY_SIGNAL_REJECT key=0x%08x\n",key);
        fflush(s->log);return 0;
    }
    unsigned woke=0;
    int rc=0;
    for (unsigned i=0;i<VP_KEY_WAIT_LIMIT && (count<=0 || woke<(unsigned)count);++i) {
        KeyWait *w=&s->key_waits[i];
        if (!w->used || w->signaled || w->key!=key) continue;
        rc=sceKernelSetEventFlag(w->event,1);
        if (rc) break;
        w->signaled=1;
        ++woke;
    }
    ++s->key_signals;
    uint32_t has_waiter=0;
    for (unsigned i=0;i<VP_KEY_WAIT_LIMIT;++i) {
        const KeyWait *w=&s->key_waits[i];
        if (w->used && !w->signaled && w->key==key) { has_waiter=1;break; }
    }
    memcpy((void *)(uintptr_t)key,&has_waiter,4);
    f->regs[0]=(uint32_t)rc;
    fprintf(s->log,"SVC_KEY_SIGNAL key=0x%08x count=%d woke=%u live=%u thread=0x%08x caller=0x%08x status=0x%08x\n",
        key,(int)count,woke,s->key_waits_live,s->svc.current_thread_handle,f->lr,(uint32_t)rc);
    fflush(s->log);
    return rc;
}
/* Diagnostic input-path probe. The marker-gated islands below enter this
 * service immediately after the title's per-style pad readers call
 * GetNpadState/GetNpadStates. The handler only reads the caller's buffer and
 * the npad-id pointer; it returns 0, so the reader continues with exactly the
 * state the SDK produced. kind 0 = FullKey reader, kind 1 = Handheld reader. */
static int hid_helper_state_probe(Session *s,VPBridgeFrame *f,unsigned kind) {
    static unsigned calls[2],logged[2];
    static uint32_t last_id[2],last_conn[2],last_ctx[2];
    static uint64_t last_buttons[2];
    static int32_t last_axes[2][4];
    static int have[2];
    uint32_t id_pointer=f->regs[5],buffer=f->regs[7],context=f->regs[6];
    uint32_t npad=0xffffffffu,conn=0xffffffffu,ctx_flags=0xffffffffu;
    uint64_t buttons=~0ull;
    int32_t axes[4]={0,0,0,0};
    int changed;
    if (kind>1u) kind=0u;
    if (vp_guest_svc_readable(&s->svc,context+4,1))
        ctx_flags=*(uint8_t *)(uintptr_t)(context+4);
    if (vp_guest_svc_readable(&s->svc,id_pointer,4))
        npad=*(uint32_t *)(uintptr_t)id_pointer;
    if (vp_guest_svc_readable(&s->svc,buffer,0x28)) {
        conn=*(uint32_t *)(uintptr_t)(buffer+0x20);
        memcpy(&buttons,(void *)(uintptr_t)(buffer+0x08),8);
        axes[0]=*(int32_t *)(uintptr_t)(buffer+0x10);
        axes[1]=*(int32_t *)(uintptr_t)(buffer+0x14);
        axes[2]=*(int32_t *)(uintptr_t)(buffer+0x18);
        axes[3]=*(int32_t *)(uintptr_t)(buffer+0x1c);
    }
    ++calls[kind];
    changed=!have[kind] || npad!=last_id[kind] || conn!=last_conn[kind]
        || ctx_flags!=last_ctx[kind]
        || buttons!=last_buttons[kind] || memcmp(axes,last_axes[kind],sizeof(axes))!=0;
    if (calls[kind]<=4u) {
        fprintf(s->log,"HID_HELPER_FRAME kind=%s call=%u sp=0x%08x pc=0x%08x lr=0x%08x "
            "regs=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
            kind?"handheld":"fullkey",calls[kind],f->sp,f->pc,f->lr,
            f->regs[0],f->regs[1],f->regs[2],f->regs[3],f->regs[4],f->regs[5],
            f->regs[6],f->regs[7],f->regs[8],f->regs[9],f->regs[10],f->regs[11],
            f->regs[12]);
        fflush(s->log);
    }
    if ((logged[kind]<40u && (logged[kind]<10u || changed)) || (calls[kind]%4096u)==0u) {
        if (logged[kind]<40u) ++logged[kind];
        have[kind]=1;last_id[kind]=npad;last_conn[kind]=conn;last_buttons[kind]=buttons;
        last_ctx[kind]=ctx_flags;
        memcpy(last_axes[kind],axes,sizeof(axes));
        fprintf(s->log,"HID_HELPER_STATE kind=%s call=%u id=0x%08x conn=0x%08x "
            "buttons=0x%016llx axes=%d,%d,%d,%d ctx=0x%08x ctx4=0x%02x buffer=0x%08x\n",
            kind?"handheld":"fullkey",calls[kind],npad,conn,
            (unsigned long long)buttons,axes[0],axes[1],axes[2],axes[3],context,ctx_flags,buffer);
        fflush(s->log);
    }
    return 0;
}
/* Kernel SVC result codes (module 1 = Kernel): 114 InvalidHandle,
 * 117 TimedOut, 118 Cancelled, matching the locked kernel ABI. */
#define VP_SVC_RESULT_INVALID_HANDLE 0xE401u
#define VP_SVC_RESULT_TIMED_OUT 0xEA01u
#define VP_SVC_RESULT_CANCELLED 0xEC01u

/* svcWaitSynchronization with an empty handle list is the SDK's cancellable
 * sleep: the kernel parks the thread until svcCancelSynchronization names its
 * thread handle or the timeout expires. The movie player's shutdown path uses
 * exactly this shape to join its decoder threads, so it must park (with the
 * session lock dropped) instead of being rejected as an unsupported count. */
static int wait_cancellable(Session *s, VPExecutor *e, VPBridgeFrame *f,
                            uint64_t timeout) {
    VPGuestThread *t = thread_for_executor(e);
    if (!t) return VP_NOT_HANDLED; /* main thread has no kernel thread object */
    if (timeout == 0) {
        f->regs[0] = VP_SVC_RESULT_TIMED_OUT;
        f->regs[1] = 0;
        fprintf(s->log, "SVC_WAIT_EMPTY thread=0x%08x timeout=0 result=0x%08x\n",
            s->svc.current_thread_handle, f->regs[0]);
        fflush(s->log);
        return 0;
    }
    if (t->wait_cancel_requested) {
        t->wait_cancel_requested = 0;
        f->regs[0] = VP_SVC_RESULT_CANCELLED;
        f->regs[1] = 0;
        fprintf(s->log, "SVC_WAIT_EMPTY thread=0x%08x timeout=0x%llx result=0x%08x model=pending-cancel\n",
            s->svc.current_thread_handle, (unsigned long long)timeout, f->regs[0]);
        fflush(s->log);
        return 0;
    }
    t->wait_cancellable = 1;
    uint64_t start_us = sceKernelGetProcessTimeWide();
    unsigned waited_us = 0;
    uint32_t result = 0;
    VPPark cancel_park;
    for (;;) {
        if (t->wait_cancel_requested || s->stopping) {
            result = VP_SVC_RESULT_CANCELLED;
            if (s->stopping) t->wait_cancel_requested = 0;
            break;
        }
        if (timeout != UINT64_MAX) {
            uint64_t elapsed_us = (sceKernelGetProcessTimeWide() - start_us);
            if (elapsed_us * 1000ull >= timeout) { result = VP_SVC_RESULT_TIMED_OUT; break; }
        }
        park_begin(s,&cancel_park);
        sceKernelDelayThread(1000);
        park_end(s,&cancel_park);
        waited_us += 1000;
    }
    t->wait_cancellable = 0;
    if (result == VP_SVC_RESULT_CANCELLED) t->wait_cancel_requested = 0;
    f->regs[0] = result;
    f->regs[1] = 0;
    fprintf(s->log, "SVC_WAIT_EMPTY thread=0x%08x handle=0x%08x timeout=0x%llx waited_us=%u result=0x%08x model=cancellable-sleep\n",
        s->svc.current_thread_handle, t->handle, (unsigned long long)timeout,
        waited_us, result);
    fflush(s->log);
    return 0;
}

/* svcCancelSynchronization(thread_handle): mark the target thread's wait as
 * cancelled. The reference kernel keeps the request pending when the thread
 * is not yet parked, and the next cancellable wait returns ResultCancelled;
 * a parked thread observes the flag while its wait loop holds the session
 * lock. The handle names a thread object, not the waited-on object. */
static int cancel_synchronization(Session *s, VPBridgeFrame *f) {
    uint32_t handle = f->regs[0];
    VPGuestThread *t = thread_for_handle(&s->svc, handle);
    if (!t) {
        f->regs[0] = VP_SVC_RESULT_INVALID_HANDLE;
        fprintf(s->log, "SVC_CANCEL_SYNC handle=0x%08x result=0x%08x\n",
            handle, f->regs[0]);
        fflush(s->log);
        return 0;
    }
    t->wait_cancel_requested = 1;
    f->regs[0] = 0;
    fprintf(s->log, "SVC_CANCEL_SYNC handle=0x%08x state=%d waiting=%u pending=1 result=0 model=thread-wait-cancel\n",
        handle, t->state, t->wait_cancellable ? 1u : 0u);
    fflush(s->log);
    return 0;
}

static int thread_service(Session *s,VPExecutor *e,VPBridgeFrame *f) {
    uint32_t id=f->service-VP_GUEST_SVC_BASE;
    if (id==0x7d) return hid_helper_state_probe(s,f,0u);
    if (id==0x7c) return hid_helper_state_probe(s,f,1u);
    if (id==0x08) return create_guest_thread(s,f);
    if (id==0x09) return start_guest_thread(s,f);
    if (id==0x0a) return exit_guest_thread(s,e,f);
    if (id==0x0b) return sleep_guest_thread(s,f);
    if (id==0x1c) return wait_guest_key(s,f);
    if (id==0x1d) return signal_guest_key(s,f);
    if (id==0x19) return cancel_synchronization(s,f);
    if (id==0x18) {
        uint32_t handles=f->regs[1],count=f->regs[2];
        uint64_t timeout=(uint64_t)f->regs[0]|((uint64_t)f->regs[3]<<32);
        if (count==0) return wait_cancellable(s,e,f,timeout);
        if (count==1 && vp_guest_svc_readable(&s->svc,handles,4)) {
            uint32_t handle=*(uint32_t *)(uintptr_t)handles;
            VPGuestThread *t=thread_for_handle(&s->svc,handle);
            if (t) {
                uint64_t waited=0;
                VPPark thread_park;
                VPGuestThread *self=thread_for_executor(e);
                while (t->state!=3 && !s->stopping && waited<10000000
                    && !(self && self->wait_cancel_requested)
                    && (timeout==UINT64_MAX || waited<timeout/1000)) {
                    park_begin(s,&thread_park);
                    sceKernelDelayThread(1000);
                    park_end(s,&thread_park);
                    waited+=1000;
                }
                if (self && self->wait_cancel_requested) {
                    self->wait_cancel_requested=0;
                    f->regs[0]=VP_SVC_RESULT_CANCELLED;f->regs[1]=0;
                } else {
                    f->regs[0]=t->state==3?0:0xEA01u;f->regs[1]=0;
                }
                fprintf(s->log,"SVC_WAIT_THREAD handle=0x%08x state=%d waited_us=%llu timeout=0x%llx result=0x%08x\n",
                    *(uint32_t *)(uintptr_t)handles,t->state,(unsigned long long)waited,
                    (unsigned long long)timeout,f->regs[0]);
                fflush(s->log);return 0;
            }
            /* Event handles use the same reviewed WaitSynchronizationN ABI.
             * Unlike a synthesized signal, this waits on the host clock while
             * the session lock is dropped, so a signaller on another guest
             * thread can reach the signal service. An infinite wait is the
             * thread's real parked state; it ends when the process cleanup
             * deletes the host thread. */
            int signaled=0;
            if (handle==0x118u && !s->wait118_dumped) {
                s->wait118_dumped=1;
                fprintf(s->log,"SVC_WAIT118 sp=0x%08x pc=0x%08x lr=0x%08x thread=0x%08x\n",
                    f->sp,f->pc,f->lr,s->svc.current_thread_handle);
                for (unsigned i=0;i<0x100;++i) {
                    uint32_t a=f->sp+i*4u;
                    if (!vp_guest_svc_readable(&s->svc,a,4)) break;
                    fprintf(s->log,"SVC_WAIT118_STACK +0x%03x=0x%08x\n",i*4u,*(uint32_t *)(uintptr_t)a);
                }
                uint32_t fp=f->regs[11];
                for (unsigned i=0;i<16 && fp;++i) {
                    if (!vp_guest_svc_readable(&s->svc,fp,8)) break;
                    uint32_t next=*(uint32_t *)(uintptr_t)fp;
                    uint32_t lr=*(uint32_t *)(uintptr_t)(fp+4);
                    fprintf(s->log,"SVC_WAIT118_FRAME %u fp=0x%08x next=0x%08x lr=0x%08x\n",i,fp,next,lr);
                    if (next<=fp) break;
                    fp=next;
                }
                /* The polling wait function's object (sdk 0x322874) is reached
                 * through the buffer-queue path. Its saved registers cannot be
                 * read from the stale return address on this stack, so scan the
                 * live stack for the initialized slot: state==2, small index,
                 * and the target fence value the client incremented. */
                unsigned found=0;
                for (unsigned i=0;i<0x800 && found<4;++i) {
                    uint32_t a=f->sp+i*4u;
                    if (!vp_guest_svc_readable(&s->svc,a,4)) break;
                    uint32_t p=*(uint32_t *)(uintptr_t)a;
                    if (p<0x80000000u || (p&3u) || !vp_guest_svc_readable(&s->svc,p+0x1534u,8)) continue;
                    uint32_t q=*(uint32_t *)(uintptr_t)(p+0x1538u);
                    if (!vp_guest_svc_readable(&s->svc,q,0x60)) continue;
                    uint32_t vt=*(uint32_t *)(uintptr_t)q;
                    if (!vp_guest_svc_readable(&s->svc,vt+0x18u,4)) continue;
                    uint32_t fn=*(uint32_t *)(uintptr_t)(vt+0x14u);
                    uint32_t base=*(uint32_t *)(uintptr_t)(q+0x58u);
                    if (!vp_guest_svc_readable(&s->svc,base,0x40)) continue;
                    fprintf(s->log,"SVC_WAIT118_PROVIDER at=0x%08x provider=0x%08x q1538=0x%08x vtable=0x%08x fn=0x%08x base=0x%08x counters=%08x %08x %08x %08x\n",
                        a,p,q,vt,fn,base,
                        *(uint32_t *)(uintptr_t)base,
                        *(uint32_t *)(uintptr_t)(base+0x10u),
                        *(uint32_t *)(uintptr_t)(base+0x20u),
                        *(uint32_t *)(uintptr_t)(base+0x30u));
                    ++found;
                }
                if (!found) fprintf(s->log,"SVC_WAIT118_PROVIDER none\n");
                fflush(s->log);
            }
            if (vp_guest_svc_event_state(&s->svc,handle,&signaled)) {
                uint64_t timeout=(uint64_t)f->regs[0]|((uint64_t)f->regs[3]<<32);
                uint64_t waited=0;
                VPGuestThread *self=thread_for_executor(e);
                /* Record only waits that actually park: an already-signaled
                 * wait is routine (the audio and applet events are checked
                 * every frame), while a blocking wait identifies the event and
                 * the guest path that owns it. */
                if (!signaled) {
                    fprintf(s->log,"SVC_WAIT_EVENT_BEGIN handle=0x%08x signaled=%d timeout=0x%llx thread=0x%08x pc=0x%08x lr=0x%08x sp=0x%08x\n",
                        handle,signaled,(unsigned long long)timeout,s->svc.current_thread_handle,
                        f->pc,f->lr,f->sp);
                    fflush(s->log);
                }
                VPPark event_park;
                while (!signaled && timeout!=0
                    && !(self && self->wait_cancel_requested)
                    && (timeout==UINT64_MAX || waited<timeout/1000)) {
                    park_begin(s,&event_park);
                    sceKernelDelayThread(1000);
                    park_end(s,&event_park);
                    waited+=1000;
                    vp_guest_svc_event_state(&s->svc,handle,&signaled);
                }
                if (signaled) {
                    /* Horizon events created through svcCreateEvent (KEvent)
                     * are manual-clear: a successful wait leaves the signal
                     * set until svcResetSignal. The SDK relies on this for the
                     * library-applet launchable event, which it waits on twice
                     * around one creation. The guest resets the audio and applet
                     * events it owns explicitly (SVC 0x17). */
                    f->regs[0]=0;f->regs[1]=0;
                } else if (self && self->wait_cancel_requested) {
                    self->wait_cancel_requested=0;
                    f->regs[0]=VP_SVC_RESULT_CANCELLED;f->regs[1]=0;
                } else {
                    f->regs[0]=0xEA01u;f->regs[1]=0;
                }
                if (waited || !signaled) {
                    fprintf(s->log,"SVC_WAIT_EVENT handle=0x%08x signaled=%d waited_us=%llu timeout=0x%llx result=0x%08x\n",
                        handle,signaled,(unsigned long long)waited,
                        (unsigned long long)timeout,f->regs[0]);
                    fflush(s->log);
                }
                return 0;
            }
        }
        return VP_NOT_HANDLED;
    }
    if (id==0x25) {
        uint32_t handle=f->regs[1];
        VPGuestThread *t=thread_for_handle(&s->svc,handle);
        if (t) {
            uint64_t tid=((uint64_t)s->svc.process_id<<32)|(0x100u+(uint32_t)(t-thread_pool.threads));
            f->regs[0]=0;f->regs[1]=(uint32_t)tid;f->regs[2]=(uint32_t)(tid>>32);
            fprintf(s->log,"SVC_GETTHREADID handle=0x%08x id=0x%llx\n",
                handle,(unsigned long long)tid);
            fflush(s->log);return 0;
        }
        return VP_NOT_HANDLED;
    }
    return VP_NOT_HANDLED;
}
/* sceKernelDeleteThread accepts only dormant threads. Vita3K's
 * sceKernelWaitThreadEnd ignores its timeout, so it cannot bound shutdown.
 * Poll the exit status instead and stop the whole process rather than tear
 * down the shared session semaphore or stacks under a live worker. */
#define VP_THREAD_STOP_POLL_MS 1000u
static int thread_wait_dormant(int host) {
    for (unsigned i=0;i<VP_THREAD_STOP_POLL_MS;++i) {
        int status=0;
        int rc=sceKernelGetThreadExitStatus(host,&status);
        if (!rc) return 0;
        if (rc!=(int)SCE_KERNEL_ERROR_NOT_DORMANT) return rc;
        sceKernelDelayThread(1000);
    }
    return SCE_KERNEL_ERROR_NOT_DORMANT;
}
static _Noreturn void thread_pool_fatal(FILE *log,const char *operation,
                                        unsigned index,int host,int status) {
    fprintf(log,"SDK_THREAD_POOL_FATAL operation=%s index=%u handle=0x%x status=%d action=process-exit\n",
        operation,index,host,status);
    fflush(log);
    sceKernelExitProcess(-160);
    __builtin_trap(); /* A failed exit must not free resources still in use. */
}
int vp_sdk_thread_pool_stop(FILE *log) {
    unsigned running=0,exited=0,killed=0;
    for (unsigned i=0;i<VP_GUEST_THREAD_LIMIT;++i) {
        VPGuestThread *t=&thread_pool.threads[i];
        if (t->state==2) ++running;
        if (t->host<0) continue;
        if (t->state==3) ++exited;
        int rc=thread_wait_dormant(t->host);
        if (rc)
            thread_pool_fatal(log,"wait-dormant",i,t->host,rc);
        rc=sceKernelDeleteThread(t->host);
        if (rc)
            thread_pool_fatal(log,"delete",i,t->host,rc);
        t->host=-1;
        t->state=3;
        ++killed;
    }
    fprintf(log,"SDK_THREAD_STOP running=%u exited=%u killed=%u retained=0\n",
        running,exited,killed);
    fflush(log);
    if (thread_pool.prepared) {
        int released=sceKernelFreeMemBlock(thread_pool.stack_block);
        fprintf(log,"SDK_THREAD_POOL_RELEASE status=%d\n",released);
        fflush(log);
        thread_pool.stack_block=-1;thread_pool.stack_base=0;thread_pool.prepared=0;
        if (released) return released;
    }
    return 0;
}
/* ---- M5 audio assertion probe ------------------------------------------
 * The area is ordinary host memory, page-aligned and registered with the
 * service's guest memory model. It never overlaps the guest pool, so the
 * title's heap and address-space allocator are untouched. */
#define VP_AP_AREA_BYTES (4u << 20)
#define VP_AP_PARAM 0x0000u
#define VP_AP_CONFIG 0x0040u
#define VP_AP_HANDLE 0x00a0u
#define VP_AP_VOICE 0x00b0u
#define VP_AP_FINAL 0x00c0u
#define VP_AP_SINK 0x00d0u
#define VP_AP_ARGS 0x0100u
#define VP_AP_THREAD 0x0200u
#define VP_AP_STACK 0x1000u
#define VP_AP_STACK_BYTES 0x10000u
#define VP_AP_CFG 0x11000u
#define VP_AP_CFG_BYTES 0x40000u
#define VP_AP_WORK 0x51000u
#define VP_AP_WORK_BYTES 0x80000u
#define VP_AP_PCM 0xd1000u
#define VP_AP_PCM_FRAMES 480000u  /* 10 s mono at 48 kHz */

static void vp_sdk_audio_probe_resolve(Session *s,VPNativeModule *sdk) {
    struct { const char *name; uint32_t *dst; } syms[] = {
        {"_ZN2nn5audio32InitializeAudioRendererParameterEPNS0_22AudioRendererParameterE",
         &s->audio_probe_sdk.init_param},
        {"_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE",
         &s->audio_probe_sdk.get_work_size},
        {"_ZN2nn5audio36GetAudioRendererConfigWorkBufferSizeERKNS0_22AudioRendererParameterE",
         &s->audio_probe_sdk.get_cfg_size},
        {"_ZN2nn5audio29InitializeAudioRendererConfigEPNS0_19AudioRendererConfigERKNS0_22AudioRendererParameterEPvj",
         &s->audio_probe_sdk.init_cfg},
        {"_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvj",
         &s->audio_probe_sdk.open},
        {"_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE",
         &s->audio_probe_sdk.start},
        {"_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE",
         &s->audio_probe_sdk.stop},
        {"_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE",
         &s->audio_probe_sdk.update},
        {"_ZN2nn5audio16AcquireVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEiiNS0_12SampleFormatEiPKvj",
         &s->audio_probe_sdk.acquire_voice},
        {"_ZN2nn5audio19SetVoiceDestinationEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEPNS0_12FinalMixTypeE",
         &s->audio_probe_sdk.set_voice_dest},
        {"_ZN2nn5audio17SetVoiceMixVolumeEPNS0_9VoiceTypeEPNS0_12FinalMixTypeEfii",
         &s->audio_probe_sdk.set_voice_mix_volume},
        {"_ZN2nn5audio17SetVoicePlayStateEPNS0_9VoiceTypeENS1_9PlayStateE",
         &s->audio_probe_sdk.set_play_state},
        {"_ZN2nn5audio15AcquireFinalMixEPNS0_19AudioRendererConfigEPNS0_12FinalMixTypeEi",
         &s->audio_probe_sdk.acquire_final_mix},
        {"_ZN2nn5audio13AddDeviceSinkEPNS0_19AudioRendererConfigEPNS0_14DeviceSinkTypeEPNS0_12FinalMixTypeEPKaiPKc",
         &s->audio_probe_sdk.add_sink},
        {"_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_jii",
         &s->audio_probe_sdk.create_thread},
        {"_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE",
         &s->audio_probe_sdk.start_thread},
    };
    int failed=0;
    for (unsigned i=0;i<sizeof(syms)/sizeof(syms[0]);++i) {
        int rc=vp_native_export(sdk,syms[i].name,syms[i].dst);
        if (rc) {
            failed=rc;
            fprintf(s->log,"AUDIO_PROBE_SYMBOL name=%s status=%d\n",syms[i].name,rc);
        }
    }
    s->audio_probe_sdk.status=failed?-1:0;
    fprintf(s->log,"AUDIO_PROBE_SDK status=%d init_param=0x%08x work=0x%08x cfg=0x%08x open=0x%08x start=0x%08x update=0x%08x voice=0x%08x mix_vol=0x%08x sink=0x%08x create_thread=0x%08x\n",
        s->audio_probe_sdk.status,s->audio_probe_sdk.init_param,
        s->audio_probe_sdk.get_work_size,s->audio_probe_sdk.get_cfg_size,
        s->audio_probe_sdk.open,s->audio_probe_sdk.start,s->audio_probe_sdk.update,
        s->audio_probe_sdk.acquire_voice,s->audio_probe_sdk.set_voice_mix_volume,
        s->audio_probe_sdk.add_sink,
        s->audio_probe_sdk.create_thread);
    fflush(s->log);
}

static int audio_probe_monitor_thread(SceSize args,void *argp) {
    (void)args;
    Session *s=*(Session **)argp;
    VPAudioProbeArgs *a=(VPAudioProbeArgs *)(uintptr_t)s->audio_probe_args_addr;
    /* The probe's stage writes are the only progress record that survives an
     * emulator abort. fflush only reaches the emulated file layer's 4 KiB
     * block buffer, so each transition is written with its own open/close. */
    uint32_t last_stage=~0u;
    for (unsigned i=0;i<6000 && !a->done;++i) {
        uint32_t stage=a->stage;
        if (stage!=last_stage) {
            FILE *out=fopen("ux0:data/mhgu-vita-port/audio-probe.log","a");
            if (out) {
                fprintf(out,"AUDIO_PROBE_STAGE stage=%u done=%u poll=%u\n",
                    stage,a->done,i);
                fclose(out);
            }
            last_stage=stage;
        }
        sceKernelDelayThread(10000);
    }
    FILE *out=fopen("ux0:data/mhgu-vita-port/audio-probe.log","a");
    if (!out) return -1;
    VPAudioSinkStats st;vp_audio_sink_stats(&st);
    if (!a->done)
        fprintf(out,"AUDIO_PROBE_TIMEOUT stage=%u sink_output=%llu\n",
            a->stage,(unsigned long long)st.output_frames);
    else
        fprintf(out,"AUDIO_PROBE_DONE stage=0x%x error_stage=%u error_result=0x%x"
            " work=0x%x cfg=0x%x updates_done=%u updates_failed=%u last_update=0x%x\n",
            a->stage,a->error_stage,a->error_result,a->work_size,a->cfg_size,
            a->updates_done,a->updates_failed,a->last_update_result);
    fprintf(out,"AUDIO_PROBE_SINK open=%u running=%u rate=%u block=%u written=%llu"
        " output_frames=%llu blocks=%llu underruns=%llu silence=%llu overruns=%llu peak=%u zc=%llu"
        " rest=%u..%u max_gap_us=%llu error=0x%x first_write_us=%llu first_output_us=%llu last_output_us=%llu\n",
        st.open,st.running,st.rate,st.block_frames,
        (unsigned long long)st.written_frames,(unsigned long long)st.output_frames,
        (unsigned long long)st.output_blocks,(unsigned long long)st.underruns,
        (unsigned long long)st.silence_frames,(unsigned long long)st.overruns,st.peak,
        (unsigned long long)st.zero_crossings,st.rest_min,st.rest_max,
        (unsigned long long)st.max_output_gap_us,st.last_error,
        (unsigned long long)st.first_write_us,
        (unsigned long long)st.first_output_us,
        (unsigned long long)st.last_output_us);
    fclose(out);
    return 0;
}

static void vp_audio_probe_mark_host(uint32_t log_addr,uint32_t stage,uint32_t arg) {
    /* The probe calls this as an ordinary function. The same code issued as a
     * raw `svc` would be an import stub for the Vita kernel (NID = the next
     * instruction word), not a call into this loader's guest-SVC bridge. */
    FILE *log=(FILE *)(uintptr_t)log_addr;
    if (log) {
        fprintf(log,"AUDIO_PROBE_MARK stage=%u arg=0x%08x\n",stage,arg);
        fflush(log);
    }
    /* The emulated file layer keeps a 4 KiB block buffer and loses the tail
     * when the emulator aborts, so fflush is not durable. Closing is; the
     * probe makes about thirty marks, so one open/close per mark is cheap and
     * leaves the last completed stage on the host even after a guest fault. */
    FILE *progress=fopen("ux0:data/mhgu-vita-port/audio-probe-progress.log","a");
    if (progress) {
        fprintf(progress,"AUDIO_PROBE_MARK stage=%u arg=0x%08x\n",stage,arg);
        fclose(progress);
    }
}
static void vp_audio_probe_sleep_host(uint32_t microseconds) {
    if (microseconds) sceKernelDelayThread(microseconds);
}

static void vp_sdk_audio_probe_request(void *user) {
    Session *s=user;
    if (!s || s->audio_probe_launched) return;
    s->audio_probe_launched=1;
    fprintf(s->log,"AUDIO_PROBE_REQUEST marker=%d sdk_status=%d\n",
        s->audio_probe_marker,s->audio_probe_sdk.status);
    fflush(s->log);
    if (!s->audio_probe_marker || s->audio_probe_sdk.status) return;

    void *raw=malloc(VP_AP_AREA_BYTES+0x1000u);
    if (!raw) {
        fprintf(s->log,"AUDIO_PROBE_AREA status=out-of-memory bytes=%u\n",VP_AP_AREA_BYTES);
        fflush(s->log);return;
    }
    uintptr_t base=((uintptr_t)raw+0xFFFu)&~(uintptr_t)0xFFFu;
    if (base>UINT32_MAX-VP_AP_AREA_BYTES) {
        free(raw);
        fprintf(s->log,"AUDIO_PROBE_AREA status=above-32-bit base=0x%08x\n",(unsigned)base);
        fflush(s->log);return;
    }
    s->audio_probe_area=raw;
    s->audio_probe_area_addr=(uint32_t)base;
    memset((void *)base,0,VP_AP_AREA_BYTES);
    int reg=vp_guest_svc_region(&s->svc,(uint32_t)base,VP_AP_AREA_BYTES,5,3,"audio-probe");
    fprintf(s->log,"AUDIO_PROBE_AREA base=0x%08x bytes=%u region_status=%d\n",
        (uint32_t)base,VP_AP_AREA_BYTES,reg);
    if (reg) { fflush(s->log);return; }

    uint32_t param=(uint32_t)base+VP_AP_PARAM;
    uint32_t config=(uint32_t)base+VP_AP_CONFIG;
    uint32_t handle=(uint32_t)base+VP_AP_HANDLE;
    uint32_t voice=(uint32_t)base+VP_AP_VOICE;
    uint32_t final_mix=(uint32_t)base+VP_AP_FINAL;
    uint32_t sink=(uint32_t)base+VP_AP_SINK;
    uint32_t args_addr=(uint32_t)base+VP_AP_ARGS;
    uint32_t thread=(uint32_t)base+VP_AP_THREAD;
    uint32_t stack=(uint32_t)base+VP_AP_STACK;
    uint32_t cfg=(uint32_t)base+VP_AP_CFG;
    uint32_t work=(uint32_t)base+VP_AP_WORK;
    uint32_t pcm=(uint32_t)base+VP_AP_PCM;

    /* Deterministic 440 Hz mono test signal, integer phase accumulator,
     * amplitude 0.5. This is loader-generated content, not game audio, and is
     * named as such in the evidence. The voice is mono because the locked
     * VoiceInfoManager has one 0x70-byte channel resource for the probe's
     * single voice slot. */
    int16_t *samples=(int16_t *)(uintptr_t)pcm;
    uint32_t phase=0;
    for (uint32_t i=0;i<VP_AP_PCM_FRAMES;++i) {
        phase=(phase+440u)%VP_AUDIO_PROBE_PCM_RATE;
        samples[i]=(int16_t)(phase<VP_AUDIO_PROBE_PCM_RATE/2u?16384:-16384);
    }

    VPAudioProbeArgs *a=(VPAudioProbeArgs *)(uintptr_t)args_addr;
    memset(a,0,sizeof(*a));
    a->magic=VP_AUDIO_PROBE_MAGIC;a->version=VP_AUDIO_PROBE_VERSION;
    a->fn_init_param=s->audio_probe_sdk.init_param;
    a->fn_get_work_size=s->audio_probe_sdk.get_work_size;
    a->fn_get_cfg_size=s->audio_probe_sdk.get_cfg_size;
    a->fn_init_cfg=s->audio_probe_sdk.init_cfg;
    a->fn_open=s->audio_probe_sdk.open;
    a->fn_start=s->audio_probe_sdk.start;
    a->fn_stop=s->audio_probe_sdk.stop;
    a->fn_update=s->audio_probe_sdk.update;
    a->fn_acquire_voice=s->audio_probe_sdk.acquire_voice;
    a->fn_set_voice_dest=s->audio_probe_sdk.set_voice_dest;
    a->fn_set_voice_mix_volume=s->audio_probe_sdk.set_voice_mix_volume;
    a->fn_set_play_state=s->audio_probe_sdk.set_play_state;
    a->fn_acquire_final_mix=s->audio_probe_sdk.acquire_final_mix;
    a->fn_add_sink=s->audio_probe_sdk.add_sink;
    a->param=param;a->config=config;a->handle=handle;
    a->voice=voice;a->final_mix=final_mix;a->sink=sink;
    a->cfg_buffer=cfg;a->cfg_capacity=VP_AP_CFG_BYTES;
    a->work_buffer=work;a->work_capacity=VP_AP_WORK_BYTES;
    a->pcm=pcm;a->pcm_bytes=VP_AP_PCM_FRAMES*2u;a->pcm_frames=VP_AP_PCM_FRAMES;
    /* 400 updates x 960 frames = 384000 frames (8 s), inside the 10 s PCM
     * buffer. The title's VI frame loop starts ~2.7 s after the probe's
     * first write, so a shorter run only overlaps the video timeline by
     * ~1.7 s; 8 s gives ~5 s of paired A/V samples. */
    a->update_count=400u;
    a->fn_mark=(uint32_t)(uintptr_t)vp_audio_probe_mark_host;
    a->fn_sleep=(uint32_t)(uintptr_t)vp_audio_probe_sleep_host;
    a->log_ptr=(uint32_t)(uintptr_t)s->log;
    s->audio_probe_args_addr=args_addr;
    s->audio_probe_thread_addr=thread;
    s->audio_probe_stack_addr=stack;
    s->svc.audio_probe_args=args_addr;
    s->svc.audio_probe_pending_field=args_addr+(uint32_t)offsetof(VPAudioProbeArgs,sink_pending);
    s->svc.audio_probe_done_field=args_addr+(uint32_t)offsetof(VPAudioProbeArgs,done);

    uint32_t targs[7]={thread,(uint32_t)(uintptr_t)vp_audio_probe_entry,args_addr,
        stack,VP_AP_STACK_BYTES,16u,0xFFFFFFFFu};
    uint32_t result=0;
    s->svc.audio_probe_expect_thread=1;
    int rc=vp_sdk_guest_call_args(s,3,s->audio_probe_sdk.create_thread,targs,7,&result);
    fprintf(s->log,"AUDIO_PROBE_THREAD create_rc=%d create_result=0x%08x"
        " entry=0x%08x thread=0x%08x stack=0x%08x\n",
        rc,result,(uint32_t)(uintptr_t)vp_audio_probe_entry,thread,stack);
    if (!rc && !result) {
        uint32_t sargs[1]={thread};
        int src=vp_sdk_guest_call_args(s,3,s->audio_probe_sdk.start_thread,sargs,1,&result);
        fprintf(s->log,"AUDIO_PROBE_THREAD start_rc=%d\n",src);
        rc=src;
        result=0;
    }
    if (rc || result) {
        fprintf(s->log,"AUDIO_PROBE_THREAD status=failed rc=%d result=0x%08x\n",rc,result);
        fflush(s->log);return;
    }
    /* Harness ordering. This hook runs on the title's own audio-init thread,
     * inside the OpenAudioRenderer reply, and that thread calls
     * StartAudioRenderer as soon as the reply reaches it. Without ordering
     * the probe and the title race for the single SceAudioOut sink: on one
     * run the probe's Start won, on another the title's did and the probe was
     * refused with 0xe0010002 before its first update. Wait (bounded) until
     * the probe's own Start has returned - stage 10 on success, done=1 on
     * failure - and only then let the title continue. The probe still runs
     * its update loop concurrently afterwards; this only orders Start. */
    {
        VPAudioProbeArgs *live=(VPAudioProbeArgs *)(uintptr_t)s->audio_probe_args_addr;
        int waited_ms=0;
        VPPark probe_park;
        /* The SDK's thread trampoline reaches its first SDK call before the
         * probe entry runs, and that call is dispatched under the session
         * lock this Open handler is itself holding. Waiting while holding it
         * parks the probe permanently (probe81: 5 s at stage 0). Use the
         * common park protocol so the ambient thread/TLS values are restored
         * when the session lock is reacquired. */
        park_begin(s,&probe_park);
        while (!((volatile VPAudioProbeArgs *)live)->done
            && ((volatile VPAudioProbeArgs *)live)->stage<10u && waited_ms<5000) {
            sceKernelDelayThread(1000);
            ++waited_ms;
        }
        park_end(s,&probe_park);
        fprintf(s->log,"AUDIO_PROBE_HANDSHAKE waited_ms=%d stage=%u done=%u\n",
            waited_ms,live->stage,live->done);
        fflush(s->log);
    }
    {   /* Optional: the monitor is a diagnostics thread. A second thread
         * writing device files while the probe thread and the game log can
         * disturb emulator IO, so it is enabled only by an explicit marker. */
        FILE *enable=fopen("ux0:data/mhgu-vita-port/audio-probe-monitor.enable","rb");
        if (!enable) {
            fprintf(s->log,"AUDIO_PROBE_MONITOR status=disabled\n");
        } else {
            fclose(enable);
            s->audio_probe_monitor=sceKernelCreateThread("vp-audio-probe-mon",
                audio_probe_monitor_thread,0x10000080,0x8000,0,SCE_KERNEL_CPU_MASK_USER_ALL,NULL);
            if (s->audio_probe_monitor>=0) {
                int mrc=sceKernelStartThread(s->audio_probe_monitor,sizeof(s),&s);
                fprintf(s->log,"AUDIO_PROBE_MONITOR thread=%d start=%d\n",(int)s->audio_probe_monitor,mrc);
            } else {
                fprintf(s->log,"AUDIO_PROBE_MONITOR thread=%d\n",(int)s->audio_probe_monitor);
            }
        }
    }
    fflush(s->log);
}
int vp_sdk_dso_execute(FILE *log,VPExecutor *e) {
    uint32_t saved_host_tls=host_tls(),saved_host_fp=host_fp();
    memset(&session,0,sizeof(session));Session *s=&session;s->log=log;s->resume=resume;
    s->pool_block=-1;s->launch_block=-1;s->state_lock=-1;
    s->state_gate.semaphore=-1;
    s->state_lock=sceKernelCreateMutex("vp-guest-state",SCE_KERNEL_MUTEX_ATTR_RECURSIVE,0,NULL);
    if (s->state_lock<0) return s->state_lock;
    {
        FILE *kernel=fopen("ux0:data/mhgu-vita-port/sdk-session-kernel.enable","rb");
        s->session_kernel=kernel!=NULL;
        if (kernel) fclose(kernel);
        if (!s->session_kernel) {
            s->state_gate.semaphore=sceKernelCreateSema("vp-session-gate",0,0,0x7fffffff,NULL);
            if (s->state_gate.semaphore<0) {
                int rc=s->state_gate.semaphore;
                int deleted=sceKernelDeleteMutex(s->state_lock);s->state_lock=-1;
                fprintf(log,"SDK_SESSION_LOCK_INIT status=%d mutex_cleanup=%d\n",rc,deleted);
                fflush(log);return rc;
            }
        }
        fprintf(log,"SDK_SESSION_LOCK_MODE backend=%s\n",
            s->session_kernel?"kernel-mutex":"user-gate");
    }
    {
        FILE *disable=fopen("ux0:data/mhgu-vita-port/sdk-lock-fast.disable","rb");
        s->fast_enabled=disable==NULL;
        if (disable) fclose(disable);
        fprintf(log,"SDK_LOCK_FAST_MODE enabled=%d\n",s->fast_enabled);
    }
    for (unsigned i=0;i<VP_GUEST_THREAD_LIMIT;++i) {
        thread_pool.threads[i].executor.handler=dispatch;
        thread_pool.threads[i].executor.user=s;
    }
    for (unsigned i=0;i<VP_SDK_LOCK_LIMIT;++i) s->locks[i].id=-1;
    VPExecutorHandler old_handler=e->handler;void *old_user=e->user;e->handler=dispatch;e->user=s;
    s->main_executor=e;
    vp_thread_local_allocator_init(&s->allocator);
    vp_guest_svc_init(&s->svc,log);
    s->svc.device_wait_begin=device_wait_begin;
    s->svc.device_wait_end=device_wait_end;
    s->svc.device_wait_user=s;
    for (unsigned i=0;i<4;i++) vp_guest_svc_set_module_base(&s->svc,i,e->modules[i].base);
    s->termination_test=termination_test_enabled();
    {
        FILE *play=fopen("ux0:data/mhgu-vita-port/play.enable","rb");
        s->continuous_play=play!=NULL;
        if (play) fclose(play);
    }
    fprintf(log,"SDK_PLAY_MODE enabled=%d evidence_stop=%d injected_exit=%d\n",
            s->continuous_play,!s->continuous_play,
            s->termination_test && !s->continuous_play);
    s->termination_delay_us=30000000ull;
    {
        FILE *delay=fopen("ux0:data/mhgu-vita-port/applet-exit-delay-ms","rb");
        if (delay) {
            unsigned ms=0;
            if (fscanf(delay,"%u",&ms)==1 && ms)
                s->termination_delay_us=(uint64_t)ms*1000ull;
            fclose(delay);
        }
    }
    fprintf(log,"SDK_TERMINATION_TEST enabled=%d marker=ux0:data/mhgu-vita-port/applet-exit delay_us=%llu\n",
            s->termination_test,(unsigned long long)s->termination_delay_us);
    fflush(log);
    vp_guest_svc_tls(&s->svc,e->bridge.guest.tls_ro);
    uint32_t args[4]={(uint32_t)(uintptr_t)vp_sdk_alloc,(uint32_t)(uintptr_t)vp_sdk_free,0,0},result=0;
    VPNativeModule *m=&e->modules[3];int rc=setup_guest_memory(s,e);
    if(!rc) rc=vp_executor_invoke(e,3,m->base+0x9e10c,args,&result);
    fprintf(log,"SDK_SET_ALLOCATOR status=%d\n",rc);fflush(log);
    /* The original startup order is: Start runs nnosInitialize (which
     * constructs nn::os::detail::ThreadManager and installs the initial
     * ThreadType at TPIDRURO+0x1fc), and only then does the rtld init loop
     * call each DSO's .init. Calling the .init wrappers here would let the
     * deferred constructor chain run before the SDK thread manager exists,
     * so DSO registration is left entirely to the original Start path. */
    VPBridgeFrame *f=&e->fault_frame;
    if (!rc) rc=vp_unwind_register(&s->exceptions,e->modules,4);
    for (unsigned i=0;i<4 && !rc;++i) {
        /* The private sorted view follows the whole code-island block; SVC
         * islands made the previous fixed +8192 overlap the island payload. */
        /* The private sorted view follows the whole code-island block; SVC
         * islands made the previous fixed +8192 overlap the island payload. */
        uint32_t index_offset=e->modules[i].island_offset+((e->modules[i].island_bytes+4095u)&~4095u);
        rc=vp_sdk_exception_index(&s->exceptions.modules[i],index_offset,&s->indexes[i]);
        fprintf(log,"SDK_EXCEPTION_INDEX module=%u offset=0x%x entries=%u order_breaks=%u sha256=",i,s->indexes[i].offset,s->indexes[i].entries,s->exceptions.modules[i].order_breaks);
        for (unsigned j=0;j<32;++j) fprintf(log,"%02x",s->indexes[i].digest[j]);
        fprintf(log," status=%d\n",rc);fflush(log);
    }
    for (unsigned i=0;i<4 && !rc;++i) {
        VPNativeModule *owner=&e->modules[i];memset(s->exception_output,0,sizeof(s->exception_output));
        uint32_t query[4]={(uint32_t)(uintptr_t)s->exception_output,owner->base+owner->init,0,0};
        rc=vp_executor_invoke(e,3,m->base+0xa514c,query,&result);
        uint32_t offset=s->indexes[i].offset;
        uint32_t bytes=word(owner->image+owner->mod0+20)-word(owner->image+owner->mod0+16);
        if (!rc && (result!=1 || s->exception_output[0]!=owner->base || s->exception_output[1]!=owner->capacity
            || s->exception_output[2]!=offset || s->exception_output[3]!=bytes)) rc=-145;
        fprintf(log,"SDK_EXCEPTION_INFO module=%u base=0x%08x span=%u table=0x%x bytes=%u status=%d\n",i,s->exception_output[0],s->exception_output[1],s->exception_output[2],s->exception_output[3],rc);fflush(log);
    }
    static const uint32_t probes[][2]={{3,0x439864},{0,0xf4},{1,0x28},{2,0x28},{2,0x32c3a8}};
    for (unsigned test=0;test<sizeof(probes)/sizeof(probes[0]) && !rc;++test) {
        VPNativeModule *owner=&e->modules[probes[test][0]];
        uint32_t query[4]={owner->base+probes[test][1],(uint32_t)(uintptr_t)s->exception_output,0,0};
        rc=vp_executor_invoke(e,3,m->base+0x3f11fc,query,&result);
        uint32_t begin=owner->mod0+(int32_t)word(owner->image+owner->mod0+16);
        uint32_t end=owner->mod0+(int32_t)word(owner->image+owner->mod0+20),best=0,position=UINT32_MAX;
        for (uint32_t off=begin;off<end;off+=8) {
            uint32_t raw=word(owner->image+off);
            int64_t function=(int64_t)off+((raw&0x40000000)?(int64_t)(raw&0x7fffffff)-0x80000000ll:raw);
            if (function<=probes[test][1] && function>=best) { best=(uint32_t)function;position=off; }
        }
        uint32_t rank=0;
        if (position!=UINT32_MAX) for (uint32_t off=begin;off<end;off+=8) {
            uint32_t raw=word(owner->image+off);int64_t function=(int64_t)off+((raw&0x40000000)?(int64_t)(raw&0x7fffffff)-0x80000000ll:raw);
            if (function<best || (function==best && off<position)) ++rank;
        }
        uint32_t expected=position==UINT32_MAX?0:owner->base+s->indexes[probes[test][0]].offset+rank*8+4;
        if (position!=UINT32_MAX) {
            uint32_t desc=word(owner->image+position+4);
            if (desc==1) expected=0;
            else if (!(desc&0x80000000u)) expected=owner->base+position+4+((desc&0x40000000u)?(int64_t)(desc&0x7fffffffu)-0x80000000ll:desc);
        }
        fprintf(log,"SDK_FIND_FDE pc=0x%08x result=0x%08x status=%d queries=%u module=%u offset=0x%x expected=0x%08x matches=%d\n",query[0],result,rc,s->exception_queries,probes[test][0],probes[test][1],expected,result==expected);fflush(log);
        if (!rc && result!=expected) rc=-146;
    }
    if (!rc) {
        static const char *names[]={"unw_getcontext","unw_init_local","unw_set_reg","unw_get_proc_info"};
        uint32_t functions[4]={0},info[16]={0};
        for (unsigned i=0;i<4 && !rc;++i) rc=vp_native_export(m,names[i],&functions[i]);
        VPNativeModule fixture[4];memcpy(fixture,e->modules,sizeof(fixture));
        uint32_t start=(uint32_t)(uintptr_t)vp_sdk_unwind_query,end=(uint32_t)(uintptr_t)vp_sdk_unwind_query_end;
        memset(&fixture[0],0,sizeof(fixture[0]));fixture[0].base=start&~4095u;fixture[0].image=(void *)(uintptr_t)fixture[0].base;
        fixture[0].state=3;fixture[0].nso.span=fixture[0].nso.segments[0].bytes=end-fixture[0].base;
        VPNativeModule *original=e->modules;e->modules=fixture;
        uint32_t call[4]={m->base+0x52a4,(uint32_t)(uintptr_t)info,(uint32_t)(uintptr_t)functions,0};
        if (!rc) rc=vp_executor_invoke(e,0,start,call,&result);
        fprintf(log,"SDK_CANTUNWIND module=3 offset=0x52a4 result=%d status=%d\n",(int)result,rc);fflush(log);
        if (!rc && (int)result>=0) rc=-155;
        e->modules=original;
    }
    if (!rc) {
        VPNativeModule fixture[4];memcpy(fixture,e->modules,sizeof(fixture));
        uint32_t start=(uint32_t)(uintptr_t)vp_sdk_argument_fixture,end=(uint32_t)(uintptr_t)vp_sdk_argument_fixture_end;
        memset(&fixture[0],0,sizeof(fixture[0]));fixture[0].base=start&~4095u;fixture[0].image=(void *)(uintptr_t)fixture[0].base;
        fixture[0].state=3;fixture[0].nso.span=fixture[0].nso.segments[0].bytes=end-fixture[0].base;
        VPNativeModule *original=e->modules;e->modules=fixture;
        for (unsigned test=0;test<3 && !rc;++test) {
            uint32_t words[24]={0},expected=0;
            if (!test) for (unsigned i=0;i<24;++i) words[i]=0x81230000u+i*0x12345u;
            else words[test==1?4:23]=0x13579bdfu;
            for (unsigned i=0;i<24;++i) expected+=words[i]*(i+1);
            rc=vp_executor_invoke_args(e,0,start,words,24,&result);
            if (!rc && result!=expected) rc=-152;
            fprintf(log,"SDK_ARGUMENTS case=%u words=24 expected=0x%08x actual=0x%08x status=%d\n",test,expected,result,rc);fflush(log);
        }
        e->modules=original;
    }
    if (!rc) {
        static const char *names[]={"__cxa_allocate_exception","__cxa_begin_catch","__cxa_end_catch","__cxa_end_cleanup",
            "__cxa_throw","__gxx_personality_v0","__aeabi_unwind_cpp_pr0"};
        for (unsigned i=0;i<7 && !rc;++i) {
            rc=vp_native_export(m,names[i],&vp_sdk_cpp_functions[i]);
            fprintf(log,"SDK_CPP_PROVIDER name=%s address=0x%08x status=%d\n",names[i],vp_sdk_cpp_functions[i],rc);
        }
        uint32_t typeinfo=0;if (!rc) rc=vp_native_export(m,"_ZTIi",&typeinfo);
        if (!rc) memcpy(vp_cpp_typeinfo,(void *)(uintptr_t)typeinfo,8);
        VPNativeModule fixture[4];memcpy(fixture,e->modules,sizeof(fixture));
        uint32_t start=(uint32_t)(uintptr_t)vp_sdk_cpp_fixture,end=(uint32_t)(uintptr_t)vp_sdk_cpp_fixture_end;
        memset(&fixture[0],0,sizeof(fixture[0]));fixture[0].base=start&~4095u;fixture[0].image=(void *)(uintptr_t)fixture[0].base;
        fixture[0].state=3;fixture[0].nso.span=fixture[0].nso.segments[0].bytes=end-fixture[0].base;
        VPNativeModule *original=e->modules;e->modules=fixture;
        uint32_t live=s->allocator.live_allocations,persistent=live;
        for (unsigned i=0;i<3 && !rc;++i) {
            int values[]={0,7,-5};uint32_t call[4]={(uint32_t)values[i],(uint32_t)(uintptr_t)&s->destructors,0,0};
            unsigned frees=s->heap_frees;
            rc=vp_executor_invoke(e,0,start,call,&result);
            int expected=values[i]?values[i]+100:11;
            if (i==1) persistent=s->allocator.live_allocations;
            if (!rc && ((int)result!=expected || s->destructors!=i+1 || (!i && s->allocator.live_allocations!=live)
                || (i==2 && s->allocator.live_allocations!=persistent) || (i && s->heap_frees!=frees+1))) rc=-147;
            fprintf(log,"SDK_CPP_CASE value=%d result=%d destructors=%u live=%u status=%d\n",values[i],(int)result,s->destructors,s->allocator.live_allocations,rc);fflush(log);
        }
        e->modules=original;
    }
    if (!rc) for (unsigned pass=0;pass<4 && !rc;++pass) {
        unsigned called=0;
        for (unsigned i=0;i<32 && !rc;++i) if ((s->tls_pool.used&(1u<<i)) && s->tls_values.generations[i]==s->tls_pool.generations[i] && s->tls_values.values[i] && s->tls_pool.destructors[i]) {
            uint32_t call[4]={s->tls_values.values[i],0,0,0},pc=s->tls_pool.destructors[i];s->tls_values.values[i]=0;
            unsigned owner=0;while (owner<4 && ((pc&~1u)<e->modules[owner].base || (pc&~1u)>=e->modules[owner].base+e->modules[owner].nso.segments[0].bytes)) ++owner;
            if (owner==4) rc=-149;else rc=vp_executor_invoke(e,owner,pc,call,&result);
            ++called;++s->tls_destructors;
        }
        if (!called) break;
    }
    fprintf(log,"SDK_TLS_CLEANUP destructors=%u heap_allocs=%u heap_frees=%u status=%d\n",s->tls_destructors,s->heap_allocs,s->heap_frees,rc);fflush(log);
    if (!rc) {
        /* Original Start with the real rtld callbacks and a real NSO argument
         * block. nnosInitialize is no longer patched: it runs its original body
         * and reaches the native SVC services above. The callback flag at
         * rtld+0x3000 is set by rtld+0x16f8, which proves nnosInitialize
         * returned before the next stage. */
        uint32_t start_args[4]={s->start_handle,s->launch_address,
            e->modules[0].base+0x16f8,e->modules[0].base+0x900};
        int stopped=vp_executor_invoke(e,3,m->base+0x78d80,start_args,&result);
        VPBridgeFrame *fault=&e->fault_frame;
        memcpy(&s->start_flag,e->modules[0].image+0x3000,4);s->start_result=result;
        int nnos_returned=s->start_flag==1;
        fprintf(log,"SDK_START entry=0x78d80 status=%d service=0x%x continuation=0x%08x nnos_returned=%d callback_flag=%u result=%u svc_calls=%u svc_unsupported=%u svc_breaks=%u\n",
            stopped,fault->service,fault->pc,nnos_returned,s->start_flag,result,s->svc.calls,s->svc.unsupported,s->svc.breaks);
        for (unsigned i=0;i<13;++i) fprintf(log,"START_REG r%u=0x%08x\n",i,fault->regs[i]);
        fprintf(log,"START_FAULT sp=0x%08x lr=0x%08x pc=0x%08x\n",fault->sp,fault->lr,fault->pc);fflush(log);
        /* The NVN client in main dispatches through .bss tables installed by
         * its bootstrap. When the guest exits, record which implementations
         * the pool path called so the failing routine can be identified. */
        if (fault->service==VP_GUEST_SVC_BASE+0x07u) {
            static const uint32_t nvn_slots[]={0x1840d94,0x1840d98,0x1840d9c,0x1840da0,0x1840da4,0x1840dac};
            for (unsigned i=0;i<sizeof(nvn_slots)/sizeof(nvn_slots[0]);++i) {
                uint32_t table=word((void *)(uintptr_t)(e->modules[1].base+nvn_slots[i]));
                uint32_t function=0;
                if (table>=e->modules[1].base && table<e->modules[1].base+e->modules[1].nso.span-4)
                    function=word((void *)(uintptr_t)table);
                fprintf(log,"SDK_EXIT_NVN slot=0x%x table=0x%08x function=0x%08x\n",
                    nvn_slots[i],table,function);
            }
            /* nvn::MemoryPool instances register themselves in this list;
             * the last one is the pool whose initialization just failed. */
            uint32_t count_pointer=word((void *)(uintptr_t)(e->modules[1].base+0x1840d44));
            uint32_t array_pointer=word((void *)(uintptr_t)(e->modules[1].base+0x1840d48));
            uint32_t count=0;
            if (count_pointer && vp_guest_svc_readable(&s->svc,count_pointer,4))
                count=word((void *)(uintptr_t)count_pointer);
            fprintf(log,"SDK_EXIT_POOLS count=%u count_pointer=0x%08x array=0x%08x\n",
                count,count_pointer,array_pointer);
            /* nn::os::AllocateMemoryBlock/FreeMemoryBlock keep the process
             * heap's used and capacity words at manager+0x1e0/+0x1e4; the
             * manager pointer lives at sdk+0x85bd18. */
            uint32_t manager=word((void *)(uintptr_t)(e->modules[3].base+0x85bd18));
            fprintf(log,"SDK_EXIT_HEAP manager=0x%08x\n",manager);
            if (manager && vp_guest_svc_readable(&s->svc,manager+0x1d0,32))
                for (unsigned i=0;i<8;++i)
                    fprintf(log,"SDK_EXIT_HEAP offset=0x%x value=0x%08x\n",
                        0x1d0+i*4,word((void *)(uintptr_t)(manager+0x1d0+i*4)));
            if (count>32) count=32;
            for (unsigned i=0;i<count;++i) {
                uint32_t slot=array_pointer+i*4,object=0,table=0,method=0;
                uint32_t page=0,size=0,flags=0,address=0,device=0;
                if (!vp_guest_svc_readable(&s->svc,slot,4)) break;
                object=word((void *)(uintptr_t)slot);
                if (!object || !vp_guest_svc_readable(&s->svc,object,0x5b4)) break;
                table=word((void *)(uintptr_t)object);
                page=word((void *)(uintptr_t)(object+0x34));
                size=word((void *)(uintptr_t)(object+0x64));
                flags=word((void *)(uintptr_t)(object+0x4a8));
                device=word((void *)(uintptr_t)(object+0x5b0));
                address=word((void *)(uintptr_t)(object+0x5bc));
                if (table && vp_guest_svc_readable(&s->svc,table,4))
                    method=word((void *)(uintptr_t)table);
                fprintf(log,"SDK_EXIT_POOL index=%u object=0x%08x table=0x%08x method=0x%08x page=0x%x size=0x%x flags=0x%x device=0x%08x address=0x%08x\n",
                    i,object,table,method,page,size,flags,device,address);
            }
            fflush(log);
        }
        /* DSO registration and constructor dispatch now happen inside the
         * original Start path, after nnosInitialize installed ThreadManager. */
        uint32_t flag=word(m->image+0x862754),tp=word((void *)(uintptr_t)(e->bridge.guest.tls_ro+0x1f8));
        if (engine_submit_probe_enabled) engine_submit_report(s);
        vp_engine_layout_report(log);
        fprintf(log,"SDK_DSO status=%d service=0x%x observed=%u mask=%u gated=%u sdk_flag=%u tls_ro=0x%08x tp=0x%08x tls_rw=0x%08x live=%u peak=%u locks=%u unlocks=%u recycles=%u nnmain=%u session_stop=%u\n",
            stopped,f->service,s->observed,s->observed_mask,s->gated,flag,e->bridge.guest.tls_ro,tp,e->bridge.guest.tls_rw,s->allocator.live_allocations,s->allocator.peak_bytes,s->lock_calls,s->unlock_calls,s->lock_recycles,
            s->nnmain_found?1u:0u,s->session_stopped?1u:0u);
        for (unsigned i=1;i<4;++i) fprintf(log,"SDK_MODULE_INIT module=%u flag=%u\n",i,init_flag(&e->modules[i]));
        fflush(log);
        /* Any nonzero bridge result is a real stop: the executor only returns
         * nonzero when a service was rejected or the guest did not return.
         * The old constructor-gate exemption reported an unimplemented applet
         * service as RESULT COMPLETE, which is a false success. The one
         * exception is the loader's own evidence-bounded session stop after
         * the original nnMain was observed live; it is recorded separately
         * and is never reported as a title exit. */
        if (!nnos_returned) rc=-153;
        else if (stopped==VP_BRIDGE_SESSION_STOP)
            fprintf(log,"SDK_SESSION_STOP reason=nnmain-and-frame-loop-observed nnmain=%u frames=%u presents=%u\n",
                s->nnmain_found?1u:0u,s->frame_queues,s->frame_presents);
        else if (stopped==VP_BRIDGE_PROCESS_EXIT)
            fprintf(log,"SDK_PROCESS_EXIT reason=title-applet-exit-unlocked nnmain=%u frames=%u presents=%u exit_lock_depth=0 not-a-loader-stop\n",
                s->nnmain_found?1u:0u,s->frame_queues,s->frame_presents);
        else if (stopped) rc=stopped;
    }
    /* Original fini wrappers only unregister DSO/TLS metadata in this build;
     * no game constructor has run. Exercise them before the owner fallback.
     * A process terminated by the applet exit model does not run guest fini,
     * matching the reference Terminate semantics. */
    if (s->title_exited) {
        fprintf(log,"SDK_FINI_SKIP reason=applet-process-terminate\n");fflush(log);
    } else for (unsigned i=1;i<4 && !rc;++i) {
        rc=vp_executor_invoke(e,i,e->modules[i].base+e->modules[i].fini,(uint32_t[4]){0},&result);
        fprintf(log,"SDK_FINI_WRAPPER module=%u result=%u live=%u status=%d\n",i,result,s->allocator.live_allocations,rc);fflush(log);
    }
    fprintf(log,"SDK_DSO_UNREGISTER live=%u bytes=%u status=%d\n",s->allocator.live_allocations,s->allocator.live_bytes,rc);fflush(log);
    int pool_rc=vp_sdk_thread_pool_stop(log);
    if (!rc) rc=pool_rc;
    for (unsigned i=0;i<VP_SDK_LOCK_LIMIT;++i) if (s->locks[i].id>=0) {
        int released=s->locks[i].held?sceKernelUnlockMutex(s->locks[i].id,1):0;
        int deleted=sceKernelDeleteMutex(s->locks[i].id);if (!rc) rc=released?released:deleted;
    }
    if (s->launch_block>=0) { int released=sceKernelFreeMemBlock(s->launch_block);fprintf(log,"SVC_LAUNCH_RELEASE status=%d\n",released);s->launch_block=-1; }
    if (s->pool_block>=0) { int released=sceKernelFreeMemBlock(s->pool_block);fprintf(log,"SVC_POOL_RELEASE status=%d\n",released);s->pool_block=-1; }
    /* Whole bounded session stops here, before any constructor. No guest
     * worker survives; release all owned allocations and detach its TLS view. */
    vp_thread_local_allocator_destroy(&s->allocator);
    vp_unwind_reset(&s->exceptions);
    memset((void *)(uintptr_t)(e->bridge.guest.tls_ro+0x1f8),0,4);e->bridge.guest.tls_rw=0;
    e->handler=old_handler;e->user=old_user;
    int host_ok=host_tls()==saved_host_tls && host_fp()==saved_host_fp;
    if (!rc && !host_ok) rc=-154;
    fprintf(log,"SDK_HOST_CONTEXT tls_unchanged=%d fpscr_unchanged=%d status=%d\n",host_tls()==saved_host_tls,host_fp()==saved_host_fp,rc);fflush(log);
    if (s->state_lock>=0) {
        int deleted=sceKernelDeleteMutex(s->state_lock);
        fprintf(log,"SDK_STATE_RELEASE status=%d\n",deleted);fflush(log);
        if (!rc) rc=deleted;
        s->state_lock=-1;
    }
    if (s->state_gate.semaphore>=0) {
        int deleted=sceKernelDeleteSema(s->state_gate.semaphore);
        fprintf(log,"SDK_SESSION_GATE_RELEASE status=%d\n",deleted);fflush(log);
        if (!rc) rc=deleted;
        s->state_gate.semaphore=-1;
    }
    fprintf(log,"SDK_DSO_CLEANUP live=%u quiescent=%d status=%d\n",s->allocator.live_allocations,vp_executor_quiescent(e),rc);fflush(log);
    return rc;
}

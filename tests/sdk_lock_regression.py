#!/usr/bin/env python3
"""Generate a Windows-only host simulation of SDK lock and condition variables.

The generated harness checks production control flow and bookkeeping with mock
mutexes and events. It does not prove real kernel scheduling or timing behavior.
Generated C, CMake files, and executables belong in the requested build directory.
"""

import argparse
import hashlib
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, required=True,
                    help="Fresh directory for generated C and CMake files")
args = parser.parse_args()
directory = args.output.resolve()
directory.mkdir(parents=True, exist_ok=False)

root = Path(__file__).resolve().parents[1]
source_bytes = (root / "src/loader/sdk_dso.c").read_bytes()
source = source_bytes.decode("utf-8")
source_hash = hashlib.sha256(source_bytes).hexdigest()
gate_source = (root / "src/platform/session_gate.h").read_text(encoding="utf-8")

def block(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

lock_start = source.index('typedef struct { uint32_t address;SceUID id;')
lock_end = source.index('} Lock;', lock_start) + len('} Lock;')
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#define __sync_fetch_and_add(p,v) InterlockedExchangeAdd((volatile LONG *)(p),(v))
#define __ATOMIC_RELAXED 0
#define __ATOMIC_ACQUIRE 1
#define __ATOMIC_RELEASE 2
static uint32_t atomic_load32(uint32_t *p) {
    return (uint32_t)InterlockedCompareExchange((volatile LONG *)p, 0, 0);
}
static uint32_t atomic_exchange32(uint32_t *p, uint32_t value) {
    return (uint32_t)InterlockedExchange((volatile LONG *)p, (LONG)value);
}
static int atomic_compare32(uint32_t *p, uint32_t *expected, uint32_t value) {
    uint32_t old = (uint32_t)InterlockedCompareExchange(
        (volatile LONG *)p, (LONG)value, (LONG)*expected);
    if (old == *expected) return 1;
    *expected = old;
    return 0;
}
#define __atomic_load_n(p, order) atomic_load32(p)
#define __atomic_store_n(p, v, order) ((void)atomic_exchange32((p), (v)))
#define __atomic_exchange_n(p, v, order) atomic_exchange32((p), (v))
#define __atomic_compare_exchange_n(p, e, v, weak, success, failure) atomic_compare32((p), (e), (v))
typedef int SceUID;
typedef unsigned SceUInt;
#define VP_SDK_LOCK_LIMIT 512u
#define VP_KEY_WAIT_LIMIT 32u
#define VP_SDK_LOCK_SLICE_US 1000000u
#define VP_SDK_LOCK_BLOCK_LIMIT_US 5000000u
#define VP_SDK_LOCK_DEADLOCK (-143)
#define SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN (-10)
#define SCE_KERNEL_ERROR_MUTEX_RECURSIVE (-11)
#define SCE_KERNEL_ERROR_MUTEX_NOT_OWNED (-12)
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT (-13)
#define SCE_KERNEL_MUTEX_ATTR_RECURSIVE 1
#define SCE_EVENT_WAITOR 1
#define SCE_EVENT_WAITCLEAR 2
#define VP_BRIDGE_UNSUPPORTED (-100)
static int sceKernelWaitSema(int semaphore,int count,unsigned *timeout);
static int sceKernelSignalSema(int semaphore,int count);
'''
types = r'''
typedef struct { uint32_t key,address,tag;SceUID event;unsigned used,signaled; } KeyWait;
typedef struct { uint32_t current_thread_handle,tls_buffer; } Svc;
typedef struct VPExecutor VPExecutor;
typedef struct { Lock locks[VP_SDK_LOCK_LIMIT];unsigned lock_hint[64];
    KeyWait key_waits[VP_KEY_WAIT_LIMIT];
    unsigned key_waits_live,key_signals,key_wakes,key_timeouts;
    unsigned lock_calls,unlock_calls,lock_recycles,lock_cursor;
    unsigned fast_enter,fast_leave,fast_try,fast_islocked,fast_fallback;
    unsigned fast_miss[6];
    int fast_enabled,state_lock,stopping;uint32_t start_handle;VPExecutor *main_executor;
    Svc svc;FILE *log;VPSessionGate state_gate;int session_kernel; } Session;
struct VPExecutor { void *user;struct {uint32_t base;} modules[4];
    struct { struct {uint32_t tls_ro;} guest; } bridge; };
typedef struct {uint32_t handle;} VPGuestThread;
typedef struct { uint32_t service,regs[21],lr,sp,pc; } VPBridgeFrame;
typedef struct {uint32_t thread,tls;} VPPark;
static unsigned g_sdk_lock_wait_lines,g_sdk_hold_lines,g_svc_key_lines;
static Session state;
#define session state
static int host_owner[513],next_id,session_locked,current_thread,host_thread_id,session_state_depth;
static int m_try,m_lock,m_unlock,m_park,m_clock,m_events;
static int forced_try,forced_unlock,force_busy,wait_result,release_on_park,require_park;
static int state_try_error,state_unlock_error,state_busy,m_state_try,m_state_unlock,memory_valid;
static int m_state_waits;
static uint32_t denied_address,last_wait_result;
static unsigned event_used[VP_KEY_WAIT_LIMIT+1],event_bits[VP_KEY_WAIT_LIMIT+1];
static int forced_event_create,forced_event_set,m_event_sets;
static void (*park_hook)(void);
static VPExecutor *recognized_worker;
static VPGuestThread worker;
static int gate_tokens,gate_wait_error,gate_signal_error;
static unsigned gate_wait_calls,gate_signal_calls,gate_wait_pause_at;
static void (*gate_wait_hook)(void);
static uint32_t host_tls(void) {return (uint32_t)host_thread_id;}
static int sceKernelWaitSema(int semaphore,int count,unsigned *timeout) {
    assert(semaphore==1 && count==1 && !timeout);
    ++gate_wait_calls;
    if(gate_wait_pause_at && gate_wait_calls==gate_wait_pause_at) {
        gate_wait_pause_at=0;
        if(gate_wait_hook) {
            void (*hook)(void)=gate_wait_hook;gate_wait_hook=NULL;hook();
        }
    }
    if(gate_wait_error) return gate_wait_error;
    if(gate_tokens>0) {--gate_tokens;return 0;}
    assert(0 && "gate wait with no token or wake hook");
    return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
}
static int sceKernelSignalSema(int semaphore,int count) {
    assert(semaphore==1 && count==1);
    ++gate_signal_calls;
    if(gate_signal_error) return gate_signal_error;
    ++gate_tokens;
    return 0;
}
static int session_enter(Session *s);
static int session_leave(Session *s);
static int session_held(void) {
    if(state.session_kernel) return session_locked!=0;
    return state.state_gate.depth!=0 && state.state_gate.owner==host_tls();
}
static void hold_session(void) {assert(session_enter(&state)==0);}
static void release_session(void) {assert(session_leave(&state)==0);}
static VPGuestThread *thread_for_executor(VPExecutor *e) {
    return e==recognized_worker?&worker:NULL;
}
static int svc_arrival_enabled(void) {return 0;}
static uint64_t sceKernelGetProcessTimeWide(void) {return (uint64_t)++m_clock*100;}
static int sceKernelCreateMutex(const char *name,int attr,int initial,void *unused) {
    (void)name;(void)attr;(void)initial;(void)unused;return ++next_id;
}
static int sceKernelTryLockMutex(int id,int count) {
    (void)count;
    if(id==state.state_lock) {
        ++m_state_try;
        if(state_try_error) return state_try_error;
        if(state_busy || session_locked) return SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN;
        session_locked=1;session_state_depth=1;return 0;
    }
    ++m_try;if(require_park) assert(!session_held());
    if(forced_try) {int result=forced_try;forced_try=0;return result;}
    if(force_busy || (host_owner[id] && host_owner[id]!=current_thread))
        return SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN;
    if(host_owner[id]==current_thread) return SCE_KERNEL_ERROR_MUTEX_RECURSIVE;
    host_owner[id]=current_thread;return 0;
}
static int sceKernelLockMutex(int id,int count,unsigned *timeout) {
    if(id==state.state_lock) {
        (void)count;++m_state_try;assert(!timeout);
        if(state_try_error) return state_try_error;
        if(session_locked) {++session_state_depth;return 0;}
        if(state_busy) {++m_state_waits;state_busy=0;}
        session_locked=1;session_state_depth=1;return 0;
    }
    (void)id;(void)count;++m_lock;assert(!session_held());assert(timeout && *timeout==1000000u);
    *timeout=0;return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
}
static int sceKernelUnlockMutex(int id,int count) {
    (void)count;
    if(id==state.state_lock) {
        ++m_state_unlock;assert(session_locked);
        if(state_unlock_error) return state_unlock_error;
        assert(session_state_depth>0);--session_state_depth;
        if(!session_state_depth) session_locked=0;
        return 0;
    }
    ++m_unlock;
    if(forced_unlock) {int result=forced_unlock;forced_unlock=0;return result;}
    if(host_owner[id]!=current_thread) return SCE_KERNEL_ERROR_MUTEX_NOT_OWNED;
    host_owner[id]=0;return 0;
}
static void park_begin(Session *s,VPPark *park) {
    assert(session_held());assert(s->locks[0].pending);
    park->thread=s->svc.current_thread_handle;park->tls=s->svc.tls_buffer;
    int rc=session_leave(s);assert(rc==0);++m_park;
    if(release_on_park) {
        Lock *lock=&s->locks[0];host_owner[lock->id]=0;
        lock->depth=0;lock->host_depth=0;lock->owner=0;lock->held=0;release_on_park=0;
    }
    if(park_hook) {void (*hook)(void)=park_hook;park_hook=NULL;hook();}
}
static void park_end(Session *s,const VPPark *park) {
    assert(!session_held());int rc=session_enter(s);assert(rc==0);
    s->svc.current_thread_handle=park->thread;s->svc.tls_buffer=park->tls;
}
static int writable(VPExecutor *e,Session *s,uint32_t address,unsigned size) {
    (void)e;(void)s;(void)address;(void)size;return memory_valid;
}
static int vp_guest_svc_readable(Svc *s,uint32_t address,unsigned size) {
    (void)s;(void)address;(void)size;return 0;
}
static int vp_guest_svc_writable(Svc *s,uint32_t address,unsigned size) {
    (void)s;return memory_valid && address!=denied_address && size<=4096
        && ((address>=0x10000u && address<=0x11000u-size)
            || (address>=0x20000u && address<=0x21000u-size));
}
static uint32_t word(const void *pointer) {(void)pointer;return 0;}
static int sceKernelCreateEventFlag(const char *name,int attr,int value,void *unused) {
    (void)name;(void)attr;(void)value;(void)unused;
    if(forced_event_create) return forced_event_create;
    for(unsigned i=1;i<=VP_KEY_WAIT_LIMIT;++i) if(!event_used[i]) {
        event_used[i]=1;event_bits[i]=0;++m_events;return (int)i;
    }
    assert(0);return -1;
}
static int sceKernelSetEventFlag(int event,int bits) {
    assert(event>0 && event<=VP_KEY_WAIT_LIMIT && event_used[event]);++m_event_sets;
    if(forced_event_set) {int rc=forced_event_set;forced_event_set=0;return rc;}
    event_bits[event]|=(unsigned)bits;return 0;
}
static int sceKernelDeleteEventFlag(int event) {
    assert(event>0 && event<=VP_KEY_WAIT_LIMIT && event_used[event]);
    event_used[event]=event_bits[event]=0;--m_events;return 0;
}
static int sceKernelWaitEventFlag(int event,int mask,int mode,unsigned *bits,unsigned *timeout) {
    (void)timeout;
    assert(!session_held());assert(state.locks[0].depth==0);
    assert(state.locks[0].host_depth==0);assert(host_owner[state.locks[0].id]==0);
    if(event_bits[event]&(unsigned)mask) {
        *bits=event_bits[event]&(unsigned)mask;
        if(mode&SCE_EVENT_WAITCLEAR) event_bits[event]=0;
        return 0;
    }
    return wait_result;
}
'''
service = block('if (f->service==0x232 || f->service==0x233 || f->service==0x245 || f->service==0x246)')
body = '\n'.join(block(sig) for sig in (
    'static unsigned sdk_lock_index(', 'static int sdk_lock_acquire(',
    'int vp_sdk_lock_fast('))
body += '\nstatic int dispatch_locked(VPExecutor *e,VPBridgeFrame *f) { Session *s=e->user;\n'
body += service + '\nreturn VP_BRIDGE_UNSUPPORTED;\n}\n'
body += '\n'.join(block(sig) for sig in (
    'static Lock *find_lock(', 'static int wait_guest_key(', 'static int signal_guest_key('))
session_body = '\n'.join(block(sig) for sig in (
    'static int session_enter(', 'static int session_leave('))
tests = r'''
static VPExecutor executor;
static unsigned address=0x10000u;
static int call(unsigned service,unsigned target) {
    VPBridgeFrame frame={0};frame.service=service;frame.regs[0]=target;
    int rc=dispatch_locked(&executor,&frame);
    if(!rc && (service==0x245 || service==0x246)) return (int)frame.regs[0];
    return rc;
}
static void reset_backend(int kernel) {
    if(state.log) fclose(state.log);
    memset(&state,0,sizeof(state));memset(host_owner,0,sizeof(host_owner));
    next_id=0;session_locked=0;current_thread=host_thread_id=0x142;session_state_depth=0;
    state.svc.current_thread_handle=current_thread;
    state.state_lock=512;state.start_handle=(uint32_t)current_thread;state.fast_enabled=1;
    state.main_executor=&executor;recognized_worker=NULL;worker.handle=0;
    state.log=tmpfile();assert(state.log);executor.user=&state;
    m_try=m_lock=m_unlock=m_park=m_clock=m_events=0;
    forced_try=forced_unlock=force_busy=wait_result=release_on_park=require_park=0;
    state_try_error=state_unlock_error=state_busy=m_state_try=m_state_unlock=0;memory_valid=1;
    denied_address=last_wait_result=0;forced_event_create=forced_event_set=m_event_sets=0;
    memset(event_used,0,sizeof(event_used));memset(event_bits,0,sizeof(event_bits));park_hook=NULL;
    gate_tokens=0;gate_wait_error=gate_signal_error=0;
    gate_wait_calls=gate_signal_calls=0;gate_wait_pause_at=0;gate_wait_hook=NULL;
    state.session_kernel=kernel;
    state.state_gate.semaphore=kernel?-1:1;
    memset((void *)(uintptr_t)address,0,4096);memset((void *)(uintptr_t)0x20000,0,4096);
    hold_session();
}
static void reset(void) {reset_backend(1);}
static void reset_gate(void) {reset_backend(0);}
static int keywait(uint64_t ns) {
    VPBridgeFrame frame={0};frame.regs[0]=address;frame.regs[1]=0x20000;
    frame.regs[3]=(uint32_t)ns;frame.regs[4]=(uint32_t)(ns>>32);
    int rc=wait_guest_key(&state,&frame);last_wait_result=frame.regs[0];return rc;
}
static int signal(uint32_t key,int32_t count,uint32_t *result) {
    VPBridgeFrame frame={0};frame.regs[0]=key;frame.regs[1]=(uint32_t)count;
    int rc=signal_guest_key(&state,&frame);if(result) *result=frame.regs[0];return rc;
}
static unsigned register_waiter(uint32_t key) {
    for(unsigned i=0;i<VP_KEY_WAIT_LIMIT;++i) if(!state.key_waits[i].used) {
        KeyWait *w=&state.key_waits[i];w->key=key;w->used=1;
        w->event=sceKernelCreateEventFlag("test",0,0,NULL);assert(w->event>0);
        ++state.key_waits_live;return i;
    }
    assert(0);return 0;
}
static void signal_after_release(void) {
    assert(!session_held() && state.key_waits_live==1 && state.key_waits[0].used);
    assert(state.locks[0].depth==0 && host_owner[state.locks[0].id]==0);
    assert(*(uint32_t *)(uintptr_t)0x20000==1);
    hold_session();assert(signal(0x20000,1,NULL)==0);release_session();
}
static void release_gate_from_other(void) {
    uint32_t saved=host_thread_id;
    host_thread_id=0x200;
    release_session();
    host_thread_id=saved;
}
static void release_gate_and_arm_signal_error(void) {
    uint32_t saved=host_thread_id;
    host_thread_id=0x200;
    release_session();
    host_thread_id=saved;
    gate_signal_error=-85;
}
int main(void) {
    void *page=VirtualAlloc((void *)(uintptr_t)address,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(page==(void *)(uintptr_t)address);
    void *key_page=VirtualAlloc((void *)(uintptr_t)0x20000,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(key_page==(void *)(uintptr_t)0x20000);
    reset();
    assert(call(0x232,address)==0);assert(state.locks[0].depth==1);
    assert(m_try==1 && m_lock==0 && m_park==0 && m_clock==0);
    assert(call(0x232,address)==0);assert(call(0x245,address)==1);
    assert(state.locks[0].depth==3 && state.locks[0].host_depth==1 && m_try==1);
    assert(call(0x246,address)==1);
    assert(call(0x233,address)==0);assert(call(0x233,address)==0);assert(m_unlock==0);
    assert(call(0x233,address)==0);assert(m_unlock==1 && !state.locks[0].depth && !state.locks[0].host_depth);
    assert(call(0x246,address)==0);assert(call(0x233,address)==SCE_KERNEL_ERROR_MUTEX_NOT_OWNED);
    puts("PASS mixed recursive Enter/TryLock/Leave uses one host hold; no fast-path clocks or park");

    reset();assert(call(0x232,address)==0);current_thread=0x148;state.svc.current_thread_handle=current_thread;
    assert(call(0x246,address)==0);assert(call(0x245,address)==0);
    assert(call(0x233,address)==SCE_KERNEL_ERROR_MUTEX_NOT_OWNED);
    assert(state.locks[0].owner==0x142 && state.locks[0].depth==1 && m_unlock==0);
    release_on_park=1;assert(call(0x232,address)==0);assert(m_park==1);
    assert(state.locks[0].owner==0x148 && state.locks[0].depth==1 && !state.locks[0].pending);
    puts("PASS other-owner refusal and parked acquisition preserves pending and transfers owner");

    reset();forced_try=-99;assert(call(0x232,address)==-99);
    assert(!state.locks[0].depth && !state.locks[0].pending && m_park==0 && m_lock==0);
    reset();forced_try=SCE_KERNEL_ERROR_MUTEX_RECURSIVE;
    assert(call(0x232,address)==SCE_KERNEL_ERROR_MUTEX_RECURSIVE);
    assert(!state.locks[0].depth && !state.locks[0].pending && m_lock==0);
    reset();forced_try=-98;assert(call(0x245,address)==-98);assert(!state.locks[0].depth);
    puts("PASS unexpected TryLock errors never become successful acquisition");

    reset();force_busy=1;assert(call(0x232,address)==VP_SDK_LOCK_DEADLOCK);
    assert(m_lock==5 && m_try<30 && !state.locks[0].pending && !state.locks[0].depth);
    puts("PASS 1 ms spin limit and five one-second waits even when kernel zeroes timeout");

    reset();assert(call(0x232,address)==0);assert(call(0x233,address)==0);
    state.locks[0].pending=1;require_park=1;m_try=0;m_park=0;
    assert(call(0x232,address)==0);assert(m_park==1 && m_try==1 && state.locks[0].pending==1);
    puts("PASS pending waiter excludes direct unparked TryLock");

    reset();assert(call(0x232,address)==0);assert(call(0x232,address)==0);assert(call(0x245,address)==1);
    assert(keywait(1000000)==0);assert(state.locks[0].depth==3 && state.locks[0].host_depth==1);
    assert(m_unlock==1 && m_try==2 && m_events==0 && !state.locks[0].pending);
    assert(call(0x233,address)==0);assert(call(0x233,address)==0);assert(call(0x233,address)==0);
    assert(m_unlock==2 && host_owner[1]==0);
    puts("PASS key wait drops one host hold and restores full mixed recursive guest depth");

    reset();assert(call(0x232,address)==0);assert(call(0x232,address)==0);
    assert(keywait(0)==0);assert(!state.locks[0].depth && !state.locks[0].host_depth);
    assert(!state.locks[0].pending && m_events==0);
    assert(call(0x232,address)==0);assert(state.locks[0].depth==1);
    puts("PASS timed-out key wait leaves ownership released for SDK re-entry");

    reset();assert(call(0x232,address)==0);assert(call(0x232,address)==0);
    forced_unlock=-97;assert(keywait(1000000)==-97);
    assert(state.locks[0].depth==2 && state.locks[0].host_depth==1 && state.locks[0].owner==0x142);
    assert(!state.locks[0].pending && m_events==0 && host_owner[1]==0x142);
    puts("PASS failed key-wait unlock preserves state and cleans event/pin");

    reset();assert(call(0x232,address)==0);state.locks[0].depth=2;
    forced_try=-96;assert(keywait(1000000)==-96);
    assert(!state.locks[0].depth && !state.locks[0].host_depth && !state.locks[0].pending && !m_events);
    puts("PASS key-wait reacquire failure preserves released state and real error");

    reset();
    for(unsigned i=0;i<VP_SDK_LOCK_LIMIT;++i) {state.locks[i].address=0x10000+4*i;state.locks[i].id=(int)i+1;state.locks[i].pending=1;}
    state.locks[2].pending=0;state.locks[3].pending=0;state.locks[3].host_depth=1;
    state.lock_hint[(state.locks[2].address>>2)&63u]=3;
    assert(call(0x232,0x30000)==0);assert(state.locks[2].address==0x30000);
    assert(state.locks[3].address==0x1000c && state.locks[0].address==0x10000);
    assert(find_lock(&state,0x10008)==NULL);assert(find_lock(&state,0x30000)==&state.locks[2]);
    puts("PASS table recycling protects pending/host holds and rejects stale address hints");

    reset();assert(call(0x232,address)==0);assert(call(0x233,address)==0);
    release_session();m_try=m_unlock=m_clock=m_park=0;
    assert(vp_sdk_lock_fast(address,&executor,0x232)==1);
    assert(state.locks[0].depth==1 && state.locks[0].host_depth==1 && !session_held());
    assert(vp_sdk_lock_fast(address,&executor,0x232)==1);
    assert(vp_sdk_lock_fast(address,&executor,0x245)==1);
    assert(vp_sdk_lock_fast(address,&executor,0x246)==1);
    assert(state.locks[0].depth==3 && m_try==1 && m_clock==0 && m_park==0);
    assert(vp_sdk_lock_fast(address,&executor,0x233)==1);
    assert(vp_sdk_lock_fast(address,&executor,0x233)==1);assert(m_unlock==0);
    assert(vp_sdk_lock_fast(address,&executor,0x233)==1);assert(m_unlock==1);
    assert(vp_sdk_lock_fast(address,&executor,0x246)==2);
    assert(state.fast_enter==2 && state.fast_try==1 && state.fast_leave==3 && state.fast_islocked==2);
    puts("PASS fast main uncontended and recursive paths preserve single host hold and avoid clocks/park");

    reset();assert(call(0x232,address)==0);release_session();
    VPExecutor worker_executor={0};worker_executor.user=&state;
    recognized_worker=&worker_executor;worker.handle=0x148;current_thread=0x148;
    assert(vp_sdk_lock_fast(address,&worker_executor,0x246)==2);
    assert(vp_sdk_lock_fast(address,&worker_executor,0x232)==0);
    assert(vp_sdk_lock_fast(address,&worker_executor,0x233)==0);
    assert(vp_sdk_lock_fast(address,&worker_executor,0x245)==0);
    hold_session();state.svc.current_thread_handle=0x148;
    assert(call(0x245,address)==0);release_session();
    current_thread=0x142;assert(vp_sdk_lock_fast(address,&executor,0x233)==1);
    current_thread=0x148;assert(vp_sdk_lock_fast(address,&worker_executor,0x232)==1);
    assert(state.locks[0].owner==0x148);assert(vp_sdk_lock_fast(address,&worker_executor,0x233)==1);
    puts("PASS fast worker uses its own owner; other-owner Enter/Leave/TryLock fall back to real slow result");

    reset();assert(call(0x232,address)==0);assert(call(0x233,address)==0);release_session();
    VPExecutor spare={0};spare.user=&state;
    assert(vp_sdk_lock_fast(address,&spare,0x232)==0);
    recognized_worker=&spare;worker.handle=0;
    assert(vp_sdk_lock_fast(address,&spare,0x232)==0);
    assert(vp_sdk_lock_fast(address+4,&executor,0x232)==0);
    assert(vp_sdk_lock_fast(address+1,&executor,0x232)==0);
    memory_valid=0;assert(vp_sdk_lock_fast(address,&executor,0x232)==0);memory_valid=1;
    state.locks[0].pending=1;assert(vp_sdk_lock_fast(address,&executor,0x232)==0);state.locks[0].pending=0;
    state.stopping=1;assert(vp_sdk_lock_fast(address,&executor,0x232)==0);state.stopping=0;
    state.fast_enabled=0;assert(vp_sdk_lock_fast(address,&executor,0x232)==0);state.fast_enabled=1;
    state_busy=1;int waits=m_state_waits;
    assert(vp_sdk_lock_fast(address,&executor,0x232)==1);
    assert(m_state_waits==waits+1 && !state_busy && !session_held());
    assert(vp_sdk_lock_fast(address,&executor,0x233)==1);
    assert(!state.locks[0].depth && !session_held());
    puts("PASS fast unsupported inputs fall back; Session contention waits before acquiring the guest lock");

    reset();assert(call(0x232,address)==0);assert(call(0x233,address)==0);release_session();
    int unlocks=m_state_unlock;
    state_try_error=-88;assert(vp_sdk_lock_fast(address,&executor,0x232)==-88);
    assert(m_state_unlock==unlocks && !state.locks[0].depth);state_try_error=0;
    forced_try=-87;assert(vp_sdk_lock_fast(address,&executor,0x232)==-87);
    assert(!state.locks[0].depth && !session_held());
    forced_try=SCE_KERNEL_ERROR_MUTEX_RECURSIVE;
    assert(vp_sdk_lock_fast(address,&executor,0x245)==SCE_KERNEL_ERROR_MUTEX_RECURSIVE);
    assert(!state.locks[0].depth && !session_held());
    assert(vp_sdk_lock_fast(address,&executor,0x232)==1);
    forced_unlock=-86;assert(vp_sdk_lock_fast(address,&executor,0x233)==-86);
    assert(state.locks[0].depth==1 && state.locks[0].host_depth==1 && !session_held());
    state_unlock_error=-85;assert(vp_sdk_lock_fast(address,&executor,0x246)==-85);
    assert(state.locks[0].depth==1 && session_held());
    puts("PASS fast unexpected session/host mutex errors return exact errors without retry or false success");

    reset();assert(call(0x232,address)==0);release_session();
    assert(vp_sdk_lock_fast(address,&executor,0x245)==1);assert(state.locks[0].depth==2);
    hold_session();assert(call(0x232,address)==0);assert(state.locks[0].depth==3);release_session();
    assert(vp_sdk_lock_fast(address,&executor,0x233)==1);
    hold_session();assert(call(0x233,address)==0);release_session();
    assert(vp_sdk_lock_fast(address,&executor,0x233)==1);
    assert(state.locks[0].depth==0 && state.locks[0].host_depth==0 && m_unlock==1);
    hold_session();assert(call(0x232,address)==0);release_session();
    assert(vp_sdk_lock_fast(address,&executor,0x232)==1);
    hold_session();assert(keywait(1000000)==0);release_session();
    assert(state.locks[0].depth==2 && state.locks[0].host_depth==1);
    assert(vp_sdk_lock_fast(address,&executor,0x233)==1);
    hold_session();assert(call(0x233,address)==0);
    puts("PASS slow-fast mixed ownership and key wait restore identical guest/host depths");

    reset();*(uint32_t *)(uintptr_t)0x20000=99;
    assert(signal(0x20000,1,NULL)==0);
    assert(*(uint32_t *)(uintptr_t)0x20000==0 && !m_event_sets && !state.key_waits_live);
    assert(call(0x232,address)==0);assert(call(0x232,address)==0);
    wait_result=SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    assert(keywait(1000000)==0 && last_wait_result==0xEA01);
    assert(!state.key_wakes && state.key_timeouts==1 && !state.locks[0].depth && !m_events);
    assert(*(uint32_t *)(uintptr_t)0x20000==1);
    assert(call(0x232,address)==0 && state.locks[0].depth==1);
    assert(signal(0x20000,1,NULL)==0 && *(uint32_t *)(uintptr_t)0x20000==0);
    puts("PASS signal before wait grants no future credit; timeout preserves SDK re-entry and conservative key hint");

    reset();assert(call(0x232,address)==0);assert(call(0x232,address)==0);
    assert(call(0x245,address)==1);park_hook=signal_after_release;
    wait_result=SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    assert(keywait(1000000)==0 && last_wait_result==0);
    assert(state.key_wakes==1 && !state.key_timeouts && !state.key_waits_live && !m_events);
    assert(m_event_sets==1 && *(uint32_t *)(uintptr_t)0x20000==0);
    assert(state.locks[0].depth==3 && state.locks[0].host_depth==1 && !state.locks[0].pending);
    puts("PASS registered waiter retains signal between mutex release and kernel wait; full recursion is restored");

    reset();unsigned first=register_waiter(0x20000),second=register_waiter(0x20000);
    unsigned third=register_waiter(0x20000),other=register_waiter(0x20004);
    *(uint32_t *)(uintptr_t)0x20000=1;*(uint32_t *)(uintptr_t)0x20004=1;
    assert(signal(0x20000,1,NULL)==0 && m_event_sets==1);
    assert(state.key_waits[first].signaled && !state.key_waits[second].signaled);
    assert(!state.key_waits[third].signaled && !state.key_waits[other].signaled);
    assert(*(uint32_t *)(uintptr_t)0x20000==1);
    assert(signal(0x20000,20,NULL)==0 && m_event_sets==3);
    assert(state.key_waits[second].signaled && state.key_waits[third].signaled);
    assert(!state.key_waits[other].signaled && *(uint32_t *)(uintptr_t)0x20000==0);
    assert(*(uint32_t *)(uintptr_t)0x20004==1);
    assert(signal(0x20000,1,NULL)==0 && m_event_sets==3);
    assert(call(0x232,address)==0);wait_result=SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    assert(keywait(1000000)==0 && last_wait_result==0xEA01);
    puts("PASS count limits and excess signals affect only current unsignaled waiters on the selected key");

    reset();first=register_waiter(0x20000);second=register_waiter(0x20000);
    other=register_waiter(0x20004);*(uint32_t *)(uintptr_t)0x20004=1;
    assert(signal(0x20000,-1,NULL)==0 && m_event_sets==2);
    assert(state.key_waits[first].signaled && state.key_waits[second].signaled);
    assert(!state.key_waits[other].signaled && *(uint32_t *)(uintptr_t)0x20000==0);
    third=register_waiter(0x20000);assert(!state.key_waits[third].signaled);
    assert(!event_bits[state.key_waits[third].event]);
    assert(signal(0x20000,0,NULL)==0 && m_event_sets==3);
    assert(state.key_waits[third].signaled && !state.key_waits[other].signaled);
    puts("PASS Broadcast and zero count wake all current waiters without granting credits to new waiters");

    reset();first=register_waiter(0x20000);second=register_waiter(0x20000);
    forced_event_set=-79;uint32_t result=0;
    assert(signal(0x20000,-1,&result)==-79 && result==(uint32_t)-79);
    assert(!state.key_waits[first].signaled && !state.key_waits[second].signaled);
    assert(!event_bits[state.key_waits[first].event] && *(uint32_t *)(uintptr_t)0x20000==1);
    assert(signal(0x20000,1,NULL)==0 && state.key_waits[first].signaled);
    forced_event_set=-78;assert(signal(0x20000,-1,&result)==-78 && result==(uint32_t)-78);
    assert(!state.key_waits[second].signaled && *(uint32_t *)(uintptr_t)0x20000==1);
    puts("PASS event signal errors propagate exactly and leave failed waiters eligible without false success");

    reset();assert(call(0x232,address)==0);assert(call(0x232,address)==0);
    *(uint32_t *)(uintptr_t)0x20000=0xfeed;denied_address=0x20000;
    assert(keywait(1000000)==0 && last_wait_result==0xD401);
    assert(signal(0x20000,1,&result)==0 && result==0xD401);
    assert(state.locks[0].depth==2 && !m_events && !m_event_sets);
    assert(*(uint32_t *)(uintptr_t)0x20000==0xfeed && !state.key_waits_live);
    denied_address=0;forced_event_create=-77;
    assert(keywait(1000000)==0 && last_wait_result==(uint32_t)-77);
    assert(state.locks[0].depth==2 && state.locks[0].host_depth==1 && !state.locks[0].pending);
    assert(!m_events && !state.key_waits_live && *(uint32_t *)(uintptr_t)0x20000==0xfeed);
    puts("PASS key permission and event creation failures precede writes and preserve existing recursive ownership");

    reset_gate();
    assert(session_held() && state.state_gate.owner==0x142 && state.state_gate.depth==1);
    assert(call(0x232,address)==0);release_session();
    int kernel_tries=m_state_try;
    assert(vp_sdk_lock_fast(address,&executor,0x232)==1);
    assert(vp_sdk_lock_fast(address,&executor,0x245)==1);
    assert(vp_sdk_lock_fast(address,&executor,0x246)==1);
    assert(vp_sdk_lock_fast(address,&executor,0x233)==1);
    assert(vp_sdk_lock_fast(address,&executor,0x233)==1);
    assert(vp_sdk_lock_fast(address,&executor,0x233)==1);
    assert(!state.locks[0].depth && !session_held());
    assert(m_state_try==kernel_tries && gate_wait_calls==0 && gate_signal_calls==0);
    puts("PASS user-gate backend recursion and fast helper never enter the kernel mutex backend");

    reset_gate();assert(call(0x232,address)==0);assert(call(0x233,address)==0);
    release_session();
    host_thread_id=0x200;assert(session_enter(&state)==0);
    host_thread_id=0x142;
    gate_wait_pause_at=1;gate_wait_hook=release_gate_from_other;
    assert(vp_sdk_lock_fast(address,&executor,0x232)==1);
    assert(state.locks[0].depth==1 && state.locks[0].owner==0x142);
    assert(gate_wait_calls==1 && gate_signal_calls==2 && gate_tokens==1);
    assert(!session_held());
    assert(vp_sdk_lock_fast(address,&executor,0x233)==1);
    puts("PASS user-gate contention retains the wake and a stale token cannot bypass the state check");

    reset_gate();assert(call(0x232,address)==0);assert(call(0x233,address)==0);
    release_session();
    host_thread_id=0x200;assert(session_enter(&state)==0);
    host_thread_id=0x142;
    gate_wait_error=-88;
    assert(vp_sdk_lock_fast(address,&executor,0x232)==-88);
    assert(!state.locks[0].depth && !state.locks[0].pending && !session_held());
    assert(gate_wait_calls==1 && gate_signal_calls==0);
    host_thread_id=0x200;release_session();host_thread_id=0x142;
    assert(gate_signal_calls==1 && gate_tokens==1);
    gate_wait_error=0;
    puts("PASS user-gate wait error is returned exactly before any guest lock is acquired");

    reset_gate();assert(call(0x232,address)==0);assert(call(0x233,address)==0);
    release_session();
    host_thread_id=0x200;assert(session_enter(&state)==0);
    host_thread_id=0x142;
    gate_wait_pause_at=1;gate_wait_hook=release_gate_and_arm_signal_error;
    assert(vp_sdk_lock_fast(address,&executor,0x232)==-85);
    assert(state.locks[0].depth==1 && !state.locks[0].pending);
    assert(!session_held());
    assert(state.state_gate.state==0 && state.state_gate.owner==0 && state.state_gate.depth==0);
    assert(gate_wait_calls==1 && gate_signal_calls==2);
    gate_signal_error=0;
    hold_session();assert(call(0x233,address)==0);release_session();
    puts("PASS user-gate signal error releases ownership but returns the exact error");

    reset_gate();assert(call(0x232,address)==0);assert(call(0x232,address)==0);
    wait_result=SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    assert(keywait(1000000)==0 && last_wait_result==0xEA01);
    assert(!state.locks[0].depth && !state.locks[0].host_depth && session_held());
    assert(gate_wait_calls==0 && gate_signal_calls==0);
    reset_gate();assert(call(0x232,address)==0);assert(call(0x232,address)==0);
    assert(call(0x245,address)==1);park_hook=signal_after_release;
    wait_result=SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    assert(keywait(1000000)==0 && last_wait_result==0);
    assert(state.key_wakes==1 && !state.key_waits_live && state.locks[0].depth==3);
    assert(session_held());
    puts("PASS user-gate park releases and restores the session across key waits and signaller dispatch");

    reset();assert(call(0x232,address)==0);assert(call(0x233,address)==0);
    release_session();
    m_state_try=0;unsigned gate_waits=gate_wait_calls;
    assert(vp_sdk_lock_fast(address,&executor,0x232)==1);
    assert(m_state_try>0 && gate_wait_calls==gate_waits);
    assert(vp_sdk_lock_fast(address,&executor,0x233)==1);
    puts("PASS kernel-mutex rollback backend stays available and is selected only by session_kernel");

    fclose(state.log);VirtualFree(page,0,MEM_RELEASE);VirtualFree(key_page,0,MEM_RELEASE);return 0;
}
'''
provenance = f"/* Source: src/loader/sdk_dso.c; SHA256: {source_hash} */\n"
(directory / "sdk_lock_regression.c").write_text(
    provenance + prefix + gate_source + source[lock_start:lock_end] + types
    + session_body + body + tests,
    encoding="utf-8", newline="\n")
(directory / "CMakeLists.txt").write_text("""cmake_minimum_required(VERSION 3.20)
project(sdk_lock_regression C)
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "SDK lock host simulation requires Windows and MSVC")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()
add_executable(sdk_lock_regression sdk_lock_regression.c)
target_compile_options(sdk_lock_regression PRIVATE /W4 /UNDEBUG /wd4996)
add_test(NAME sdk_lock_regression COMMAND sdk_lock_regression)
""", encoding="utf-8", newline="\n")
print(f"Generated SDK lock host simulation in {directory}")
print(f"Production source SHA256: {source_hash}")

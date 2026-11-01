#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <windows.h>

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

static HANDLE semaphore, waiting, allow_wait, finished, start;
static volatile LONG wait_calls, signal_calls;
static int wait_error, signal_error, pause_before_wait, pause_on_wait;
static int sceKernelWaitSema(int id, int count, unsigned *timeout) {
    assert(id == 7 && count == 1 && !timeout);
    LONG call = InterlockedIncrement(&wait_calls);
    if (wait_error) return wait_error;
    if (pause_before_wait || (pause_on_wait && call == pause_on_wait)) {
        assert(SetEvent(waiting));
        assert(WaitForSingleObject(allow_wait, 5000) == WAIT_OBJECT_0);
    }
    return WaitForSingleObject(semaphore, 5000) == WAIT_OBJECT_0 ? 0 : -90;
}
static int sceKernelSignalSema(int id, int count) {
    assert(id == 7 && count == 1);
    InterlockedIncrement(&signal_calls);
    if (signal_error) return signal_error;
    return ReleaseSemaphore(semaphore, count, NULL) ? 0 : -91;
}
#include "session_gate.h"

static VPSessionGate gate;
static unsigned shared_count;
static void reset(void) {
    if (semaphore) assert(CloseHandle(semaphore));
    semaphore = CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL);
    assert(semaphore);
    gate = (VPSessionGate){0, 0, 0, 7};
    shared_count = 0;
    wait_calls = signal_calls = 0;
    wait_error = signal_error = pause_before_wait = pause_on_wait = 0;
}
static DWORD WINAPI contender(void *unused) {
    (void)unused;
    uint32_t owner = GetCurrentThreadId();
    assert(vp_session_gate_enter(&gate, owner) == 0);
    assert(gate.depth == 1);
    ++shared_count;
    assert(vp_session_gate_leave(&gate, owner) == 0);
    assert(SetEvent(finished));
    return 0;
}
static DWORD WINAPI stress(void *unused) {
    (void)unused;
    uint32_t owner = GetCurrentThreadId();
    assert(WaitForSingleObject(start, 5000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < 10000u; ++i) {
        assert(vp_session_gate_enter(&gate, owner) == 0);
        assert(vp_session_gate_enter(&gate, owner) == 0);
        assert(gate.depth == 2);
        ++shared_count;
        if ((i & 31u) == 0u) SwitchToThread();
        assert(vp_session_gate_leave(&gate, owner) == 0);
        assert(gate.depth == 1);
        assert(vp_session_gate_leave(&gate, owner) == 0);
    }
    return 0;
}
int main(void) {
    uint32_t owner = GetCurrentThreadId();
    waiting = CreateEventA(NULL, TRUE, FALSE, NULL);
    allow_wait = CreateEventA(NULL, TRUE, FALSE, NULL);
    finished = CreateEventA(NULL, TRUE, FALSE, NULL);
    assert(waiting && allow_wait && finished);
    reset();
    assert(vp_session_gate_enter(&gate, 0) == -1);
    assert(vp_session_gate_enter(&gate, owner) == 0);
    assert(vp_session_gate_enter(&gate, owner) == 0);
    assert(gate.depth == 2 && wait_calls == 0);
    assert(vp_session_gate_leave(&gate, owner + 1u) == -1);
    assert(vp_session_gate_leave(&gate, owner) == 0 && gate.depth == 1);
    assert(vp_session_gate_leave(&gate, owner) == 0 && gate.state == 0);
    assert(vp_session_gate_leave(&gate, owner) == -1);
    assert(signal_calls == 0);

    reset();
    assert(vp_session_gate_enter(&gate, owner) == 0);
    pause_before_wait = 1;
    HANDLE thread = CreateThread(NULL, 0, contender, NULL, 0, NULL);
    assert(thread && WaitForSingleObject(waiting, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(finished, 0) == WAIT_TIMEOUT);
    assert(vp_session_gate_leave(&gate, owner) == 0);
    /* Release precedes the actual semaphore wait: the wake must be retained. */
    assert(SetEvent(allow_wait));
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    assert(shared_count == 1 && wait_calls == 1 && signal_calls == 2);
    assert(CloseHandle(thread));

    reset();
    /* A token retained without a waiter must not make a contender believe it
     * owns the gate while the owner still holds it. The first wait consumes
     * the stale token and the second wait parks. */
    assert(ReleaseSemaphore(semaphore, 1, NULL));
    assert(vp_session_gate_enter(&gate, owner) == 0);
    assert(wait_calls == 0);
    ResetEvent(waiting);
    ResetEvent(allow_wait);
    ResetEvent(finished);
    pause_on_wait = 2;
    thread = CreateThread(NULL, 0, contender, NULL, 0, NULL);
    assert(thread && WaitForSingleObject(waiting, 5000) == WAIT_OBJECT_0);
    assert(shared_count == 0);
    assert(InterlockedCompareExchange(&wait_calls, 0, 0) == 2);
    assert(gate.state == 2 && gate.owner == owner);
    assert(vp_session_gate_leave(&gate, owner) == 0);
    assert(SetEvent(allow_wait));
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    assert(shared_count == 1 && wait_calls == 2 && signal_calls == 2);
    assert(gate.state == 0 && gate.owner == 0 && gate.depth == 0);
    assert(CloseHandle(thread));

    reset();
    start = CreateEventA(NULL, TRUE, FALSE, NULL);
    assert(start);
    HANDLE threads[8];
    for (unsigned i = 0; i < 8u; ++i) {
        threads[i] = CreateThread(NULL, 0, stress, NULL, 0, NULL);
        assert(threads[i]);
    }
    assert(SetEvent(start));
    assert(WaitForMultipleObjects(8, threads, TRUE, 15000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < 8u; ++i) assert(CloseHandle(threads[i]));
    assert(shared_count == 80000u && gate.state == 0 && gate.owner == 0 && gate.depth == 0);
    assert(wait_calls > 0 && signal_calls > 0);
    assert(CloseHandle(start));

    reset();
    gate.state = 1;gate.owner = owner + 1u;gate.depth = 1;
    wait_error = -77;
    assert(vp_session_gate_enter(&gate, owner) == -77);
    assert(gate.owner == owner + 1u && gate.depth == 1);
    gate.state = 2;gate.owner = owner;signal_error = -78;
    assert(vp_session_gate_leave(&gate, owner) == -78);
    assert(gate.state == 0 && gate.owner == 0);
    assert(CloseHandle(semaphore));
    assert(CloseHandle(waiting) && CloseHandle(allow_wait) && CloseHandle(finished));
    puts("PASS Session gate recursion, retained wake, stale-token recheck, 8-thread exclusion and exact errors");
    return 0;
}

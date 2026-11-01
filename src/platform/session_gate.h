#ifndef VP_SESSION_GATE_H
#define VP_SESSION_GATE_H

#include <stdint.h>
#include <stddef.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

#if defined(__GNUC__) && (defined(__arm__) || defined(__thumb__))
#define VP_GATE_INTEGER_ONLY __attribute__((target("general-regs-only")))
#else
#define VP_GATE_INTEGER_ONLY
#endif

/* A recursive host lock. Owner must identify the actual host thread, including
 * callbacks outside a guest executor. State: 0 free, 1 held, 2 contended.
 * The semaphore retains an unlock that races with the waiter's kernel entry.
 * Initialize and destroy only while all users are quiescent. */
typedef struct {
    uint32_t state;
    uint32_t owner;
    unsigned depth;
    int semaphore;
} VPSessionGate;

VP_GATE_INTEGER_ONLY
static inline int vp_session_gate_enter(VPSessionGate *gate, uint32_t owner) {
    if (!owner) return -1;
    if (__atomic_load_n(&gate->owner, __ATOMIC_RELAXED) == owner) {
        ++gate->depth;
        return 0;
    }
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&gate->state, &expected, 1u, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        while (__atomic_exchange_n(&gate->state, 2u, __ATOMIC_ACQUIRE) != 0u) {
            int rc = sceKernelWaitSema(gate->semaphore, 1, NULL);
            if (rc) return rc;
        }
    }
    gate->depth = 1;
    __atomic_store_n(&gate->owner, owner, __ATOMIC_RELAXED);
    return 0;
}

VP_GATE_INTEGER_ONLY
static inline int vp_session_gate_leave(VPSessionGate *gate, uint32_t owner) {
    if (!owner || __atomic_load_n(&gate->owner, __ATOMIC_RELAXED) != owner)
        return -1;
    if (--gate->depth) return 0;
    __atomic_store_n(&gate->owner, 0u, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&gate->state, 0u, __ATOMIC_RELEASE) == 2u)
        return sceKernelSignalSema(gate->semaphore, 1);
    return 0;
}

#endif

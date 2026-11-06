#ifndef VP_WORK_LOG_H
#define VP_WORK_LOG_H

#include <stdint.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

/* Normal records are explicitly selected at audited successful call sites.
 * Error, refusal and evidence records never enter this policy. Use 32-bit
 * atomics: SVC producers can overlap and ARM must not require libatomic64. */
typedef struct {
    uint32_t emitted;
    uint32_t suppressed;
} VPWorkLogStats;

static inline void vp_work_log_increment(uint32_t *counter) {
#ifdef _MSC_VER
    _InterlockedIncrement((volatile long *)counter);
#else
    __atomic_fetch_add(counter, 1u, __ATOMIC_RELAXED);
#endif
}

static inline uint32_t vp_work_log_load(const uint32_t *counter) {
#ifdef _MSC_VER
    return (uint32_t)_InterlockedCompareExchange((volatile long *)counter, 0, 0);
#else
    return __atomic_load_n(counter, __ATOMIC_RELAXED);
#endif
}

static inline void vp_work_log_snapshot(const VPWorkLogStats *stats,
                                        VPWorkLogStats *out) {
    out->emitted = vp_work_log_load(&stats->emitted);
    out->suppressed = vp_work_log_load(&stats->suppressed);
}

static inline int vp_work_log_emit(VPWorkLogStats *stats, unsigned reduce) {
    if (reduce) {
        vp_work_log_increment(&stats->suppressed);
        return 0;
    }
    vp_work_log_increment(&stats->emitted);
    return 1;
}

#endif

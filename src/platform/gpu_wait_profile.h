#ifndef VP_GPU_WAIT_PROFILE_H
#define VP_GPU_WAIT_PROFILE_H

#include <stdint.h>
#include <string.h>

/* Keep all causes in every snapshot, including zero rows. Bucket boundaries
 * describe API blocking, not GPU execution time. */
static const char *const vp_gpu_wait_names[] = {
    "submit-end", "limit", "budget", "scope", "rt-change", "depth-change",
    "mode-change", "ineligible", "clear", "present", "readback", "resource",
    "failure", "legacy", "arena-reuse", "texture-pool", "surface-release",
    "depth-destroy", "resident-evict", "pair-release", "surface-recycle",
    "abort", "frame-finish", "refresh", "sync-scene", "sync-blit",
    "clear-materialize", "other"
};
#define VP_GPU_WAIT_CAUSES (sizeof(vp_gpu_wait_names) / sizeof(vp_gpu_wait_names[0]))
#define VP_GPU_WAIT_BUCKETS 5u

typedef struct {
    uint64_t calls, real_waits, polls, failures, block_us;
    uint64_t pending_sum, draws, synchronous;
    uint64_t buckets[VP_GPU_WAIT_BUCKETS];
} VPGpuWaitCause;

typedef struct {
    VPGpuWaitCause causes[VP_GPU_WAIT_CAUSES];
} VPGpuWaitProfile;

static inline unsigned vp_gpu_wait_cause(const char *name) {
    unsigned i;
    for (i = 0u; i + 1u < VP_GPU_WAIT_CAUSES; ++i)
        if (strcmp(name, vp_gpu_wait_names[i]) == 0) return i;
    return (unsigned)VP_GPU_WAIT_CAUSES - 1u;
}

static inline void vp_gpu_wait_record(VPGpuWaitProfile *profile,
    const char *name, uint64_t started, uint64_t finished, int real_wait,
    int rc, unsigned pending, unsigned draws, int synchronous) {
    VPGpuWaitCause *cause;
    uint64_t blocked;
    unsigned bucket;
    if (started == 0u) return; /* Existing play-perf timestamp gate. */
    cause = &profile->causes[vp_gpu_wait_cause(name)];
    blocked = real_wait ? finished - started : 0u;
    cause->calls++;
    cause->real_waits += real_wait ? 1u : 0u;
    cause->polls += real_wait ? 0u : 1u;
    cause->failures += rc < 0 ? 1u : 0u;
    cause->block_us += blocked;
    cause->pending_sum += pending;
    cause->draws += draws;
    cause->synchronous += synchronous ? 1u : 0u;
    if (!real_wait) return;
    bucket = blocked < 100u ? 0u : blocked < 1000u ? 1u :
             blocked < 5000u ? 2u : blocked < 20000u ? 3u : 4u;
    cause->buckets[bucket]++;
}

#endif

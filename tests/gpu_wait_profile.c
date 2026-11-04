#include "gpu_wait_profile.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    VPGpuWaitProfile profile = {0};
    VPGpuWaitCause *cause = &profile.causes[vp_gpu_wait_cause("arena-reuse")];
    vp_gpu_wait_record(&profile, "arena-reuse", 0u, 10u, 1, 0, 3, 12, 0);
    assert(cause->calls == 0u);
    vp_gpu_wait_record(&profile, "arena-reuse", 10u, 110u, 1, 0, 3, 12, 0);
    vp_gpu_wait_record(&profile, "arena-reuse", 110u, 115u, 0, 0, 2, 5, 0);
    vp_gpu_wait_record(&profile, "arena-reuse", 120u, 5120u, 1, -7, 2, 5, 0);
    assert(cause->calls == 3u && cause->real_waits == 2u && cause->polls == 1u);
    assert(cause->failures == 1u && cause->block_us == 5100u);
    assert(cause->pending_sum == 7u && cause->draws == 22u);
    assert(cause->buckets[1] == 1u && cause->buckets[3] == 1u);
    vp_gpu_wait_record(&profile, "sync-blit", 1u, 20001u, 1, 0, 0, 0, 1);
    cause = &profile.causes[vp_gpu_wait_cause("sync-blit")];
    assert(cause->synchronous == 1u && cause->buckets[4] == 1u);
    /* The mode-6 materialization drain is a named cause, not the fallback
     * "other" row, so its waits reconcile against the global total. */
    assert(vp_gpu_wait_cause("clear-materialize") !=
           vp_gpu_wait_cause("unrecognized"));
    vp_gpu_wait_record(&profile, "clear-materialize", 5u, 65u, 1, 0, 1, 2, 0);
    cause = &profile.causes[vp_gpu_wait_cause("clear-materialize")];
    assert(cause->calls == 1u && cause->real_waits == 1u &&
           cause->block_us == 60u && cause->buckets[0] == 1u);
    assert(vp_gpu_wait_cause("unrecognized") == VP_GPU_WAIT_CAUSES - 1u);
    assert(sizeof(profile) < 4096u);
    puts("gpu_wait_profile: all checks passed");
    return 0;
}

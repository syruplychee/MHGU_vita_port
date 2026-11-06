#include "work_log.h"
#include "engine_full.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    VPWorkLogStats stats = {0};
    unsigned formatted = 0u;
    for (unsigned round = 0u; round < 4u; ++round) {
        unsigned mode = round & 1u ? 9u : 4u;
        for (unsigned record = 0u; record < 100u; ++record) {
            if (vp_work_log_emit(&stats,
                    vp_engine_ab_mode_reduces_work_log(mode))) formatted++;
        }
    }
    assert(stats.emitted == 200u && stats.suppressed == 200u);
    assert(formatted == 200u);
    for (unsigned mode = 0u; mode < 9u; ++mode)
        assert(!vp_engine_ab_mode_reduces_work_log(mode));
    assert(vp_engine_ab_mode_texture_memory(9u) == 0u);
    assert(vp_engine_ab_mode_uses_deep_texture(9u));
    assert(!vp_engine_ab_mode_uses_idle_resize(9u));
    assert(!vp_engine_ab_mode_uses_clear_defer(9u));
    puts("PASS normal record accounting and isolated mode-9 switch");
    return 0;
}

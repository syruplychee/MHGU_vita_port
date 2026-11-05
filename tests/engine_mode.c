/* Host regression for the A/B control-file contract and mode names.
 *
 * The loader and this test include the same inline helpers from
 * engine_full.h; this is not a copy of the validation logic. */
#include "engine_full.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* Exactly one digit 0..8 plus trailing whitespace is valid. */
    for (int ch = '0'; ch <= '8'; ++ch)
        assert(vp_engine_ab_control_digit_valid(ch));
    assert(!vp_engine_ab_control_digit_valid('9'));
    assert(!vp_engine_ab_control_digit_valid('/'));
    assert(!vp_engine_ab_control_digit_valid(':'));
    assert(!vp_engine_ab_control_digit_valid(' '));
    assert(!vp_engine_ab_control_digit_valid('\n'));
    assert(!vp_engine_ab_control_digit_valid(-1)); /* fgetc EOF */

    for (unsigned mode = 0u; mode <= VP_ENGINE_AB_MODE_MAX; ++mode)
        assert(vp_engine_ab_mode_valid(mode));
    assert(!vp_engine_ab_mode_valid(VP_ENGINE_AB_MODE_MAX + 1u));

    assert(strcmp(vp_engine_ab_mode_name(0u), "original") == 0);
    assert(strcmp(vp_engine_ab_mode_name(1u), "native") == 0);
    assert(strcmp(vp_engine_ab_mode_name(2u), "fast") == 0);
    assert(strcmp(vp_engine_ab_mode_name(3u), "shared") == 0);
    assert(strcmp(vp_engine_ab_mode_name(4u), "deep") == 0);
    assert(strcmp(vp_engine_ab_mode_name(5u), "deep-recycle") == 0);
    assert(strcmp(vp_engine_ab_mode_name(6u), "deep-clear") == 0);
    assert(strcmp(vp_engine_ab_mode_name(7u), "deep-large") == 0);
    assert(strcmp(vp_engine_ab_mode_name(8u), "deep-split") == 0);
    assert(strcmp(vp_engine_ab_mode_name(9u), "unknown") == 0);

    /* Modes 5/6 add idle resize; only 6 defers clear. Modes 7/8 are deep but
     * do not combine either of those changes. */
    for (unsigned mode = 0u; mode <= 8u; ++mode) {
        int deep = vp_engine_ab_mode_uses_deep_texture(mode);
        int idle = vp_engine_ab_mode_uses_idle_resize(mode);
        int defer = vp_engine_ab_mode_uses_clear_defer(mode);
        unsigned memory = vp_engine_ab_mode_texture_memory(mode);
        assert(deep == (mode >= 4u));
        assert(idle == (mode == 5u || mode == 6u));
        assert(defer == (mode == 6u));
        /* 0 = 64 MiB unified, 1 = 128 MiB unified, 2 = 64 MiB split. */
        assert(memory == (mode == 7u ? 1u : mode == 8u ? 2u : 0u));
        assert(deep || memory == 0u);
        assert(mode != 7u || (!idle && !defer));
        assert(mode != 8u || (!idle && !defer));
    }
    assert(!vp_engine_ab_mode_uses_deep_texture(9u));
    assert(!vp_engine_ab_mode_uses_idle_resize(7u));
    assert(!vp_engine_ab_mode_uses_idle_resize(8u));
    assert(!vp_engine_ab_mode_uses_clear_defer(7u));
    assert(!vp_engine_ab_mode_uses_clear_defer(8u));

    puts("engine_mode: all checks passed");
    return 0;
}

/* Host regression for the A/B control-file contract and mode names.
 *
 * The loader and this test include the same inline helpers from
 * engine_full.h; this is not a copy of the validation logic. */
#include "engine_full.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* Exactly one digit 0..6 plus trailing whitespace is valid. */
    for (int ch = '0'; ch <= '6'; ++ch)
        assert(vp_engine_ab_control_digit_valid(ch));
    assert(!vp_engine_ab_control_digit_valid('7'));
    assert(!vp_engine_ab_control_digit_valid('8'));
    assert(!vp_engine_ab_control_digit_valid('/'));
    assert(!vp_engine_ab_control_digit_valid(':'));
    assert(!vp_engine_ab_control_digit_valid(' '));
    assert(!vp_engine_ab_control_digit_valid('\n'));
    assert(!vp_engine_ab_control_digit_valid(-1)); /* fgetc EOF */

    assert(strcmp(vp_engine_ab_mode_name(0u), "original") == 0);
    assert(strcmp(vp_engine_ab_mode_name(1u), "native") == 0);
    assert(strcmp(vp_engine_ab_mode_name(2u), "fast") == 0);
    assert(strcmp(vp_engine_ab_mode_name(3u), "shared") == 0);
    assert(strcmp(vp_engine_ab_mode_name(4u), "deep") == 0);
    assert(strcmp(vp_engine_ab_mode_name(5u), "deep-recycle") == 0);
    assert(strcmp(vp_engine_ab_mode_name(6u), "deep-clear") == 0);
    assert(strcmp(vp_engine_ab_mode_name(7u), "unknown") == 0);

    /* Mode 6 is mode 5 plus the clear-defer switch: deep texture and idle
     * resize stay on, and no other mode publishes clear defer. */
    for (unsigned mode = 0u; mode <= 6u; ++mode) {
        int deep = vp_engine_ab_mode_uses_deep_texture(mode);
        int idle = vp_engine_ab_mode_uses_idle_resize(mode);
        int defer = vp_engine_ab_mode_uses_clear_defer(mode);
        assert(deep == (mode >= 4u));
        assert(idle == (mode == 5u || mode == 6u));
        assert(defer == (mode == 6u));
    }
    assert(!vp_engine_ab_mode_uses_deep_texture(7u));
    assert(!vp_engine_ab_mode_uses_idle_resize(7u));
    assert(!vp_engine_ab_mode_uses_clear_defer(7u));

    puts("engine_mode: all checks passed");
    return 0;
}

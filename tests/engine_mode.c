/* Host regression for the A/B control-file contract, mode bitmap and names.
 *
 * The loader and this test include the same inline helpers from
 * engine_full.h; this is not a copy of the validation logic. */
#include "engine_full.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int parses_to(const char *text, unsigned expected) {
    unsigned mode = 99u;
    if (vp_engine_ab_control_parse(text, strlen(text), &mode) != 0) return 0;
    return mode == expected;
}

static int rejects(const char *text) {
    unsigned mode = 99u;
    return vp_engine_ab_control_parse(text, strlen(text), &mode) != 0;
}

int main(void) {
    /* Single-digit predicate stays 0..9. ':' and ';' are the next characters
     * after '9' and must never become valid control digits. */
    for (int ch = '0'; ch <= '9'; ++ch)
        assert(vp_engine_ab_control_digit_valid(ch));
    assert(!vp_engine_ab_control_digit_valid(':'));
    assert(!vp_engine_ab_control_digit_valid(';'));
    assert(!vp_engine_ab_control_digit_valid('/'));
    assert(!vp_engine_ab_control_digit_valid(' '));
    assert(!vp_engine_ab_control_digit_valid('\n'));
    assert(!vp_engine_ab_control_digit_valid(-1)); /* fgetc EOF */

    /* The complete mode range grows to 12; 13 and above stay invalid. */
    for (unsigned mode = 0u; mode <= VP_ENGINE_AB_MODE_LIMIT; ++mode)
        assert(vp_engine_ab_mode_valid(mode));
    assert(!vp_engine_ab_mode_valid(VP_ENGINE_AB_MODE_LIMIT + 1u));
    assert(!vp_engine_ab_mode_valid(99u));

    assert(strcmp(vp_engine_ab_mode_name(0u), "original") == 0);
    assert(strcmp(vp_engine_ab_mode_name(1u), "native") == 0);
    assert(strcmp(vp_engine_ab_mode_name(2u), "fast") == 0);
    assert(strcmp(vp_engine_ab_mode_name(3u), "shared") == 0);
    assert(strcmp(vp_engine_ab_mode_name(4u), "deep") == 0);
    assert(strcmp(vp_engine_ab_mode_name(5u), "deep-recycle") == 0);
    assert(strcmp(vp_engine_ab_mode_name(6u), "deep-clear") == 0);
    assert(strcmp(vp_engine_ab_mode_name(7u), "deep-large") == 0);
    assert(strcmp(vp_engine_ab_mode_name(8u), "deep-split") == 0);
    assert(strcmp(vp_engine_ab_mode_name(9u), "deep-quiet") == 0);
    assert(strcmp(vp_engine_ab_mode_name(10u), "deep-compact") == 0);
    assert(strcmp(vp_engine_ab_mode_name(11u), "deep-expanded") == 0);
    assert(strcmp(vp_engine_ab_mode_name(12u), "deep-large-quiet") == 0);
    assert(strcmp(vp_engine_ab_mode_name(13u), "unknown") == 0);

    /* Mode bitmap. Modes 5/6/10/11 add idle resize; only 6 defers clear.
     * Modes 7/11/12 publish the 128 MiB budget; only 8 splits raw. Modes
     * 9/10/11/12 reduce normal work logging. */
    for (unsigned mode = 0u; mode <= VP_ENGINE_AB_MODE_LIMIT; ++mode) {
        int deep = vp_engine_ab_mode_uses_deep_texture(mode);
        int idle = vp_engine_ab_mode_uses_idle_resize(mode);
        int defer = vp_engine_ab_mode_uses_clear_defer(mode);
        unsigned memory = vp_engine_ab_mode_texture_memory(mode);
        int quiet = vp_engine_ab_mode_reduces_work_log(mode);
        assert(deep == (mode >= 4u));
        assert(idle == (mode == 5u || mode == 6u ||
                        mode == 10u || mode == 11u));
        assert(defer == (mode == 6u));
        /* 0 = 64 MiB unified, 1 = 128 MiB unified, 2 = 64 MiB split. */
        assert(memory == (mode == 8u ? 2u :
                          (mode == 7u || mode == 11u || mode == 12u) ? 1u : 0u));
        assert(quiet == (mode == 9u || mode == 10u ||
                         mode == 11u || mode == 12u));
        assert(deep || memory == 0u);
        assert(vp_engine_ab_mode_uses_split_raw(mode) == (memory == 2u));
        assert(vp_engine_ab_mode_uses_native_c(mode) == (mode == 1u));
        assert(vp_engine_ab_mode_uses_fast_c(mode) == (mode == 2u));
        assert(vp_engine_ab_mode_uses_shared_prepare(mode) == (mode == 3u));
    }
    assert(!vp_engine_ab_mode_uses_idle_resize(7u));
    assert(!vp_engine_ab_mode_uses_idle_resize(8u));
    assert(!vp_engine_ab_mode_uses_idle_resize(12u));
    assert(!vp_engine_ab_mode_uses_clear_defer(7u));
    assert(!vp_engine_ab_mode_uses_clear_defer(8u));
    assert(!vp_engine_ab_mode_uses_clear_defer(10u));

    /* Modes 10/11/12 are the normal-startup combinations expressed as A/B
     * modes. They must never select a native C body, shared preparation,
     * clear defer or split raw storage. */
    for (unsigned mode = 10u; mode <= 12u; ++mode) {
        assert(vp_engine_ab_mode_uses_deep_texture(mode));
        assert(!vp_engine_ab_mode_uses_native_c(mode));
        assert(!vp_engine_ab_mode_uses_fast_c(mode));
        assert(!vp_engine_ab_mode_uses_shared_prepare(mode));
        assert(!vp_engine_ab_mode_uses_clear_defer(mode));
        assert(!vp_engine_ab_mode_uses_split_raw(mode));
        assert(vp_engine_ab_mode_reduces_work_log(mode));
    }
    assert(vp_engine_ab_mode_texture_memory(10u) == 0u);
    assert(vp_engine_ab_mode_texture_memory(11u) == 1u);
    assert(vp_engine_ab_mode_texture_memory(12u) == 1u);
    assert(vp_engine_ab_mode_uses_idle_resize(10u));
    assert(vp_engine_ab_mode_uses_idle_resize(11u));
    assert(!vp_engine_ab_mode_uses_idle_resize(12u));

    /* Normal-startup admission: deep accepts every marker combination;
     * without deep, 128 MiB and idle recycle are refused while the work-log
     * marker stays independent. */
    assert(vp_engine_startup_texture_valid(1, 0, 0));
    assert(vp_engine_startup_texture_valid(1, 1, 0));
    assert(vp_engine_startup_texture_valid(1, 0, 1));
    assert(vp_engine_startup_texture_valid(1, 1, 1));
    assert(vp_engine_startup_texture_valid(0, 0, 0));
    assert(!vp_engine_startup_texture_valid(0, 1, 0));
    assert(!vp_engine_startup_texture_valid(0, 0, 1));
    assert(!vp_engine_startup_texture_valid(0, 1, 1));

    /* Token parser: single-digit and 10..12 modes with optional trailing
     * whitespace. */
    for (unsigned mode = 0u; mode <= 9u; ++mode) {
        char text[2] = {(char)('0' + mode), 0};
        assert(parses_to(text, mode));
    }
    assert(parses_to("10", 10u));
    assert(parses_to("11", 11u));
    assert(parses_to("12", 12u));
    assert(parses_to("4 ", 4u));
    assert(parses_to("4\t", 4u));
    assert(parses_to("9\v\f\r\n", 9u));
    assert(parses_to("10\n", 10u));
    assert(parses_to("12 \t\r\n", 12u));

    /* Leading whitespace, signs, leading-zero pairs, out-of-range values,
     * embedded non-whitespace and overlong tokens are all invalid. */
    assert(rejects(""));
    assert(rejects(" "));
    assert(rejects("\t4"));
    assert(rejects(" 4"));
    assert(rejects("-1"));
    assert(rejects("+1"));
    assert(rejects(":"));
    assert(rejects(";"));
    assert(rejects("00"));
    assert(rejects("01"));
    assert(rejects("09"));
    assert(rejects("010"));
    assert(rejects("013"));
    assert(rejects("13"));
    assert(rejects("19"));
    assert(rejects("20"));
    assert(rejects("99"));
    assert(rejects("100"));
    assert(rejects("1a"));
    assert(rejects("10a"));
    assert(rejects("4;"));
    assert(rejects("1 2"));
    assert(rejects("10.0"));
    assert(rejects("12 3"));

    /* VP_ENGINE_AB_CONTROL_MAX_BYTES includes trailing whitespace. */
    {
        char text[VP_ENGINE_AB_CONTROL_MAX_BYTES + 1];
        unsigned mode = 99u;
        memset(text, ' ', sizeof(text));
        text[0] = '1';
        text[1] = '2';
        assert(vp_engine_ab_control_parse(
            text, VP_ENGINE_AB_CONTROL_MAX_BYTES, &mode) == 0);
        assert(mode == 12u);
        assert(vp_engine_ab_control_parse(text, sizeof(text), &mode) != 0);
    }

    puts("engine_mode: all checks passed");
    return 0;
}

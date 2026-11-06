#ifndef VP_ENGINE_FULL_H
#define VP_ENGINE_FULL_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Set before guest execution. Nonzero selects the alternating A/B wrapper at
 * vp_engine_full_entry; zero keeps the single-path native C entry. The value
 * itself is read from assembly and is not changed after the patch is built. */
extern volatile uint32_t vp_engine_full_profile_enabled;
extern volatile uint32_t vp_engine_full_fast_enabled;
/* Published by the host-side A/B control poll before the root entry
 * timestamp. Nonzero only while mode 3 ("shared") is selected; downstream
 * draw preparation reads this atomic flag and never touches the control file. */
extern volatile uint32_t vp_engine_shared_prepare_enabled;
/* Independent decoder-owned texture pipeline switch. In A/B mode it follows
 * engine-full-mode.txt (modes 4 "deep", 5 "deep-recycle", 6 "deep-clear",
 * 7 "deep-large", 8 "deep-split", 9 "deep-quiet" and 10/11/12); outside A/B
 * it follows texture-pipeline.disable (enabled unless present). It is
 * published on the host thread but applied only at a GXM worker boundary. */
extern volatile uint32_t vp_engine_texture_pipeline_enabled;
/* One atomic texture memory configuration, applied together with the pipeline
 * at a GXM worker boundary: 0 = 64 MiB unified (raw and decoded share class 0),
 * 1 = 128 MiB unified, 2 = 64 MiB split (raw class 1 cached CPU-only and
 * never GXM-mapped; decoded class 0 uncached and GXM READ-mapped). Publishing
 * one value avoids a half-updated budget/class pair. */
extern volatile uint32_t vp_engine_texture_memory_mode;
/* Modes 9/10/11/12 and the independent normal-startup marker: omit explicitly
 * selected normal work records before formatting. Error/refusal/evidence
 * logging and all rendering operations remain active. */
extern volatile uint32_t vp_engine_work_log_reduce_enabled;
/* A/B modes 5/6/10/11 and the independent normal-startup marker: page-backed
 * idle texture resize before GPU drain. */
extern volatile uint32_t vp_engine_texture_idle_resize_enabled;
/* A/B-only mode 6: record an admitted full-colour clear and defer its
 * materialization instead of draining the scene at the clear boundary. */
extern volatile uint32_t vp_engine_clear_defer_enabled;

/* A/B control file contract. Modes 0..9 are one ASCII digit; modes 10..12 are
 * two digits and must not have a leading zero. Trailing whitespace is allowed;
 * a leading whitespace byte, a negative sign, any other trailing byte and a
 * token longer than VP_ENGINE_AB_CONTROL_MAX_BYTES are invalid. The full-token
 * parser and the digit predicate are pure so host tests execute the same
 * decisions the loader uses. VP_ENGINE_AB_MODE_MAX remains the single-digit
 * bound and must not be widened: ':' and ';' are not mode digits. */
#define VP_ENGINE_AB_MODE_MAX 9u
#define VP_ENGINE_AB_MODE_LIMIT 12u
#define VP_ENGINE_AB_CONTROL_MAX_BYTES 16u

static inline int vp_engine_ab_control_digit_valid(int ch) {
    return ch >= '0' && ch <= (int)('0' + VP_ENGINE_AB_MODE_MAX);
}

static inline int vp_engine_ab_mode_valid(unsigned mode) {
    return mode <= VP_ENGINE_AB_MODE_LIMIT;
}

static inline int vp_engine_ab_control_space(int ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' ||
        ch == '\f' || ch == '\r';
}

/* Parse a complete control token. Returns 0 and stores the mode on success,
 * nonzero on any contract violation. The output is written only on success. */
static inline int vp_engine_ab_control_parse(const char *text, size_t length,
                                             unsigned *mode) {
    if (!text || !mode || !length || length > VP_ENGINE_AB_CONTROL_MAX_BYTES)
        return -1;
    int first = (unsigned char)text[0];
    if (first < '0' || first > '9') return -1;
    unsigned value;
    size_t at;
    if (length > 1u && text[1] >= '0' && text[1] <= '9') {
        if (first != '1' || text[1] > '2') return -1;
        value = 10u + (unsigned)(text[1] - '0');
        at = 2u;
    } else {
        value = (unsigned)(first - '0');
        at = 1u;
    }
    for (; at < length; ++at)
        if (!vp_engine_ab_control_space((unsigned char)text[at])) return -1;
    *mode = value;
    return 0;
}

static inline const char *vp_engine_ab_mode_name(unsigned mode) {
    switch (mode) {
    case 0u: return "original";
    case 1u: return "native";
    case 2u: return "fast";
    case 3u: return "shared";
    case 4u: return "deep";
    case 5u: return "deep-recycle";
    case 6u: return "deep-clear";
    case 7u: return "deep-large";
    case 8u: return "deep-split";
    case 9u: return "deep-quiet";
    case 10u: return "deep-compact";
    case 11u: return "deep-expanded";
    case 12u: return "deep-large-quiet";
    default: return "unknown";
    }
}

/* Modes 4..12 select the deep texture pipeline and execute the original root
 * trampoline. Modes 5, 6, 10 and 11 also allow resizing idle page regions
 * before a GPU drain. Only mode 6 defers the clear boundary wait. Modes 7, 11
 * and 12 raise the shared budget to 128 MiB; mode 8 splits raw into cached
 * CPU-only chunks while decoded stays uncached and mapped. Modes 10/11/12 are
 * the normal-startup optimised combinations expressed as A/B modes: they never
 * select the native/fast C paths, shared preparation, clear defer or split.
 * Modes 9/10/11/12 reduce normal work logging. */
static inline int vp_engine_ab_mode_uses_deep_texture(unsigned mode) {
    return mode >= 4u && mode <= VP_ENGINE_AB_MODE_LIMIT;
}
static inline int vp_engine_ab_mode_uses_idle_resize(unsigned mode) {
    return mode == 5u || mode == 6u || mode == 10u || mode == 11u;
}
static inline int vp_engine_ab_mode_uses_clear_defer(unsigned mode) {
    return mode == 6u;
}
static inline int vp_engine_ab_mode_uses_native_c(unsigned mode) {
    return mode == 1u;
}
static inline int vp_engine_ab_mode_uses_fast_c(unsigned mode) {
    return mode == 2u;
}
static inline int vp_engine_ab_mode_uses_shared_prepare(unsigned mode) {
    return mode == 3u;
}
static inline unsigned vp_engine_ab_mode_texture_memory(unsigned mode) {
    switch (mode) {
    case 7u:
    case 11u:
    case 12u:
        return 1u;
    case 8u:
        return 2u;
    default:
        return 0u;
    }
}
static inline int vp_engine_ab_mode_uses_split_raw(unsigned mode) {
    return vp_engine_ab_mode_texture_memory(mode) == 2u;
}
static inline int vp_engine_ab_mode_reduces_work_log(unsigned mode) {
    return mode == 9u || mode == 10u || mode == 11u || mode == 12u;
}

/* Normal startup (no A/B control): the 128 MiB budget and idle page recycle
 * only exist inside the deep pipeline, while work-log reduction is
 * independent. This is the exact admission rule the loader applies before it
 * publishes texture-pool-128.enable or texture-idle-recycle.enable. */
static inline int vp_engine_startup_texture_valid(int pipeline_enabled,
                                                  int pool128, int idle) {
    return pipeline_enabled || (!pool128 && !idle);
}

int vp_engine_full_init(uint32_t main_base);
int vp_engine_full_run(uint32_t context);
int vp_engine_full_run_fast(uint32_t context);
void vp_engine_full_report(FILE *log);
#endif

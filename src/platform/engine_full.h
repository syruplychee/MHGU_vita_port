#ifndef VP_ENGINE_FULL_H
#define VP_ENGINE_FULL_H
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
 * engine-full-mode.txt (modes 4 "deep", 5 "deep-recycle" and 6 "deep-clear"); outside A/B it follows
 * texture-pipeline.disable (enabled unless present). It is published
 * on the host thread but applied only at a GXM worker boundary. */
extern volatile uint32_t vp_engine_texture_pipeline_enabled;
/* A/B-only modes 5/6: page-backed idle texture resize before GPU drain. */
extern volatile uint32_t vp_engine_texture_idle_resize_enabled;
/* A/B-only mode 6: record an admitted full-colour clear and defer its
 * materialization instead of draining the scene at the clear boundary. */
extern volatile uint32_t vp_engine_clear_defer_enabled;

/* A/B control file contract: exactly one ASCII digit selects the mode. The
 * valid range is 0 through 6; any other first byte or trailing non-whitespace
 * is invalid. These helpers are pure so the host tests execute the same
 * decision the loader uses. */
static inline int vp_engine_ab_control_digit_valid(int ch) {
    return ch >= '0' && ch <= '6';
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
    default: return "unknown";
    }
}

/* Mode 4/5/6 select the deep texture pipeline. Modes 5 and 6 also allow
 * resizing idle page regions before a GPU drain. Only mode 6 defers the clear
 * boundary wait. */
static inline int vp_engine_ab_mode_uses_deep_texture(unsigned mode) {
    return mode >= 4u && mode <= 6u;
}
static inline int vp_engine_ab_mode_uses_idle_resize(unsigned mode) {
    return mode == 5u || mode == 6u;
}
static inline int vp_engine_ab_mode_uses_clear_defer(unsigned mode) {
    return mode == 6u;
}

int vp_engine_full_init(uint32_t main_base);
int vp_engine_full_run(uint32_t context);
int vp_engine_full_run_fast(uint32_t context);
void vp_engine_full_report(FILE *log);
#endif

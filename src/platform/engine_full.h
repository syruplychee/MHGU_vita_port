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
 * engine-full-mode.txt (modes 4 "deep", 5 "deep-recycle", 6 "deep-clear",
 * 7 "deep-large" and 8 "deep-split"); outside A/B it follows
 * texture-pipeline.disable (enabled unless present). It is published
 * on the host thread but applied only at a GXM worker boundary. */
extern volatile uint32_t vp_engine_texture_pipeline_enabled;
/* One atomic texture memory configuration, applied together with the pipeline
 * at a GXM worker boundary: 0 = 64 MiB unified (raw and decoded share class 0),
 * 1 = 128 MiB unified, 2 = 64 MiB split (raw class 1 cached CPU-only and
 * never GXM-mapped; decoded class 0 uncached and GXM READ-mapped). Publishing
 * one value avoids a half-updated budget/class pair. */
extern volatile uint32_t vp_engine_texture_memory_mode;
/* Mode 9 only: omit explicitly selected normal work records before formatting.
 * Error/refusal/evidence logging and all rendering operations remain active. */
extern volatile uint32_t vp_engine_work_log_reduce_enabled;
/* A/B-only modes 5/6: page-backed idle texture resize before GPU drain. */
extern volatile uint32_t vp_engine_texture_idle_resize_enabled;
/* A/B-only mode 6: record an admitted full-colour clear and defer its
 * materialization instead of draining the scene at the clear boundary. */
extern volatile uint32_t vp_engine_clear_defer_enabled;

/* A/B control file contract: exactly one ASCII digit selects the mode. The
 * valid range is 0 through 9; any other first byte or trailing non-whitespace
 * is invalid. These helpers are pure so the host tests execute the same
 * decision the loader uses. */
#define VP_ENGINE_AB_MODE_MAX 9u

static inline int vp_engine_ab_control_digit_valid(int ch) {
    return ch >= '0' && ch <= (int)('0' + VP_ENGINE_AB_MODE_MAX);
}

static inline int vp_engine_ab_mode_valid(unsigned mode) {
    return mode <= VP_ENGINE_AB_MODE_MAX;
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
    default: return "unknown";
    }
}

/* Modes 4/5/6/7/8 select the deep texture pipeline. Modes 5 and 6 also allow
 * resizing idle page regions before a GPU drain. Only mode 6 defers the clear
 * boundary wait. Modes 7 and 8 change only the texture memory configuration:
 * 7 raises the shared budget to 128 MiB; 8 splits raw into cached CPU-only
 * chunks while decoded stays uncached and mapped. They do not enable idle
 * resize or clear defer, so the two memory changes are never combined with
 * either mode-5/6 behavior. */
static inline int vp_engine_ab_mode_uses_deep_texture(unsigned mode) {
    return mode >= 4u && mode <= 9u;
}
static inline int vp_engine_ab_mode_uses_idle_resize(unsigned mode) {
    return mode == 5u || mode == 6u;
}
static inline int vp_engine_ab_mode_uses_clear_defer(unsigned mode) {
    return mode == 6u;
}
static inline unsigned vp_engine_ab_mode_texture_memory(unsigned mode) {
    switch (mode) {
    case 7u: return 1u;
    case 8u: return 2u;
    default: return 0u;
    }
}
static inline int vp_engine_ab_mode_reduces_work_log(unsigned mode) {
    return mode == 9u;
}

int vp_engine_full_init(uint32_t main_base);
int vp_engine_full_run(uint32_t context);
int vp_engine_full_run_fast(uint32_t context);
void vp_engine_full_report(FILE *log);
#endif

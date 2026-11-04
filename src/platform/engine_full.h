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
 * engine-full-mode.txt (modes 4 "deep" and 5 "deep-recycle"); outside A/B it follows
 * texture-pipeline.disable (enabled unless present). It is published
 * on the host thread but applied only at a GXM worker boundary. */
extern volatile uint32_t vp_engine_texture_pipeline_enabled;
/* A/B-only mode 5: page-backed idle texture resize before GPU drain. */
extern volatile uint32_t vp_engine_texture_idle_resize_enabled;

int vp_engine_full_init(uint32_t main_base);
int vp_engine_full_run(uint32_t context);
int vp_engine_full_run_fast(uint32_t context);
void vp_engine_full_report(FILE *log);
#endif

#ifndef VP_ENGINE_FULL_H
#define VP_ENGINE_FULL_H
#include <stdint.h>
#include <stdio.h>

/* Set before guest execution. Nonzero selects the alternating A/B wrapper at
 * vp_engine_full_entry; zero keeps the single-path native C entry. The value
 * itself is read from assembly and is not changed after the patch is built. */
extern volatile uint32_t vp_engine_full_profile_enabled;
extern volatile uint32_t vp_engine_full_fast_enabled;

int vp_engine_full_init(uint32_t main_base);
int vp_engine_full_run(uint32_t context);
int vp_engine_full_run_fast(uint32_t context);
void vp_engine_full_report(FILE *log);
#endif

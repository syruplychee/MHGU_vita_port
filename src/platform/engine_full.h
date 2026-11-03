#ifndef VP_ENGINE_FULL_H
#define VP_ENGINE_FULL_H
#include <stdint.h>
#include <stdio.h>

int vp_engine_full_init(uint32_t main_base);
int vp_engine_full_run(uint32_t context);
void vp_engine_full_report(FILE *log);
#endif

#ifndef VP_ENGINE_CAPTURE_H
#define VP_ENGINE_CAPTURE_H
#include "engine_nvn.h"
#include <stdio.h>

int vp_engine_capture_init(uint32_t base);
void vp_engine_capture_begin(uint32_t count, uint32_t ctx, uint32_t state,
    uint32_t scratch, uint32_t gpu_root);
void vp_engine_capture_range(uint32_t address, uint32_t size);
void vp_engine_capture_before(const VPEngineNvnCall *call, uint32_t function);
void vp_engine_capture_after(const VPEngineNvnResult *out);
int vp_engine_capture_end(void);
void vp_engine_capture_report(FILE *log);
#endif

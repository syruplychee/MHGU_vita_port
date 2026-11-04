#ifndef VP_ENGINE_NVN_INTERNAL_H
#define VP_ENGINE_NVN_INTERNAL_H

#include "engine_nvn.h"
#include "engine_memory.h"

#include <string.h>

typedef struct {
    const VPEngineNvnEnv *env;
    int error;
} VPEngineMem;

static uint8_t vpm_zero_page[16];

static inline void *vpm_ptr(VPEngineMem *m, uint32_t address)
{
    void *p;
    if (m->error) return vpm_zero_page;
    if (!address) {
        m->error = VP_ENGINE_NATIVE_ERR_MEMORY;
        return vpm_zero_page;
    }
    p = m->env->resolve ? m->env->resolve(m->env->opaque, address) : NULL;
    if (!p) {
        m->error = VP_ENGINE_NATIVE_ERR_MEMORY;
        return vpm_zero_page;
    }
    return p;
}

static inline uint8_t vpm_r8(VPEngineMem *m, uint32_t address)
{
    return *(const uint8_t *)vpm_ptr(m,address);
}

static inline uint16_t vpm_r16(VPEngineMem *m, uint32_t address)
{
    return vp_engine_read16(vpm_ptr(m,address));
}

static inline uint32_t vpm_r32(VPEngineMem *m, uint32_t address)
{
    return vp_engine_read32(vpm_ptr(m,address));
}

static inline uint64_t vpm_r64(VPEngineMem *m, uint32_t address)
{
    return (uint64_t)vpm_r32(m, address) |
           ((uint64_t)vpm_r32(m, address + 4u) << 32);
}

static inline void vpm_w8(VPEngineMem *m, uint32_t address, uint8_t value)
{
    *(uint8_t *)vpm_ptr(m,address)=value;
}

static inline void vpm_w16(VPEngineMem *m, uint32_t address, uint16_t value)
{
    vp_engine_write16(vpm_ptr(m,address),value);
}

static inline void vpm_w32(VPEngineMem *m, uint32_t address, uint32_t value)
{
    vp_engine_write32(vpm_ptr(m,address),value);
}

static inline void vpm_w64(VPEngineMem *m, uint32_t address, uint64_t value)
{
    vpm_w32(m, address, (uint32_t)value);
    vpm_w32(m, address + 4u, (uint32_t)(value >> 32));
}

static inline void vpm_call_init(VPEngineNvnCall *call, uint32_t site,
                                 uint32_t slot)
{
    memset(call, 0, sizeof(*call));
    call->site = site;
    call->slot = slot;
}

static inline void vpm_send(VPEngineMem *m, const VPEngineNvnCall *call,
                            VPEngineNvnResult *result)
{
    if (m->error) return;
    if (!m->env->emit) {
        m->error = VP_ENGINE_NATIVE_ERR_CALLBACK;
        return;
    }
    m->env->emit(m->env->opaque, call, result);
}

/* Emit a query call and return its real 64-bit result.  A sink that only logs
 * and leaves has_value unset is an explicit error. */
static inline uint64_t vpm_query(VPEngineMem *m, uint32_t site, uint32_t slot,
                                 uint32_t r0, uint32_t r1, uint32_t r2,
                                 uint32_t r3)
{
    VPEngineNvnCall call;
    VPEngineNvnResult result;
    result.value = 0;
    result.has_value = 0;
    vpm_call_init(&call, site, slot);
    call.r[0] = r0;
    call.r[1] = r1;
    call.r[2] = r2;
    call.r[3] = r3;
    vpm_send(m, &call, &result);
    if (m->error) return 0;
    if (!result.has_value) {
        m->error = VP_ENGINE_NATIVE_ERR_CALLBACK;
        return 0;
    }
    return result.value;
}

static inline void vpm_memclr(VPEngineMem *m, uint32_t site, uint32_t pointer,
                              uint32_t size)
{
    if (m->error) return;
    if (!m->env->memclr) {
        m->error = VP_ENGINE_NATIVE_ERR_CALLBACK;
        return;
    }
    m->env->memclr(m->env->opaque, site, pointer, size);
}

#endif

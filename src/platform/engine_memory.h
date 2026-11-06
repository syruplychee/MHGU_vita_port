#ifndef VP_ENGINE_MEMORY_H
#define VP_ENGINE_MEMORY_H
#include <stdint.h>
#include <string.h>

/* These preparation fields are guest words, not host objects. GCC's unaligned
 * alias type permits the original A32 load/store without entering guest-unsafe
 * libc or calling a byte-copy loop for every four-byte field. Host fixtures
 * retain memcpy so synthetic mappings need no alignment assumption. */
#if defined(__GNUC__) && defined(__arm__)
typedef uint32_t VPEngineWord __attribute__((aligned(1),may_alias));
typedef uint16_t VPEngineHalf __attribute__((aligned(1),may_alias));
#endif
static inline uint32_t vp_engine_read32(const void *p) {
#if defined(__GNUC__) && defined(__arm__)
    return *(const VPEngineWord *)p;
#else
    uint32_t value; memcpy(&value,p,sizeof(value)); return value;
#endif
}
static inline uint16_t vp_engine_read16(const void *p) {
#if defined(__GNUC__) && defined(__arm__)
    return *(const VPEngineHalf *)p;
#else
    uint16_t value; memcpy(&value,p,sizeof(value)); return value;
#endif
}
static inline void vp_engine_write32(void *p,uint32_t value) {
#if defined(__GNUC__) && defined(__arm__)
    *(VPEngineWord *)p=value;
#else
    memcpy(p,&value,sizeof(value));
#endif
}
static inline void vp_engine_write16(void *p,uint16_t value) {
#if defined(__GNUC__) && defined(__arm__)
    *(VPEngineHalf *)p=value;
#else
    memcpy(p,&value,sizeof(value));
#endif
}
#endif

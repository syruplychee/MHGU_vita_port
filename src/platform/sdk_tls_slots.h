#ifndef VP_SDK_TLS_SLOTS_H
#define VP_SDK_TLS_SLOTS_H
#include <stdint.h>
/* Locked SDK TlsManager: 32 process slots; at most 16 normal allocations;
 * normal searches upward, SDK-private searches downward. Caller serializes
 * pool access. Generations clear other threads' stale values on slot reuse.
 * Values belong to individual guest threads, never host TLS. */
typedef struct { uint32_t used,private_mask,normal_count,destructors[32],generations[32]; } VPSdkTlsPool;
typedef struct { uint32_t values[32],generations[32]; } VPSdkTlsValues;
enum { VP_TLS_SLOT_ARGUMENT=-150,VP_TLS_SLOT_OWNERSHIP=-151,VP_TLS_SLOT_EXHAUSTED=0x1203 };
int vp_sdk_tls_allocate(VPSdkTlsPool *,uint32_t *,uint32_t,int);
int vp_sdk_tls_free(VPSdkTlsPool *,VPSdkTlsValues *,uint32_t);
int vp_sdk_tls_get(const VPSdkTlsPool *,const VPSdkTlsValues *,uint32_t,uint32_t *);
int vp_sdk_tls_set(const VPSdkTlsPool *,VPSdkTlsValues *,uint32_t,uint32_t);
#endif

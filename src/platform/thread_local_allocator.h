#ifndef VP_THREAD_LOCAL_ALLOCATOR_H
#define VP_THREAD_LOCAL_ALLOCATOR_H

#include "host_bridge.h"
#include <stdint.h>

#define VP_THREAD_LOCAL_ALLOCATION_LIMIT 64u
enum {
    VP_SERVICE_THREAD_LOCAL_ALLOCATE = 0x100u,
    VP_SERVICE_THREAD_LOCAL_FREE = 0x101u,
    VP_THREAD_LOCAL_ARGUMENT = -20,
    VP_THREAD_LOCAL_CAPACITY = -21,
    VP_THREAD_LOCAL_MEMORY = -22,
    VP_THREAD_LOCAL_OWNERSHIP = -23
};

typedef struct {
    void *pointer;
    uint32_t size;
} VPThreadLocalAllocation;

typedef struct {
    VPThreadLocalAllocation allocations[VP_THREAD_LOCAL_ALLOCATION_LIMIT];
    uint32_t live_allocations;
    uint32_t live_bytes;
    uint32_t peak_bytes;
} VPThreadLocalAllocator;

void vp_thread_local_allocator_init(VPThreadLocalAllocator *);
void vp_thread_local_allocator_destroy(VPThreadLocalAllocator *);
void *vp_thread_local_allocate(VPThreadLocalAllocator *,uint32_t,uint32_t);
int vp_thread_local_free(VPThreadLocalAllocator *,void *,uint32_t);
int vp_thread_local_bridge_handler(VPHostBridge *,VPBridgeFrame *);

#endif

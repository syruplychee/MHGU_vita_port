#include "thread_local_allocator.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

void vp_thread_local_allocator_init(VPThreadLocalAllocator *allocator) {
    if (allocator) memset(allocator,0,sizeof(*allocator));
}

void vp_thread_local_allocator_destroy(VPThreadLocalAllocator *allocator) {
    if (!allocator) return;
    for (uint32_t index=0;index<VP_THREAD_LOCAL_ALLOCATION_LIMIT;++index) {
        free(allocator->allocations[index].pointer);
    }
    memset(allocator,0,sizeof(*allocator));
}

void *vp_thread_local_allocate(VPThreadLocalAllocator *allocator,uint32_t size,uint32_t alignment) {
    if (!allocator || !size || alignment<8u || (alignment&(alignment-1u))) return NULL;
    uint32_t slot=VP_THREAD_LOCAL_ALLOCATION_LIMIT;
    for (uint32_t index=0;index<VP_THREAD_LOCAL_ALLOCATION_LIMIT;++index) {
        if (!allocator->allocations[index].pointer) { slot=index;break; }
    }
    if (slot==VP_THREAD_LOCAL_ALLOCATION_LIMIT) return NULL;
    void *pointer=memalign(alignment,size);
    if (!pointer) return NULL;
    allocator->allocations[slot].pointer=pointer;
    allocator->allocations[slot].size=size;
    ++allocator->live_allocations;
    allocator->live_bytes+=size;
    if (allocator->live_bytes>allocator->peak_bytes) allocator->peak_bytes=allocator->live_bytes;
    return pointer;
}

int vp_thread_local_free(VPThreadLocalAllocator *allocator,void *pointer,uint32_t size) {
    if (!allocator || !pointer || !size) return VP_THREAD_LOCAL_ARGUMENT;
    for (uint32_t index=0;index<VP_THREAD_LOCAL_ALLOCATION_LIMIT;++index) {
        VPThreadLocalAllocation *allocation=&allocator->allocations[index];
        if (allocation->pointer!=pointer) continue;
        if (allocation->size!=size) return VP_THREAD_LOCAL_OWNERSHIP;
        free(pointer);
        allocation->pointer=NULL;
        allocation->size=0;
        --allocator->live_allocations;
        allocator->live_bytes-=size;
        return 0;
    }
    return VP_THREAD_LOCAL_OWNERSHIP;
}

int vp_thread_local_bridge_handler(VPHostBridge *bridge,VPBridgeFrame *frame) {
    if (!bridge || !frame || !bridge->user) return VP_THREAD_LOCAL_ARGUMENT;
    VPThreadLocalAllocator *allocator=bridge->user;
    if (frame->service==VP_SERVICE_THREAD_LOCAL_ALLOCATE) {
        void *pointer=vp_thread_local_allocate(allocator,frame->regs[0],frame->regs[1]);
        if (!pointer) return VP_THREAD_LOCAL_MEMORY;
        frame->regs[0]=(uint32_t)(uintptr_t)pointer;
        return 0;
    }
    if (frame->service==VP_SERVICE_THREAD_LOCAL_FREE) {
        int result=vp_thread_local_free(allocator,(void *)(uintptr_t)frame->regs[0],frame->regs[1]);
        if (!result) frame->regs[0]=0;
        return result;
    }
    return VP_BRIDGE_UNSUPPORTED;
}

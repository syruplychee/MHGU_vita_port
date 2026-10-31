#ifndef VP_GPU_INDICES_H
#define VP_GPU_INDICES_H
#include <stdint.h>
#include <string.h>

static inline uint32_t vp_gpu_index_read(const void *data,unsigned bytes,unsigned i) {
    const uint8_t *at = (const uint8_t *)data + (size_t)i * bytes;
    if (bytes == 1u) return *at;
    if (bytes == 2u) { uint16_t value; memcpy(&value, at, 2u); return value; }
    if (bytes == 4u) { uint32_t value; memcpy(&value, at, 4u); return value; }
    return 0u;
}

/* Join restarted triangle strips with zero-area triangles. Every real strip
 * starts at even parity, so its winding matches an independent draw. */
static inline int vp_gpu_strip_remap(const void *source, unsigned count,
    unsigned in_bytes, uint32_t marker, uint32_t first_index, void *output,
    size_t capacity, unsigned out_bytes, unsigned *segments) {
    unsigned at = 0u, written = 0u, strips = 0u;
    uint32_t previous = 0u;
    if (!source || !output || (in_bytes != 1u && in_bytes != 2u && in_bytes != 4u) ||
        (out_bytes != 2u && out_bytes != 4u)) return -1;
    marker &= in_bytes == 1u ? 0xffu : in_bytes == 2u ? 0xffffu : 0xffffffffu;
    while (at < count) {
        unsigned begin = at;
        while (at < count && vp_gpu_index_read(source, in_bytes, at) != marker) ++at;
        unsigned length = at - begin;
        if (at < count) ++at;
        if (length < 3u) continue;
        unsigned bridge = written ? 2u + (written & 1u) : 0u;
        if ((size_t)written + bridge + length > capacity / out_bytes) return -1;
        uint32_t head = vp_gpu_index_read(source, in_bytes, begin);
        if (head < first_index) return -1;
        head -= first_index;
        for (unsigned i = 0; i < bridge + length; ++i) {
            uint32_t value;
            if (i < bridge) value = i + 1u == bridge ? head : previous;
            else {
                value = vp_gpu_index_read(source, in_bytes, begin + i - bridge);
                if (value < first_index) return -1;
                value -= first_index;
            }
            if (out_bytes == 2u) {
                if (value > 0xffffu) return -1;
                uint16_t narrow = (uint16_t)value;
                memcpy((uint8_t *)output + (size_t)written * 2u, &narrow, 2u);
            } else memcpy((uint8_t *)output + (size_t)written * 4u, &value, 4u);
            ++written;
        }
        previous = vp_gpu_index_read(source, in_bytes, begin + length - 1u) - first_index;
        ++strips;
    }
    if (segments) *segments = strips;
    return (int)written;
}

typedef struct {
    uint32_t minimum;
    uint32_t maximum;
    uint32_t real_indices;
    uint32_t restart_indices;
} VpGpuIndexRange;

/* Scan an index buffer without dereferencing it as a wider type, so unaligned
 * guest addresses are safe. Restart markers are excluded from minimum/maximum.
 * Returns 1 when at least one real index exists, 0 when the buffer is empty or
 * every index is a restart marker, and -1 for an invalid argument. */
static inline int vp_gpu_index_range(const void *data,unsigned bytes,unsigned count,
                                     int restart,uint32_t marker,
                                     VpGpuIndexRange *out) {
    uint32_t masked=marker;
    if (data==NULL || out==NULL || (bytes!=1u && bytes!=2u && bytes!=4u)) return -1;
    if (bytes==1u) masked&=0xffu;
    else if (bytes==2u) masked&=0xffffu;
    memset(out,0,sizeof(*out));
    for (unsigned i=0;i<count;i++) {
        uint32_t value=vp_gpu_index_read(data,bytes,i);
        if (restart && value==masked) { out->restart_indices++; continue; }
        if (out->real_indices==0u || value<out->minimum) out->minimum=value;
        if (out->real_indices==0u || value>out->maximum) out->maximum=value;
        out->real_indices++;
    }
    return out->real_indices!=0u;
}

static inline int vp_gpu_index_max(const void *data,unsigned bytes,unsigned count,
                                   int restart,uint32_t marker,uint32_t *maximum) {
    VpGpuIndexRange range;
    int found=vp_gpu_index_range(data,bytes,count,restart,marker,&range);
    if (maximum!=NULL) *maximum=range.maximum;
    return found;
}
#endif

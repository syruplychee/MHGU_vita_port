#ifndef VP_MEDIA_STRIDED_COPY_H
#define VP_MEDIA_STRIDED_COPY_H

#include <stdint.h>
#include <string.h>

/* The caller owns memory permissions and ensures the buffers do not overlap. */
static inline int vp_media_strided_copy(
    void *destination, uint32_t destination_capacity,
    uint32_t destination_stride, const void *source,
    uint32_t source_capacity, uint32_t source_stride,
    uint32_t row_bytes, uint32_t rows) {
    uint64_t destination_span;
    uint64_t source_span;
    uint32_t row;

    if (!destination || !source || !row_bytes || !rows ||
        destination_stride < row_bytes || source_stride < row_bytes) {
        return 0;
    }
    destination_span = (uint64_t)(rows - 1u) * destination_stride + row_bytes;
    source_span = (uint64_t)(rows - 1u) * source_stride + row_bytes;
    if (destination_span > destination_capacity || source_span > source_capacity) {
        return 0;
    }
    if (destination_stride == row_bytes && source_stride == row_bytes) {
        memcpy(destination, source, (size_t)source_span);
        return 1;
    }
    for (row = 0; row < rows; row++) {
        memcpy((uint8_t *)destination + (size_t)row * destination_stride,
               (const uint8_t *)source + (size_t)row * source_stride, row_bytes);
    }
    return 1;
}

#endif /* VP_MEDIA_STRIDED_COPY_H */

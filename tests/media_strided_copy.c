#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "media_strided_copy.h"

static void test_strides(void) {
    const uint8_t source[15] = {
        1, 2, 3, 99, 99, 99,
        4, 5, 6, 99, 99, 99,
        7, 8, 9
    };
    const uint8_t expected[16] = {
        1, 2, 3, 0xa5, 0xa5,
        4, 5, 6, 0xa5, 0xa5,
        7, 8, 9, 0xa5, 0xa5, 0xa5
    };
    uint8_t destination[16];

    memset(destination, 0xa5, sizeof(destination));
    /* Each capacity ends exactly at the last copied byte. */
    assert(vp_media_strided_copy(destination, 13, 5, source, 15, 6, 3, 3));
    assert(memcmp(destination, expected, sizeof(destination)) == 0);
}

static void test_compact(void) {
    const uint8_t source[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    uint8_t destination[14];

    memset(destination, 0xa5, sizeof(destination));
    assert(vp_media_strided_copy(destination + 1, 12, 4, source, 12, 4, 4, 3));
    assert(memcmp(destination + 1, source, sizeof(source)) == 0);
    assert(destination[0] == 0xa5 && destination[13] == 0xa5);
    /* One row does not require trailing stride padding in either buffer. */
    assert(vp_media_strided_copy(destination, 4, UINT32_MAX,
                                 source, 4, UINT32_MAX, 4, 1));
    assert(memcmp(destination, source, 4) == 0);
}

static void test_rejected(void) {
    const uint8_t source[16] = {1, 2, 3, 4};
    uint8_t destination[16];
    uint8_t original[16];

    memset(destination, 0xa5, sizeof(destination));
    memcpy(original, destination, sizeof(original));
#define REJECT(dst, dc, ds, src, sc, ss, bytes, count) \
    do { \
        assert(!vp_media_strided_copy(dst, dc, ds, src, sc, ss, bytes, count)); \
        assert(memcmp(destination, original, sizeof(destination)) == 0); \
    } while (0)

    REJECT(destination, 12, 5, source, 15, 6, 3, 3);
    REJECT(destination, 13, 5, source, 14, 6, 3, 3);
    REJECT(destination, 0, 4, source, 16, 4, 4, 1);
    REJECT(destination, 16, 4, source, 0, 4, 4, 1);
    REJECT(NULL, 16, 4, source, 16, 4, 4, 1);
    REJECT(destination, 16, 4, NULL, 16, 4, 4, 1);
    REJECT(destination, 16, 4, source, 16, 4, 0, 1);
    REJECT(destination, 16, 4, source, 16, 4, 4, 0);
    REJECT(destination, 16, 3, source, 16, 4, 4, 1);
    REJECT(destination, 16, 4, source, 16, 3, 4, 1);
    /* These wrapped to small spans if multiplication used uint32_t. */
    REJECT(destination, 16, 0x80000000u, source, 16, 1, 1, 3);
    REJECT(destination, 16, 1, source, 16, 0x80000000u, 1, 3);
    REJECT(destination, 16, UINT32_MAX,
           source, 16, UINT32_MAX, 1, UINT32_MAX);
    REJECT(destination, 16, 1, source, 16, 1, 1, UINT32_MAX);
#undef REJECT
}

int main(void) {
    test_strides();
    test_compact();
    test_rejected();
    puts("MEDIA_STRIDED_COPY passed");
    return 0;
}

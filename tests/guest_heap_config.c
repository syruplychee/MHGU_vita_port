#include "guest_heap_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint32_t bytes = 0u;
    assert(vp_guest_heap_parse("512\n", 4u, &bytes) == 0);
    assert(bytes == (512u << 20));
    assert(vp_guest_pool_bytes(bytes) == (536u << 20));
    assert(vp_guest_heap_parse("1895", 4u, &bytes) == 0);
    assert(bytes == VP_GUEST_HEAP_DEFAULT_BYTES);
    assert(vp_guest_pool_bytes(bytes) == 0x77f00000u);
    const char *bad[] = {"", "127", "1896", "512x", "-512", "+512", "999999999999"};
    for (unsigned i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(vp_guest_heap_parse(bad[i], strlen(bad[i]), &bytes) < 0);
    puts("PASS heap capacity controls and identical reported/backed layout");
    return 0;
}

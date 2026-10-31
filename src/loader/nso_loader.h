#ifndef VP_NSO_LOADER_H
#define VP_NSO_LOADER_H
#include <stddef.h>
#include <stdint.h>

/* Portable, bounded NSO0 reader. No guest instructions are interpreted. */
enum { VP_NSO_INPUT=-40, VP_NSO_HASH=-41, VP_NSO_LAYOUT=-42,
       VP_NSO_LZ4=-43, VP_NSO_CAPACITY=-44, VP_NSO_MEMORY=-45 };
typedef struct { uint32_t source, offset, bytes, stored; unsigned char sha256[32]; } VPNSOSegment;
typedef struct {
    uint32_t flags, file_bytes, span, bss_begin, bss_end;
    uint32_t strings, string_bytes, symbols, symbol_bytes;
    VPNSOSegment segments[3];
    unsigned char build_id[32], file_sha256[32];
} VPNSO;
void vp_sha256(const void *, size_t, unsigned char out[32]);
int vp_nso_inspect(VPNSO *, const void *, uint32_t,
                   const unsigned char expected_sha256[32], const unsigned char expected_build_id[32]);
/* On a failed decode the entire output span is cleared; never executable yet. */
int vp_nso_decode(const VPNSO *, const void *, uint32_t, void *, uint32_t);
#endif

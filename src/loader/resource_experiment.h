#ifndef VP_RESOURCE_EXPERIMENT_H
#define VP_RESOURCE_EXPERIMENT_H
#include <stdint.h>

#define VP_TITLE_HEAP_GUARD_OFFSET 0x151cccu
#define VP_TITLE_HEAP_GUARD_BYTES 0x468u
#define VP_TITLE_HEAP_GUARD_SHA "de53969f545d0c46a59f9b38eb256e973b0059303267c03feca3010a74486883"

/* Original graphics initialization has separate docked/handheld presets.
 * Native fonts use 1280x720 for both, so its max-size VI allocation matches
 * the handheld viewport. No mode check or resize failure is bypassed. */
#define VP_NATIVE_DISPLAY_GUARD_OFFSET 0xbbcd10u
#define VP_NATIVE_DISPLAY_GUARD_BYTES 0x80u
#define VP_NATIVE_DISPLAY_GUARD_SHA "4920676f70bd71a9d4767e819c167fb06093a3147da29b847c6e3cb782572678"
typedef struct { uint32_t offset, original, replacement; } VPNativeDisplayWord;
static const VPNativeDisplayWord vp_native_display_words[] = {
    {0xbbcd34u,0xe3a00d1eu,0xe3a00c05u}, /* MOV r0,#1920 -> #1280. */
    {0xbbcd38u,0xe3007438u,0xe30072d0u}, /* MOVW r7,#1080 -> #720. */
};

typedef struct { uint32_t offset, original, reduced256, reduced512; } VPTitleHeapWord;
static const VPTitleHeapWord vp_title_heap_words[] = {
    {0x151e40u, 0xe3432520u, 0xe3412000u, 0xe3422000u},
    {0x151fa4u, 0xe3433520u, 0xe3413000u, 0xe3423000u},
};

typedef struct {
    uint32_t offset, first, second, service;
    const char *name, *sha;
} VPResourceProbeSite;
static const VPResourceProbeSite vp_resource_probe_sites[] = {
    {0xb51464u, 0xe92d4ff0u, 0xe24dd09cu, 0x257u, "load-start",
     "fd275fccd6ee7a288e6cc2339b8fa727179312828c78ef8e999fde42f4efc25c"},
    {0xb51ac4u, 0xe3560000u, 0xe58460d0u, 0x258u, "load-commit",
     "506011ed7f2dd5354dfdf628ffba75d6111269ace5b02e72cd55c2497f859328"},
    {0xb51bfcu, 0xe92d4010u, 0xe1a04000u, 0x259u, "release",
     "4184b90ddc10a643b26c07ce0725708ef5fe484c29fa72db9f44627f1241e32f"},
    {0xb51b00u, 0xe92d4830u, 0xe1a05000u, 0x25au, "replace",
     "5a77b6f8fb559332ec2de7ad640d65216a3cb0770aa2e231b2d207966fc8b4cf"},
    {0xb513b4u, 0xe92d4010u, 0xe1a04000u, 0x25bu, "destroy",
     "f02b9fa54dec8dd87834f00763de8c5c2373c7b319d415aacf49eeac0068d7c3"},
    {0xb513fcu, 0xe92d4010u, 0xe1a04000u, 0x25cu, "destroy-delete",
     "de25ad4dcaadb19912791f29b9b786343baed9823e01a9d8cb8962f7953ed9aa"},
    {0xb82684u, 0xe92d4010u, 0xe1a02000u, 0x25du, "font-glyph",
     "5bbbaf50dbe0e63e5ea0e3bb85970b743e269c57f7c0033153d95b80d6b06f48"},
    {0xb82c78u, 0xe5d020c4u, 0xe3120001u, 0x25eu, "font-page",
     "d3633506c2cdf79dcf4c78752e76f340c1bd6cee4f15399a1e67f739c7f466e5"},
    {0xa3f8c4u, 0xe92d48f0u, 0xe1a04002u, 0x25fu, "font-quad-ui",
     "a59923991c0b581c6902ff0aa81658a8ef73320aac6eaa382ecc6e13e5fd9c28"},
    {0xa55524u, 0xe92d48f0u, 0xe1a04002u, 0x260u, "font-quad-control",
     "c98f302b91374c1edd7919acbcf903ff94cef39d4835ec327c1acd7355633870"},
    {0xa3fc10u, 0xed810a17u, 0xe8900006u, 0x261u, "font-uv-ui",
     "122ca809b963c4036cf0bcb12e70ec1bbf9fd4a10b87e0ca85e215788a5c0538"},
    {0xa55874u, 0xed810a17u, 0xe8900006u, 0x262u, "font-uv-control",
     "bf66a89ee5b8906bcf7357bf3563dc048cb845a61d32e41bc8952294975c4c98"},
};
#endif

#ifndef VP_RESOURCE_EXPERIMENT_H
#define VP_RESOURCE_EXPERIMENT_H
#include <stdint.h>

#define VP_TITLE_HEAP_GUARD_OFFSET 0x151cccu
#define VP_TITLE_HEAP_GUARD_BYTES 0x468u
#define VP_TITLE_HEAP_GUARD_SHA "de53969f545d0c46a59f9b38eb256e973b0059303267c03feca3010a74486883"

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
};
#endif

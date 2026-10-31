#ifndef VP_TEST_PSP2_TYPES_H
#define VP_TEST_PSP2_TYPES_H
/* Minimal host-side subset of the VitaSDK scalar types used by
 * src/platform/guest_storage.c. Layouts match psp2common/types.h. */
#include <stdint.h>

typedef int SceUID;
typedef int SceMode;
typedef int64_t SceOff;
typedef unsigned int SceSize;
typedef signed int SceSSize;

typedef struct SceDateTime {
    unsigned short year;
    unsigned short month;
    unsigned short day;
    unsigned short hour;
    unsigned short minute;
    unsigned short second;
    unsigned int microsecond;
} SceDateTime;

#endif

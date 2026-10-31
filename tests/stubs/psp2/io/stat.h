#ifndef VP_TEST_PSP2_IO_STAT_H
#define VP_TEST_PSP2_IO_STAT_H
#include <psp2/types.h>

/* Same field order and 0x58-byte size as the locked VitaSDK SceIoStat. */
typedef struct SceIoStat {
    SceMode st_mode;
    unsigned int st_attr;
    SceOff st_size;
    SceDateTime st_ctime;
    SceDateTime st_atime;
    SceDateTime st_mtime;
    unsigned int st_private[6];
} SceIoStat;

int sceIoGetstat(const char *file, SceIoStat *stat);
int sceIoGetstatByFd(SceUID fd, SceIoStat *stat);

#endif

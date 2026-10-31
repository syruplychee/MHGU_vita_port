#ifndef VP_TEST_PSP2_IO_FCNTL_H
#define VP_TEST_PSP2_IO_FCNTL_H
#include <psp2/types.h>

#define SCE_O_RDONLY 0x0001

SceUID sceIoOpen(const char *file, int flags, SceMode mode);
int sceIoClose(SceUID fd);
int sceIoPread(SceUID fd, void *data, SceSize size, SceOff offset);

#endif

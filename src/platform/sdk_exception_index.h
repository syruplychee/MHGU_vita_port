#ifndef VP_SDK_EXCEPTION_INDEX_H
#define VP_SDK_EXCEPTION_INDEX_H
#include "native_unwind.h"
typedef struct {
    uint32_t offset,entries;
    unsigned char digest[32];
} VPSdkExceptionIndex;
/* A private sorted EHABI view. Preserve source records, stable duplicate order,
 * and rebase both PREL31 fields; extab and personality code remain original.
 * Storage is inside the owning arena, beyond its original image and islands. */
int vp_sdk_exception_index(const VPUnwindModule *,uint32_t,VPSdkExceptionIndex *);
int vp_sdk_exception_index_verify(const VPUnwindModule *,const VPSdkExceptionIndex *);
#endif

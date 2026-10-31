#ifndef VP_NATIVE_KUBRIDGE_H
#define VP_NATIVE_KUBRIDGE_H
#include "native_startup.h"
/* Requires a compatible installed kubridge kernel plugin and its import stub.
 * Compile/link support is not evidence of hardware permission/cache behavior. */
int vp_vita_module_permissions(VPNativeModule *,uint32_t);
#endif

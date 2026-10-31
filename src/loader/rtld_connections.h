#ifndef VP_RTLD_CONNECTIONS_H
#define VP_RTLD_CONNECTIONS_H
#include "native_link.h"
/* Exact locked 1.4.0 rtld ModuleObject, 0x00 next / 0x04 prev.
 * Field offsets are bound to audited rtld+0xac4, not a host C++ ABI. */
typedef struct { uint32_t words[24]; } VPRtldObject;
/* Objects must remain alive until all guest execution stops. All preconditions
 * are checked before linking; original rtld relocation/init entries stay unused. */
int vp_rtld_connect(VPNativeModule modules[4],VPRtldObject objects[4],const uint32_t scope[4]);
#endif

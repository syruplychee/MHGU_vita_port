#ifndef VP_NATIVE_SDK_DSO_H
#define VP_NATIVE_SDK_DSO_H
#include "native_executor.h"
#include <stdio.h>
int vp_sdk_dso_patch(FILE *,VPNativeModule[4]);
int vp_sdk_dso_execute(FILE *,VPExecutor *);
int vp_sdk_dso_replaces_tls(uint32_t);
int vp_sdk_thread_pool_prepare(VPNativeModule *,unsigned,FILE *);
int vp_sdk_thread_pool_stop(FILE *);
/* Execution facts observed by the original-Start run. `nnos_returned` is the
 * rtld callback flag written when nnosInitialize returns; `nnmain_entered`
 * is set only when a live return address inside main's nnMain body was found
 * on the main thread's stack. Because the original Start chain runs all
 * DT_INIT callbacks before nnMain, this observation also implies the original
 * constructors completed. No value is set from a timeout or a fabricated
 * service answer. */
unsigned vp_sdk_dso_nnos_returned(void);
unsigned vp_sdk_dso_nnmain_entered(void);
unsigned vp_sdk_dso_session_stopped(void);
/* 1 only when the title released its exit lock after an applet Exit request,
 * so the platform's ISelfController model terminated the process. This is a
 * process termination, not a clean nnMain return and not the loader stop. */
unsigned vp_sdk_dso_title_exited(void);
#endif

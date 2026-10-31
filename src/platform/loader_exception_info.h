#ifndef VP_LOADER_EXCEPTION_INFO_H
#define VP_LOADER_EXCEPTION_INFO_H
#include "native_unwind.h"
/* Locked SDK nn::ro::detail::ExceptionInfo ABI: base, image span, module-relative
 * ARM exception table offset, table bytes. Caller validates writable output.
 * Returns 1 found, 0 outside module text, negative on invalid/stale registry. */
int vp_loader_exception_info(const VPUnwindRegistry *,uint32_t,uint32_t[4]);
#endif

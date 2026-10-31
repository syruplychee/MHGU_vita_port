#include "loader_exception_info.h"
#include <string.h>
int vp_loader_exception_info(const VPUnwindRegistry *registry,uint32_t pc,uint32_t out[4]) {
    if (!out) return VP_UNWIND_ARGUMENT;
    int rc=vp_unwind_verify(registry);if (rc) return rc;
    pc&=~1u;
    for (unsigned i=0;i<registry->count;++i) {
        const VPUnwindModule *m=&registry->modules[i];
        if (pc<m->text_begin || pc>=m->text_end) continue;
        uint32_t values[4]={m->base,m->nso.span,m->offset,m->entries*8};
        memcpy(out,values,sizeof(values));return 1;
    }
    return 0;
}

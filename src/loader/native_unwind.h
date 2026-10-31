#ifndef VP_NATIVE_UNWIND_H
#define VP_NATIVE_UNWIND_H
#include "native_link.h"
enum { VP_UNWIND_ARGUMENT=-90,VP_UNWIND_METADATA=-91,VP_UNWIND_STATE=-92,VP_UNWIND_NOT_FOUND=-93,
       VP_UNWIND_AMBIGUOUS=-94 };
typedef struct {
    const VPNativeModule *module;
    const unsigned char *image;
    VPNSO nso;
    uint32_t mod0;
    uint32_t base,text_begin,text_end,offset,entries,order_breaks;
    unsigned char digest[32];
} VPUnwindModule;
typedef struct { VPUnwindModule modules[4];uint32_t count,sealed; } VPUnwindRegistry;
typedef struct {
    uint32_t module,table_address,entries,entry_index,function_address,descriptor_address,descriptor;
} VPUnwindLookup;
/* Register ARM EHABI PREL31 tables supplied by MOD0. No relocations are applied
 * to PREL31 words: module-internal relative distances survive whole-image moves.
 * This registry supplies metadata/diagnostics; language personalities remain
 * original SDK code. It is not an exception interpreter or a cross-host unwinder. */
int vp_unwind_register(VPUnwindRegistry *,const VPNativeModule *,uint32_t);
int vp_unwind_lookup(const VPUnwindRegistry *,uint32_t pc,VPUnwindLookup *);
int vp_unwind_verify(const VPUnwindRegistry *);
void vp_unwind_reset(VPUnwindRegistry *);
#endif

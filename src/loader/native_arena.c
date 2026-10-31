#include "native_arena.h"
static int align_up(uint32_t n,uint32_t alignment,uint32_t *out) {
    if (n>UINT32_MAX-(alignment-1)) return VP_LINK_STATE;
    *out=(n+alignment-1)&~(alignment-1);return 0;
}
int vp_native_arena_layout(uint32_t span,uint32_t bytes,VPArenaLayout *out) {
    if (!out || !span || span>0x8000000u || bytes>0x100000u) return VP_LINK_STATE;
    VPArenaLayout next={0};
    if (align_up(span,4096,&next.image_bytes)) return VP_LINK_STATE;
    next.capacity=next.image_bytes;
    if (bytes) {
        if (align_up(span,0x100000,&next.island_offset) || align_up(bytes,4096,&next.island_capacity)
            || next.island_offset>UINT32_MAX-next.island_capacity) return VP_LINK_STATE;
        next.capacity=next.island_offset+next.island_capacity;
    }
    *out=next;return 0;
}
int vp_native_arenas_check(const VPNativeModule *m,uint32_t count,int native) {
    if (!m || !count || count>4) return VP_LINK_STATE;
    for (uint32_t i=0;i<count;++i) {
        uint32_t image_pages,island_pages=0;uintptr_t ptr=(uintptr_t)m[i].image;
        if (!ptr || !m[i].nso.span || align_up(m[i].nso.span,4096,&image_pages)
            || (m[i].base&4095) || (m[i].capacity&4095) || m[i].capacity<image_pages
            || m[i].base>UINT32_MAX-m[i].capacity || ptr>UINTPTR_MAX-m[i].capacity
            || (native && ptr!=m[i].base)) return VP_LINK_STATE;
        if (m[i].island_bytes && (align_up(m[i].island_bytes,4096,&island_pages)
            || (m[i].island_offset&4095) || m[i].island_offset<image_pages
            || m[i].island_offset>m[i].capacity || island_pages>m[i].capacity-m[i].island_offset)) return VP_LINK_STATE;
        for (uint32_t j=0;j<i;++j) {
            uintptr_t other=(uintptr_t)m[j].image;
            if ((m[i].base<m[j].base+m[j].capacity && m[j].base<m[i].base+m[i].capacity)
                || (ptr<other+m[j].capacity && other<ptr+m[i].capacity)) return VP_LINK_STATE;
        }
    }
    return 0;
}

#include <stdio.h>
#include "gpu_indices.h"
int main(void) {
    const uint16_t strip[]={0,1,2,0xffff,2,3,4,5};
    uint32_t maximum;
    VpGpuIndexRange range;
    if (!vp_gpu_index_max(strip,2,8,1,0xffff,&maximum) || maximum!=5) return 1;
    if (!vp_gpu_index_max(strip,2,8,0,0xffff,&maximum) || maximum!=65535) return 2;
    const uint32_t wide[]={4,0xffffffff,17};
    if (!vp_gpu_index_max(wide,4,3,1,0xffffffff,&maximum) || maximum!=17) return 3;
    const uint8_t empty[]={255,255};
    if (vp_gpu_index_max(empty,1,2,1,255,&maximum)) return 4;
    const uint8_t unaligned[]={99,3,0,7,0};
    if (!vp_gpu_index_max(unaligned+1,2,2,0,0,&maximum) || maximum!=7) return 5;

    if (!vp_gpu_index_range(strip,2,8,1,0xffff,&range) ||
        range.minimum!=0 || range.maximum!=5 || range.real_indices!=7 ||
        range.restart_indices!=1) return 6;
    if (!vp_gpu_index_range(wide,4,3,1,0xffffffff,&range) ||
        range.minimum!=4 || range.maximum!=17 || range.real_indices!=2 ||
        range.restart_indices!=1) return 7;
    if (vp_gpu_index_range(empty,1,2,1,255,&range) ||
        range.real_indices!=0 || range.restart_indices!=2) return 8;
    if (!vp_gpu_index_range(unaligned+1,2,2,0,0,&range) ||
        range.minimum!=3 || range.maximum!=7) return 9;
    if (vp_gpu_index_range(unaligned+1,2,0,0,0,&range) ||
        range.real_indices!=0 || range.restart_indices!=0) return 10;
    if (vp_gpu_index_range(unaligned,3,1,0,0,&range)>=0) return 11;
    if (vp_gpu_index_range(NULL,2,1,0,0,&range)>=0) return 12;
    if (vp_gpu_index_range(unaligned,2,1,0,0,NULL)>=0) return 13;
    puts("PASS GPU index range: restart, ordinary sentinel, U32, empty, min/max and unaligned");
    return 0;
}

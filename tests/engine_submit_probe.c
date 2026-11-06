#include "engine_submit_probe.h"
#include <assert.h>
#include <string.h>

int main(void) {
    const uint32_t sites[8]={0xbbee38u,0xbbefa0u,0xbbf2b8u,0xbbf3ecu,
        0xbbf83cu,0xbbfd04u,0xbbffd8u,0xbc0120u};
    VPEngineSubmitStats stats;
    memset(&stats,0,sizeof(stats));
    for (unsigned i=0;i<8u;i++) {
        assert(vp_engine_submit_family(sites[i]+4u)==i);
        assert(vp_engine_submit_family(sites[i])==8u);
        vp_engine_submit_account(&stats,i,i==0u,i<4u,UINT32_MAX,64u);
    }
    vp_engine_submit_account(&stats,99u,0,0,1u,0u);
    assert(stats.calls==9u && stats.same_object==1u && stats.same_layout==4u);
    assert(stats.elapsed_us==(uint64_t)UINT32_MAX*8u+1u);
    assert(stats.encoded_bytes==512u && stats.max_us==UINT32_MAX);
    for (unsigned i=0;i<9u;i++) assert(stats.families[i]==1u);
    return 0;
}

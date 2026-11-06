#include "engine_queue_native.h"
#include <stdio.h>
static int traced_queue(const VPEngineNvnEnv *,const VPEngineMaterialEnv *,uint32_t,uint32_t);
#define vp_engine_queue_native_run traced_queue
#include "../../../tests/engine_queue_native.c"
#undef vp_engine_queue_native_run
static int traced_queue(const VPEngineNvnEnv *e,const VPEngineMaterialEnv *m,uint32_t ctx,uint32_t state) {
    static unsigned serial;unsigned id=++serial,begin=world.event_count;char path[100];FILE *f;
    sprintf(path,"case-%u-before.bin",id);f=fopen(path,"wb");fwrite(world.memory,1,world.size,f);fclose(f);
    int rc=vp_engine_queue_native_run(e,m,ctx,state);
    sprintf(path,"case-%u-after.bin",id);f=fopen(path,"wb");fwrite(world.memory,1,world.size,f);fclose(f);
    sprintf(path,"case-%u-events.bin",id);f=fopen(path,"wb");fwrite(world.events+begin,sizeof(Event),world.event_count-begin,f);fclose(f);
    sprintf(path,"case-%u.txt",id);f=fopen(path,"w");fprintf(f,"%d %u %u %u %u %u %u %llu\n",rc,ctx,state,e->default_viewport,e->srgb_config,e->gpu_root,e->scratch,(unsigned long long)world.buffer_address);fclose(f);return rc;
}

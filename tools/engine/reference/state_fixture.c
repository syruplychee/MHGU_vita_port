#include "engine_state.h"
#include <stdio.h>
static int traced_targets(const VPEngineNvnEnv *,uint32_t,uint32_t,uint32_t);
static int traced_pipeline(const VPEngineNvnEnv *,uint32_t,uint32_t,uint32_t);
#define vp_engine_state_targets traced_targets
#define vp_engine_state_pipeline traced_pipeline
#include "../../../tests/engine_state.c"
#undef vp_engine_state_targets
#undef vp_engine_state_pipeline
static int run(int type,const VPEngineNvnEnv *e,uint32_t ctx,uint32_t st,uint32_t obj) {
    static unsigned serial;unsigned id=++serial,begin=world.count;char path[100];FILE *f;
    sprintf(path,"case-%u-before.bin",id);f=fopen(path,"wb");fwrite(world.memory,1,world.size,f);fclose(f);
    int rc=type?vp_engine_state_pipeline(e,ctx,st,obj):vp_engine_state_targets(e,ctx,st,obj);
    sprintf(path,"case-%u-after.bin",id);f=fopen(path,"wb");fwrite(world.memory,1,world.size,f);fclose(f);
    sprintf(path,"case-%u-events.bin",id);f=fopen(path,"wb");fwrite(world.events+begin,sizeof(Event),world.count-begin,f);fclose(f);
    sprintf(path,"case-%u.txt",id);f=fopen(path,"w");fprintf(f,"%d %d %u %u %u %u %u\n",rc,type,ctx,st,obj,e->default_viewport,e->scratch);fclose(f);return rc;
}
static int traced_targets(const VPEngineNvnEnv *e,uint32_t c,uint32_t s,uint32_t o){return run(0,e,c,s,o);}
static int traced_pipeline(const VPEngineNvnEnv *e,uint32_t c,uint32_t s,uint32_t o){return run(1,e,c,s,o);}

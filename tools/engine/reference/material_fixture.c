#include "engine_material.h"
#include <stdio.h>
static int traced_submit(const VPEngineMaterialEnv *,uint32_t,uint32_t,uint32_t);
#define vp_engine_material_submit traced_submit
#include "../../../tests/engine_material.c"
#undef vp_engine_material_submit
static int traced_submit(const VPEngineMaterialEnv *e,uint32_t ctx,uint32_t state,uint32_t mat) {
    static unsigned serial;unsigned id=++serial,begin=event_count,tq=texture_queries;
    char path[100];FILE *f;
    sprintf(path,"case-%u-before.bin",id);f=fopen(path,"wb");fwrite(arena,1,sizeof(arena),f);fclose(f);
    int rc=vp_engine_material_submit(e,ctx,state,mat);
    sprintf(path,"case-%u-after.bin",id);f=fopen(path,"wb");fwrite(arena,1,sizeof(arena),f);fclose(f);
    sprintf(path,"case-%u.txt",id);f=fopen(path,"w");
    fprintf(f,"%d %u %u %u %u %u\n",rc,ctx,state,mat,e?e->gpu_root:0,tq);
    for(unsigned i=begin;i<event_count;i++) {
        Event *v=&events[i];uint32_t bits;memcpy(&bits,&v->f,4);
        fprintf(f,"%u %u %u %u %u %llu %u\n",v->kind,v->a,v->b,v->c,v->e,(unsigned long long)v->d,bits);
    }
    fclose(f);return rc;
}

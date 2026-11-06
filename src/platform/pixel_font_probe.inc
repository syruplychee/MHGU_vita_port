/* Opt-in evidence capture; font payload identity is established by the resolver. */
static void pixel_font_probe_file(unsigned n, const char *name,
                                  const void *data, size_t bytes) {
    if (!data || !bytes) return;
    char path[160];
    snprintf(path,sizeof(path),"ux0:data/mhgu-vita-port/font-probe-%03u-%s.bin",n,name);
    FILE *file=fopen(path,"wb");
    if (file) { fwrite(data,1u,bytes,file); fclose(file); }
}

static void pixel_font_probe_draw(const VPGxmDrawRequest *req) {
    static int enabled=-1;
    static unsigned bucket=~0u, in_bucket, captures;
    static uint64_t lineage[64];
    static unsigned lineage_used, passes;
    static uint32_t signatures[64];
    if (enabled<0) {
        FILE *marker=fopen("ux0:data/mhgu-vita-port/pixel-font-trace.enable","rb");
        enabled=marker!=NULL;
        if (marker) fclose(marker);
    }
    if (!enabled) return;
    int font=req->tex_format==VP_TEXTURE_UPLOAD_ALPHA8;
    int inherited=0;
    for (unsigned i=0;i<lineage_used;++i) {
        if (req->texture_va==lineage[i]) inherited=1;
        for (unsigned t=0;t<VP_GXM_FRAGMENT_SAMPLER_LIMIT-1u;++t)
            if (req->extra_textures[t].va==lineage[i]) inherited=1;
    }
    if (font || inherited) {
        unsigned i=0;
        for (;i<lineage_used;++i) if (lineage[i]==req->target_va) break;
        if (i==lineage_used && lineage_used<64u) lineage[lineage_used++]=req->target_va;
    }
    int consumer=0;
    if (inherited && !font && passes<64u) {
        uint32_t signature=req->hash^(uint32_t)req->texture_va^(uint32_t)req->target_va;
        unsigned i=0;
        for (;i<passes;++i) if (signatures[i]==signature) break;
        if (i==passes) { signatures[passes++]=signature;consumer=1; }
    }
    if ((!font && !consumer) || captures>=80u) return;
    unsigned now=(unsigned)(sceKernelGetProcessTimeWide()/1000000u);
    if (bucket!=now/10u) { bucket=now/10u; in_bucket=0u; }
    if (font && in_bucket++>=2u) return;
    unsigned n=captures++;
    char path[160];
    snprintf(path,sizeof(path),"ux0:data/mhgu-vita-port/font-probe-%03u.json",n);
    FILE *file=fopen(path,"wb");
    if (!file) return;
    fprintf(file,"{\"seconds\":%u,\"vs\":%u,\"fs\":%u,\"hash\":%u,"
        "\"count\":%u,\"stride\":%u,\"indexed\":%d,\"index_count\":%u,"
        "\"index_format\":%u,\"target\":[%u,%u],\"viewport\":[%u,%u,%u,%u],\"attributes\":[",
        now,req->vs_token,req->fs_token,req->hash,req->count,req->stride,
        req->indexed,req->index_count,req->index_format,req->target_width,
        req->target_height,req->viewport_x,req->viewport_y,req->viewport_width,req->viewport_height);
    for (unsigned i=0;i<16u;++i) fprintf(file,"%s%u",i?",":"",req->attributes[i]);
    fprintf(file,"],\"display_target\":%u,\"font\":%d,\"target_va\":\"%llx\",\"texture_va\":\"%llx\","
            "\"texture_size\":[%u,%u],\"sampler\":[%u,%u],\"extra\":[",
            req->display_target,font,(unsigned long long)req->target_va,
            (unsigned long long)req->texture_va,req->tex_width,req->tex_height,req->sampler[0],req->sampler[1]);
    for (unsigned i=0;i<VP_GXM_FRAGMENT_SAMPLER_LIMIT-1u;++i) {
        const VPGxmExtraTexture *t=&req->extra_textures[i];
        fprintf(file,"%s{\"va\":\"%llx\",\"size\":[%u,%u]}",i?",":"",(unsigned long long)t->va,t->width,t->height);
    }
    fprintf(file,"]}\n");
    fclose(file);
    pixel_font_probe_file(n,"vertices",req->vertices,(size_t)req->count*req->stride);
    if (req->indexed) pixel_font_probe_file(n,"indices",req->indices,
        (size_t)req->index_count*(req->index_format==SCE_GXM_INDEX_FORMAT_U32?4u:2u));
    for (unsigned s=0;s<2u;++s) for (unsigned b=0;b<18u;++b) {
        char name[32];
        snprintf(name,sizeof(name),"%s-cbuf%u",s?"fs":"vs",b);
        const VPGxmUniform *u=&req->stage_cbuf[s][b];
        pixel_font_probe_file(n,name,u->data,u->size);
    }
    glog("PIXEL_FONT_DRAW_PROBE n=%u seconds=%u hash=%x vs=%x fs=%x count=%u stride=%u\n",
         n,now,req->hash,req->vs_token,req->fs_token,req->count,req->stride);
}

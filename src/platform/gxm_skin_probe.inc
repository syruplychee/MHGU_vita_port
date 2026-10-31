/* Bounded, opt-in capture of real skinning inputs. Disabled in normal
 * play. Two snapshots per shader/layout, at most sixteen draws in total.
 *
 * B78 frame mode is selected by skin-frame.enable (delay in process-time
 * milliseconds, default 158000). It captures every eligible draw of exactly
 * one present interval's main-view draws instead of legacy per-layout samples. */
static void gxm_skin_frame_raw(const char *path, const void *data, size_t bytes,
                               int *io_error) {
    FILE *file;
    size_t done;
    if (data == NULL || bytes == 0u) return;
    file = fopen(path, "wb");
    if (file == NULL) {
        glog("VP_SKIN_FRAME_IO op=open path=%s\n", path);
        *io_error = 1;
        return;
    }
    done = fwrite(data, 1u, bytes, file);
    if (done != bytes || ferror(file)) {
        glog("VP_SKIN_FRAME_IO op=write path=%s want=%llu got=%llu\n", path,
             (unsigned long long)bytes, (unsigned long long)done);
        *io_error = 1;
    }
    if (fclose(file) != 0) {
        glog("VP_SKIN_FRAME_IO op=close path=%s\n", path);
        *io_error = 1;
    }
}

static void gxm_skin_frame_probe(const VPGxmDrawRequest *req, unsigned delay_ms) {
    enum { SKIN_ARMED = 0, SKIN_WAIT, SKIN_CAPTURE, SKIN_DONE };
    static int state;
    static uint64_t wait_present, frame_present, payload_bytes;
    static unsigned draws, truncated;
    static int io_error;
    const VPGxmExtraTexture *bone = &req->vertex_texture;
    uint64_t present = gxm_perf_present_frames;
    uint64_t now_ms = sceKernelGetProcessTimeWide() / 1000u;
    FILE *file;
    char path[160];
    if (state == SKIN_DONE) return;
    if (state == SKIN_ARMED) {
        if (now_ms < (uint64_t)delay_ms) return;
        wait_present = present;
        state = SKIN_WAIT;
        glog("VP_SKIN_FRAME wait_present=%llu delay_ms=%u\n",
             (unsigned long long)wait_present, delay_ms);
        return;
    }
    if (state == SKIN_WAIT) {
        if (present == wait_present) return;
        frame_present = present;
        state = SKIN_CAPTURE;
        glog("VP_SKIN_FRAME begin frame=%llu wait_present=%llu viewport=1280x720 max_draws=256 max_bytes=134217728\n",
             (unsigned long long)frame_present, (unsigned long long)wait_present);
    } else if (present != frame_present) {
        const char *status = truncated ? (io_error ? "truncated+io-error" : "truncated")
                             : io_error ? "io-error" : "complete";
        state = SKIN_DONE;
        glog("VP_SKIN_FRAME_DONE frame=%llu draws=%u bytes=%llu truncated=%u "
             "io_error=%u status=%s\n", (unsigned long long)frame_present, draws,
             (unsigned long long)payload_bytes, truncated, io_error, status);
        return;
    }
    if (truncated) return;
    if (bone->texels == NULL || bone->tex_format != VP_TEXTURE_UPLOAD_RGBA16F ||
        req->viewport_width != 1280u || req->viewport_height != 720u) return;
    unsigned n = draws;
    unsigned index_size = req->index_format == 2u ? 4u : req->index_format == 1u ? 2u : 1u;
    uint64_t vertex_bytes = req->vertices ? (uint64_t)req->count * req->stride : 0u;
    uint64_t index_bytes = req->indexed && req->indices
                         ? (uint64_t)req->index_count * index_size : 0u;
    uint64_t bone_bytes = (uint64_t)vp_texture_upload_bytes(bone->width, bone->height,
                                                            bone->tex_format);
    uint64_t total = vertex_bytes + index_bytes + bone_bytes;
    for (unsigned s = 0; s < 2u; ++s)
        for (unsigned b = 0; b < 18u; ++b)
            if (req->stage_cbuf[s][b].data && req->stage_cbuf[s][b].size)
                total += req->stage_cbuf[s][b].size;
    if (draws >= 256u || payload_bytes + total > (128ull << 20)) {
        truncated = 1;
        glog("VP_SKIN_FRAME_TRUNCATED frame=%llu draws=%u bytes=%llu draw=%u "
             "need=%llu reason=%s\n", (unsigned long long)frame_present, draws,
             (unsigned long long)payload_bytes, req->draw_index,
             (unsigned long long)total, draws >= 256u ? "draws" : "bytes");
        return;
    }
    snprintf(path, sizeof(path), "ux0:data/mhgu-vita-port/skin-frame-%u.json", n);
    file = fopen(path, "wb");
    if (file == NULL) {
        glog("VP_SKIN_FRAME_IO op=open path=%s\n", path);
        io_error = 1;
    } else {
        fprintf(file, "{\"present\":%llu,\"ms\":%llu,\"draw\":%u,\"hash\":%u,\"viewport_transform\":%u,"
            "\"vs\":%u,\"fs\":%u,\"count\":%u,\"stride\":%u,\"indexed\":%d,"
            "\"topology\":%u,\"index_count\":%u,\"index_format\":%u,"
            "\"base_vertex\":%d,\"window_first\":%u,\"restart\":%u,"
            "\"restart_index\":%u,\"attributes\":[",
            (unsigned long long)present, (unsigned long long)now_ms,
            req->draw_index, req->hash, req->viewport_transform, req->vs_token, req->fs_token,
            req->count, req->stride, req->indexed, req->topology,
            req->index_count, req->index_format, (int)req->base_vertex,
            req->index_window_first, req->restart_enabled, req->restart_index);
        for (unsigned a = 0; a < 16u; ++a)
            fprintf(file, "%s%u", a ? "," : "", req->attributes[a]);
        fprintf(file, "],\"target\":{\"va\":%llu,\"width\":%u,\"height\":%u,"
            "\"format\":%u},\"viewport\":{\"x\":%u,\"y\":%u,\"width\":%u,"
            "\"height\":%u,\"scale_z\":%u,\"translate_z\":%u,\"z_valid\":%u},"
            "\"depth\":{\"test\":%u,\"write\":%u,\"func\":%u,"
            "\"bias_factor\":%d,\"bias_units\":%d},"
            "\"depth_state\":{\"enable\":%u,\"va\":%llu,\"width\":%u,"
            "\"height\":%u,\"depth\":%u,\"dim_control\":%u,\"format\":%u,"
            "\"tile_mode\":%u,\"array_pitch\":%u,\"stencil_enable\":%u},"
            "\"cull\":{\"enable\":%u,\"front_face\":%u,\"face\":%u},"
            "\"color_mask\":%u,\"blend\":{\"color_op\":%u,\"color_src\":%u,"
            "\"color_dst\":%u,\"alpha_op\":%u,\"alpha_src\":%u,"
            "\"alpha_dst\":%u},\"scissor\":{\"enable\":%u,\"min_x\":%u,"
            "\"max_x\":%u,\"min_y\":%u,\"max_y\":%u},"
            "\"bone\":{\"width\":%u,\"height\":%u,\"va\":%llu,\"format\":%u,"
            "\"sampler\":[%u,%u]},\"fs_tex\":{\"va\":%llu,\"width\":%u,"
            "\"height\":%u,\"format\":%u,\"source\":%u,\"tic_format\":%u,"
            "\"descriptor_type\":%u,\"sampler\":[%u,%u]},\"extra\":[",
            (unsigned long long)req->target_va, req->target_width,
            req->target_height, req->target_format, req->viewport_x,
            req->viewport_y, req->viewport_width, req->viewport_height,
            req->viewport_scale_z, req->viewport_translate_z,
            req->viewport_z_valid, req->depth_test, req->depth_write,
            req->depth_func, req->depth_bias_factor, req->depth_bias_units,
            req->depth_state.enable, (unsigned long long)req->depth_state.va,
            req->depth_state.width, req->depth_state.height,
            req->depth_state.depth, req->depth_state.dim_control,
            req->depth_state.format, req->depth_state.tile_mode,
            req->depth_state.array_pitch, req->depth_state.stencil_enable,
            req->cull_enable, req->front_face, req->cull_face,
            req->color_mask, req->blend_color_op, req->blend_color_src,
            req->blend_color_dst, req->blend_alpha_op, req->blend_alpha_src,
            req->blend_alpha_dst, req->scissor_enable, req->scissor_min_x,
            req->scissor_max_x, req->scissor_min_y, req->scissor_max_y,
            bone->width, bone->height, (unsigned long long)bone->va,
            bone->tex_format, bone->sampler[0], bone->sampler[1],
            (unsigned long long)req->texture_va, req->tex_width,
            req->tex_height, req->tex_format, req->texture_source,
            req->tex_tic_format, req->tex_descriptor_type,
            req->sampler[0], req->sampler[1]);
        for (unsigned e = 0; e < VP_GXM_FRAGMENT_SAMPLER_LIMIT - 1u; ++e) {
            const VPGxmExtraTexture *extra = &req->extra_textures[e];
            fprintf(file, "%s{\"va\":%llu,\"width\":%u,\"height\":%u,"
                "\"format\":%u,\"source\":%u,\"tic_format\":%u,"
                "\"descriptor_type\":%u,\"sampler\":[%u,%u]}",
                e ? "," : "", (unsigned long long)extra->va, extra->width,
                extra->height, extra->tex_format, extra->source,
                extra->tic_format, extra->descriptor_type,
                extra->sampler[0], extra->sampler[1]);
        }
        fprintf(file, "],\"vs_cbuf_bytes\":[");
        for (unsigned b = 0; b < 18u; ++b)
            fprintf(file, "%s%u", b ? "," : "",
                    req->stage_cbuf[0][b].data ? req->stage_cbuf[0][b].size : 0u);
        fprintf(file, "],\"fs_cbuf_bytes\":[");
        for (unsigned b = 0; b < 18u; ++b)
            fprintf(file, "%s%u", b ? "," : "",
                    req->stage_cbuf[1][b].data ? req->stage_cbuf[1][b].size : 0u);
        fprintf(file, "]}\n");
        if (ferror(file)) {
            glog("VP_SKIN_FRAME_IO op=write path=%s\n", path);
            io_error = 1;
        }
        if (fclose(file) != 0) {
            glog("VP_SKIN_FRAME_IO op=close path=%s\n", path);
            io_error = 1;
        }
    }
    if (vertex_bytes) {
        snprintf(path, sizeof(path), "ux0:data/mhgu-vita-port/skin-frame-%u-vertices.bin", n);
        gxm_skin_frame_raw(path, req->vertices, (size_t)vertex_bytes, &io_error);
    }
    if (index_bytes) {
        snprintf(path, sizeof(path), "ux0:data/mhgu-vita-port/skin-frame-%u-indices.bin", n);
        gxm_skin_frame_raw(path, req->indices, (size_t)index_bytes, &io_error);
    }
    if (bone_bytes) {
        snprintf(path, sizeof(path), "ux0:data/mhgu-vita-port/skin-frame-%u-bone.bin", n);
        gxm_skin_frame_raw(path, bone->texels, (size_t)bone_bytes, &io_error);
    }
    for (unsigned s = 0; s < 2u; ++s) {
        for (unsigned b = 0; b < 18u; ++b) {
            const VPGxmUniform *cb = &req->stage_cbuf[s][b];
            if (cb->data == NULL || cb->size == 0u) continue;
            snprintf(path, sizeof(path),
                     "ux0:data/mhgu-vita-port/skin-frame-%u-%s-cbuf%u.bin",
                     n, s == 0u ? "vs" : "fs", b);
            gxm_skin_frame_raw(path, cb->data, cb->size, &io_error);
        }
    }
    payload_bytes += total;
    draws++;
}

static void gxm_skin_probe(const VPGxmDrawRequest *req) {
    static int enabled = -1, frame_enabled = -1;
    static unsigned count, delay_ms = 160000u, frame_delay_ms = 158000u;
    static unsigned live_generation, live_limit;
    static struct { unsigned token, stride, attr; unsigned hits; } seen[16];
    static unsigned used;
    const VPGxmExtraTexture *bone = &req->vertex_texture;
    FILE *file;
    char path[160];
    if (frame_enabled < 0) {
        FILE *marker = fopen("ux0:data/mhgu-vita-port/skin-frame.enable", "rb");
        frame_enabled = marker != NULL;
        if (marker) {
            unsigned delay;
            if (fscanf(marker, "%u", &delay) == 1) frame_delay_ms = delay;
            fclose(marker);
        }
        if (frame_enabled) glog("VP_SKIN_FRAME enabled=1 delay_ms=%u\n", frame_delay_ms);
    }
    if (frame_enabled) { gxm_skin_frame_probe(req, frame_delay_ms); return; }
    if (enabled < 0) {
        file = fopen("ux0:data/mhgu-vita-port/skin-probe.enable", "rb");
        enabled = file != NULL;
        if (file) { unsigned delay; if (fscanf(file, "%u", &delay) == 1) delay_ms = delay; fclose(file); }
    }
    b34_dump_due(); /* Poll only the explicitly enabled live mailbox. */
    if (b34_live_generation != live_generation) {
        live_generation = b34_live_generation;
        if (b34_live_inputs) {
            memset(seen, 0, sizeof(seen)); used = 0u;
            live_limit = count + 16u;
            enabled = 1; delay_ms = 0u;
        }
    }
    if (!enabled || count >= (live_limit ? live_limit : 16u) || bone->texels == NULL ||
        bone->tex_format != VP_TEXTURE_UPLOAD_RGBA16F ||
        sceKernelGetProcessTimeWide() / 1000u < delay_ms) return;
    unsigned slot;
    for (slot = 0; slot < used; ++slot)
        if (seen[slot].token == req->vs_token && seen[slot].stride == req->stride &&
            seen[slot].attr == req->attributes[3]) break;
    if (slot == used) {
        if (used == 16u) return;
        seen[used].token = req->vs_token; seen[used].stride = req->stride;
        seen[used].attr = req->attributes[3]; ++used;
    }
    if (seen[slot].hits++ >= 2u) return;
    unsigned n = count++;
    snprintf(path, sizeof(path), "ux0:data/mhgu-vita-port/skin-%u.json", n);
    file = fopen(path, "wb");
    if (file) {
        fprintf(file, "{\"n\":%u,\"draw\":%u,\"vs\":%u,\"fs\":%u,\"count\":%u,\"stride\":%u,"
            "\"index_count\":%u,\"index_format\":%u,\"window_first\":%u,\"restart\":%u,\"restart_index\":%u,"
            "\"bone_width\":%u,\"bone_height\":%u,\"bone_va\":%llu,\"sampler\":[%u,%u],\"attributes\":[",
            n,req->draw_index,req->vs_token,req->fs_token,req->count,req->stride,
            req->index_count,req->index_format,req->index_window_first,req->restart_enabled,req->restart_index,
            bone->width,bone->height,(unsigned long long)bone->va,bone->sampler[0],bone->sampler[1]);
        for (unsigned a=0;a<16u;++a) fprintf(file,"%s%u",a?",":"",req->attributes[a]);
        fprintf(file,"]}\n"); fclose(file);
    }
#define SKIN_BYTES(label, pointer, size) do { \
    const void *skin_data=(pointer); size_t skin_size=(size); \
    if (skin_data && skin_size) { \
        snprintf(path,sizeof(path),"ux0:data/mhgu-vita-port/skin-%u-%s.bin",n,label); \
        file=fopen(path,"wb"); if(file){fwrite(skin_data,1,skin_size,file);fclose(file);} \
    } \
} while(0)
    SKIN_BYTES("vertices",req->vertices,(size_t)req->count*req->stride);
    SKIN_BYTES("indices",req->indices,(size_t)req->index_count*(req->index_format==2u?4u:req->index_format==1u?2u:1u));
    SKIN_BYTES("bone",bone->texels,vp_texture_upload_bytes(bone->width,bone->height,bone->tex_format));
    for (unsigned b=0;b<18u;++b) {
        char name[24]; snprintf(name,sizeof(name),"vs-cbuf%u",b);
        SKIN_BYTES(name,req->stage_cbuf[0][b].data,req->stage_cbuf[0][b].size);
    }
#undef SKIN_BYTES
    glog("VP_SKIN_PROBE n=%u idx=%u vs=0x%x count=%u stride=%u bone=%ux%u\n",
         n,req->draw_index,req->vs_token,req->count,req->stride,bone->width,bone->height);
}

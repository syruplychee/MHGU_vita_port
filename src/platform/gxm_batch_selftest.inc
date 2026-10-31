/* Opt-in GPU mechanism diagnostic. Included after the production executor.
 * Uses the locked f9d passthrough/sample pair, real batch staging, uniform
 * reservations, clear, depth attachment and scene completion. Synthetic
 * resources never count as actual game/full-frame acceptance. */

/* B44: write one diagnostic image of a 64x32 fixture surface. The bytes come
 * from the CPU mapping that the locked backend writes back when a scene ends,
 * so they are only meaningful when the clear/sample scene actually completed. */
static int gxm_batch_selftest_image(const char *kind, unsigned mode,
                                    unsigned test, unsigned phase,
                                    const VPGxmSurface *s) {
    char path[160];
    FILE *dump;
    size_t count = (size_t)s->width * s->height;

    snprintf(path, sizeof(path),
             "ux0:data/mhgu-vita-port/batch-selftest-%s-m%u-c%u-p%u.rgba",
             kind, mode, test, phase);
    dump = fopen(path, "wb");
    if (dump == NULL) return -1;
    if (fwrite(s->color_mem, 4u, count, dump) != count) {
        fclose(dump);
        return -1;
    }
    return fclose(dump) == 0 ? 0 : -1;
}

/* B44 regression: render A, fully clear A, and sample A into B as the very
 * next draw, with no intervening draw that could hide stale contents. The
 * failing game case (B41 seq1 / B43 probe) is a reused 1280x720 colour
 * surface, so the fixture uses that shape rather than the 64x32 cases above.
 * test 6 clears to transparent black, test 7 to an opaque non-zero colour, and
 * test 8 repeats the cycle with a second render/clear/sample using reused
 * surfaces. Both the post-clear CPU image of A and the sampled image of B are
 * dumped so a stale texture view can be distinguished from a clear that never
 * wrote. */
#define VP_B44_W 1280u
#define VP_B44_H 720u
static int gxm_batch_selftest_clear_sample(unsigned mode, unsigned test,
                                           unsigned *encoded_out) {
    static const float quad[16] = {-1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0.f,
                                   -1.f, -1.f, 0.f, 1.f, 1.f, -1.f, 1.f, 1.f};
    const unsigned phases = test == 8u ? 2u : 1u;
    float vertices[16], gain[4];
    uint32_t texels[64];
    unsigned encoded = 0u;
    int rc = 0;

    for (unsigned phase = 0u; phase < phases && rc == 0; ++phase) {
        uint32_t render_color = phase == 1u ? 0xff0000c8u : 0xff00a000u;
        uint32_t clear_color = test == 7u ? 0xff00c800u
                              : phase == 1u ? 0xffffffffu : 0x00000000u;
        VPGxmDrawRequest q;
        VPGxmClearRequest clear;
        VPGxmSurface *surface;

        memset(&q, 0, sizeof(q));
        memset(&clear, 0, sizeof(clear));
        memset(gain, 0, sizeof(gain));
        gain[0] = gain[1] = 1.f;
        q.vertices = vertices;
        q.count = 4u;
        q.stride = 16u;
        q.hash = q.vs_token = q.fs_token = 0xf9du;
        q.viewport_transform = 1u;
        q.attributes[0] = (7u << 27) | (1u << 21); /* F32x4 */
        q.stage_cbuf[1][3].data = gain;
        q.stage_cbuf[1][3].size = sizeof(gain);
        q.texture_source = VP_GXM_EXTRA_SOURCE_CPU;
        q.sampler[1] = 0x11u; /* point min/mag, repeat U/V */
        q.screen_width = q.target_width = q.viewport_width = VP_B44_W;
        q.screen_height = q.target_height = q.viewport_height = VP_B44_H;
        q.target_format = 0xd5u;
        q.target_va = 0x7fff00000ull; /* A */
        q.topology = 5u;
        q.frame_draw = 1;
        q.quiet = 1u;
        q.color_mask = 0x1111u;
        q.blend_color_op = q.blend_alpha_op = 1u;
        q.blend_color_src = q.blend_alpha_src = 2u;
        q.blend_color_dst = q.blend_alpha_dst = 1u;
        q.batch_scope = 0x7fff00001ull;

        clear.target_va = q.target_va;
        clear.width = VP_B44_W;
        clear.height = VP_B44_H;
        clear.format = q.target_format;
        clear.mask = 0x3cu;
        clear.color = render_color;
        clear.quiet = 1u;

        /* Known content in A through the real production draw/clear paths. */
        memcpy(vertices, quad, sizeof(vertices));
        for (unsigned i = 0u; i < 64u; ++i) texels[i] = render_color;
        q.texels = texels;
        q.tex_width = q.tex_height = 8u;
        q.draw_index = phase * 2u;
        rc = gxm_batch_flush_worker(GXM_BATCH_REASON_CLEAR);
        if (rc == 0) rc = frame_clear_internal(&clear);
        if (rc == 0) rc = frame_draw_internal(&q);
        encoded += q.batch_encoded;
        if (rc == 0) rc = gxm_batch_flush_worker(GXM_BATCH_REASON_SUBMIT);

        /* The clear under test, then the sample as the immediate next draw. */
        clear.color = clear_color;
        if (rc == 0) rc = frame_clear_internal(&clear);
        surface = surface_find(q.target_va);
        if (rc == 0 && surface != NULL)
            rc = gxm_batch_selftest_image("cleared", mode, test, phase,
                                          surface);
        else if (rc == 0)
            rc = -1;

        if (rc == 0) {
            q.texture_va = 0x7fff00000ull; /* sample A */
            q.texture_source = VP_GXM_EXTRA_SOURCE_GPU;
            q.texels = NULL;
            q.tex_width = VP_B44_W;
            q.tex_height = VP_B44_H;
            q.target_va = 0x7fff10000ull; /* B */
            q.draw_index = phase * 2u + 1u;
            rc = frame_draw_internal(&q);
            encoded += q.batch_encoded;
        }
        if (rc == 0) rc = gxm_batch_flush_worker(GXM_BATCH_REASON_SUBMIT);
        surface = surface_find(q.target_va);
        if (rc == 0 && surface != NULL) {
            VPGxmSurface *producer = surface_find(0x7fff00000ull);
            glog("VP_BATCH_SELFTEST_CLEAR_SAMPLE mode=%u case=%u phase=%u "
                 "clear=0x%08x a_mem=0x%llx b_mem=0x%llx a_drawn=%u\n",
                 mode, test, phase, clear_color,
                 (unsigned long long)(uintptr_t)
                     (producer != NULL ? producer->color_mem : NULL),
                 (unsigned long long)(uintptr_t)surface->color_mem,
                 producer != NULL ? producer->color_drawn : 0u);
        }
        if (rc == 0 && surface != NULL)
            rc = gxm_batch_selftest_image("sampled", mode, test, phase,
                                          surface);
        else if (rc == 0)
            rc = -1;
        if (rc == 0 && surface != NULL) {
            char path[128];
            FILE *dump;
            size_t count = (size_t)surface->width * surface->height;
            /* Generic per-case image used by the shared oracle checks. */
            snprintf(path, sizeof(path),
                     "ux0:data/mhgu-vita-port/batch-selftest-m%u-c%u.rgba",
                     mode, test);
            dump = fopen(path, "wb");
            if (dump == NULL) rc = -1;
            else {
                if (fwrite(surface->color_mem, 4u, count, dump) != count)
                    rc = -1;
                if (fclose(dump) != 0) rc = -1;
            }
        }
    }
    if (encoded_out != NULL) *encoded_out = encoded;
    return rc;
}

static int gxm_batch_selftest(void) {
    FILE *marker = fopen("ux0:data/mhgu-vita-port/batch-selftest.enable", "rb");
    VPGxmDrawRequest q;
    VPGxmClearRequest clear;
    float vertices[16], gain[4];
    uint32_t texels[64];
    unsigned api_failures = 0u;
    if (marker == NULL) return 0;
    fclose(marker);
    if (!static_mode) return -1;
    glog("VP_BATCH_SELFTEST begin fixture=f9d-vp1 size=64x32\n");
    for (unsigned mode = 0u; mode < 3u; ++mode) {
        for (unsigned test = 0u; test < (mode == 2u ? 1u : 9u); ++test) {
            unsigned encoded_before = static_stats.batch_encoded;
            unsigned completed_before = static_stats.batch_completed;
            unsigned failed_before = static_stats.batch_failed;
            unsigned encoded = 0u;
            int rc = 0;
            VPGxmSurface *output;
            char path[128];
            FILE *dump;
            gxm_batch_test_mode = mode == 0u ? 0 : 1;
            if (test >= 6u) {
                rc = gxm_batch_selftest_clear_sample(mode, test, &encoded);
                if (rc < 0) api_failures++;
                glog("VP_BATCH_SELFTEST mode=%u case=%u rc=%d encoded=%u "
                     "completed=%u failed=%u pending=%u actual_encoded=%u\n",
                     mode, test, rc,
                     static_stats.batch_encoded - encoded_before,
                     static_stats.batch_completed - completed_before,
                     static_stats.batch_failed - failed_before,
                     gxm_batch.policy.draws, encoded);
                continue;
            }
            memset(&q, 0, sizeof(q));
            q.vertices = vertices;
            q.count = 4u;
            q.stride = 16u;
            q.hash = q.vs_token = q.fs_token = 0xf9du;
            q.viewport_transform = 1u;
            q.attributes[0] = (7u << 27) | (1u << 21); /* F32x4 */
            q.stage_cbuf[1][3].data = gain;
            q.stage_cbuf[1][3].size = sizeof(gain);
            q.texels = texels;
            q.tex_width = q.tex_height = 8u;
            q.texture_source = VP_GXM_EXTRA_SOURCE_CPU;
            q.sampler[1] = 0x11u; /* point min/mag, repeat U/V */
            q.screen_width = q.target_width = q.viewport_width = 64u;
            q.screen_height = q.target_height = q.viewport_height = 32u;
            q.target_format = 0xd5u;
            q.target_va = 0x7fff00000ull;
            q.topology = 5u;
            q.frame_draw = 1;
            q.quiet = 1u;
            q.color_mask = 0x1111u;
            q.blend_color_op = q.blend_alpha_op = 1u;
            q.blend_color_src = q.blend_alpha_src = 2u;
            q.blend_color_dst = q.blend_alpha_dst = 1u;
            q.batch_scope = 0x7fff00001ull;
            if (test == 1u) {
                q.blend_color_src = 5u;
                q.blend_color_dst = 6u;
            }
            if (test == 2u) {
                q.depth_test = q.depth_write = 1u;
                q.depth_func = 2u; /* LESS: equal-depth second draw must fail. */
                q.depth_state.enable = 1u;
                q.depth_state.va = 0x7fff20000ull;
                q.depth_state.width = 64u;
                q.depth_state.height = 32u;
                q.depth_state.depth = 1u;
                q.depth_state.format = VP_GXM_DEPTH_FORMAT_Z32_FLOAT;
            }
            memset(&clear, 0, sizeof(clear));
            clear.target_va = q.target_va;
            clear.width = 64u;
            clear.height = 32u;
            clear.format = q.target_format;
            clear.mask = test == 2u ? 0x3du : 0x3cu;
            clear.color = 0xff000000u;
            clear.depth_clear = 0x3f800000u;
            clear.depth_state = q.depth_state;
            clear.quiet = 1u;
            gxm_batch_test_mode = mode == 0u ? 0 : 1;
            rc = gxm_batch_flush_worker(GXM_BATCH_REASON_CLEAR);
            if (rc == 0) rc = frame_clear_internal(&clear);
            for (unsigned draw = 0u; draw < 2u && rc == 0; ++draw) {
                float left = test == 0u ? (draw ? 0.f : -1.f) : -1.f;
                float right = test == 0u ? (draw ? 1.f : 0.f) : 1.f;
                const float v[16] = {left,1,0,0, right,1,1,0,
                                    left,-1,0,1, right,-1,1,1};
                uint32_t color = draw ? 0xff00a000u : 0xff0000c8u;
                if (test == 1u) color = draw ? 0x8000a000u : 0x800000c8u;
                memcpy(vertices, v, sizeof(v));
                gain[0] = test == 0u && draw ? 0.5f : 1.f;
                gain[1] = 1.f;
                gain[2] = gain[3] = 0.f;
                for (unsigned i = 0u; i < 64u; ++i) texels[i] = color;
                if (test == 5u) {
                    if (draw == 0u) {
                        for (unsigned i = 32u; i < 64u; ++i) texels[i] = 0xff00a000u;
                    } else {
                        vertices[0] = vertices[8] = 0.f;
                        for (unsigned i = 0u; i < 64u; ++i) texels[i] = 0xffc80000u;
                    }
                }
                q.draw_index = draw;
                if (test == 3u && draw) {
                    /* Real producer -> consumer boundary must flush A before B
                     * samples it. No manual flush is inserted here. */
                    q.target_va = 0x7fff10000ull;
                    q.texture_va = 0x7fff00000ull;
                    q.texture_source = VP_GXM_EXTRA_SOURCE_GPU;
                    q.texels = NULL;
                    q.tex_width = 64u;
                    q.tex_height = 32u;
                    gain[0] = 0.5f;
                }
                if (test == 4u && draw) {
                    /* Use the same clear-boundary path as worker dispatch. */
                    rc = gxm_batch_flush_worker(GXM_BATCH_REASON_CLEAR);
                    clear.color = 0xff00a000u;
                    if (rc == 0) rc = frame_clear_internal(&clear);
                    if (rc < 0) break;
                    vertices[0] = vertices[8] = 0.f;
                    for (unsigned i = 0u; i < 64u; ++i) texels[i] = 0xffc80000u;
                }
                /* Negative input control: make the first quad degenerate
                 * before encoding; the independent oracle must notice. */
                if (mode == 2u && draw == 0u) memset(vertices, 0, sizeof(vertices));
                rc = frame_draw_internal(&q);
                encoded += q.batch_encoded;
                /* Same addresses immediately reused by the submitter. A batch
                 * must own its vertex/texture copies and reserved uniforms. */
                memset(vertices, 0, sizeof(vertices));
                memset(texels, 0, sizeof(texels));
                memset(gain, 0, sizeof(gain));
            }
            if (rc == 0) rc = gxm_batch_flush_worker(GXM_BATCH_REASON_SUBMIT);
            output = surface_find(q.target_va);
            snprintf(path, sizeof(path),
                     "ux0:data/mhgu-vita-port/batch-selftest-m%u-c%u.rgba",
                     mode, test);
            dump = rc == 0 && output != NULL ? fopen(path, "wb") : NULL;
            if (dump != NULL) {
                if (fwrite(output->color_mem, 4u, 64u * 32u, dump) != 64u * 32u)
                    rc = -1;
                if (fclose(dump) != 0) rc = -1;
            } else rc = -1;
            if (test == 5u && rc == 0) {
                static void *present_pixels;
                if (present_pixels == NULL) present_pixels = gpu_alloc(8192u, 0, NULL);
                if (present_pixels == NULL) rc = -1;
                for (unsigned control=0u; control < (mode==1u ? 2u : 1u) && rc==0; ++control) {
                    /* The positive control uses the canonical Vulkan surface
                     * orientation (unswapped); the negative control flips V to
                     * prove the check is not trivially satisfied. */
                    rc = frame_blit_gpu(output,present_pixels,0,0,64,32,64,32,64,control!=0u);
                    snprintf(path,sizeof(path),
                        "ux0:data/mhgu-vita-port/batch-selftest-present-m%u-n%u.rgba",mode,control);
                    dump = rc==0 ? fopen(path,"wb") : NULL;
                    if (dump != NULL) {
                        if (fwrite(present_pixels,4u,2048u,dump)!=2048u) rc=-1;
                        if (fclose(dump)!=0) rc=-1;
                    } else rc=-1;
                }
            }
            if (rc < 0) api_failures++;
            glog("VP_BATCH_SELFTEST mode=%u case=%u rc=%d encoded=%u "
                 "completed=%u failed=%u pending=%u actual_encoded=%u\n",
                 mode, test, rc, static_stats.batch_encoded - encoded_before,
                 static_stats.batch_completed - completed_before,
                 static_stats.batch_failed - failed_before,
                 gxm_batch.policy.draws, encoded);
        }
    }
    gxm_batch_test_mode = -1;
    glog("VP_BATCH_SELFTEST end api_failures=%u\n", api_failures);
    return api_failures ? -1 : 0;
}

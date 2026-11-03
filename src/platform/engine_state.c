#include "engine_state.h"
#include "engine_nvn_internal.h"

/* Original call sites. */
#define SITE_TARGETS_RT        0xbc1200u
#define SITE_TARGETS_VIEWPORT  0xbc12ccu
#define SITE_TARGETS_SCISSOR   0xbc13b8u
#define SITE_STATE_BLEND_COLOR 0xbc14c8u
#define SITE_STATE_CHANNEL     0xbc1568u
#define SITE_STATE_DEPTH       0xbc15fcu
#define SITE_STATE_STENCIL_VAL 0xbc1618u
#define SITE_STATE_STENCIL_MSK 0xbc1634u
#define SITE_STATE_STENCIL_REF 0xbc1650u
#define SITE_STATE_POLYGON     0xbc16e0u
#define SITE_STATE_OFFSET      0xbc1704u
#define SITE_STATE_DEPTH_CLAMP 0xbc1720u

static const uint32_t blend_sites[8] = {
    0xbc14e0u, 0xbc14f0u, 0xbc1500u, 0xbc1510u,
    0xbc1520u, 0xbc1530u, 0xbc1540u, 0xbc1550u
};

#define ST_CMD          0x004u
#define ST_LAST_TARGET  0x008u
#define ST_LAST_STATE   0x00cu
#define ST_LAST_MATERIAL 0x010u
#define ST_COLOR_CACHE  0x018u
#define ST_POLY_CACHE   0x01cu
#define ST_DEPTH_CACHE  0x020u
#define ST_VIEWPORT     0x074u
#define ST_SCISSOR      0x084u
#define ST_TARGET_CACHE 0x094u
#define ST_AUX_TARGET   0x0a4u
#define ST_TEXTURE_CACHE 0x128u
#define ST_COLOR_WORD   0x1170u

#define TARGET_COUNT    0x014u
#define TARGET_AUX      0x010u
#define TARGET_VP_A     0x018u
#define TARGET_VP_B     0x028u
#define TARGET_SC_A     0x038u
#define TARGET_SC_B     0x048u
#define TARGET_DIM      0x058u

#define CTX_CURRENT_A   0x389c0u
#define CTX_CURRENT_B   0x389ccu
#define CTX_LRU_BASE    0x34070u
#define CTX_LRU_COUNT   32u

static uint32_t deref_current(VPEngineMem *m, uint32_t base)
{
    uint32_t p = vpm_r32(m, base);
    p = vpm_r32(m, p + 0x204u);
    return vpm_r32(m, p);
}

static void lru_insert(VPEngineMem *m, uint32_t ctx, uint32_t value)
{
    unsigned i;
    if (!value) return;
    for (i = 0; i < CTX_LRU_COUNT; i++) {
        uint32_t address = ctx + CTX_LRU_BASE + i * 4u;
        if (vpm_r32(m, address) == 0u) {
            vpm_w32(m, address, value);
            return;
        }
    }
}

static void lru_remove(VPEngineMem *m, uint32_t ctx, uint32_t value)
{
    unsigned i;
    if (!value) return;
    for (i = 0; i < CTX_LRU_COUNT; i++) {
        uint32_t address = ctx + CTX_LRU_BASE + i * 4u;
        if (vpm_r32(m, address) == value) {
            vpm_w32(m, address, 0u);
            return;
        }
    }
}

static void copy_default_viewport(VPEngineMem *m, uint32_t st)
{
    uint32_t src = m->env->default_viewport;
    uint32_t w0 = vpm_r32(m, src);
    uint32_t w1 = vpm_r32(m, src + 4u);
    uint32_t w2 = vpm_r32(m, src + 8u);
    uint32_t w3 = vpm_r32(m, src + 12u);
    vpm_w32(m, st + ST_VIEWPORT + 0u, w0);
    vpm_w32(m, st + ST_VIEWPORT + 4u, w1);
    vpm_w32(m, st + ST_VIEWPORT + 8u, w2);
    vpm_w32(m, st + ST_VIEWPORT + 12u, w3);
    vpm_w32(m, st + ST_SCISSOR + 0u, w0);
    vpm_w32(m, st + ST_SCISSOR + 4u, w1);
    vpm_w32(m, st + ST_SCISSOR + 8u, w2);
    vpm_w32(m, st + ST_SCISSOR + 12u, w3);
}

static void emit_simple(VPEngineMem *m, uint32_t site, uint32_t slot,
                        uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3)
{
    VPEngineNvnCall call;
    vpm_call_init(&call, site, slot);
    call.r[0] = r0;
    call.r[1] = r1;
    call.r[2] = r2;
    call.r[3] = r3;
    vpm_send(m, &call, NULL);
}

int vp_engine_state_targets(const VPEngineNvnEnv *env, uint32_t ctx,
                            uint32_t st, uint32_t target)
{
    VPEngineMem m;
    uint32_t cmd, flags, changed = 0, aux, i;
    uint32_t rects_a[8], rects_b[8], n = 0;
    uint32_t vp_x, vp_y, vp_w, vp_h;

    if (!env || !env->resolve || !ctx || !st || !target ||
        !env->default_viewport)
        return VP_ENGINE_NATIVE_ERR_ARGUMENT;
    m.env = env;
    m.error = 0;
    if (vpm_r32(&m, st + ST_LAST_TARGET) == target) return VP_ENGINE_NATIVE_OK;
    cmd = vpm_r32(&m, st + ST_CMD);
    flags = vpm_r32(&m, target + TARGET_COUNT);

    for (i = 0; i < (flags & 7u); i++) {
        uint32_t cache = st + ST_TARGET_CACHE + i * 4u;
        uint32_t new_target = vpm_r32(&m, target + i * 4u);
        uint32_t old_target = vpm_r32(&m, cache);
        if (old_target == new_target) continue;
        {
            uint32_t obj = old_target;
            uint32_t current = deref_current(&m, ctx + CTX_CURRENT_A);
            if (current == old_target)
                obj = deref_current(&m, ctx + CTX_CURRENT_B);
            if (obj) lru_insert(&m, ctx, vpm_r32(&m, obj + 0x10u));
        }
        if (new_target) {
            uint32_t new_obj = vpm_r32(&m, new_target + 0x10u);
            if (new_obj) lru_remove(&m, ctx, new_obj);
        }
        vpm_w32(&m, cache, new_target);
        changed = 1;
        copy_default_viewport(&m, st);
    }

    aux = vpm_r32(&m, target + TARGET_AUX);
    if (vpm_r32(&m, st + ST_AUX_TARGET) != aux) {
        uint32_t old_aux = vpm_r32(&m, st + ST_AUX_TARGET);
        if (old_aux) lru_insert(&m, ctx, vpm_r32(&m, old_aux + 0x10u));
        if (aux) lru_remove(&m, ctx, vpm_r32(&m, aux + 0x10u));
        vpm_w32(&m, st + ST_AUX_TARGET, aux);
        copy_default_viewport(&m, st);
    } else if (!changed) {
        goto viewport;
    }

    vpm_memclr(&m, 0xbc1134u, st + ST_TEXTURE_CACHE, 0x80u);
    vpm_w32(&m, st + ST_LAST_MATERIAL, 0u);

    for (i = 0; i < (flags & 7u); i++) {
        uint32_t cached, current, selected;
        if (!vpm_r32(&m, target + i * 4u)) continue;
        cached = vpm_r32(&m, st + ST_TARGET_CACHE + i * 4u);
        current = deref_current(&m, ctx + CTX_CURRENT_A);
        /* 0xBC117C keeps cached in r4; only equality replaces it with the
         * alternate current target. Do not replace every offscreen target. */
        selected = cached;
        if (current == cached) selected = deref_current(&m, ctx + CTX_CURRENT_B);
        if (!selected) continue;
        rects_a[n] = vpm_r32(&m, selected + 0x14u);
        rects_b[n] = vpm_r32(&m, vpm_r32(&m, selected + 0x10u) + 0x3cu);
        n++;
    }
    {
        uint32_t stack0 = 0, stack1 = 0;
        VPEngineNvnCall call;
        if (aux) {
            stack0 = vpm_r32(&m, vpm_r32(&m, aux + 0x10u) + 0x3cu);
            stack1 = vpm_r32(&m, aux + 0x14u);
        }
        if (env->scratch_size < 0x50u) {
            return VP_ENGINE_NATIVE_ERR_ARGUMENT;
        }
        for (i = 0; i < n; i++) {
            vpm_w32(&m, env->scratch + i * 4u, rects_a[i]);
            vpm_w32(&m, env->scratch + 0x20u + i * 4u, rects_b[i]);
        }
        vpm_call_init(&call, SITE_TARGETS_RT, VP_SLOT_SET_RENDER_TARGETS);
        call.r[0] = cmd;
        call.r[1] = n;
        call.r[2] = env->scratch + 0x20u;
        call.r[3] = env->scratch;
        call.stack[0] = stack0;
        call.stack[1] = stack1;
        call.stack_words = 2;
        vpm_send(&m, &call, NULL);
    }

viewport:
    {
        uint32_t select = flags & 0x200u;
        uint32_t base = target + (select ? TARGET_VP_B : TARGET_VP_A);
        uint32_t r1 = vpm_r32(&m, base + 0u);
        uint32_t r6 = vpm_r32(&m, base + 4u);
        uint32_t r5 = vpm_r32(&m, base + 8u);
        uint32_t r0 = vpm_r32(&m, base + 12u);
        uint32_t r7 = vpm_r32(&m, target + TARGET_DIM);
        uint32_t cached = vpm_r32(&m, st + ST_VIEWPORT + 8u);
        uint32_t lr, ip, r2, r3;
        if ((int32_t)r0 >= (int32_t)(r7 >> 16)) r0 = r7 >> 16;
        if ((int32_t)r6 <= 0) r6 = 0;
        lr = r0;
        r2 = r6;
        if ((int32_t)r0 < (int32_t)r6) { lr = 0; r2 = 0; }
        r3 = ((int32_t)r1 > 0) ? r1 : 0;
        r7 &= 0xffffu;
        if ((int32_t)r5 < (int32_t)r7) r7 = r5;
        r1 = r3;
        ip = r7;
        if ((int32_t)r7 < (int32_t)r3) { ip = 0; r1 = 0; }
        if (ip == cached &&
            r2 == vpm_r32(&m, st + ST_VIEWPORT + 4u) &&
            r1 == vpm_r32(&m, st + ST_VIEWPORT + 0u) &&
            lr == vpm_r32(&m, st + ST_VIEWPORT + 12u)) {
            vp_x = r1; vp_y = r2; vp_w = ip; vp_h = lr;
            goto scissor;
        }
        r0 = (int32_t)(r0 - r6) < 0 ? 0 : r0 - r6;
        vpm_w32(&m, st + ST_VIEWPORT + 0u, r1);
        vpm_w32(&m, st + ST_VIEWPORT + 4u, r2);
        vpm_w32(&m, st + ST_VIEWPORT + 8u, ip);
        vpm_w32(&m, st + ST_VIEWPORT + 12u, lr);
        r3 = (int32_t)(r7 - r3) < 0 ? 0 : r7 - r3;
        {
            VPEngineNvnCall call;
            vpm_call_init(&call, SITE_TARGETS_VIEWPORT, VP_SLOT_SET_VIEWPORT);
            call.r[0] = cmd;
            call.r[1] = r1;
            call.r[2] = r2;
            call.r[3] = r3;
            call.stack[0] = r0;
            call.stack_words = 1;
            vpm_send(&m, &call, NULL);
        }
        vp_x = r1; vp_y = r2; vp_w = ip; vp_h = lr;
    }

scissor:
    {
        uint32_t select = flags & 0x200u;
        uint32_t base = target + (select ? TARGET_SC_B : TARGET_SC_A);
        uint32_t r4 = vpm_r32(&m, base + 0u);
        uint32_t r5 = vpm_r32(&m, base + 4u);
        uint32_t r3 = vpm_r32(&m, base + 8u);
        uint32_t r0 = vpm_r32(&m, base + 12u);
        uint32_t r1 = vp_x, r2 = vp_y, lr = vp_h, ip = vp_w;
        if ((int32_t)r0 < (int32_t)lr) lr = r0;
        if ((int32_t)r5 > (int32_t)r2) r2 = r5;
        if ((int32_t)r2 > (int32_t)lr) { r2 = 0; lr = 0; }
        if ((int32_t)r3 >= (int32_t)ip) r3 = ip;
        if ((int32_t)r4 > (int32_t)r1) r1 = r4;
        if ((int32_t)r1 > (int32_t)r3) { r1 = 0; r3 = 0; }
        /* 0xBC1330..0xBC1364: the default rectangle is a sentinel. When
         * intersection equals it, use the whole clipped viewport. In real
         * queues the sentinel can be all zero; emitting a zero-area scissor
         * instead suppresses every following draw on that target. */
        if (lr == vpm_r32(&m, env->default_viewport + 12u) &&
            r1 == vpm_r32(&m, env->default_viewport) &&
            r3 == vpm_r32(&m, env->default_viewport + 8u) &&
            r2 == vpm_r32(&m, env->default_viewport + 4u)) {
            r1 = vp_x;
            r2 = vp_y;
            r3 = vp_w;
            lr = vp_h;
        }
        if (r3 == vpm_r32(&m, st + ST_SCISSOR + 8u) &&
            r2 == vpm_r32(&m, st + ST_SCISSOR + 4u) &&
            r1 == vpm_r32(&m, st + ST_SCISSOR + 0u) &&
            lr == vpm_r32(&m, st + ST_SCISSOR + 12u)) {
            goto done;
        }
        vpm_w32(&m, st + ST_SCISSOR + 0u, r1);
        vpm_w32(&m, st + ST_SCISSOR + 4u, r2);
        vpm_w32(&m, st + ST_SCISSOR + 8u, r3);
        vpm_w32(&m, st + ST_SCISSOR + 12u, lr);
        {
            VPEngineNvnCall call;
            vpm_call_init(&call, SITE_TARGETS_SCISSOR, VP_SLOT_SET_SCISSOR);
            call.r[0] = cmd;
            call.r[1] = r1;
            call.r[2] = r2;
            call.r[3] = r3 - r1;
            call.stack[0] = lr - r2;
            call.stack_words = 1;
            vpm_send(&m, &call, NULL);
        }
    }

done:
    if (!m.error) vpm_w32(&m, st + ST_LAST_TARGET, target);
    return m.error ? m.error : VP_ENGINE_NATIVE_OK;
}

static void emit_blend_color(VPEngineMem *m, uint32_t cmd, uint32_t color_word)
{
    VPEngineNvnCall call;
    uint32_t i;
    float values[4];
    const float scale = 0.003921568859368563f;
    values[0] = (float)(color_word & 0xffu) * scale;
    values[1] = (float)((color_word >> 8) & 0xffu) * scale;
    values[2] = (float)((color_word >> 16) & 0xffu) * scale;
    values[3] = (float)((color_word >> 24) & 0xffu) * scale;
    for (i = 0; i < 4; i++) {
        uint32_t bits;
        memcpy(&bits, &values[i], sizeof(bits));
        vpm_w32(m, m->env->scratch + 0x40u + i * 4u, bits);
    }
    vpm_call_init(&call, SITE_STATE_BLEND_COLOR, VP_SLOT_SET_BLEND_COLOR);
    call.r[0] = cmd;
    call.r[1] = m->env->scratch + 0x40u;
    vpm_send(m, &call, NULL);
}

int vp_engine_state_pipeline(const VPEngineNvnEnv *env, uint32_t ctx,
                             uint32_t st, uint32_t pipeline)
{
    VPEngineMem m;
    uint32_t cmd, old_state, color, cached_color, flags_word;
    uint32_t new_depth, cached_depth, new_poly, cached_poly, variant;
    int skip;
    unsigned i;

    (void)ctx;
    if (!env || !env->resolve || !ctx || !st || !pipeline)
        return VP_ENGINE_NATIVE_ERR_ARGUMENT;
    m.env = env;
    m.error = 0;
    if (vpm_r32(&m, st + ST_LAST_STATE) == pipeline)
        return VP_ENGINE_NATIVE_OK;
    cmd = vpm_r32(&m, st + ST_CMD);
    old_state = vpm_r32(&m, st + ST_LAST_STATE);
    color = vpm_r32(&m, pipeline + 0x10u);
    cached_color = vpm_r32(&m, st + ST_COLOR_CACHE);
    flags_word = vpm_r32(&m, pipeline + 4u);

    skip = 0;
    if (cached_color == color) {
        if (!old_state || vpm_r32(&m, old_state) == vpm_r32(&m, pipeline))
            skip = 1;
    } else if (cached_color) {
        if (old_state &&
            vpm_r32(&m, cached_color + 0xcu) ==
                vpm_r32(&m, color + 0xcu) &&
            vpm_r32(&m, old_state) == vpm_r32(&m, pipeline)) {
            vpm_w32(&m, st + ST_COLOR_WORD, vpm_r32(&m, pipeline));
            vpm_w32(&m, st + ST_COLOR_CACHE, color);
            skip = 1;
        }
    }
    if (!skip) {
        VPEngineNvnCall call;
        if (env->scratch_size < 0x50u) return VP_ENGINE_NATIVE_ERR_ARGUMENT;
        emit_blend_color(&m, cmd, vpm_r32(&m, pipeline));
        for (i = 0; i < 8u; i++) {
            vpm_call_init(&call, blend_sites[i], VP_SLOT_BIND_BLEND_STATE);
            call.r[0] = cmd;
            call.r[1] = color + 0x48u + i * 8u;
            vpm_send(&m, &call, NULL);
        }
        emit_simple(&m, SITE_STATE_CHANNEL, VP_SLOT_BIND_CHANNEL_MASK_STATE,
                    cmd, color + 0x40u, 0, 0);
        vpm_w32(&m, st + ST_COLOR_WORD, vpm_r32(&m, pipeline));
        vpm_w32(&m, st + ST_COLOR_CACHE, color);
    }

    new_depth = vpm_r32(&m, pipeline + 0x14u);
    cached_depth = vpm_r32(&m, st + ST_DEPTH_CACHE);
    skip = 0;
    if (cached_depth == new_depth) {
        if (!old_state ||
            ((vpm_r32(&m, old_state + 4u) ^ flags_word) & 0xffu) == 0u)
            skip = 1;
    } else if (cached_depth && old_state &&
               vpm_r32(&m, cached_depth + 0xcu) ==
                   vpm_r32(&m, new_depth + 0xcu) &&
               ((vpm_r32(&m, old_state + 4u) ^ flags_word) & 0xffu) == 0u) {
        skip = 1;
    }
    if (!skip) {
        emit_simple(&m, SITE_STATE_DEPTH, VP_SLOT_BIND_DEPTH_STENCIL_STATE,
                    cmd, new_depth + 0x18u, 0, 0);
        emit_simple(&m, SITE_STATE_STENCIL_VAL,
                    VP_SLOT_SET_STENCIL_VALUE_MASK, cmd, 3u,
                    vpm_r32(&m, new_depth + 0x20u), 0);
        emit_simple(&m, SITE_STATE_STENCIL_MSK,
                    VP_SLOT_SET_STENCIL_MASK, cmd, 3u,
                    vpm_r32(&m, new_depth + 0x24u), 0);
        emit_simple(&m, SITE_STATE_STENCIL_REF, VP_SLOT_SET_STENCIL_REF,
                    cmd, 3u, vpm_r8(&m, pipeline + 4u), 0);
        new_depth = vpm_r32(&m, pipeline + 0x14u);
    }
    vpm_w32(&m, st + ST_DEPTH_CACHE, new_depth);

    new_poly = vpm_r32(&m, pipeline + 0x18u);
    cached_poly = vpm_r32(&m, st + ST_POLY_CACHE);
    variant = (flags_word >> 8) & 1u;
    skip = 0;
    if (cached_poly == new_poly) {
        if (!old_state ||
            ((vpm_r32(&m, old_state + 4u) ^ flags_word) & 0x100u) == 0u)
            skip = 1;
    } else if (cached_poly && old_state &&
               vpm_r32(&m, cached_poly + 0xcu) ==
                   vpm_r32(&m, new_poly + 0xcu) &&
               ((vpm_r32(&m, old_state + 4u) ^ flags_word) & 0x100u) == 0u) {
        skip = 1;
    }
    if (!skip) {
        uint32_t poly = new_poly + variant * 20u;
        VPEngineNvnCall call;
        uint32_t bits;
        float converted;
        emit_simple(&m, SITE_STATE_POLYGON, VP_SLOT_BIND_POLYGON_STATE,
                    cmd, poly + 0x20u, 0, 0);
        vpm_call_init(&call, SITE_STATE_OFFSET, VP_SLOT_SET_POLYGON_OFFSET_CLAMP);
        call.r[0] = cmd;
        call.s[0] = vpm_r32(&m, poly + 0x30u);
        converted = (float)(int32_t)vpm_r32(&m, poly + 0x28u);
        memcpy(&bits, &converted, sizeof(bits));
        call.s[1] = bits;
        call.s[2] = vpm_r32(&m, poly + 0x2cu);
        call.s_words = 3;
        vpm_send(&m, &call, NULL);
        emit_simple(&m, SITE_STATE_DEPTH_CLAMP, VP_SLOT_SET_DEPTH_CLAMP,
                    cmd, vpm_r8(&m, poly + 0x26u) ^ 1u, 0, 0);
        new_poly = vpm_r32(&m, pipeline + 0x18u);
    }
    vpm_w32(&m, st + ST_POLY_CACHE, new_poly);
    if (!m.error) vpm_w32(&m, st + ST_LAST_STATE, pipeline);
    return m.error ? m.error : VP_ENGINE_NATIVE_OK;
}

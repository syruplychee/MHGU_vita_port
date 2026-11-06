#include "engine_material.h"
#include "engine_memory.h"

#include <string.h>

/* Reconstruction of main+0xBC1764.  Field offsets and branch semantics are
 * documented in docs/decisions/0019-engine-full-prepare.md. The original
 * disassembly remains in ignored build/engine-full/ for local review. */

/* Original call-site offsets in the locked main module.  They are passed to the
 * typed sinks so a trace can be compared instruction-for-instruction. */
#define SITE_BIND_PROGRAM 0xbc17d4u
#define SITE_BIND_ATTRIB 0xbc17f0u
#define SITE_BIND_STREAM 0xbc1810u
#define SITE_BUFADDR_GROUP0_A 0xbc18f0u
#define SITE_BIND_UNIFORM0_A 0xbc1934u
#define SITE_BUFADDR_GROUP0_B 0xbc19d4u
#define SITE_BIND_UNIFORM0_B 0xbc1a10u
#define SITE_BARRIER_A_NF_CHANGED 0xbc1b80u
#define SITE_BARRIER_A_NF_UNCHANGED 0xbc1c14u
#define SITE_BARRIER_A_FOUND_FIRST 0xbc1c60u
#define SITE_BARRIER_A_FOUND_SECOND 0xbc1cacu
#define SITE_TEXHANDLE_A 0xbc1cf4u
#define SITE_BIND_TEXTURE_A 0xbc1d10u
#define SITE_BUFADDR_GROUP1_A 0xbc1dd4u
#define SITE_BIND_UNIFORM1_A 0xbc1e14u
#define SITE_BUFADDR_GROUP1_B 0xbc1eacu
#define SITE_BIND_UNIFORM1_B 0xbc1ee4u
#define SITE_BARRIER_B_NF_CHANGED 0xbc205cu
#define SITE_BARRIER_B_NF_UNCHANGED 0xbc20f0u
#define SITE_BARRIER_B_FOUND_FIRST 0xbc213cu
#define SITE_BARRIER_B_FOUND_SECOND 0xbc218cu
#define SITE_TEXHANDLE_B 0xbc21d4u
#define SITE_BIND_TEXTURE_B 0xbc21f0u
#define SITE_SET_ALPHA_REF 0xbc22f0u
#define SITE_BIND_COLOR_STATE 0xbc232cu
#define SITE_BIND_MSAA 0xbc23b4u

static void *guest_ptr(const VPEngineMaterialEnv *env, uint32_t address)
{
    if (!address || !env->resolve) return NULL;
    return env->resolve(env->opaque, address);
}

static int read_u32(const VPEngineMaterialEnv *env, uint32_t address,
                    uint32_t *out)
{
    const void *p = guest_ptr(env, address);
    if (!p) return VP_ENGINE_MATERIAL_ERR_MEMORY;
    *out=vp_engine_read32(p);
    return VP_ENGINE_MATERIAL_OK;
}

static int read_material_resource(const VPEngineMaterialEnv *env,
                                  uint32_t material, uint32_t index,
                                  uint32_t *out)
{
    return read_u32(env,
        material + VP_ENGINE_MAT_RESOURCE_OFFSET + index * 4u, out);
}

/* CPU pointer -> GPU address.  The locked code calls nvnBufferGetAddress on
 * wrapper->+0x5B8, then adds (aligned_pointer - wrapper->+0x58) as a signed
 * 32-bit delta, sign-extended into the 64-bit addition.  Both the 64-bit base
 * and the negative/positive delta must survive unchanged. */
static int translate_gpu_address(const VPEngineMaterialEnv *env, uint32_t site,
                                 uint32_t resource, uint64_t *out)
{
    uint32_t aligned = resource & ~0xfu;
    uint32_t wrapper_address, buffer, base;
    const uint8_t *root, *wrapper;

    if (!env->buffer_get_address) return VP_ENGINE_MATERIAL_ERR_CALLBACK;
    root = (const uint8_t *)guest_ptr(env, env->gpu_root);
    if (!root) return VP_ENGINE_MATERIAL_ERR_MEMORY;
    memcpy(&wrapper_address, root + VP_ENGINE_GPU_ROOT_WRAPPER,
           sizeof(wrapper_address));
    wrapper = (const uint8_t *)guest_ptr(env, wrapper_address);
    if (!wrapper) return VP_ENGINE_MATERIAL_ERR_MEMORY;
    memcpy(&buffer, wrapper + VP_ENGINE_WRAPPER_BUFFER, sizeof(buffer));
    if (!buffer) {
        *out = 0;
        return VP_ENGINE_MATERIAL_OK;
    }
    memcpy(&base, wrapper + VP_ENGINE_WRAPPER_BASE, sizeof(base));
    {
        uint32_t difference = aligned - base;
        int32_t signed_difference = (int32_t)difference;
        uint64_t buffer_address =
            env->buffer_get_address(env->opaque, site, buffer);
        *out = buffer_address + (uint64_t)(int64_t)signed_difference;
    }
    return VP_ENGINE_MATERIAL_OK;
}

static int emit_uniform_group(const VPEngineMaterialEnv *env, uint32_t cmd,
                              uint32_t material, uint32_t descriptors,
                              unsigned count, unsigned stage,
                              uint32_t old_resources,
                              uint32_t site_buffer_a, uint32_t site_bind_a,
                              uint32_t site_buffer_b, uint32_t site_bind_b)
{
    unsigned i;
    for (i = 0; i < count; i++) {
        uint32_t record = descriptors + i * 8u;
        uint32_t w0, w1, current, size, slot, index;
        uint64_t address;
        int rc;

        rc = read_u32(env, record, &w0);
        if (rc) return rc;
        rc = read_u32(env, record + 4u, &w1);
        if (rc) return rc;
        index = (w0 >> 6) & 0x3ffu;
        rc = read_material_resource(env, material, index, &current);
        if (rc) return rc;
        if (old_resources) {
            uint32_t previous;
            rc = read_u32(env,
                old_resources + index * 4u, &previous);
            if (rc) return rc;
            if (previous == current) continue;
        }
        if ((w1 & 1u) == 0u) {
            size = w0 >> 16;
        } else if ((w1 & 0xfffeu) != 0u) {
            continue;
        } else {
            size = w1 >> 16;
        }
        slot = w0 & 0x3fu;
        rc = translate_gpu_address(env,
            old_resources ? site_buffer_a : site_buffer_b, current, &address);
        if (rc) return rc;
        if (!env->bind_uniform_buffer) return VP_ENGINE_MATERIAL_ERR_CALLBACK;
        env->bind_uniform_buffer(env->opaque,
            old_resources ? site_bind_a : site_bind_b, cmd, stage, slot,
            address, size);
    }
    return VP_ENGINE_MATERIAL_OK;
}

static int pending_sync(const VPEngineMaterialEnv *env, uint32_t site,
                        uint32_t cmd, uint8_t *ctx)
{
    uint32_t pending, update;
    memcpy(&pending, ctx + VP_ENGINE_CTX_PENDING_SYNC, sizeof(pending));
    memcpy(&update, ctx + VP_ENGINE_CTX_PENDING_UPDATE, sizeof(update));
    if (pending == update) return VP_ENGINE_MATERIAL_OK;
    if (!env->barrier) return VP_ENGINE_MATERIAL_ERR_CALLBACK;
    env->barrier(env->opaque, site, cmd, VP_ENGINE_BARRIER_TEXTURE_BITS);
    memcpy(ctx + VP_ENGINE_CTX_PENDING_SYNC, &update, sizeof(update));
    return VP_ENGINE_MATERIAL_OK;
}

/* Shared by both texture groups: if the texture type can be in the pending
 * list, remove it (when present) and flush the pending counter.  When absent,
 * the original only flushes if texture->+0x23 has any bit in 1..7.
 * sites = {not-found changed sampler, not-found unchanged sampler,
 *          found first entry, found second entry}. */
static int texture_pending_list(const VPEngineMaterialEnv *env, uint32_t ctx,
                                uint32_t cmd, uint32_t texture,
                                const uint32_t sites[4], int changed)
{
    uint8_t *host = (uint8_t *)guest_ptr(env, ctx);
    unsigned i;
    int found = 0;
    if (!host) return VP_ENGINE_MATERIAL_ERR_MEMORY;
    for (i = 0; i < VP_ENGINE_CTX_LRU_ENTRIES; i++) {
        uint32_t entry;
        memcpy(&entry, host + VP_ENGINE_CTX_LRU_BASE + i * 4u,
               sizeof(entry));
        if (entry == texture) {
            found = 1;
            break;
        }
    }
    if (found) {
        int rc = pending_sync(env, (i & 1u) ? sites[3] : sites[2], cmd, host);
        if (rc) return rc;
        {
            uint32_t zero = 0;
            memcpy(host + VP_ENGINE_CTX_LRU_BASE + i * 4u, &zero,
                   sizeof(zero));
        }
        return VP_ENGINE_MATERIAL_OK;
    }
    {
        const uint8_t *tex = (const uint8_t *)guest_ptr(env, texture);
        uint8_t flag;
        if (!tex) return VP_ENGINE_MATERIAL_ERR_MEMORY;
        memcpy(&flag, tex + VP_ENGINE_TEXTURE_FLAGS, sizeof(flag));
        if ((flag & 0xfeu) != 0u)
            return pending_sync(env, changed ? sites[0] : sites[1], cmd, host);
    }
    return VP_ENGINE_MATERIAL_OK;
}

static int emit_texture_group(const VPEngineMaterialEnv *env,
                              VPEngineSubmitState *state, uint32_t ctx,
                              uint32_t cmd, uint32_t material,
                              uint32_t descriptors, unsigned count, unsigned stage,
                              unsigned sampler_base, unsigned texture_base,
                              const uint32_t barrier_sites[4],
                              uint32_t site_texhandle, uint32_t site_bindtex)
{
    unsigned i;
    for (i = 0; i < count; i++) {
        uint32_t w0, texture, sampler, slot;
        uint32_t texture_type = 0;
        int texture_changed, sampler_changed, need_bind, rc;
        uint32_t *sampler_cache;
        uint32_t *texture_cache;

        if (i >= 16u) return VP_ENGINE_MATERIAL_ERR_OBJECT;
        rc = read_u32(env, descriptors + i * 8u, &w0);
        if (rc) return rc;
        rc = read_material_resource(env, material, w0 >> 16, &texture);
        if (rc) return rc;
        rc = read_material_resource(env, material, (w0 >> 6) & 0x3ffu,
                                    &sampler);
        if (rc) return rc;
        sampler_cache = (uint32_t *)((uint8_t *)state + sampler_base);
        texture_cache = (uint32_t *)((uint8_t *)state + texture_base);
        texture_changed = texture_cache[i] != texture;
        if (texture_changed) texture_cache[i] = texture;
        sampler_changed = sampler_cache[i] != sampler;

        if (!texture) return VP_ENGINE_MATERIAL_ERR_OBJECT;
        {
            const uint8_t *tex = (const uint8_t *)guest_ptr(env, texture);
            if (!tex) return VP_ENGINE_MATERIAL_ERR_MEMORY;
            memcpy(&texture_type, tex + VP_ENGINE_TEXTURE_TYPE,
                   sizeof(texture_type));
        }

        if (sampler_changed) {
            sampler_cache[i] = sampler;
            if (texture_type == 2u || texture_type == 5u) {
                rc = texture_pending_list(env, ctx, cmd, texture,
                                          barrier_sites, 1);
                if (rc) return rc;
            }
            need_bind = 1;
        } else {
            if (texture_type == 2u || texture_type == 5u) {
                rc = texture_pending_list(env, ctx, cmd, texture,
                                          barrier_sites, 0);
                if (rc) return rc;
            }
            need_bind = texture_changed;
        }

        if (need_bind) {
            const uint8_t *tex, *smp;
            uint32_t device, texture_id, sampler_id;
            uint64_t handle;
            slot = w0 & 0x3fu;
            if (!sampler) return VP_ENGINE_MATERIAL_ERR_OBJECT;
            tex = (const uint8_t *)guest_ptr(env, texture);
            smp = (const uint8_t *)guest_ptr(env, sampler);
            if (!tex || !smp) return VP_ENGINE_MATERIAL_ERR_MEMORY;
            if (!env->device_get_texture_handle || !env->bind_texture)
                return VP_ENGINE_MATERIAL_ERR_CALLBACK;
            rc = read_u32(env, ctx + VP_ENGINE_CTX_NV_DEVICE, &device);
            if (rc) return rc;
            memcpy(&texture_id, tex + VP_ENGINE_TEXTURE_ID,
                   sizeof(texture_id));
            memcpy(&sampler_id, smp + VP_ENGINE_SAMPLER_ID,
                   sizeof(sampler_id));
            handle = env->device_get_texture_handle(
                env->opaque, site_texhandle,
                device + VP_ENGINE_DEVICE_HANDLE_OFFSET, texture_id,
                sampler_id);
            env->bind_texture(env->opaque, site_bindtex, cmd, stage, slot,
                              handle);
        }
    }
    return VP_ENGINE_MATERIAL_OK;
}

static int submit_tail(const VPEngineMaterialEnv *env,
                       VPEngineSubmitState *state, uint32_t ctx,
                       uint32_t material, uint32_t cmd)
{
    uint32_t material0, material1, st0, color_bits, color_index;
    int rc;

    rc = read_u32(env, material, &material0);
    if (rc) return rc;
    rc = read_u32(env, material + 4u, &material1);
    if (rc) return rc;

    st0 = state->flags;
    color_bits = material0 & 0x180000u;
    {
        int old_bit = (int)((st0 >> 25) & 1u);
        int new_bit = color_bits == 0x180000u;
        if (old_bit != new_bit) {
            st0 = (st0 & ~(1u << 25)) | ((uint32_t)new_bit << 25);
            state->flags = st0;
        }
    }

    if (color_bits == 0x180000u) {
        uint32_t packed = (material0 >> 3) | (material1 << 29);
        uint32_t index = (st0 >> 9) & 0xffffu;
        uint32_t r1_value = st0;
        uint32_t color_source;
        if (index != (packed & 0xffu)) {
            st0 = (st0 & 0xfe0001ffu) | ((packed << 9) & 0x1fe00u);
            state->flags = st0;
            r1_value = st0;
        }
        color_index = (r1_value >> 9) & 0xffffu;
        color_source = (material0 >> 11) | (material1 << 21);
        if (((color_source ^ (st0 >> 1)) & 0xffu) != 0u) {
            st0 = (st0 & ~0x1feu) | ((color_source & 0xffu) << 1);
            state->flags = st0;
            if (!env->set_alpha_ref)
                return VP_ENGINE_MATERIAL_ERR_CALLBACK;
            /* 0xBC22D8: the reference byte comes from material word 0,
             * bits 11..18, not from the updated state flags. */
            env->set_alpha_ref(env->opaque, SITE_SET_ALPHA_REF, cmd,
                (float)((material0 >> 11) & 0xffu) / VP_ENGINE_ALPHA_SCALE);
        }
    } else {
        st0 = (st0 & 0xfe0001ffu) | 0xe00u;
        state->flags = st0;
        color_index = 7u;
    }

    if (!env->bind_color_state) return VP_ENGINE_MATERIAL_ERR_CALLBACK;
    env->bind_color_state(env->opaque, SITE_BIND_COLOR_STATE, cmd,
        state->color_state + 0x20u + color_index * 4u);

    {
        uint32_t samples = 0;
        if (state->color_target) {
            uint32_t value;
            rc = read_u32(env,
                state->color_target + VP_ENGINE_RENDER_TARGET_SAMPLES,
                &value);
            if (rc) return rc;
            samples = (value >> 8) & 0xfu;
        }
        if (state->depth_target) {
            uint32_t value;
            rc = read_u32(env,
                state->depth_target + VP_ENGINE_RENDER_TARGET_SAMPLES,
                &value);
            if (rc) return rc;
            samples = (value >> 8) & 0xfu;
        }
        {
            uint32_t variant = (material0 >> 2) & 1u;
            uint32_t cache = state->multisample_cache;
            if (((cache >> 1) & 3u) != variant ||
                ((cache >> 3) & 7u) != samples) {
                uint32_t msaa = ctx + VP_ENGINE_CTX_MULTISAMPLE_BASE +
                                variant * 96u + samples * 24u;
                if (!env->bind_multisample_state)
                    return VP_ENGINE_MATERIAL_ERR_CALLBACK;
                env->bind_multisample_state(env->opaque, SITE_BIND_MSAA, cmd,
                                           msaa);
                cache = (cache & ~0x3eu) |
                        ((variant & 1u) << 1) | ((samples & 7u) << 3);
                state->multisample_cache = cache;
            }
        }
    }
    return VP_ENGINE_MATERIAL_OK;
}

int vp_engine_material_submit(const VPEngineMaterialEnv *env, uint32_t ctx,
                              uint32_t state_address, uint32_t material)
{
    VPEngineSubmitState *state;
    const VPEngineLayout *layout;
    uint32_t layout_address, material_flags, old_material;
    uint32_t old_resources = 0, stage1_old_resources;
    uint32_t descriptors, program_meta, program_flags, layout_desc, cmd;
    uint32_t groups0, groups1, n0, n1, n2, n3;
    int same_layout = 0;
    int rc;

    if (!env || !state_address || !material || !ctx)
        return VP_ENGINE_MATERIAL_ERR_ARGUMENT;
    if (!env->resolve) return VP_ENGINE_MATERIAL_ERR_CALLBACK;
    state = (VPEngineSubmitState *)guest_ptr(env, state_address);
    if (!state) return VP_ENGINE_MATERIAL_ERR_MEMORY;

    /* Same-material early exit still executes the state tail. */
    if (state->last_material == material)
        return submit_tail(env, state, ctx, material,
                           state->command_buffer);

    rc = read_u32(env, material, &material_flags);
    if (rc) return rc;
    rc = read_u32(env, material + VP_ENGINE_MAT_LAYOUT_OFFSET,
                  &layout_address);
    if (rc) return rc;
    if (!layout_address) return VP_ENGINE_MATERIAL_ERR_OBJECT;
    layout = (const VPEngineLayout *)guest_ptr(env, layout_address);
    if (!layout) return VP_ENGINE_MATERIAL_ERR_MEMORY;
    cmd = state->command_buffer;

    old_material = state->last_material;
    if (old_material) {
        uint32_t previous_layout;
        rc = read_u32(env,
            old_material + VP_ENGINE_MAT_LAYOUT_OFFSET, &previous_layout);
        if (rc) return rc;
        if (previous_layout == layout_address) {
            same_layout = 1;
            old_resources = old_material + VP_ENGINE_MAT_RESOURCE_OFFSET;
        }
    }

    if (!same_layout) {
        uint32_t program;
        if (!old_material) state->last_material = material;
        if (!env->bind_program || !env->bind_vertex_attrib_state ||
            !env->bind_vertex_stream_state)
            return VP_ENGINE_MATERIAL_ERR_CALLBACK;
        program_meta = layout->program_meta;
        program = program_meta + VP_ENGINE_PROGRAM_OFFSET;
        rc = read_u32(env, program_meta + VP_ENGINE_PROGRAM_META_FLAGS_OFFSET,
                      &program_flags);
        if (rc) return rc;
        env->bind_program(env->opaque, SITE_BIND_PROGRAM, cmd, program,
                          program_flags);
        layout_desc = layout->layout_desc;
        env->bind_vertex_attrib_state(env->opaque, SITE_BIND_ATTRIB, cmd,
                                      VP_ENGINE_ATTRIB_COUNT, layout_desc);
        env->bind_vertex_stream_state(env->opaque, SITE_BIND_STREAM, cmd,
                                      VP_ENGINE_STREAM_COUNT,
                                      layout_desc + 0x40u);
        memset((uint8_t *)state + VP_ENGINE_STATE_CACHE_OFFSET, 0,
               VP_ENGINE_STATE_CACHE_BYTES);
    }

    /* Bit 2 of the material flags forces rebinding even when the previous
     * material used the same layout and resource pointers. */
    /* 0xBC1828 saves the old resources before the bit2 override. The
     * second uniform group reloads that saved pointer at 0xBC1D40. */
    stage1_old_resources=old_resources;
    if (material_flags & 4u) old_resources = 0;

    groups0 = layout->groups0;
    groups1 = layout->groups1;
    n0 = groups0 >> 22;
    n1 = (groups0 >> 12) & 0x3ffu;
    n2 = groups1 >> 22;
    n3 = (groups1 >> 12) & 0x3ffu;
    descriptors = layout->descriptors;
    if ((n0 || n1) && !descriptors)
        return VP_ENGINE_MATERIAL_ERR_OBJECT;
    if ((n2 || n3) && !descriptors)
        return VP_ENGINE_MATERIAL_ERR_OBJECT;

    rc = emit_uniform_group(env, cmd, material, descriptors,
                            n0, 0u, old_resources,
                            SITE_BUFADDR_GROUP0_A, SITE_BIND_UNIFORM0_A,
                            SITE_BUFADDR_GROUP0_B, SITE_BIND_UNIFORM0_B);
    if (rc) return rc;
    {
        static const uint32_t barrier_a[4] = {
            SITE_BARRIER_A_NF_CHANGED, SITE_BARRIER_A_NF_UNCHANGED,
            SITE_BARRIER_A_FOUND_FIRST, SITE_BARRIER_A_FOUND_SECOND
        };
        static const uint32_t barrier_b[4] = {
            SITE_BARRIER_B_NF_CHANGED, SITE_BARRIER_B_NF_UNCHANGED,
            SITE_BARRIER_B_FOUND_FIRST, SITE_BARRIER_B_FOUND_SECOND
        };
        rc = emit_texture_group(env, state, ctx, cmd, material,
                                descriptors + n0 * 8u, n1, 0u,
                                VP_ENGINE_STATE_CACHE_OFFSET,
                                VP_ENGINE_STATE_CACHE_OFFSET + 0x80u,
                                barrier_a, SITE_TEXHANDLE_A,
                                SITE_BIND_TEXTURE_A);
        if (rc) return rc;
        rc = emit_uniform_group(env, cmd, material,
                                descriptors + (n0 + n1) * 8u,
                                n2, 1u, stage1_old_resources,
                                SITE_BUFADDR_GROUP1_A, SITE_BIND_UNIFORM1_A,
                                SITE_BUFADDR_GROUP1_B, SITE_BIND_UNIFORM1_B);
        if (rc) return rc;
        rc = emit_texture_group(env, state, ctx, cmd, material,
                                descriptors + (n0 + n1 + n2) * 8u, n3, 1u,
                                VP_ENGINE_STATE_CACHE_OFFSET + 0x40u,
                                VP_ENGINE_STATE_CACHE_OFFSET + 0xc0u,
                                barrier_b, SITE_TEXHANDLE_B,
                                SITE_BIND_TEXTURE_B);
        if (rc) return rc;
    }

    state->last_material = material;
    return submit_tail(env, state, ctx, material, cmd);
}

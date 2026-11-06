/* Host regression for the pure shader-resource plan and its bounded,
 * generation-checked cache. The production builder/cache are the static-inline
 * functions in src/platform/gxm_shader_plan.h. The fake callbacks implement the
 * original per-slot API semantics so the plan is compared against the exact
 * call sequence guest_svc used before this experiment. */
#include "gxm_shader_plan.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FAKE_STAGE_NONE 0xffffu
#define FAKE_PROGRAMS 4u
#define FAKE_RESIDENTS 8u

typedef struct {
    uint32_t token;
    uint16_t vs_stage;
    uint16_t fs_stage;
} FakeProgram;

typedef struct {
    unsigned used;
    uint16_t stage;
    uint16_t variant;
    unsigned declared_mask;
} FakeResident;

typedef struct {
    FakeProgram programs[FAKE_PROGRAMS];
    unsigned program_count;
    FakeResident residents[FAKE_RESIDENTS];
    unsigned resident_count;
    int active;
    unsigned layout_stage[4][2];
    unsigned layout_slot[4][2][5];
} FakeWorld;

static FakeProgram *fake_program(FakeWorld *world, uint32_t token) {
    unsigned i;
    for (i = 0u; i < world->program_count; ++i)
        if (world->programs[i].token == token) return &world->programs[i];
    return NULL;
}

static unsigned fake_stage(FakeWorld *world, uint32_t token,
                           unsigned gpu_stage) {
    FakeProgram *program = fake_program(world, token);
    if (program == NULL) return FAKE_STAGE_NONE;
    return gpu_stage == 0u ? program->vs_stage : program->fs_stage;
}

static FakeResident *fake_resident(FakeWorld *world, unsigned stage,
                                   unsigned variant) {
    unsigned i;
    for (i = 0u; i < world->resident_count; ++i)
        if (world->residents[i].used && world->residents[i].stage == stage &&
            world->residents[i].variant == variant)
            return &world->residents[i];
    return NULL;
}

static FakeResident *fake_any_resident(FakeWorld *world, unsigned stage) {
    unsigned i;
    for (i = 0u; i < world->resident_count; ++i)
        if (world->residents[i].used && world->residents[i].stage == stage)
            return &world->residents[i];
    return NULL;
}

/* vp_gxm_shader_has_{fragment,vertex}_sampler: the registered reflection of a
 * resident loaded for this stage/viewport declares the slot. */
static int fake_declared(void *user, uint32_t token, unsigned gpu_stage,
                         unsigned viewport, unsigned slot) {
    FakeWorld *world = user;
    unsigned stage;
    FakeResident *resident;
    if (gpu_stage == 0u && slot > 2u) return 0; /* vertex keeps tex0..tex2 */
    stage = fake_stage(world, token, gpu_stage);
    if (stage == FAKE_STAGE_NONE) return 0;
    resident = fake_resident(world, stage, viewport & 1u);
    if (resident == NULL) resident = fake_any_resident(world, stage);
    if (resident == NULL) return 0;
    return (resident->declared_mask & (1u << slot)) != 0u;
}

static int fake_resources_active(void *user) {
    return ((FakeWorld *)user)->active;
}

/* vp_gxm_shader_resource: every return code and the value copy. */
static int fake_shader_resource(void *user, uint32_t token, unsigned gpu_stage,
                                unsigned viewport, unsigned slot,
                                VPResourceTexture *out) {
    FakeWorld *world = user;
    unsigned stage, column = gpu_stage == 0u ? 0u : 1u;
    memset(out, 0, sizeof(*out));
    if (gpu_stage != 0u && gpu_stage != 4u) return -1;
    if (slot >= VP_GXM_FRAGMENT_SAMPLER_LIMIT) return -1;
    if (world->active == 0) return -3;
    if (world->active < 0) return -2;
    stage = fake_stage(world, token, gpu_stage);
    if (stage == FAKE_STAGE_NONE || stage >= 4u) return -4;
    if (!fake_declared(user, token, gpu_stage, viewport, slot)) return 1;
    if (!world->layout_stage[stage][column]) return -5;
    if (!world->layout_slot[stage][column][slot]) return -6;
    out->slot = (uint8_t)slot;
    out->texture_type = (uint8_t)(VP_RESOURCE_TEX_2D + column);
    out->offset = (uint16_t)(32u + 8u * slot);
    out->flags = (uint8_t)(stage + 1u);
    return 0;
}

static const VPGxmShaderPlanOps fake_ops = {
    NULL, fake_declared, fake_resources_active, fake_shader_resource
};

/* Exact old guest_svc query sequence for one stage, used as the oracle. */
static void old_queries(FakeWorld *world, uint32_t token, unsigned gpu_stage,
                        unsigned viewport, VPGxmShaderResourcePlan *out) {
    unsigned slot;
    memset(out, 0, sizeof(*out));
    out->resources_active = fake_resources_active(world);
    for (slot = 0u; slot < VP_GXM_FRAGMENT_SAMPLER_LIMIT; ++slot) {
        int declared =
            fake_declared(world, token, gpu_stage, viewport, slot);
        if (!declared) {
            out->slots[slot].layout_rc = 1;
            continue;
        }
        out->declared_mask |= 1u << slot;
        if (out->resources_active != 0)
            out->slots[slot].layout_rc = fake_shader_resource(
                world, token, gpu_stage, viewport, slot,
                &out->slots[slot].layout);
    }
}

static void expect_equal(const VPGxmShaderResourcePlan *actual,
                         const VPGxmShaderResourcePlan *expected) {
    unsigned slot;
    assert(actual->declared_mask == expected->declared_mask);
    assert(actual->resources_active == expected->resources_active);
    for (slot = 0u; slot < VP_GXM_FRAGMENT_SAMPLER_LIMIT; ++slot) {
        assert(actual->slots[slot].layout_rc ==
               expected->slots[slot].layout_rc);
        if (actual->slots[slot].layout_rc == 0)
            assert(memcmp(&actual->slots[slot].layout,
                          &expected->slots[slot].layout,
                          sizeof(VPResourceTexture)) == 0);
    }
}

static void world_seed(FakeWorld *world) {
    memset(world, 0, sizeof(*world));
    world->active = 1;
    world->program_count = 2u;
    world->programs[0].token = 0x100u;
    world->programs[0].vs_stage = 0u;
    world->programs[0].fs_stage = 1u;
    world->programs[1].token = 0x200u;
    world->programs[1].vs_stage = 2u;
    world->programs[1].fs_stage = 3u;
    world->resident_count = 2u;
    world->residents[0].used = 1u;
    world->residents[0].stage = 0u;
    world->residents[0].variant = 0u;
    world->residents[0].declared_mask = 1u << 0;
    world->residents[1].used = 1u;
    world->residents[1].stage = 1u;
    world->residents[1].variant = 0u;
    world->residents[1].declared_mask = (1u << 0) | (1u << 2);
    world->layout_stage[0][0] = 1u;
    world->layout_stage[1][1] = 1u;
    world->layout_stage[2][0] = 1u;
    world->layout_stage[3][1] = 1u;
    world->layout_slot[0][0][0] = 1u;
    world->layout_slot[1][1][0] = 1u;
    world->layout_slot[1][1][2] = 1u;
    world->layout_slot[2][0][0] = 1u;
    world->layout_slot[3][1][0] = 1u;
}

static void bind_ops(FakeWorld *world, VPGxmShaderPlanOps *ops) {
    *ops = fake_ops;
    ops->user = world;
}

static void test_stage_and_viewport_differences(void) {
    FakeWorld world;
    VPGxmShaderPlanOps ops;
    VPGxmShaderResourcePlan expected, plan;
    world_seed(&world);
    bind_ops(&world, &ops);
    /* Same token, FS declares tex0/tex2 while VS declares tex0 only. */
    old_queries(&world, 0x100u, 4u, 0u, &expected);
    assert(vp_gxm_shader_plan_build(&ops, 0x100u, 4u, 0u, &plan) == 0);
    expect_equal(&plan, &expected);
    assert(plan.declared_mask == ((1u << 0) | (1u << 2)));
    old_queries(&world, 0x100u, 0u, 0u, &expected);
    assert(vp_gxm_shader_plan_build(&ops, 0x100u, 0u, 0u, &plan) == 0);
    expect_equal(&plan, &expected);
    assert(plan.declared_mask == (1u << 0));

    /* A different viewport selects the variant-1 resident with a different
     * declaration set. */
    world.residents[1].variant = 1u;
    world.residents[1].declared_mask = 1u << 3;
    old_queries(&world, 0x100u, 4u, 1u, &expected);
    assert(vp_gxm_shader_plan_build(&ops, 0x100u, 4u, 1u, &plan) == 0);
    expect_equal(&plan, &expected);
    assert(plan.declared_mask == (1u << 3));
    puts("PASS shader plan separates VS/FS stages and viewport variants");
}

static void test_undeclared_and_layout_failures(void) {
    FakeWorld world;
    VPGxmShaderPlanOps ops;
    VPGxmShaderResourcePlan expected, plan;
    world_seed(&world);
    bind_ops(&world, &ops);
    /* Resident declares nothing: every slot keeps rc=1 and no layout query. */
    world.residents[1].declared_mask = 0u;
    old_queries(&world, 0x100u, 4u, 0u, &expected);
    assert(vp_gxm_shader_plan_build(&ops, 0x100u, 4u, 0u, &plan) == 0);
    expect_equal(&plan, &expected);
    assert(plan.declared_mask == 0u);
    assert(plan.slots[0].layout_rc == 1);

    /* Declared but the stage sidecar is missing. */
    world.residents[1].declared_mask = 1u << 0;
    world.layout_stage[1][1] = 0u;
    old_queries(&world, 0x100u, 4u, 0u, &expected);
    assert(vp_gxm_shader_plan_build(&ops, 0x100u, 4u, 0u, &plan) == 0);
    expect_equal(&plan, &expected);
    assert(plan.slots[0].layout_rc == -5);

    /* Declared, stage present, but the slot has no descriptor. */
    world.layout_stage[1][1] = 1u;
    world.layout_slot[1][1][0] = 0u;
    old_queries(&world, 0x100u, 4u, 0u, &expected);
    assert(vp_gxm_shader_plan_build(&ops, 0x100u, 4u, 0u, &plan) == 0);
    expect_equal(&plan, &expected);
    assert(plan.slots[0].layout_rc == -6);

    /* resources_active < 0: the original caller still invoked
     * vp_gxm_shader_resource, which reports its own error; the caller then
     * takes the resources_active < 0 branch and logs layout-unavailable. */
    world.active = -1;
    old_queries(&world, 0x100u, 4u, 0u, &expected);
    assert(vp_gxm_shader_plan_build(&ops, 0x100u, 4u, 0u, &plan) == 0);
    expect_equal(&plan, &expected);
    assert(plan.resources_active == -1);
    assert(plan.slots[0].layout_rc == -2);
    puts("PASS shader plan matches undeclared/layout/active failure semantics");
}

static void test_missing_resident_then_generation(void) {
    FakeWorld world;
    VPGxmShaderPlanOps ops;
    VPGxmShaderResourcePlan expected, plan;
    VPGxmShaderPlanCacheEntry cache[8];
    VPGxmShaderPlanCounts counts;
    uint32_t generation = 1u;
    world_seed(&world);
    world.resident_count = 1u; /* only the VS stage is resident */
    bind_ops(&world, &ops);
    memset(cache, 0, sizeof(cache));
    memset(&counts, 0, sizeof(counts));
    assert(vp_gxm_shader_plan_cached(cache, 8u, generation, &ops, 0x100u,
                                     4u, 0u, &plan, &counts) == 0);
    assert(plan.declared_mask == 0u);
    assert(counts.misses == 1u && counts.builds == 1u);
    /* Same generation: the cached "undeclared" result is reused exactly. */
    assert(vp_gxm_shader_plan_cached(cache, 8u, generation, &ops, 0x100u,
                                     4u, 0u, &plan, &counts) == 0);
    assert(plan.declared_mask == 0u);
    assert(counts.hits == 1u);
    /* The resident is registered now. Without a generation bump the cache is
     * stale by design; with it the new declaration must appear. */
    world.resident_count = 2u;
    assert(vp_gxm_shader_plan_cached(cache, 8u, generation, &ops, 0x100u,
                                     4u, 0u, &plan, &counts) == 0);
    assert(plan.declared_mask == 0u);
    generation++;
    assert(vp_gxm_shader_plan_cached(cache, 8u, generation, &ops, 0x100u,
                                     4u, 0u, &plan, &counts) == 0);
    old_queries(&world, 0x100u, 4u, 0u, &expected);
    expect_equal(&plan, &expected);
    assert(plan.declared_mask == ((1u << 0) | (1u << 2)));
    assert(counts.invalidations == 1u);
    assert(counts.builds == 2u);
    puts("PASS shader plan refresh follows a resident generation bump");
}

static void test_index_reload_and_key_collision(void) {
    FakeWorld world;
    VPGxmShaderPlanOps ops;
    VPGxmShaderResourcePlan expected, plan;
    VPGxmShaderPlanCacheEntry cache[8];
    VPGxmShaderPlanCounts counts;
    uint32_t token2 = 0x101u, index;
    uint32_t generation = 1u;
    world_seed(&world);
    world.program_count = 2u;
    world.programs[1].token = token2;
    world.programs[1].vs_stage = 2u;
    world.programs[1].fs_stage = 3u;
    world.resident_count = 4u;
    world.residents[2].used = 1u;
    world.residents[2].stage = 2u;
    world.residents[2].variant = 0u;
    world.residents[2].declared_mask = 1u << 1;
    world.residents[3].used = 1u;
    world.residents[3].stage = 3u;
    world.residents[3].variant = 0u;
    world.residents[3].declared_mask = 1u << 4;
    world.layout_slot[2][0][1] = 1u;
    world.layout_slot[3][1][4] = 1u;
    bind_ops(&world, &ops);
    memset(cache, 0, sizeof(cache));
    memset(&counts, 0, sizeof(counts));
    index = vp_gxm_shader_plan_cache_index(0x100u, 4u, 0u, 8u);
    while (vp_gxm_shader_plan_cache_index(token2, 4u, 0u, 8u) != index)
        token2++;
    world.programs[1].token = token2;
    assert(vp_gxm_shader_plan_cached(cache, 8u, generation, &ops, 0x100u,
                                     4u, 0u, &plan, &counts) == 0);
    assert(plan.declared_mask == ((1u << 0) | (1u << 2)));
    assert(vp_gxm_shader_plan_cached(cache, 8u, generation, &ops, token2,
                                     4u, 0u, &plan, &counts) == 0);
    assert(plan.declared_mask == (1u << 4));
    /* Collision evicted the first entry; the complete key decides the miss. */
    assert(vp_gxm_shader_plan_cached(cache, 8u, generation, &ops, 0x100u,
                                     4u, 0u, &plan, &counts) == 0);
    assert(plan.declared_mask == ((1u << 0) | (1u << 2)));
    assert(counts.hits == 0u && counts.builds == 3u);

    /* Index reload moves the same token to a different stage and invalidates
     * the previously cached declaration set. */
    world.programs[0].fs_stage = 3u;
    world.residents[3].declared_mask = 1u << 1;
    generation++;
    assert(vp_gxm_shader_plan_cached(cache, 8u, generation, &ops, 0x100u,
                                     4u, 0u, &plan, &counts) == 0);
    old_queries(&world, 0x100u, 4u, 0u, &expected);
    expect_equal(&plan, &expected);
    assert(plan.declared_mask == (1u << 1));
    assert(counts.invalidations >= 1u);
    puts("PASS shader plan collision and index reload use full identity");
}

int main(void) {
    test_stage_and_viewport_differences();
    test_undeclared_and_layout_failures();
    test_missing_resident_then_generation();
    test_index_reload_and_key_collision();
    puts("gxm_shader_plan: all checks passed");
    return 0;
}

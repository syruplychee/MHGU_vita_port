/* Host-level (L1) tests for the native state helpers main+0xBC0E98 and
 * main+0xBC13E0.  Synthetic guest memory only; no runtime claim. */

#include "engine_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_SIZE 0x400000u
#define CTX        0x1000u
#define STATE      0x8000u
#define TARGET     0x10000u
#define TARGET2    0x12000u
#define T0         0x14000u
#define CUR        0x15000u
#define CURHOLDER  0x16000u
#define PIPELINE   0x18000u
#define PIPELINE2  0x19000u
#define COLOR      0x1a000u
#define DEPTH      0x1b000u
#define POLY       0x1c000u
#define SCRATCH    0x20000u
#define DEFAULT_VP 0x21000u

#define MAX_EVENTS 64

typedef struct {
    uint32_t site;
    uint32_t slot;
    uint32_t r[4];
    uint32_t stack[8];
    unsigned stack_words;
    uint32_t s[4];
    unsigned s_words;
} Event;

typedef struct {
    uint8_t *memory;
    unsigned size;
    Event events[MAX_EVENTS];
    unsigned count;
    unsigned memclr_count;
    uint32_t memclr_site;
    uint32_t memclr_pointer;
    uint32_t memclr_size;
} World;

static World world;
static VPEngineNvnEnv test_env;

static void fail(const char *message)
{
    fprintf(stderr, "engine_state: %s\n", message);
    exit(1);
}

#define CHECK(cond, message) do { if (!(cond)) fail(message); } while (0)

static void *resolve(void *opaque, uint32_t address)
{
    World *w = (World *)opaque;
    if (address >= w->size) return NULL;
    return w->memory + address;
}

static void emit(void *opaque, const VPEngineNvnCall *call,
                 VPEngineNvnResult *result)
{
    World *w = (World *)opaque;
    Event *e;
    (void)result;
    CHECK(w->count < MAX_EVENTS, "event overflow");
    e = &w->events[w->count++];
    memset(e, 0, sizeof(*e));
    e->site = call->site;
    e->slot = call->slot;
    memcpy(e->r, call->r, sizeof(e->r));
    memcpy(e->stack, call->stack, sizeof(e->stack));
    e->stack_words = call->stack_words;
    memcpy(e->s, call->s, sizeof(e->s));
    e->s_words = call->s_words;
}

static void memclr(void *opaque, uint32_t site, uint32_t pointer, uint32_t size)
{
    World *w = (World *)opaque;
    w->memclr_count++;
    w->memclr_site = site;
    w->memclr_pointer = pointer;
    w->memclr_size = size;
    CHECK(pointer < w->size && size <= w->size - pointer, "memclr range");
    memset(w->memory + pointer, 0, size);
}

static void w32(uint32_t address, uint32_t value)
{
    CHECK((size_t)address + 4u <= ARENA_SIZE, "write range");
    memcpy(world.memory + address, &value, 4);
}

static uint32_t r32(uint32_t address)
{
    uint32_t value;
    CHECK((size_t)address + 4u <= ARENA_SIZE, "read range");
    memcpy(&value, world.memory + address, 4);
    return value;
}

static void setup(void)
{
    memset(&world, 0, sizeof(world));
    world.memory = (uint8_t *)calloc(1, ARENA_SIZE);
    CHECK(world.memory != NULL, "arena allocation");
    world.size = ARENA_SIZE;
    memset(&test_env, 0, sizeof(test_env));
    test_env.opaque = &world;
    test_env.resolve = resolve;
    test_env.emit = emit;
    test_env.memclr = memclr;
    test_env.default_viewport = DEFAULT_VP;
    test_env.scratch = SCRATCH;
    test_env.scratch_size = 0x80u;
    w32(CTX + 0x389c0u, CURHOLDER);
    w32(CURHOLDER + 0x204u, 0x17000u);
    w32(0x17000u, CUR);
    w32(CUR + 0x14u, 0x1234u);
    w32(CUR + 0x10u, 0x17500u);
    w32(0x17500u + 0x3cu, 0x5678u);
    w32(CTX + 0x389ccu, 0);
}

static VPEngineNvnEnv *env_get(void)
{
    return &test_env;
}

static void teardown(void)
{
    free(world.memory);
    world.memory = NULL;
}

static void setup_target(uint32_t target, uint32_t count, uint32_t first,
                         uint32_t width, uint32_t height)
{
    w32(target + 0x0u, first);
    w32(target + 0x10u, 0u);
    w32(target + 0x14u, count);
    w32(target + 0x18u, 0u);
    w32(target + 0x1cu, 0u);
    w32(target + 0x20u, width);
    w32(target + 0x24u, height);
    w32(target + 0x38u, 0u);
    w32(target + 0x3cu, 0u);
    w32(target + 0x40u, width);
    w32(target + 0x44u, height);
    w32(target + 0x58u, width | (height << 16));
}

static void test_targets(void)
{
    uint32_t state = STATE;
    uint32_t target = TARGET;
    uint32_t target2 = TARGET2;

    setup();
    memset(world.memory + DEFAULT_VP, 0, 16);
    setup_target(target, 1u, T0, 320u, 240u);
    w32(T0 + 0x10u, 0x19000u);
    w32(T0 + 0x14u, 0x9abcu);
    w32(0x19000u + 0x3cu, 0xdef0u);
    setup_target(target2, 0u, 0u, 320u, 240u);

    CHECK(vp_engine_state_targets(env_get(), CTX, state, target) ==
              VP_ENGINE_NATIVE_OK, "targets failed");
    CHECK(world.count == 3u, "targets event count");
    CHECK(world.events[0].slot == VP_SLOT_SET_RENDER_TARGETS &&
          world.events[0].site == 0xbc1200u, "render targets call");
    CHECK(world.events[0].r[0] == 0u && world.events[0].r[1] == 1u,
          "render targets count");
    CHECK(world.events[0].r[2] == SCRATCH + 0x20u &&
          world.events[0].r[3] == SCRATCH, "render targets arrays");
    CHECK(world.events[0].stack[0] == 0u && world.events[0].stack[1] == 0u,
          "render targets aux stack");
    CHECK(world.events[1].slot == VP_SLOT_SET_VIEWPORT &&
          world.events[1].site == 0xbc12ccu, "viewport call");
    CHECK(world.events[1].r[1] == 0u && world.events[1].r[2] == 0u &&
          world.events[1].r[3] == 320u && world.events[1].stack[0] == 240u,
          "viewport args");
    CHECK(world.events[2].slot == VP_SLOT_SET_SCISSOR &&
          world.events[2].site == 0xbc13b8u, "scissor call");
    CHECK(world.events[2].r[1] == 0u && world.events[2].r[2] == 0u &&
          world.events[2].r[3] == 320u && world.events[2].stack[0] == 240u,
          "scissor args");
    CHECK(r32(state + 0x8u) == target, "last target");
    CHECK(r32(state + 0x94u) == T0, "target cache");
    CHECK(world.memclr_count == 1u && world.memclr_site == 0xbc1134u &&
          world.memclr_pointer == state + 0x128u &&
          world.memclr_size == 0x80u, "texture cache clear");
    CHECK(r32(SCRATCH) == 0x9abcu && r32(SCRATCH + 0x20u) == 0xdef0u,
          "render target rect arrays");

    world.count = 0;
    CHECK(vp_engine_state_targets(env_get(), CTX, state, target) ==
              VP_ENGINE_NATIVE_OK, "targets repeat failed");
    CHECK(world.count == 0u, "targets repeat emitted");

    world.count = 0;
    CHECK(vp_engine_state_targets(env_get(), CTX, state, target2) ==
              VP_ENGINE_NATIVE_OK, "targets second failed");
    CHECK(world.count == 0u, "targets empty change emitted");
    CHECK(r32(state + 0x8u) == target2, "last target second");
    teardown();
}

static void setup_pipeline(uint32_t pipeline)
{
    w32(pipeline + 0x0u, 0x00112233u);
    w32(pipeline + 0x4u, 0u);
    w32(pipeline + 0x10u, COLOR);
    w32(pipeline + 0x14u, DEPTH);
    w32(pipeline + 0x18u, POLY);
}

static void test_pipeline(void)
{
    uint32_t state = STATE;
    float expected_blend;
    uint32_t expected_bits;

    setup();
    setup_pipeline(PIPELINE);
    w32(PIPELINE + 0x0u, 0x11223344u);
    w32(COLOR + 0x0u, 0x11223344u);
    w32(COLOR + 0xcu, 0x100u);
    w32(DEPTH + 0x18u, 0xaaaaaaaa);
    w32(DEPTH + 0x20u, 0x55555555u);
    w32(DEPTH + 0x24u, 0x66666666u);
    w32(POLY + 0x26u, 0u);
    w32(POLY + 0x28u, 0xfffffffeu); /* -2 */
    w32(POLY + 0x2cu, 0x3f000000u);
    w32(POLY + 0x30u, 0x3e800000u);
    expected_blend = 0x44u / 255.0f;
    memcpy(&expected_bits, &expected_blend, sizeof(expected_bits));

    CHECK(vp_engine_state_pipeline(env_get(), CTX, state, PIPELINE) ==
              VP_ENGINE_NATIVE_OK, "pipeline failed");
    CHECK(world.count == 17u, "pipeline event count");
    CHECK(world.events[0].slot == VP_SLOT_SET_BLEND_COLOR &&
          world.events[0].site == 0xbc14c8u, "blend color first");
    CHECK(world.events[0].r[0] == 0u && world.events[0].r[1] ==
              SCRATCH + 0x40u, "blend color args");
    CHECK(world.events[1].slot == VP_SLOT_BIND_BLEND_STATE &&
          world.events[1].site == 0xbc14e0u &&
          world.events[1].r[1] == COLOR + 0x48u, "blend state 0");
    CHECK(world.events[8].slot == VP_SLOT_BIND_BLEND_STATE &&
          world.events[8].site == 0xbc1550u &&
          world.events[8].r[1] == COLOR + 0x80u, "blend state 7");
    CHECK(world.events[9].slot == VP_SLOT_BIND_CHANNEL_MASK_STATE &&
          world.events[9].site == 0xbc1568u &&
          world.events[9].r[1] == COLOR + 0x40u, "channel mask");
    CHECK(world.events[10].slot == VP_SLOT_BIND_DEPTH_STENCIL_STATE &&
          world.events[10].site == 0xbc15fcu &&
          world.events[10].r[1] == DEPTH + 0x18u, "depth state");
    CHECK(world.events[11].slot == VP_SLOT_SET_STENCIL_VALUE_MASK &&
          world.events[11].r[1] == 3u &&
          world.events[11].r[2] == 0x55555555u, "stencil value mask");
    CHECK(world.events[12].slot == VP_SLOT_SET_STENCIL_MASK &&
          world.events[12].r[2] == 0x66666666u, "stencil mask");
    CHECK(world.events[13].slot == VP_SLOT_SET_STENCIL_REF &&
          world.events[13].r[2] == 0u, "stencil ref byte");
    CHECK(world.events[14].slot == VP_SLOT_BIND_POLYGON_STATE &&
          world.events[14].r[1] == POLY + 0x20u, "polygon state");
    CHECK(world.events[15].slot == VP_SLOT_SET_POLYGON_OFFSET_CLAMP &&
          world.events[15].s[0] == 0x3e800000u &&
          world.events[15].s[1] == 0xc0000000u &&
          world.events[15].s[2] == 0x3f000000u, "polygon offset floats");
    CHECK(world.events[15].site == 0xbc1704u, "polygon offset site");
    CHECK(world.events[16].slot == VP_SLOT_SET_DEPTH_CLAMP &&
          world.events[16].site == 0xbc1720u &&
          world.events[16].r[1] == 1u, "depth clamp");
    CHECK(r32(state + 0xcu) == PIPELINE, "last pipeline");
    CHECK(r32(state + 0x18u) == COLOR, "color cache");
    CHECK(r32(state + 0x1cu) == POLY, "polygon cache");
    CHECK(r32(state + 0x20u) == DEPTH, "depth cache");
    CHECK(r32(state + 0x1170u) == 0x11223344u, "color word");
    CHECK(r32(SCRATCH + 0x40u) == expected_bits, "blend color word");

    world.count = 0;
    CHECK(vp_engine_state_pipeline(env_get(), CTX, state, PIPELINE) ==
              VP_ENGINE_NATIVE_OK, "pipeline repeat failed");
    CHECK(world.count == 0u, "pipeline repeat emitted");
    teardown();
}

static void test_default_scissor_sentinel(void)
{
    setup();
    setup_target(TARGET, 0u, 0u, 1280u, 720u);
    memset(world.memory + TARGET + 0x38u, 0, 16u);
    CHECK(vp_engine_state_targets(env_get(), CTX, STATE, TARGET) == 0,
          "default scissor sentinel run");
    CHECK(world.count == 2u, "viewport and full scissor must be emitted");
    CHECK(world.events[1].slot == VP_SLOT_SET_SCISSOR &&
          world.events[1].r[3] == 1280u &&
          world.events[1].stack[0] == 720u,
          "default scissor sentinel must restore the full viewport");
    CHECK(r32(STATE + 0x8cu) == 1280u && r32(STATE + 0x90u) == 720u,
          "full scissor cache writeback");
    teardown();
}

int main(void)
{
    test_targets();
    test_pipeline();
    test_default_scissor_sentinel();
    printf("engine_state: all checks passed\n");
    return 0;
}

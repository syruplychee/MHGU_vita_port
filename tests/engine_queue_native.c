/* Host-level (L1) tests for the native queue and state reconstruction.
 * These tests exercise the C translation against synthetic guest memory; they
 * do not claim anything about Vita3K/runtime behaviour. */

#include "engine_queue_native.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_SIZE 0x500000u

#define CTX            0x1000u
#define STATE          0x8000u
#define TARGET         0x10000u
#define PIPELINE       0x11000u
#define MATERIAL       0x12000u
#define LAYOUT         0x13000u
#define PROGRAM_META   0x14000u
#define VERTEX_STATE   0x15000u
#define NODES          0x20000u
#define NESTED_NODES   0x21000u
#define RECORDS        0x30000u
#define AUX_OBJECT     0x40000u
#define SRGB_GLOBAL    0x50000u
#define DEFAULT_VP     0x51000u
#define SCRATCH        0x52000u
#define GPU_ROOT       0x53000u
#define CMD            0x54000u

#define MAX_EVENTS 256

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
    unsigned event_count;
    uint64_t buffer_address;
    unsigned query_count;
    unsigned memclr_count;
    uint32_t last_memclr_site;
    uint32_t last_memclr_pointer;
    uint32_t last_memclr_size;
    unsigned external_count;
    uint32_t last_external_site;
    uint32_t last_external_arg0;
    uint32_t last_external_arg1;
    unsigned pow_count;
    uint32_t pow_sites[4];
    double pow_bases[4];
    double pow_exponents[4];
} World;

static World world;

#if defined(_MSC_VER)
__declspec(noreturn)
#else
__attribute__((noreturn))
#endif
static void fail(const char *message)
{
    fprintf(stderr, "engine_queue_native: %s\n", message);
    exit(1);
}

#define CHECK(cond, message) do { if (!(cond)) fail(message); } while (0)

static void *world_resolve(void *opaque, uint32_t address)
{
    World *w = (World *)opaque;
    if (address >= w->size) return NULL;
    return w->memory + address;
}

static void world_emit(void *opaque, const VPEngineNvnCall *call,
                       VPEngineNvnResult *result)
{
    World *w = (World *)opaque;
    Event *event;
    CHECK(w->event_count < MAX_EVENTS, "event log overflow");
    event = &w->events[w->event_count++];
    memset(event, 0, sizeof(*event));
    event->site = call->site;
    event->slot = call->slot;
    memcpy(event->r, call->r, sizeof(event->r));
    memcpy(event->stack, call->stack, sizeof(event->stack));
    event->stack_words = call->stack_words;
    memcpy(event->s, call->s, sizeof(event->s));
    event->s_words = call->s_words;

    if (call->slot == VP_SLOT_BUFFER_GET_ADDRESS) {
        w->query_count++;
        if (result) {
            result->value = w->buffer_address;
            result->has_value = 1;
        }
    }
}

static void world_memclr(void *opaque, uint32_t site, uint32_t pointer,
                         uint32_t size)
{
    World *w = (World *)opaque;
    w->memclr_count++;
    w->last_memclr_site = site;
    w->last_memclr_pointer = pointer;
    w->last_memclr_size = size;
    CHECK(pointer < w->size && size <= w->size - pointer,
          "memclr outside arena");
    memset(w->memory + pointer, 0, size);
}

static void world_external(void *opaque, uint32_t site, uint32_t arg0,
                           uint32_t arg1)
{
    World *w = (World *)opaque;
    w->external_count++;
    w->last_external_site = site;
    w->last_external_arg0 = arg0;
    w->last_external_arg1 = arg1;
}

static double world_pow(void *opaque, uint32_t site, double base,
                        double exponent)
{
    World *w = (World *)opaque;
    if (w->pow_count < 4u) {
        w->pow_sites[w->pow_count] = site;
        w->pow_bases[w->pow_count] = base;
        w->pow_exponents[w->pow_count] = exponent;
    }
    w->pow_count++;
    return 0.25;
}

static void material_bind_program(void *opaque, uint32_t site, uint32_t cmd,
                                  uint32_t program, uint32_t flags)
{
    World *w = (World *)opaque;
    Event *e = &w->events[w->event_count++];
    memset(e, 0, sizeof(*e));
    e->site = site;
    e->slot = VP_SLOT_BIND_PROGRAM;
    e->r[0] = cmd;
    e->r[1] = program;
    e->r[2] = flags;
}

static void material_bind_attrib(void *opaque, uint32_t site, uint32_t cmd,
                                 unsigned count, uint32_t desc)
{
    World *w = (World *)opaque;
    Event *e = &w->events[w->event_count++];
    memset(e, 0, sizeof(*e));
    e->site = site;
    e->slot = VP_SLOT_BIND_VERTEX_ATTRIB_STATE;
    e->r[0] = cmd;
    e->r[1] = count;
    e->r[2] = desc;
}

static void material_bind_stream(void *opaque, uint32_t site, uint32_t cmd,
                                 unsigned count, uint32_t desc)
{
    World *w = (World *)opaque;
    Event *e = &w->events[w->event_count++];
    memset(e, 0, sizeof(*e));
    e->site = site;
    e->slot = VP_SLOT_BIND_VERTEX_STREAM_STATE;
    e->r[0] = cmd;
    e->r[1] = count;
    e->r[2] = desc;
}

static uint64_t material_buffer_address(void *opaque, uint32_t site,
                                        uint32_t buffer)
{
    World *w = (World *)opaque;
    Event *e = &w->events[w->event_count++];
    (void)buffer;
    memset(e, 0, sizeof(*e));
    e->site = site;
    e->slot = VP_SLOT_BUFFER_GET_ADDRESS;
    e->r[0] = buffer;
    w->query_count++;
    return w->buffer_address;
}

static void material_bind_uniform(void *opaque, uint32_t site, uint32_t cmd,
                                  unsigned stage, unsigned index,
                                  uint64_t address, uint32_t size)
{
    (void)opaque;
    (void)site;
    (void)cmd;
    (void)stage;
    (void)index;
    (void)address;
    (void)size;
    fail("unexpected uniform bind in host test");
}

static uint64_t material_texture_handle(void *opaque, uint32_t site,
                                        uint32_t device, uint32_t texture_id,
                                        uint32_t sampler_id)
{
    (void)opaque;
    (void)site;
    (void)device;
    (void)texture_id;
    (void)sampler_id;
    fail("unexpected texture query in host test");
}

static void material_bind_texture(void *opaque, uint32_t site, uint32_t cmd,
                                  unsigned stage, unsigned index,
                                  uint64_t handle)
{
    (void)opaque;
    (void)site;
    (void)cmd;
    (void)stage;
    (void)index;
    (void)handle;
    fail("unexpected texture bind in host test");
}

static void material_barrier(void *opaque, uint32_t site, uint32_t cmd,
                             uint32_t bits)
{
    World *w = (World *)opaque;
    Event *e = &w->events[w->event_count++];
    memset(e, 0, sizeof(*e));
    e->site = site;
    e->slot = VP_SLOT_BARRIER;
    e->r[0] = cmd;
    e->r[1] = bits;
}

static void material_alpha(void *opaque, uint32_t site, uint32_t cmd,
                           float ref)
{
    World *w = (World *)opaque;
    Event *e = &w->events[w->event_count++];
    memset(e, 0, sizeof(*e));
    e->site = site;
    e->slot = VP_SLOT_SET_ALPHA_REF;
    e->r[0] = cmd;
    memcpy(&e->s[0], &ref, sizeof(ref));
    e->s_words = 1;
}

static void material_color_state(void *opaque, uint32_t site, uint32_t cmd,
                                 uint32_t state)
{
    World *w = (World *)opaque;
    Event *e = &w->events[w->event_count++];
    memset(e, 0, sizeof(*e));
    e->site = site;
    e->slot = VP_SLOT_BIND_COLOR_STATE;
    e->r[0] = cmd;
    e->r[1] = state;
}

static void material_msaa(void *opaque, uint32_t site, uint32_t cmd,
                          uint32_t state)
{
    World *w = (World *)opaque;
    Event *e = &w->events[w->event_count++];
    memset(e, 0, sizeof(*e));
    e->site = site;
    e->slot = VP_SLOT_BIND_MULTISAMPLE_STATE;
    e->r[0] = cmd;
    e->r[1] = state;
}

static void write_u32(uint32_t address, uint32_t value)
{
    CHECK((size_t)address + 4u <= ARENA_SIZE, "guest write outside arena");
    memcpy(world.memory + address, &value, sizeof(value));
}

static uint32_t read_u32(uint32_t address)
{
    uint32_t value;
    CHECK((size_t)address + 4u <= ARENA_SIZE, "guest read outside arena");
    memcpy(&value, world.memory + address, sizeof(value));
    return value;
}

static void write_u16(uint32_t address, uint16_t value)
{
    CHECK((size_t)address + 2u <= ARENA_SIZE, "guest write outside arena");
    memcpy(world.memory + address, &value, sizeof(value));
}

static void setup_world(unsigned merge_enable)
{
    memset(&world, 0, sizeof(world));
    world.memory = (uint8_t *)calloc(1, ARENA_SIZE);
    CHECK(world.memory != NULL, "arena allocation failed");
    world.size = ARENA_SIZE;
    world.buffer_address = 0x1122334455667788ULL;

    write_u32(CTX + 0x94u, CMD);
    write_u32(CTX + 0x1fu, 0u);
    write_u32(CTX + 0x33f4cu, 0u);
    write_u32(CTX + 0x43bb10u, NODES);
    write_u32(CTX + 0x43bb18u, 0u);
    write_u32(CTX + 0x28u, merge_enable);
    write_u32(CTX + 0x340f0u, 0u);
    write_u32(CTX + 0x340f4u, 0u);
    write_u32(CTX + 0x43bb60u, 0u);

    write_u32(SRGB_GLOBAL, 0x600u);
    write_u32(0x600u + 0x29u, 0u);
    memset(world.memory + DEFAULT_VP, 0, 16);
    write_u32(GPU_ROOT + 0x14u, 0u);
}

static void teardown_world(void)
{
    free(world.memory);
    world.memory = NULL;
}

static void setup_material(void)
{
    write_u32(MATERIAL + 0x0u, 0u);   /* flags */
    write_u32(MATERIAL + 0x4u, 0u);   /* word1 */
    write_u32(MATERIAL + 0x8u, LAYOUT);
    write_u32(LAYOUT + 0x4u, 0u);
    write_u32(LAYOUT + 0x8u, 0u);
    write_u32(LAYOUT + 0xcu, 0u);
    write_u32(LAYOUT + 0x38u, PROGRAM_META);
    write_u32(LAYOUT + 0x3cu, 0x16000u);
    write_u32(PROGRAM_META + 0xc8u, 0u);
}

static void setup_pipeline(uint32_t address)
{
    write_u32(address + 0x0u, 0u);
    write_u32(address + 0x4u, 0u);
    write_u32(address + 0x10u, 0u);
    write_u32(address + 0x14u, 0u);
    write_u32(address + 0x18u, 0u);
}

static void setup_target(uint32_t address, uint32_t count,
                         uint32_t width, uint32_t height, uint32_t aux)
{
    write_u32(address + 0x10u, aux);
    write_u32(address + 0x14u, count);
    write_u32(address + 0x18u, 0u);
    write_u32(address + 0x1cu, 0u);
    write_u32(address + 0x20u, width);
    write_u32(address + 0x24u, height);
    write_u32(address + 0x28u, 0u);
    write_u32(address + 0x2cu, 0u);
    write_u32(address + 0x30u, width);
    write_u32(address + 0x34u, height);
    write_u32(address + 0x38u, 0u);
    write_u32(address + 0x3cu, 0u);
    write_u32(address + 0x40u, width);
    write_u32(address + 0x44u, height);
    write_u32(address + 0x58u, width | (height << 16));
    write_u32(address + 0x0u, 0u);
}

static uint32_t add_node(uint32_t node_array, unsigned index,
                         uint32_t record)
{
    uint32_t address = node_array + index * 8u;
    write_u32(address, 0u);
    write_u32(address + 4u, record);
    return address;
}

static void set_root(unsigned count)
{
    write_u32(CTX + 0x43bb18u, count);
}

static void setup_record(uint32_t address, unsigned type)
{
    write_u32(address + 0x0u, (uint32_t)type << 16);
    write_u32(address + 0x4u, 0u);
    write_u32(address + 0x8u, 0u);
    write_u32(address + 0xcu, 0u);
    write_u32(address + 0x10u, 0u);
    write_u32(address + 0x14u, 0u);
    write_u32(address + 0x18u, 0u);
    write_u32(address + 0x1cu, 0u);
    write_u32(address + 0x20u, 0u);
    write_u32(address + 0x24u, 0u);
    write_u32(address + 0x28u, 0u);
    write_u32(address + 0x2cu, 0u);
}

static const Event *find_event(uint32_t slot, unsigned occurrence)
{
    unsigned i;
    for (i = 0; i < world.event_count; i++) {
        if (world.events[i].slot == slot) {
            if (occurrence == 0) return &world.events[i];
            occurrence--;
        }
    }
    return NULL;
}

static unsigned count_events(uint32_t slot)
{
    unsigned i, count = 0;
    for (i = 0; i < world.event_count; i++)
        if (world.events[i].slot == slot) count++;
    return count;
}

static void make_envs(VPEngineNvnEnv *env, VPEngineMaterialEnv *material_env)
{
    memset(env, 0, sizeof(*env));
    env->opaque = &world;
    env->resolve = world_resolve;
    env->emit = world_emit;
    env->memclr = world_memclr;
    env->external_resource = world_external;
    env->numeric_pow = world_pow;
    env->srgb_config = SRGB_GLOBAL;
    env->default_viewport = DEFAULT_VP;
    env->scratch = SCRATCH;
    env->scratch_size = 0x80u;
    env->gpu_root = GPU_ROOT;

    memset(material_env, 0, sizeof(*material_env));
    material_env->opaque = &world;
    material_env->resolve = world_resolve;
    material_env->gpu_root = GPU_ROOT;
    material_env->bind_program = material_bind_program;
    material_env->bind_vertex_attrib_state = material_bind_attrib;
    material_env->bind_vertex_stream_state = material_bind_stream;
    material_env->buffer_get_address = material_buffer_address;
    material_env->bind_uniform_buffer = material_bind_uniform;
    material_env->device_get_texture_handle = material_texture_handle;
    material_env->bind_texture = material_bind_texture;
    material_env->barrier = material_barrier;
    material_env->set_alpha_ref = material_alpha;
    material_env->bind_color_state = material_color_state;
    material_env->bind_multisample_state = material_msaa;
}

static void test_type0_draw(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t record = RECORDS;
    const Event *draw;

    setup_world(0);
    make_envs(&env, &material_env);
    setup_material();
    setup_target(TARGET, 0u, 0u, 0u, 0u);
    setup_pipeline(PIPELINE);
    setup_record(record, 0u);
    write_u32(record + 0x4u, TARGET);
    write_u32(record + 0x8u, PIPELINE);
    write_u32(record + 0xcu, MATERIAL);
    write_u32(record + 0x10u, 0u);
    write_u32(record + 0x14u, 7u);
    write_u32(record + 0x18u, 3u);
    add_node(NODES, 0u, record);
    set_root(1u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "type0 run failed");
    draw = find_event(VP_SLOT_DRAW_ARRAYS, 0);
    CHECK(draw != NULL, "type0 draw missing");
    CHECK(draw->site == 0xbc014cu, "type0 draw site");
    CHECK(draw->r[0] == CMD && draw->r[1] == 0u &&
          draw->r[2] == 3u && draw->r[3] == 7u, "type0 draw args");
    CHECK(read_u32(STATE + 0x568u) == 0u, "type0 root range start unchanged");
    CHECK(read_u32(CTX + 0x340f4u) == 1u, "type0 resource update counter");
    CHECK(read_u32(STATE + 0x8u) == TARGET, "last target writeback");
    CHECK(read_u32(STATE + 0xcu) == PIPELINE, "last pipeline writeback");
    CHECK(read_u32(STATE + 0x10u) == MATERIAL, "last material writeback");
    CHECK(world.memclr_count == 2u, "prologue memclr count");
    CHECK(world.events[0].slot == VP_SLOT_SET_PRIMITIVE_RESTART,
          "initial SetPrimitiveRestart");
    CHECK(world.events[0].r[1] == 1u, "initial restart flag");
    CHECK(world.events[0].r[2] == 0xffffu, "initial restart index");
    CHECK(world.events[1].slot == VP_SLOT_SET_LINE_WIDTH,
          "initial SetLineWidth");
    CHECK(world.events[1].s[0] == 0x3f800000u, "line width 1.0");
    CHECK(world.events[2].slot == VP_SLOT_SET_POINT_SIZE,
          "initial SetPointSize");
    CHECK(world.events[3].slot == VP_SLOT_SET_ALPHA_REF,
          "initial SetAlphaRef");
    CHECK(world.events[3].s[0] == 0u, "initial alpha ref 0.0");
    teardown_world();
}

static void test_streams_and_64bit(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t record = RECORDS;
    uint32_t vs = VERTEX_STATE;
    uint32_t object = 0x16000u;
    const Event *query, *bind, *draw;

    setup_world(0);
    make_envs(&env, &material_env);
    setup_material();
    setup_target(TARGET, 0u, 0u, 0u, 0u);
    setup_pipeline(PIPELINE);
    setup_record(record, 0u);
    write_u32(record + 0x4u, TARGET);
    write_u32(record + 0x8u, PIPELINE);
    write_u32(record + 0xcu, MATERIAL);
    write_u32(record + 0x10u, vs);
    write_u32(record + 0x14u, 2u);
    write_u32(record + 0x18u, 6u);

    write_u32(vs + 0x0u, object);
    write_u32(vs + 0x10u, 4u);         /* stride */
    write_u32(vs + 0x14u, 0x100u);     /* start */
    write_u32(object + 0x14u, 0x120u); /* end */
    write_u32(object + 0x20u, 0x7000u);/* buffer */
    add_node(NODES, 0u, record);
    set_root(1u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "stream run failed");
    query = find_event(VP_SLOT_BUFFER_GET_ADDRESS, 0);
    bind = find_event(VP_SLOT_BIND_VERTEX_BUFFER, 0);
    draw = find_event(VP_SLOT_DRAW_ARRAYS, 0);
    CHECK(query != NULL && bind != NULL && draw != NULL,
          "stream events missing");
    CHECK(query->r[0] == 0x7000u, "stream buffer object");
    CHECK(bind->r[0] == CMD && bind->r[1] == 0u, "stream bind command/index");
    CHECK(bind->r[2] == 0x55667888u, "stream address low (64-bit add)");
    CHECK(bind->r[3] == 0x11223344u, "stream address high (carry)");
    CHECK(bind->stack_words == 1u && bind->stack[0] == 0x20u,
          "stream size");
    CHECK(read_u32(STATE + 0x116cu) == 8u, "stream element count");
    CHECK(read_u32(STATE + 0x44u) == 0x7000u &&
          read_u32(STATE + 0x54u) == 4u &&
          read_u32(STATE + 0x64u) == 0x100u, "stream cache writeback");
    teardown_world();
}

static void setup_stream_state(uint32_t vs, uint32_t object)
{
    write_u32(vs + 0x0u, object);
    write_u32(vs + 0x10u, 4u);
    write_u32(vs + 0x14u, 0x100u);
    write_u32(object + 0x14u, 0x120u);
    write_u32(object + 0x20u, 0x7000u);
}

static void test_type2_instanced(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t record = RECORDS;
    uint32_t vs = VERTEX_STATE;
    const Event *bind, *draw;

    setup_world(0);
    make_envs(&env, &material_env);
    setup_material();
    setup_target(TARGET, 0u, 0u, 0u, 0u);
    setup_pipeline(PIPELINE);
    setup_stream_state(vs, 0x16000u);

    setup_record(record, 2u);
    write_u32(record + 0x4u, TARGET);
    write_u32(record + 0x8u, PIPELINE);
    write_u32(record + 0xcu, MATERIAL);
    write_u32(record + 0x10u, vs);
    write_u32(record + 0x14u, 5u);
    write_u32(record + 0x18u, 2u);
    write_u32(record + 0x1cu, 0x99u);
    write_u32(record + 0x20u, 0x77u);
    add_node(NODES, 0u, record);
    set_root(1u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "type2 run failed");
    bind = find_event(VP_SLOT_BIND_VERTEX_BUFFER, 0);
    draw = find_event(VP_SLOT_DRAW_ARRAYS_INSTANCED, 0);
    CHECK(bind != NULL && draw != NULL, "type2 events missing");
    CHECK(bind->site == 0xbbef70u, "type2 bind site");
    CHECK(bind->r[0] == CMD, "type2 loads command from state+4 at 0xBBEEC0");
    CHECK(draw->site == 0xbbf734u, "type2 draw site");
    CHECK(draw->r[0] == CMD && draw->r[1] == 0u &&
          draw->r[2] == 2u && draw->r[3] == 5u, "type2 draw args");
    CHECK(draw->stack_words == 2u && draw->stack[0] == 0x77u &&
          draw->stack[1] == 0x99u, "type2 instance stack args");
    teardown_world();
}

static void test_type3_instanced(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t record = RECORDS;
    uint32_t vs = VERTEX_STATE;
    uint32_t holder = 0x17000u;
    const Event *bind, *query, *draw;

    setup_world(0);
    make_envs(&env, &material_env);
    setup_material();
    setup_target(TARGET, 0u, 0u, 0u, 0u);
    setup_pipeline(PIPELINE);
    setup_stream_state(vs, 0x16000u);
    write_u32(vs + 0x28u, holder);
    write_u32(holder + 0x20u, 0x7100u);

    setup_record(record, 3u);
    write_u32(record + 0x4u, TARGET);
    write_u32(record + 0x8u, PIPELINE);
    write_u32(record + 0xcu, MATERIAL);
    write_u32(record + 0x10u, vs);
    write_u32(record + 0x14u, 4u);
    write_u32(record + 0x18u, 3u);
    write_u32(record + 0x1cu, 0x99u);
    write_u32(record + 0x20u, 0x77u);
    write_u32(record + 0x24u, 0x1234u);
    add_node(NODES, 0u, record);
    set_root(1u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "type3 run failed");
    bind = find_event(VP_SLOT_BIND_VERTEX_BUFFER, 0);
    query = find_event(VP_SLOT_BUFFER_GET_ADDRESS, 1);
    draw = find_event(VP_SLOT_DRAW_ELEMENTS_INSTANCED, 0);
    CHECK(bind != NULL && query != NULL && draw != NULL,
          "type3 events missing");
    CHECK(bind->site == 0xbbf3c0u, "type3 bind site");
    CHECK(bind->r[0] == CMD, "type3 loads command from state+4 at 0xBBF310");
    CHECK(query->site == 0xbbf418u && query->r[0] == 0x7100u,
          "type3 query");
    CHECK(draw->site == 0xbbf734u, "type3 draw site");
    CHECK(draw->r[0] == CMD && draw->r[1] == 0u &&
          draw->r[2] == 1u && draw->r[3] == 4u, "type3 draw args");
    CHECK(draw->stack_words == 5u, "type3 stack word count");
    CHECK(draw->stack[0] == 0x5566778eu &&
          draw->stack[1] == 0x11223344u, "type3 64-bit index address");
    CHECK(draw->stack[2] == 0x99u && draw->stack[3] == 0x1234u &&
          draw->stack[4] == 0x77u, "type3 stack args");
    teardown_world();
}

static void test_pending_merge(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t r1 = RECORDS;
    uint32_t r2 = RECORDS + 0x100u;
    uint32_t term = RECORDS + 0x200u;
    uint32_t vs = VERTEX_STATE;
    uint32_t holder = 0x17000u;
    uint32_t buffer_object = 0x17100u;
    const Event *draw;

    setup_world(1u);
    make_envs(&env, &material_env);
    setup_material();
    setup_target(TARGET, 0u, 0u, 0u, 0u);
    setup_pipeline(PIPELINE);

    write_u32(vs + 0x28u, holder);
    write_u32(holder + 0x20u, buffer_object);

    setup_record(r1, 1u);
    write_u32(r1 + 0x4u, TARGET);
    write_u32(r1 + 0x8u, PIPELINE);
    write_u32(r1 + 0xcu, MATERIAL);
    write_u32(r1 + 0x10u, vs);
    write_u32(r1 + 0x14u, 100u);
    write_u32(r1 + 0x18u, 10u);
    write_u32(r1 + 0x1cu, 0x1234u);

    setup_record(r2, 1u);
    write_u32(r2 + 0x4u, TARGET);
    write_u32(r2 + 0x8u, PIPELINE);
    write_u32(r2 + 0xcu, MATERIAL);
    write_u32(r2 + 0x10u, vs);
    write_u32(r2 + 0x14u, 200u);
    write_u32(r2 + 0x18u, 111u);
    write_u32(r2 + 0x1cu, 0x1234u);

    setup_record(term, 15u);
    add_node(NODES, 0u, r1);
    add_node(NODES, 1u, r2);
    add_node(NODES, 2u, term);
    set_root(3u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "merge run failed");
    CHECK(count_events(VP_SLOT_DRAW_ARRAYS) == 0u, "merge produced DrawArrays");
    CHECK(count_events(VP_SLOT_DRAW_ELEMENTS_BASE_VERTEX) == 1u,
          "merge flush count");
    draw = find_event(VP_SLOT_DRAW_ELEMENTS_BASE_VERTEX, 0);
    CHECK(draw->site == 0xbc039cu, "merge flush site");
    CHECK(draw->r[0] == CMD && draw->r[1] == 0u && draw->r[2] == 1u,
          "merge flush leading args");
    CHECK(draw->r[3] == 301u, "merge flush count arithmetic");
    CHECK(draw->stack[0] == 0x5566779cu &&
          draw->stack[1] == 0x11223344u,
          "merge flush 64-bit index address");
    CHECK(draw->stack[2] == 0x1234u, "merge flush vertex offset");
    CHECK(read_u32(CTX + 0x340f4u) == 1u, "pending update counter");
    CHECK(read_u32(STATE + 0x568u) == 0u, "merge flush draw counter");
    teardown_world();
}

static void test_nested_range(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t push = RECORDS;
    uint32_t nested = RECORDS + 0x100u;
    uint32_t term = RECORDS + 0x200u;
    const Event *draw;

    setup_world(0);
    make_envs(&env, &material_env);
    setup_material();
    setup_target(TARGET, 0u, 0u, 0u, 0u);
    setup_pipeline(PIPELINE);

    setup_record(nested, 0u);
    write_u32(nested + 0x4u, TARGET);
    write_u32(nested + 0x8u, PIPELINE);
    write_u32(nested + 0xcu, MATERIAL);
    write_u32(nested + 0x10u, 0u);
    write_u32(nested + 0x14u, 1u);
    write_u32(nested + 0x18u, 1u);

    setup_record(push, 6u);
    write_u32(push + 0x8u, NESTED_NODES);
    write_u32(push + 0xcu, 1u);
    setup_record(term, 15u);

    add_node(NESTED_NODES, 0u, nested);
    add_node(NODES, 0u, push);
    add_node(NODES, 1u, term);
    set_root(2u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "nested run failed");
    draw = find_event(VP_SLOT_DRAW_ARRAYS, 0);
    CHECK(draw != NULL, "nested draw missing");
    CHECK(read_u32(STATE + 0x568u) == 0u, "nested root range start unchanged");
    CHECK(read_u32(CTX + 0x340f4u) == 1u, "nested resource update counter");
    CHECK(read_u32(STATE + 0x1168u) == 0u, "range count restored");
    teardown_world();
}

static void test_type4_clear(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t record = RECORDS;
    uint32_t sub = RECORDS + 0x100u;
    uint32_t target = TARGET;
    const Event *clear;

    setup_world(0);
    make_envs(&env, &material_env);
    setup_material();
    setup_target(target, 1u, 320u, 240u, 0u);
    setup_pipeline(PIPELINE);

    setup_record(record, 4u);
    write_u32(record + 0x4u, 1u);       /* color target 0 */
    write_u32(record + 0x8u, 0x3f800000u); /* depth clear value 1.0 */
    write_u32(record + 0x10u, 0x11111111u);
    write_u32(record + 0x14u, 0x22222222u);
    write_u32(record + 0x18u, 0x33333333u);
    write_u32(record + 0x1cu, 0x44444444u);
    write_u32(record + 0x20u, sub);
    write_u32(sub + 0x4u, target);
    write_u32(sub + 0x8u, PIPELINE);

    add_node(NODES, 0u, record);
    set_root(1u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "type4 clear run failed");
    clear = find_event(VP_SLOT_CLEAR_COLOR, 0);
    CHECK(clear != NULL, "type4 clear missing");
    CHECK(clear->site == 0xbbf9e0u, "type4 clear site");
    CHECK(clear->r[0] == CMD && clear->r[1] == 0u && clear->r[3] == 0x0fu,
          "type4 clear args");
    CHECK(clear->r[2] == SCRATCH + 0x50u, "type4 clear color pointer");
    CHECK(read_u32(SCRATCH + 0x50u) == 0x11111111u &&
          read_u32(SCRATCH + 0x5cu) == 0x44444444u,
          "type4 clear color contents");
    CHECK(count_events(VP_SLOT_DRAW_ARRAYS) == 0u, "type4 clear drew");
    CHECK(read_u32(STATE + 0x568u) == 0u, "type4 clear draw counter");
    teardown_world();
}

static void test_type4_srgb(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t record = RECORDS;
    uint32_t sub = RECORDS + 0x100u;
    const Event *clear;
    float input1 = 0.01f;
    float expected_linear = (float)((double)input1 / 12.92);
    uint32_t expected_linear_bits;
    uint32_t expected_gamma_bits = 0x3e800000u; /* 0.25f from world_pow */
    double expected_base0 = (0.5 + 0.055) / 1.055;
    double expected_base1 = (1.0 + 0.055) / 1.055;

    memcpy(&expected_linear_bits, &expected_linear,
           sizeof(expected_linear_bits));

    setup_world(0);
    make_envs(&env, &material_env);
    setup_material();
    write_u32(SRGB_GLOBAL, 0x700u);
    write_u32(0x700u + 0x29u, 1u);
    setup_target(TARGET, 1u, 320u, 240u, 0u);
    setup_pipeline(PIPELINE);

    setup_record(record, 4u);
    write_u32(record + 0x4u, 1u);
    write_u32(record + 0x8u, 0u);
    write_u32(record + 0x10u, 0x3f000000u); /* 0.5 */
    write_u32(record + 0x14u, 0x3c23d70au); /* 0.01 */
    write_u32(record + 0x18u, 0x3f800000u); /* 1.0 */
    write_u32(record + 0x1cu, 0x44444444u);
    write_u32(record + 0x20u, sub);
    write_u32(sub + 0x4u, TARGET);
    write_u32(sub + 0x8u, PIPELINE);
    add_node(NODES, 0u, record);
    set_root(1u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "type4 srgb run failed");
    clear = find_event(VP_SLOT_CLEAR_COLOR, 0);
    CHECK(clear != NULL, "type4 srgb clear missing");
    CHECK(world.pow_count == 2u, "type4 srgb pow count");
    CHECK(world.pow_sites[0] == 0xbbf50cu &&
          world.pow_sites[1] == 0xbbf98cu, "type4 srgb pow sites");
    CHECK(fabs(world.pow_bases[0] - expected_base0) < 1e-15,
          "type4 srgb first base");
    CHECK(fabs(world.pow_bases[1] - expected_base1) < 1e-15,
          "type4 srgb second base");
    CHECK(world.pow_exponents[0] == 2.4 && world.pow_exponents[1] == 2.4,
          "type4 srgb exponent");
    CHECK(read_u32(SCRATCH + 0x50u) == expected_gamma_bits &&
          read_u32(SCRATCH + 0x54u) == expected_linear_bits &&
          read_u32(SCRATCH + 0x58u) == expected_gamma_bits &&
          read_u32(SCRATCH + 0x5cu) == 0x44444444u,
          "type4 srgb converted colors");
    teardown_world();
}

static void test_type7_external(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t r1 = RECORDS;
    uint32_t r2 = RECORDS + 0x100u;

    setup_world(0);
    make_envs(&env, &material_env);

    setup_record(r1, 7u);
    write_u32(r1 + 0x4u, 1u);
    write_u32(r1 + 0x8u, 0x111u);
    setup_record(r2, 7u);
    write_u32(r2 + 0x4u, 0u);
    write_u32(r2 + 0x8u, 0x222u);
    add_node(NODES, 0u, r1);
    add_node(NODES, 1u, r2);
    set_root(2u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "type7 run failed");
    CHECK(world.external_count == 2u, "type7 external count");
    CHECK(world.last_external_site == 0xbbf86cu, "type7 second site");
    CHECK(world.last_external_arg0 == 0x222u &&
          world.last_external_arg1 == CMD, "type7 second args");
    teardown_world();
}

static void test_type14_copy(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t record = RECORDS;
    uint32_t object = AUX_OBJECT;
    uint32_t descriptor = 0x41000u;
    const Event *query, *copy;

    setup_world(0);
    make_envs(&env, &material_env);
    setup_material();

    setup_record(record, 14u);
    write_u32(record + 0x4u, object);
    write_u32(record + 0x8u, 0x42000u);
    write_u32(object + 0x10u, descriptor);
    write_u16(object + 0x1eu, 0x00abu); /* high channel */
    write_u16(object + 0x20u, 0x00cdu); /* low channel */
    write_u32(object + 0x23u, 0u);
    write_u32(object + 0x3cu, 0x43000u);
    write_u32(descriptor + 0x10u, 0u);
    add_node(NODES, 0u, record);
    set_root(1u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "type14 run failed");
    query = find_event(VP_SLOT_BUFFER_GET_ADDRESS, 0);
    copy = find_event(VP_SLOT_COPY_TEXTURE_TO_BUFFER, 0);
    CHECK(query != NULL && copy != NULL, "type14 events missing");
    CHECK(query->site == 0xbbf904u && query->r[0] == 0x42000u,
          "type14 query");
    CHECK(copy->site == 0xbbf924u, "type14 copy site");
    CHECK(copy->r[0] == CMD && copy->r[1] == 0x43000u &&
          copy->r[2] == 0u && copy->r[3] == SCRATCH + 0x50u,
          "type14 copy args");
    CHECK(copy->stack[0] == 0x55667788u &&
          copy->stack[1] == 0x11223344u && copy->stack[2] == 0u,
          "type14 copy 64-bit address");
    CHECK(read_u32(SCRATCH + 0x50u) == 0u &&
          read_u32(SCRATCH + 0x5cu) == 0x00abu &&
          read_u32(SCRATCH + 0x60u) == 0x00cdu &&
          read_u32(SCRATCH + 0x64u) == 1u,
          "type14 color descriptor");
    teardown_world();
}

static void test_type5_downsample(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t record = RECORDS;
    uint32_t array = 0x18000u;
    uint32_t chosen = 0x19000u;
    uint32_t descriptor_a = 0x1a000u;
    uint32_t descriptor_b = 0x1b000u;
    const Event *down, *discard;

    setup_world(0);
    make_envs(&env, &material_env);
    setup_material();

    /* deref chains resolve to a zero current object so the cached target stays
     * zero and is used as descriptor_a. */
    write_u32(CTX + 0x389c0u, 0x1c000u);
    write_u32(0x1c000u + 0x204u, 0x1c100u);
    write_u32(0x1c100u, 0u);
    write_u32(CTX + 0x389ccu, 0x1c200u);
    write_u32(0x1c200u + 0x204u, 0x1c300u);
    write_u32(0x1c300u, 0u);
    write_u32(0x10u, descriptor_a); /* cached target object is guest 0 */
    write_u32(descriptor_a + 0x10u, 0u);
    write_u32(descriptor_a + 0x24u, 0x4000u); /* format 9 */
    write_u32(descriptor_a + 0x3cu, 0x1d000u);

    write_u32(array + 0x204u, 0x1e000u);
    write_u32(0x1e000u + 4u, chosen);
    write_u32(chosen + 0x10u, descriptor_b);
    write_u32(chosen + 0x18u, 0u);
    write_u32(chosen + 0x24u, 0u);
    write_u32(descriptor_b + 0x3cu, 0x1f000u);

    setup_record(record, 5u);
    write_u32(record + 0x4u, array);
    write_u32(record + 0x8u, 1u);
    write_u32(record + 0xcu, 0u); /* subtype 0 */
    add_node(NODES, 0u, record);
    set_root(1u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "type5 downsample run failed");
    down = find_event(VP_SLOT_DOWNSAMPLE, 0);
    discard = find_event(VP_SLOT_DISCARD_COLOR, 0);
    CHECK(down != NULL && discard != NULL, "type5 downsample events missing");
    CHECK(down->site == 0xbbfb18u, "type5 downsample site");
    CHECK(down->r[0] == CMD && down->r[1] == 0x1d000u &&
          down->r[2] == 0x1f000u, "type5 downsample args");
    CHECK(discard->site == 0xbbfb6cu && discard->r[0] == CMD &&
          discard->r[1] == 0u, "type5 discard args");
    teardown_world();
}

static void setup_zero_current(void)
{
    write_u32(CTX + 0x389c0u, 0x1c000u);
    write_u32(0x1c000u + 0x204u, 0x1c100u);
    write_u32(0x1c100u, 0u);
    write_u32(CTX + 0x389ccu, 0x1c200u);
    write_u32(0x1c200u + 0x204u, 0x1c300u);
    write_u32(0x1c300u, 0u);
}

static void test_type5_subdraw(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t record = RECORDS;
    uint32_t array = 0x20000u;
    uint32_t chosen = 0x21000u;
    uint32_t descriptor_a = 0x22000u;
    uint32_t descriptor_b = 0x23000u;
    uint32_t sub = RECORDS + 0x280u;
    uint32_t target2 = 0x27000u;
    uint32_t pipeline2 = 0x28000u;
    const Event *draw;

    setup_world(0);
    make_envs(&env, &material_env);
    setup_material();
    setup_target(target2, 0u, 0u, 0u, 0u);
    setup_pipeline(pipeline2);
    setup_zero_current();

    write_u32(0x10u, descriptor_a); /* cached target object is guest 0 */
    write_u32(descriptor_a + 0x10u, 0u);
    write_u32(descriptor_a + 0x24u, 0u); /* format 1, no downsample */
    write_u32(descriptor_a + 0x3cu, 0u);

    write_u32(array + 0x204u, 0x24000u);
    write_u32(0x24000u + 4u, chosen);
    write_u32(chosen + 0x10u, descriptor_b);
    write_u32(chosen + 0x18u, 0u);
    write_u32(chosen + 0x24u, 0u);
    write_u32(descriptor_b + 0x3cu, 0u);

    setup_record(sub, 0u);
    write_u32(sub + 0x4u, target2);
    write_u32(sub + 0x8u, pipeline2);
    write_u32(sub + 0xcu, MATERIAL);
    write_u32(sub + 0x10u, 0u);
    write_u32(sub + 0x14u, 3u);
    write_u32(sub + 0x18u, 7u);

    setup_record(record, 5u);
    write_u32(record + 0x4u, array);
    write_u32(record + 0x8u, 1u);
    write_u32(record + 0x28u, sub);
    add_node(NODES, 0u, record);
    set_root(1u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "type5 subdraw run failed");
    draw = find_event(VP_SLOT_DRAW_ARRAYS, 0);
    CHECK(draw != NULL, "type5 subdraw missing");
    CHECK(draw->site == 0xbc014cu && draw->r[0] == CMD &&
          draw->r[1] == 0u && draw->r[2] == 7u && draw->r[3] == 3u,
          "type5 subdraw args");
    CHECK(read_u32(STATE + 0x8u) == target2, "type5 sub target writeback");
    CHECK(read_u32(STATE + 0xcu) == pipeline2,
          "type5 sub pipeline writeback");
    CHECK(read_u32(STATE + 0x10u) == MATERIAL,
          "type5 sub material writeback");
    CHECK(read_u32(STATE + 0x568u) == 0u, "type5 root range start unchanged");
    CHECK(read_u32(CTX + 0x340f4u) == 1u, "type5 resource update counter");
    teardown_world();
}

static void test_type5_barrier_flag(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t merge = RECORDS;
    uint32_t term = RECORDS + 0x100u;
    uint32_t record = RECORDS + 0x200u;
    uint32_t array = 0x20000u;
    uint32_t chosen = 0x21000u;
    uint32_t descriptor_a = 0x22000u;
    uint32_t descriptor_b = 0x23000u;
    uint32_t holder = 0x17000u;
    const Event *barrier, *down;

    setup_world(1u);
    make_envs(&env, &material_env);
    setup_material();
    setup_target(TARGET, 0u, 0u, 0u, 0u);
    setup_pipeline(PIPELINE);
    setup_zero_current();
    write_u32(0x28u, holder);
    write_u32(holder + 0x20u, 0x17100u);

    setup_record(merge, 1u);
    write_u32(merge + 0x4u, TARGET);
    write_u32(merge + 0x8u, PIPELINE);
    write_u32(merge + 0xcu, MATERIAL);
    write_u32(merge + 0x10u, 0u);
    write_u32(merge + 0x14u, 0u);
    write_u32(merge + 0x18u, 0u);
    setup_record(term, 15u);

    write_u32(0x10u, descriptor_a);
    write_u32(descriptor_a + 0x10u, 2u);
    write_u32(descriptor_a + 0x23u, 2u);
    write_u32(descriptor_a + 0x24u, 0x4000u);
    write_u32(descriptor_a + 0x3cu, 0x1d000u);
    write_u32(array + 0x204u, 0x24000u);
    write_u32(0x24000u + 4u, chosen);
    write_u32(chosen + 0x10u, descriptor_b);
    write_u32(chosen + 0x18u, 0u);
    write_u32(chosen + 0x24u, 0u);
    write_u32(descriptor_b + 0x3cu, 0x1f000u);

    setup_record(record, 5u);
    write_u32(record + 0x4u, array);
    write_u32(record + 0x8u, 1u);
    add_node(NODES, 0u, merge);
    add_node(NODES, 1u, term);
    add_node(NODES, 2u, record);
    set_root(3u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "type5 barrier run failed");
    CHECK(read_u32(CTX + 0x340f4u) == 1u, "type5 barrier update counter");
    barrier = find_event(VP_SLOT_BARRIER, 0);
    down = find_event(VP_SLOT_DOWNSAMPLE, 0);
    CHECK(barrier != NULL && down != NULL, "type5 barrier events missing");
    CHECK(barrier->site == 0xbbf190u && barrier->r[0] == CMD &&
          barrier->r[1] == 0x16u, "type5 barrier args");
    CHECK(read_u32(CTX + 0x340f0u) == 1u, "type5 barrier sync writeback");
    CHECK(down->site == 0xbbfb18u && down->r[1] == 0x1d000u &&
          down->r[2] == 0x1f000u, "type5 post-barrier downsample");
    teardown_world();
}

static void test_type5_copy_to_texture(void)
{
    VPEngineNvnEnv env;
    VPEngineMaterialEnv material_env;
    uint32_t record = RECORDS;
    uint32_t array = 0x20000u;
    uint32_t chosen = 0x21000u;
    uint32_t descriptor_b = 0x22000u;
    uint32_t aux_descriptor = 0x23000u;
    const Event *copy;

    setup_world(0);
    make_envs(&env, &material_env);
    setup_material();

    write_u32(0x10u, aux_descriptor); /* state+0xA4 is zero */
    write_u32(aux_descriptor + 0x10u, 0u);
    write_u16(aux_descriptor + 0x1eu, 0x0011u);
    write_u16(aux_descriptor + 0x20u, 0x0022u);
    write_u32(aux_descriptor + 0x24u, 0u);
    write_u32(aux_descriptor + 0x3cu, 0x24000u);

    write_u32(array + 0x208u, 0x25000u);
    write_u32(0x25000u, chosen);
    write_u32(chosen + 0x10u, descriptor_b);
    write_u32(chosen + 0x18u, 0u);
    write_u32(chosen + 0x24u, 0u);
    write_u16(descriptor_b + 0x1eu, 0x0033u);
    write_u16(descriptor_b + 0x20u, 0x0044u);
    write_u32(descriptor_b + 0x3cu, 0x26000u);

    setup_record(record, 5u);
    write_u32(record + 0x4u, array);
    write_u32(record + 0x8u, 0u);
    write_u32(record + 0xcu, 4u); /* subtype 4 */
    add_node(NODES, 0u, record);
    set_root(1u);

    CHECK(vp_engine_queue_native_run(&env, &material_env, CTX, STATE) ==
              VP_ENGINE_NATIVE_OK, "type5 copy run failed");
    copy = find_event(VP_SLOT_COPY_TEXTURE_TO_TEXTURE, 0);
    CHECK(copy != NULL, "type5 copy missing");
    CHECK(copy->site == 0xbc018cu, "type5 copy site");
    CHECK(copy->r[0] == CMD && copy->r[1] == 0x24000u &&
          copy->r[2] == 0u && copy->r[3] == SCRATCH + 0x50u,
          "type5 copy leading args");
    CHECK(copy->stack_words == 4u &&
          copy->stack[0] == 0x26000u && copy->stack[1] == 0u &&
          copy->stack[2] == SCRATCH + 0x68u && copy->stack[3] == 0u,
          "type5 copy stack args");
    CHECK(read_u32(SCRATCH + 0x50u) == 0u &&
          read_u32(SCRATCH + 0x5cu) == 0x0011u &&
          read_u32(SCRATCH + 0x60u) == 0x0022u &&
          read_u32(SCRATCH + 0x68u) == 0u &&
          read_u32(SCRATCH + 0x74u) == 0x0033u &&
          read_u32(SCRATCH + 0x78u) == 0x0044u,
          "type5 two color descriptors");
    teardown_world();
}

int main(void)
{
    test_type0_draw();
    test_streams_and_64bit();
    test_type2_instanced();
    test_type3_instanced();
    test_pending_merge();
    test_nested_range();
    test_type4_clear();
    test_type4_srgb();
    test_type7_external();
    test_type14_copy();
    test_type5_downsample();
    test_type5_subdraw();
    test_type5_barrier_flag();
    test_type5_copy_to_texture();
    printf("engine_queue_native: all checks passed\n");
    return 0;
}

/* Host regression for the native reconstruction of main+0xBC1764.
 *
 * The test supplies a synthetic 32-bit guest address space and records every
 * typed sink.  Expectations come from the locked disassembly: record layout,
 * group order, cache offsets, LRU/pending-counter behaviour and 64-bit GPU
 * address arithmetic.  No game bytes are embedded. */

#include "engine_material.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARENA_BASE 0x10000000u
#define ARENA_SIZE 0x00100000u

#define ADDR_STATE 0x10001000u
#define ADDR_CTX 0x10003000u
#define ADDR_MAT0 0x10040000u
#define ADDR_MAT1 0x10041000u
#define ADDR_MAT2 0x10042000u
#define ADDR_LAYOUT 0x10043000u
#define ADDR_PROGRAM_META 0x10044000u
#define ADDR_DESC 0x10045000u
#define ADDR_LAYOUT_DESC 0x10046000u
#define ADDR_TEXTURE_A 0x10047000u
#define ADDR_SAMPLER_A 0x10047100u
#define ADDR_TEXTURE_B 0x10047200u
#define ADDR_SAMPLER_B 0x10047300u
#define ADDR_COLOR_STATE 0x10047400u
#define ADDR_COLOR_TARGET 0x10047500u
#define ADDR_DEPTH_TARGET 0x10047580u
#define ADDR_GPU_ROOT 0x10047600u
#define ADDR_WRAPPER 0x10047700u
#define ADDR_BUFFER_DATA 0x10048000u

#define CMD 0x30000000u
#define DEVICE_OBJECT 0x40000000u
#define NV_BUFFER 0x20000000u
#define BUFFER_ADDRESS 0x0000000280000000ull
#define TEXTURE_HANDLE_A 0x1122334455667788ull
#define TEXTURE_HANDLE_B 0x8877665544332211ull

static uint8_t arena[ARENA_SIZE];
static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void *arena_resolve(void *opaque, uint32_t address)
{
    (void)opaque;
    if (address < ARENA_BASE || address >= ARENA_BASE + ARENA_SIZE)
        return NULL;
    return arena + (address - ARENA_BASE);
}

static void put_u32(uint32_t address, uint32_t value)
{
    memcpy(arena_resolve(NULL, address), &value, sizeof(value));
}

static uint32_t get_u32(uint32_t address)
{
    uint32_t value;
    memcpy(&value, arena_resolve(NULL, address), sizeof(value));
    return value;
}

static void put_u8(uint32_t address, uint8_t value)
{
    memcpy(arena_resolve(NULL, address), &value, sizeof(value));
}

typedef enum {
    EV_PROGRAM,
    EV_ATTRIB,
    EV_STREAM,
    EV_BUFFER_ADDRESS,
    EV_UNIFORM,
    EV_TEXTURE_HANDLE,
    EV_TEXTURE,
    EV_BARRIER,
    EV_ALPHA,
    EV_COLOR_STATE,
    EV_MSAA
} EventKind;

typedef struct {
    EventKind kind;
    uint32_t a, b, c, e;
    uint64_t d;
    float f;
} Event;

static Event events[128];
static unsigned event_count;
static unsigned texture_queries;

static void push_event(EventKind kind, uint32_t a, uint32_t b, uint32_t c,
                       uint32_t e, uint64_t d, float f)
{
    if (event_count >= sizeof(events) / sizeof(events[0])) {
        fprintf(stderr, "event overflow\n");
        failures++;
        return;
    }
    events[event_count].kind = kind;
    events[event_count].a = a;
    events[event_count].b = b;
    events[event_count].c = c;
    events[event_count].e = e;
    events[event_count].d = d;
    events[event_count].f = f;
    event_count++;
}

static void sink_program(void *opaque, uint32_t site, uint32_t cmd, uint32_t program,
                         uint32_t flags_mask)
{
    (void)opaque; (void)site;
    push_event(EV_PROGRAM, cmd, program, flags_mask, 0, 0, 0.0f);
}

static void sink_attrib(void *opaque, uint32_t site, uint32_t cmd, unsigned count,
                        uint32_t desc)
{
    (void)opaque; (void)site;
    push_event(EV_ATTRIB, cmd, count, desc, 0, 0, 0.0f);
}

static void sink_stream(void *opaque, uint32_t site, uint32_t cmd, unsigned count,
                        uint32_t desc)
{
    (void)opaque; (void)site;
    push_event(EV_STREAM, cmd, count, desc, 0, 0, 0.0f);
}

static uint64_t sink_buffer_address(void *opaque, uint32_t site, uint32_t buffer)
{
    (void)opaque; (void)site;
    push_event(EV_BUFFER_ADDRESS, buffer, 0, 0, 0, 0, 0.0f);
    return BUFFER_ADDRESS;
}

static void sink_uniform(void *opaque, uint32_t site, uint32_t cmd, unsigned stage,
                         unsigned index, uint64_t address, uint32_t size)
{
    (void)opaque; (void)site;
    push_event(EV_UNIFORM, cmd, stage, index, size, address, 0.0f);
}

static uint64_t sink_texture_handle(void *opaque, uint32_t site, uint32_t device,
                                    uint32_t texture_id, uint32_t sampler_id)
{
    (void)opaque; (void)site;
    push_event(EV_TEXTURE_HANDLE, device, texture_id, sampler_id, 0, 0, 0.0f);
    return texture_queries++ == 0 ? TEXTURE_HANDLE_A : TEXTURE_HANDLE_B;
}

static void sink_texture(void *opaque, uint32_t site, uint32_t cmd, unsigned stage,
                         unsigned index, uint64_t handle)
{
    (void)opaque; (void)site;
    push_event(EV_TEXTURE, cmd, stage, index, 0, handle, 0.0f);
}

static void sink_barrier(void *opaque, uint32_t site, uint32_t cmd, uint32_t bits)
{
    (void)opaque; (void)site;
    push_event(EV_BARRIER, cmd, bits, 0, 0, 0, 0.0f);
}

static void sink_alpha(void *opaque, uint32_t site, uint32_t cmd, float ref)
{
    (void)opaque; (void)site;
    push_event(EV_ALPHA, cmd, 0, 0, 0, 0, ref);
}

static void sink_color_state(void *opaque, uint32_t site, uint32_t cmd, uint32_t state)
{
    (void)opaque; (void)site;
    push_event(EV_COLOR_STATE, cmd, state, 0, 0, 0, 0.0f);
}

static void sink_msaa(void *opaque, uint32_t site, uint32_t cmd, uint32_t state)
{
    (void)opaque; (void)site;
    push_event(EV_MSAA, cmd, state, 0, 0, 0, 0.0f);
}

static VPEngineMaterialEnv make_env(void)
{
    VPEngineMaterialEnv env;
    memset(&env, 0, sizeof(env));
    env.resolve = arena_resolve;
    env.gpu_root = ADDR_GPU_ROOT;
    env.bind_program = sink_program;
    env.bind_vertex_attrib_state = sink_attrib;
    env.bind_vertex_stream_state = sink_stream;
    env.buffer_get_address = sink_buffer_address;
    env.bind_uniform_buffer = sink_uniform;
    env.device_get_texture_handle = sink_texture_handle;
    env.bind_texture = sink_texture;
    env.barrier = sink_barrier;
    env.set_alpha_ref = sink_alpha;
    env.bind_color_state = sink_color_state;
    env.bind_multisample_state = sink_msaa;
    return env;
}

static void reset_arena(void)
{
    memset(arena, 0, sizeof(arena));
    memset(events, 0, sizeof(events));
    event_count = 0;
    texture_queries = 0;
}

static void setup_common(void)
{
    VPEngineLayout *layout;
    VPEngineSubmitState *state;

    memset(arena, 0, sizeof(arena));
    state = (VPEngineSubmitState *)arena_resolve(NULL, ADDR_STATE);
    memset(state, 0, sizeof(*state));
    state->command_buffer = CMD;
    state->color_state = ADDR_COLOR_STATE;
    state->color_target = ADDR_COLOR_TARGET;
    state->depth_target = ADDR_DEPTH_TARGET;

    layout = (VPEngineLayout *)arena_resolve(NULL, ADDR_LAYOUT);
    memset(layout, 0, sizeof(*layout));
    layout->descriptors = ADDR_DESC;
    /* N0=3 uniforms stage 0 (one record is skipped by w1 bits 1..15),
     * N1=1 texture A, N2=1 uniform stage 1, N3=1 texture B. */
    layout->groups0 = (3u << 22) | (1u << 12);
    layout->groups1 = (1u << 22) | (1u << 12);
    layout->program_meta = ADDR_PROGRAM_META;
    layout->layout_desc = ADDR_LAYOUT_DESC;
    put_u32(ADDR_PROGRAM_META + 0xc8u, 0x5a5a5a5au);

    put_u32(ADDR_DESC + 0x00u, (0x0040u << 16) | (10u << 6) | 3u);
    put_u32(ADDR_DESC + 0x04u, 0u);
    put_u32(ADDR_DESC + 0x08u, (11u << 6) | 4u);
    put_u32(ADDR_DESC + 0x0cu, 1u | (0x0070u << 16)); /* w1 size 0x70 */
    put_u32(ADDR_DESC + 0x10u, (0x0030u << 16) | (13u << 6) | 7u);
    put_u32(ADDR_DESC + 0x14u, 3u); /* bit0 set + other low bits -> skipped */
    put_u32(ADDR_DESC + 0x18u,
            (20u << 16) | (21u << 6) | 2u);
    put_u32(ADDR_DESC + 0x1cu, 0u);
    put_u32(ADDR_DESC + 0x20u, (0x0020u << 16) | (12u << 6) | 5u);
    put_u32(ADDR_DESC + 0x24u, 0u);
    put_u32(ADDR_DESC + 0x28u,
            (22u << 16) | (23u << 6) | 6u);
    put_u32(ADDR_DESC + 0x2cu, 0u);

    put_u32(ADDR_MAT0 + 0x00u, 0x001c0200u); /* bits 19|20, bit2 clear */
    put_u32(ADDR_MAT0 + 0x04u, 0u);
    put_u32(ADDR_MAT0 + 0x08u, ADDR_LAYOUT);
    put_u32(ADDR_MAT0 + 0x0cu + 10u * 4u, ADDR_BUFFER_DATA + 0x004u);
    put_u32(ADDR_MAT0 + 0x0cu + 11u * 4u, ADDR_BUFFER_DATA + 0x101u);
    put_u32(ADDR_MAT0 + 0x0cu + 12u * 4u, ADDR_BUFFER_DATA + 0x208u);
    put_u32(ADDR_MAT0 + 0x0cu + 13u * 4u, ADDR_BUFFER_DATA + 0x300u);
    put_u32(ADDR_MAT0 + 0x0cu + 20u * 4u, ADDR_TEXTURE_A);
    put_u32(ADDR_MAT0 + 0x0cu + 21u * 4u, ADDR_SAMPLER_A);
    put_u32(ADDR_MAT0 + 0x0cu + 22u * 4u, ADDR_TEXTURE_B);
    put_u32(ADDR_MAT0 + 0x0cu + 23u * 4u, ADDR_SAMPLER_B);

    put_u32(ADDR_TEXTURE_A + 0x10u, 2u);
    put_u8(ADDR_TEXTURE_A + 0x23u, 0x02u);
    put_u32(ADDR_TEXTURE_A + 0x34u, 0x1234u);
    put_u32(ADDR_SAMPLER_A + 0x30u, 0x5678u);
    put_u32(ADDR_TEXTURE_B + 0x10u, 5u);
    put_u8(ADDR_TEXTURE_B + 0x23u, 0x00u);
    put_u32(ADDR_TEXTURE_B + 0x34u, 0x2234u);
    put_u32(ADDR_SAMPLER_B + 0x30u, 0x6678u);

    put_u32(ADDR_GPU_ROOT + 0x14u, ADDR_WRAPPER);
    put_u32(ADDR_WRAPPER + 0x5b8u, NV_BUFFER);
    put_u32(ADDR_WRAPPER + 0x58u, ADDR_BUFFER_DATA + 0x500u);

    put_u32(ADDR_CTX + 0x98u, DEVICE_OBJECT);
    put_u32(ADDR_COLOR_TARGET + 0x20u, 3u << 8);
    put_u32(ADDR_DEPTH_TARGET + 0x20u, 2u << 8);
}

static Event *event_at(unsigned index)
{
    CHECK(index < event_count);
    return index < event_count ? &events[index] : &events[0];
}

static void expect_end(unsigned count)
{
    if (event_count != count) {
        fprintf(stderr, "FAIL event count: got %u expected %u\n",
                event_count, count);
        for (unsigned i = 0; i < event_count; i++)
            fprintf(stderr, "  [%u] kind=%d a=%08x b=%08x c=%08x e=%08x "
                    "d=%016llx\n", i, (int)events[i].kind, events[i].a,
                    events[i].b, events[i].c, events[i].e,
                    (unsigned long long)events[i].d);
        failures++;
    }
}

static void test_full_submission(void)
{
    VPEngineMaterialEnv env = make_env();
    VPEngineSubmitState *state;
    uint64_t expected_a, expected_b, expected_c;
    unsigned i = 0;
    Event *e;
    int rc;

    reset_arena();
    setup_common();
    state = (VPEngineSubmitState *)arena_resolve(NULL, ADDR_STATE);
    expected_a = 0x000000027ffffb00ull;
    expected_b = 0x000000027ffffc00ull;
    expected_c = 0x000000027ffffd00ull;

    rc = vp_engine_material_submit(&env, ADDR_CTX, ADDR_STATE, ADDR_MAT0);
    CHECK(rc == VP_ENGINE_MATERIAL_OK);

    e = event_at(i++); CHECK(e->kind == EV_PROGRAM);
    CHECK(e->a == CMD && e->b == ADDR_PROGRAM_META + 8u &&
          e->c == 0x5a5a5a5au);
    e = event_at(i++); CHECK(e->kind == EV_ATTRIB);
    CHECK(e->a == CMD && e->b == 16u && e->c == ADDR_LAYOUT_DESC);
    e = event_at(i++); CHECK(e->kind == EV_STREAM);
    CHECK(e->a == CMD && e->b == 4u && e->c == ADDR_LAYOUT_DESC + 0x40u);

    e = event_at(i++); CHECK(e->kind == EV_BUFFER_ADDRESS);
    CHECK(e->a == NV_BUFFER);
    e = event_at(i++); CHECK(e->kind == EV_UNIFORM);
    CHECK(e->a == CMD && e->b == 0u && e->c == 3u && e->e == 0x40u);
    CHECK(e->d == expected_a);
    e = event_at(i++); CHECK(e->kind == EV_BUFFER_ADDRESS);
    e = event_at(i++); CHECK(e->kind == EV_UNIFORM);
    CHECK(e->a == CMD && e->b == 0u && e->c == 4u && e->e == 0x70u);
    CHECK(e->d == expected_b);

    e = event_at(i++); CHECK(e->kind == EV_TEXTURE_HANDLE);
    CHECK(e->a == DEVICE_OBJECT + 8u && e->b == 0x1234u && e->c == 0x5678u);
    e = event_at(i++); CHECK(e->kind == EV_TEXTURE);
    CHECK(e->a == CMD && e->b == 0u && e->c == 2u);
    CHECK(e->d == TEXTURE_HANDLE_A);

    e = event_at(i++); CHECK(e->kind == EV_BUFFER_ADDRESS);
    e = event_at(i++); CHECK(e->kind == EV_UNIFORM);
    CHECK(e->a == CMD && e->b == 1u && e->c == 5u && e->e == 0x20u);
    CHECK(e->d == expected_c);

    e = event_at(i++); CHECK(e->kind == EV_TEXTURE_HANDLE);
    CHECK(e->a == DEVICE_OBJECT + 8u && e->b == 0x2234u && e->c == 0x6678u);
    e = event_at(i++); CHECK(e->kind == EV_TEXTURE);
    CHECK(e->a == CMD && e->b == 1u && e->c == 6u);
    CHECK(e->d == TEXTURE_HANDLE_B);

    e = event_at(i++); CHECK(e->kind == EV_ALPHA);
    CHECK(e->a == CMD);
    /* 0xBC22D8 reads material word 0 bits 11..18: 0x80 / 255. */
    CHECK(fabsf(e->f - (128.0f / 255.0f)) < 1e-7f);
    e = event_at(i++); CHECK(e->kind == EV_COLOR_STATE);
    CHECK(e->a == CMD && e->b == ADDR_COLOR_STATE + 0x20u + 64u * 4u);
    e = event_at(i++); CHECK(e->kind == EV_MSAA);
    CHECK(e->a == CMD && e->b == ADDR_CTX + 0x389d8u + 2u * 24u);
    expect_end(i);

    CHECK(state->last_material == ADDR_MAT0);
    CHECK(state->flags == 0x2008100u);
    CHECK(state->sampler_cache_a[0] == ADDR_SAMPLER_A);
    CHECK(state->texture_cache_a[0] == ADDR_TEXTURE_A);
    CHECK(state->sampler_cache_b[0] == ADDR_SAMPLER_B);
    CHECK(state->texture_cache_b[0] == ADDR_TEXTURE_B);
    CHECK(state->multisample_cache == 0x10u);
}

static void test_same_material_tail_only(void)
{
    VPEngineMaterialEnv env = make_env();
    VPEngineSubmitState *state;
    int rc;

    reset_arena();
    setup_common();
    state = (VPEngineSubmitState *)arena_resolve(NULL, ADDR_STATE);
    state->last_material = ADDR_MAT0;
    state->flags = 0x8100u;
    state->multisample_cache = 0x10u;

    rc = vp_engine_material_submit(&env, ADDR_CTX, ADDR_STATE, ADDR_MAT0);
    CHECK(rc == VP_ENGINE_MATERIAL_OK);
    expect_end(1);
    CHECK(events[0].kind == EV_COLOR_STATE);
    CHECK(events[0].b == ADDR_COLOR_STATE + 0x20u + 64u * 4u);
    CHECK(state->flags == 0x2008100u);
    CHECK(state->multisample_cache == 0x10u);
}

static void test_same_layout_resource_diff_and_force(void)
{
    VPEngineMaterialEnv env = make_env();
    VPEngineSubmitState *state;
    int rc;

    reset_arena();
    setup_common();
    state = (VPEngineSubmitState *)arena_resolve(NULL, ADDR_STATE);
    /* Seed the state as if ADDR_MAT0 had just been submitted. */
    state->last_material = ADDR_MAT0;
    state->flags = 0x2008100u;
    state->sampler_cache_a[0] = ADDR_SAMPLER_A;
    state->texture_cache_a[0] = ADDR_TEXTURE_A;
    state->sampler_cache_b[0] = ADDR_SAMPLER_B;
    state->texture_cache_b[0] = ADDR_TEXTURE_B;
    state->multisample_cache = 0x10u;

    /* MAT1: same layout, only uniform resource 10 changed.  Flags clear bit2,
     * so the previous material array is a valid comparison source. */
    put_u32(ADDR_MAT1 + 0x00u, 0u);
    put_u32(ADDR_MAT1 + 0x04u, 0u);
    put_u32(ADDR_MAT1 + 0x08u, ADDR_LAYOUT);
    put_u32(ADDR_MAT1 + 0x0cu + 10u * 4u, ADDR_BUFFER_DATA + 0x404u);
    put_u32(ADDR_MAT1 + 0x0cu + 11u * 4u, ADDR_BUFFER_DATA + 0x101u);
    put_u32(ADDR_MAT1 + 0x0cu + 12u * 4u, ADDR_BUFFER_DATA + 0x208u);
    put_u32(ADDR_MAT1 + 0x0cu + 20u * 4u, ADDR_TEXTURE_A);
    put_u32(ADDR_MAT1 + 0x0cu + 21u * 4u, ADDR_SAMPLER_A);
    put_u32(ADDR_MAT1 + 0x0cu + 22u * 4u, ADDR_TEXTURE_B);
    put_u32(ADDR_MAT1 + 0x0cu + 23u * 4u, ADDR_SAMPLER_B);

    rc = vp_engine_material_submit(&env, ADDR_CTX, ADDR_STATE, ADDR_MAT1);
    CHECK(rc == VP_ENGINE_MATERIAL_OK);
    expect_end(3);
    CHECK(events[0].kind == EV_BUFFER_ADDRESS);
    CHECK(events[1].kind == EV_UNIFORM);
    CHECK(events[1].b == 0u && events[1].c == 3u);
    CHECK(events[1].d == 0x000000027fffff00ull); /* +0x400 aligned, base +0x500 */
    CHECK(events[2].kind == EV_COLOR_STATE);
    CHECK(state->last_material == ADDR_MAT1);
    CHECK(state->flags == 0x00000f00u);

    /* MAT2: same layout and same pointers, but bit2 forces every uniform
     * binding again. */
    event_count = 0;
    texture_queries = 0;
    put_u32(ADDR_MAT2 + 0x00u, 4u);
    put_u32(ADDR_MAT2 + 0x04u, 0u);
    put_u32(ADDR_MAT2 + 0x08u, ADDR_LAYOUT);
    put_u32(ADDR_MAT2 + 0x0cu + 10u * 4u, ADDR_BUFFER_DATA + 0x404u);
    put_u32(ADDR_MAT2 + 0x0cu + 11u * 4u, ADDR_BUFFER_DATA + 0x101u);
    put_u32(ADDR_MAT2 + 0x0cu + 12u * 4u, ADDR_BUFFER_DATA + 0x208u);
    put_u32(ADDR_MAT2 + 0x0cu + 20u * 4u, ADDR_TEXTURE_A);
    put_u32(ADDR_MAT2 + 0x0cu + 21u * 4u, ADDR_SAMPLER_A);
    put_u32(ADDR_MAT2 + 0x0cu + 22u * 4u, ADDR_TEXTURE_B);
    put_u32(ADDR_MAT2 + 0x0cu + 23u * 4u, ADDR_SAMPLER_B);

    rc = vp_engine_material_submit(&env, ADDR_CTX, ADDR_STATE, ADDR_MAT2);
    CHECK(rc == VP_ENGINE_MATERIAL_OK);
    /* Bit2 forces stage 0 only. Stage 1 reloads the saved old resource list
     * and skips its unchanged address, as confirmed against the A32 body. */
    expect_end(6);
    CHECK(events[0].kind == EV_BUFFER_ADDRESS);
    CHECK(events[1].kind == EV_UNIFORM && events[1].c == 3u);
    CHECK(events[2].kind == EV_BUFFER_ADDRESS);
    CHECK(events[3].kind == EV_UNIFORM && events[3].c == 4u);
    CHECK(events[4].kind == EV_COLOR_STATE);
    CHECK(events[5].kind == EV_MSAA);
    CHECK(events[5].b == ADDR_CTX + 0x389d8u + 96u + 2u * 24u);
    CHECK(state->last_material == ADDR_MAT2);
    CHECK(state->multisample_cache ==
          ((0u & ~0x3eu) | (1u << 1) | (2u << 3)));
}

static void test_texture_lru_and_barrier(void)
{
    VPEngineMaterialEnv env = make_env();
    VPEngineSubmitState *state;
    int rc;

    reset_arena();
    setup_common();
    state = (VPEngineSubmitState *)arena_resolve(NULL, ADDR_STATE);
    /* Pending update behind pending sync: the first type-2/5 texture that is
     * removed from (or absent from) the LRU list flushes a barrier. */
    put_u32(ADDR_CTX + 0x340f0u, 0u);
    put_u32(ADDR_CTX + 0x340f4u, 7u);
    put_u32(ADDR_CTX + 0x34070u + 5u * 4u, ADDR_TEXTURE_A);

    rc = vp_engine_material_submit(&env, ADDR_CTX, ADDR_STATE, ADDR_MAT0);
    CHECK(rc == VP_ENGINE_MATERIAL_OK);
    /* program, attrib, stream, 2 uniform pairs, [barrier, texture handle,
     * texture A], uniform, texture B query/bind, alpha, color, msaa. */
    {
        unsigned found_barrier = 0, found_texture = 0;
        for (unsigned i = 0; i < event_count; i++) {
            if (events[i].kind == EV_BARRIER) {
                found_barrier = 1;
                CHECK(events[i].a == CMD && events[i].b == 0x16u);
            }
            if (events[i].kind == EV_TEXTURE && events[i].c == 2u) {
                found_texture = 1;
                CHECK(i > 0 && events[i - 1].kind == EV_TEXTURE_HANDLE);
            }
        }
        CHECK(found_barrier && found_texture);
    }
    CHECK(get_u32(ADDR_CTX + 0x34070u + 5u * 4u) == 0u);
    CHECK(get_u32(ADDR_CTX + 0x340f0u) == 7u);
    CHECK(get_u32(ADDR_CTX + 0x340f4u) == 7u);

    /* Second material with no cache seed: texture A absent from LRU, flag
     * 0x02 has bits outside bit0, pending differs again, and sampler change
     * forces an unconditional bind. */
    reset_arena();
    setup_common();
    put_u32(ADDR_CTX + 0x340f0u, 1u);
    put_u32(ADDR_CTX + 0x340f4u, 9u);
    rc = vp_engine_material_submit(&env, ADDR_CTX, ADDR_STATE, ADDR_MAT0);
    CHECK(rc == VP_ENGINE_MATERIAL_OK);
    {
        unsigned barriers = 0;
        for (unsigned i = 0; i < event_count; i++)
            if (events[i].kind == EV_BARRIER) barriers++;
        CHECK(barriers == 1);
    }
    CHECK(get_u32(ADDR_CTX + 0x340f0u) == 9u);

    /* Type not in {2,5}: no LRU/barrier even with pending counters apart. */
    reset_arena();
    setup_common();
    put_u32(ADDR_TEXTURE_A + 0x10u, 7u);
    put_u32(ADDR_TEXTURE_B + 0x10u, 7u);
    put_u32(ADDR_CTX + 0x340f0u, 1u);
    put_u32(ADDR_CTX + 0x340f4u, 9u);
    rc = vp_engine_material_submit(&env, ADDR_CTX, ADDR_STATE, ADDR_MAT0);
    CHECK(rc == VP_ENGINE_MATERIAL_OK);
    for (unsigned i = 0; i < event_count; i++)
        CHECK(events[i].kind != EV_BARRIER);
    CHECK(get_u32(ADDR_CTX + 0x340f0u) == 1u);
}

static void test_gpu_address_positive_delta(void)
{
    VPEngineMaterialEnv env = make_env();
    uint64_t address = 0;
    int rc;

    reset_arena();
    setup_common();
    /* Resource at +0x4, base 0x20 bytes lower: alignment mask and positive
     * 32-bit delta. */
    put_u32(ADDR_WRAPPER + 0x58u, ADDR_BUFFER_DATA - 0x20u);
    put_u32(ADDR_MAT0 + 0x0cu + 10u * 4u, ADDR_BUFFER_DATA + 0x4u);
    /* Reach the translation alone through the first uniform record. */
    rc = vp_engine_material_submit(&env, ADDR_CTX, ADDR_STATE, ADDR_MAT0);
    CHECK(rc == VP_ENGINE_MATERIAL_OK);
    CHECK(event_count >= 5 && events[4].kind == EV_UNIFORM);
    address = events[4].d;
    CHECK(address == BUFFER_ADDRESS + 0x20u);
}

static void test_missing_callback_and_bad_memory(void)
{
    VPEngineMaterialEnv env = make_env();
    VPEngineSubmitState *state;
    int rc;

    reset_arena();
    setup_common();
    env.bind_texture = NULL;
    rc = vp_engine_material_submit(&env, ADDR_CTX, ADDR_STATE, ADDR_MAT0);
    CHECK(rc == VP_ENGINE_MATERIAL_ERR_CALLBACK);

    env = make_env();
    state = (VPEngineSubmitState *)arena_resolve(NULL, ADDR_STATE);
    state->last_material = 0;
    rc = vp_engine_material_submit(&env, ADDR_CTX, 0x50000000u, ADDR_MAT0);
    CHECK(rc == VP_ENGINE_MATERIAL_ERR_MEMORY);

    rc = vp_engine_material_submit(&env, ADDR_CTX, ADDR_STATE, 0u);
    CHECK(rc == VP_ENGINE_MATERIAL_ERR_ARGUMENT);
}

static void test_tail_mask_preserves_high_nibble(void)
{
    VPEngineMaterialEnv env = make_env();
    VPEngineSubmitState *state;
    int rc;

    reset_arena();
    setup_common();
    state = (VPEngineSubmitState *)arena_resolve(NULL, ADDR_STATE);
    state->last_material = ADDR_MAT0;
    state->flags = 0xf0000000u;
    state->multisample_cache = 0x10u;
    rc = vp_engine_material_submit(&env, ADDR_CTX, ADDR_STATE, ADDR_MAT0);
    CHECK(rc == VP_ENGINE_MATERIAL_OK);
    CHECK((state->flags & 0xf0000000u) == 0xf0000000u);
    CHECK(state->flags == 0xf2008100u);
    CHECK(event_count == 2u);
    CHECK(events[0].kind == EV_ALPHA);
    CHECK(fabsf(events[0].f - (128.0f / 255.0f)) < 1e-7f);
    CHECK(events[1].kind == EV_COLOR_STATE);

    /* Path B uses the same mask: material without bits 19|20. */
    reset_arena();
    setup_common();
    state = (VPEngineSubmitState *)arena_resolve(NULL, ADDR_STATE);
    put_u32(ADDR_MAT1 + 0x00u, 0u);
    put_u32(ADDR_MAT1 + 0x04u, 0u);
    state->last_material = ADDR_MAT1;
    state->flags = 0xf0000000u;
    state->multisample_cache = 0x10u;
    rc = vp_engine_material_submit(&env, ADDR_CTX, ADDR_STATE, ADDR_MAT1);
    CHECK(rc == VP_ENGINE_MATERIAL_OK);
    CHECK(state->flags == 0xf0000e00u);
    CHECK(event_count == 1u);
    CHECK(events[0].kind == EV_COLOR_STATE);
}

int main(void)
{
    test_full_submission();
    test_same_material_tail_only();
    test_same_layout_resource_diff_and_force();
    test_texture_lru_and_barrier();
    test_gpu_address_positive_delta();
    test_missing_callback_and_bad_memory();
    test_tail_mask_preserves_high_nibble();
    if (failures) {
        fprintf(stderr, "engine_material: %d failure(s)\n", failures);
        return 1;
    }
    printf("engine_material: all checks passed\n");
    return 0;
}

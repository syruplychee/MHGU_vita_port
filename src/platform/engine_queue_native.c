#include "engine_queue_native.h"

#include "engine_nvn_internal.h"
#include "engine_state.h"

#include <string.h>

/* ---------------------------------------------------------------- constants */

#define SITE_STATE_RESET      0xbbe9f0u
#define SITE_LRU_RESET        0xbbea04u
#define SITE_SET_RESTART      0xbbea60u
#define SITE_SET_LINE_WIDTH   0xbbea7cu
#define SITE_SET_POINT_SIZE   0xbbea94u
#define SITE_SET_ALPHA_REF    0xbbeaacu

#define SITE_STREAM_BUF_A     0xbbef44u
#define SITE_STREAM_VB_A      0xbbef70u
#define SITE_STREAM_BUF_B     0xbbf25cu
#define SITE_STREAM_VB_B      0xbbf288u
#define SITE_STREAM_BUF_C     0xbbf394u
#define SITE_STREAM_VB_C      0xbbf3c0u
#define SITE_STREAM_BUF_D     0xbbf7e4u
#define SITE_STREAM_VB_D      0xbbf810u
#define SITE_STREAM_BUF_E     0xbbfca4u
#define SITE_STREAM_VB_E      0xbbfcd0u
#define SITE_STREAM_BUF_F     0xbbff78u
#define SITE_STREAM_VB_F      0xbbffa4u
#define SITE_STREAM_BUF_G     0xbc00c0u
#define SITE_STREAM_VB_G      0xbc00ecu
#define SITE_STREAM_BUF_H     0xbc04d4u
#define SITE_STREAM_VB_H      0xbc0500u

#define SITE_BARRIER_T5_LRU   0xbbfaa4u
#define SITE_BARRIER_T5_ALT   0xbbf6c0u
#define SITE_BARRIER_T5_FLAG  0xbbf190u
#define SITE_BARRIER_T14_LRU  0xbbf8a4u
#define SITE_BARRIER_T14_FLAG 0xbbf614u
#define SITE_BARRIER_T5_AUX   0xbbfda8u

#define SITE_DRAW_INDEXED     0xbbf734u
#define SITE_DRAW_ARRAYS      0xbc014cu
#define SITE_DOWNSAMPLE_A     0xbbfb18u
#define SITE_DOWNSAMPLE_B     0xbbfb54u
#define SITE_DOWNSAMPLE_C     0xbbfea0u
#define SITE_DISCARD_COLOR    0xbbfb6cu
#define SITE_DISCARD_DEPTH    0xbbfeb0u
#define SITE_COPY_T2T         0xbc018cu
#define SITE_COPY_T2B         0xbbf924u
#define SITE_CLEAR_COLOR      0xbbf9e0u
#define SITE_CLEAR_DEPTH      0xbbfa68u
#define SITE_QUERY_T4         0xbbf904u
#define SITE_QUERY_T1         0xbbf704u
#define SITE_QUERY_T3         0xbbf418u
#define SITE_QUERY_FLUSH      0xbc0360u
#define SITE_EXT_A            0xbbf58cu
#define SITE_EXT_B            0xbbf86cu

#define STATE_COLOR_WORD      0x1170u
#define STATE_RANGE_COUNT     0x1168u
#define STATE_RANGES          0x568u
#define STATE_VERTEX_COUNT    0x116cu
#define STATE_MSAA_CACHE      0x117cu

#define FRAME_BYTES           12u
#define FRAME_CURSOR_OFF      4u
#define FRAME_END_OFF         8u

/* Scratch areas within VPEngineNvnEnv.scratch (minimum 0x80 bytes). */
#define SCRATCH_RT_COLOR      0x00u
#define SCRATCH_RT_RECT       0x20u
#define SCRATCH_BLEND         0x40u
#define SCRATCH_CLEAR_A       0x50u
#define SCRATCH_CLEAR_B       0x68u

#define PRIMITIVE_TABLE_SIZE  16u

/* Exact IEEE-754 bits used by the locked sRGB conversion. */
static const uint64_t VP_D_SRGB_THRESHOLD = 0x3fa4b5dcc63f1412ULL; /* 0.04045 */
static const uint64_t VP_D_SRGB_OFFSET    = 0x3fac28f5c28f5c29ULL; /* 0.055 */
static const uint64_t VP_D_SRGB_SCALE     = 0x3ff0e147ae147ae1ULL; /* 1.055 */
static const uint64_t VP_D_SRGB_GAMMA     = 0x4003333333333333ULL; /* 2.4 */
static const uint64_t VP_D_SRGB_LINEAR    = 0x4029d70a3d70a3d7ULL; /* 12.92 */

typedef struct {
    VPEngineMem m;
    const VPEngineMaterialEnv *mat;
    uint32_t ctx;
    uint32_t state;
    uint32_t cmd;
    uint32_t snapshot_current;
    uint32_t pending_snapshot;
    uint8_t merge_enable;
} Queue;

static uint32_t rd32(Queue *q, uint32_t address)
{
    return vpm_r32(&q->m, address);
}

static uint8_t rd8(Queue *q, uint32_t address)
{
    return vpm_r8(&q->m, address);
}

static uint16_t rd16(Queue *q, uint32_t address)
{
    return vpm_r16(&q->m, address);
}

static void wr32(Queue *q, uint32_t address, uint32_t value)
{
    vpm_w32(&q->m, address, value);
}

static void emit_call(Queue *q, uint32_t site, uint32_t slot, uint32_t r0,
                      uint32_t r1, uint32_t r2, uint32_t r3)
{
    VPEngineNvnCall call;
    vpm_call_init(&call, site, slot);
    call.r[0] = r0;
    call.r[1] = r1;
    call.r[2] = r2;
    call.r[3] = r3;
    vpm_send(&q->m, &call, NULL);
}

static void emit_with_stack(Queue *q, uint32_t site, uint32_t slot,
                            uint32_t r0, uint32_t r1, uint32_t r2,
                            uint32_t r3, const uint32_t *stack,
                            unsigned stack_words)
{
    VPEngineNvnCall call;
    unsigned i;
    vpm_call_init(&call, site, slot);
    call.r[0] = r0;
    call.r[1] = r1;
    call.r[2] = r2;
    call.r[3] = r3;
    for (i = 0; i < stack_words && i < VP_ENGINE_NVN_STACK_MAX; i++)
        call.stack[i] = stack[i];
    call.stack_words = stack_words;
    vpm_send(&q->m, &call, NULL);
}

static void emit_float(Queue *q, uint32_t site, uint32_t slot, uint32_t r0,
                       uint32_t r1, uint32_t r2, uint32_t r3, uint32_t f0)
{
    VPEngineNvnCall call;
    vpm_call_init(&call, site, slot);
    call.r[0] = r0;
    call.r[1] = r1;
    call.r[2] = r2;
    call.r[3] = r3;
    call.s[0] = f0;
    call.s_words = 1;
    vpm_send(&q->m, &call, NULL);
}

static uint64_t query(Queue *q, uint32_t site, uint32_t slot, uint32_t r0)
{
    return vpm_query(&q->m, site, slot, r0, 0, 0, 0);
}

static void barrier_sync(Queue *q, uint32_t site)
{
    uint32_t sync_address =
        q->ctx + VP_ENGINE_QUEUE_NATIVE_CTX_PENDING_SYNC;
    uint32_t update_address =
        q->ctx + VP_ENGINE_QUEUE_NATIVE_CTX_PENDING_UPDATE;
    uint32_t sync = rd32(q, sync_address);
    uint32_t update = rd32(q, update_address);
    if (sync == update) return;
    emit_call(q, site, VP_SLOT_BARRIER, q->cmd, 0x16u, 0, 0);
    if (q->m.error) return;
    /* 0xBBF19C reloads ctx+0x340F4 after the barrier before updating the
     * snapshot. */
    wr32(q, sync_address, rd32(q, update_address));
}

static int submit_material(Queue *q, uint32_t material)
{
    int rc = vp_engine_material_submit(q->mat, q->ctx, q->state, material);
    if (rc != VP_ENGINE_MATERIAL_OK) {
        q->m.error = VP_ENGINE_NATIVE_ERR_MATERIAL;
        return rc;
    }
    return 0;
}

static int call_targets(Queue *q, uint32_t target)
{
    int rc = vp_engine_state_targets(q->m.env, q->ctx, q->state, target);
    if (rc != VP_ENGINE_NATIVE_OK) q->m.error = rc;
    return rc;
}

static int call_pipeline(Queue *q, uint32_t pipeline)
{
    int rc = vp_engine_state_pipeline(q->m.env, q->ctx, q->state, pipeline);
    if (rc != VP_ENGINE_NATIVE_OK) q->m.error = rc;
    return rc;
}

static uint32_t primitive_of(Queue *q, uint32_t flags_pointer)
{
    static const uint32_t table[PRIMITIVE_TABLE_SIZE] = {
        0u, 1u, 3u, 4u, 5u, 10u, 11u, 12u, 13u, 7u, 0u, 0u, 0u, 0u, 0u, 0u
    };
    uint32_t index = (rd32(q, flags_pointer) >> 21) & 0xffu;
    if (index >= PRIMITIVE_TABLE_SIZE) {
        q->m.error = VP_ENGINE_NATIVE_ERR_UNRESOLVED;
        return 0;
    }
    return table[index];
}

static uint64_t add_u32_offset(uint64_t address, uint32_t offset)
{
    uint32_t low = (uint32_t)address + offset;
    uint32_t high = (uint32_t)(address >> 32) + (low < offset ? 1u : 0u);
    return ((uint64_t)high << 32) | low;
}

static void post_draw(Queue *q)
{
    /* 0xBC0150 reloads the saved ctx+0x340F4 address. state+0x568
     * belongs to the root range, not the resource-update counter. */
    uint32_t address=q->ctx + VP_ENGINE_QUEUE_NATIVE_CTX_PENDING_UPDATE;
    wr32(q, address, rd32(q,address) + 1u);
}

/* Original vertex-stream loop.  The cache arrays are state+0x44 (buffer),
 * +0x54 (stride byte) and +0x64 (start offset); the last element count is
 * stored at state+0x116C. bind_first is the command buffer, including the
 * type 2/3 loops that reload r10 from state+4 before binding streams. */
static void bind_streams(Queue *q, uint32_t vertex_state, uint32_t query_site,
                         uint32_t bind_site, uint32_t bind_first)
{
    uint32_t count = 0;
    unsigned i;

    for (i = 0; i < 4u && !q->m.error; i++) {
        uint32_t object = rd32(q, vertex_state + i * 4u);
        uint32_t start, end, stride, elements, buffer, slot;
        uint64_t address, adjusted;
        uint32_t stack[1];

        if (!object) continue;
        start = rd32(q, vertex_state + 0x14u + i * 4u);
        end = rd32(q, object + 0x14u);
        stride = rd8(q, vertex_state + 0x10u + i);
        elements = stride ? (end - start) / stride : 0u;
        if (elements > count) count = elements;
        if (!stride) continue;

        buffer = rd32(q, object + 0x20u);
        slot = q->state + 0x44u + i * 4u;
        if (rd32(q, slot) == buffer &&
            rd32(q, q->state + 0x54u + i * 4u) == stride &&
            rd32(q, q->state + 0x64u + i * 4u) == start)
            continue;

        wr32(q, slot, buffer);
        wr32(q, q->state + 0x64u + i * 4u, start);
        wr32(q, q->state + 0x54u + i * 4u, stride);

        address = query(q, query_site, VP_SLOT_BUFFER_GET_ADDRESS, buffer);
        if (q->m.error) return;
        adjusted = add_u32_offset(address, start);
        stack[0] = end - start;
        {
            VPEngineNvnCall call;
            vpm_call_init(&call, bind_site, VP_SLOT_BIND_VERTEX_BUFFER);
            call.r[0] = bind_first;
            call.r[1] = i;
            call.r[2] = (uint32_t)adjusted;
            call.r[3] = (uint32_t)(adjusted >> 32);
            call.stack[0] = stack[0];
            call.stack_words = 1;
            vpm_send(&q->m, &call, NULL);
        }
    }
    if (!q->m.error) wr32(q, q->state + STATE_VERTEX_COUNT, count);
}

static void draw_arrays(Queue *q, uint32_t record)
{
    uint32_t primitive = primitive_of(q, rd32(q, record + 0xcu));
    if (q->m.error) return;
    emit_call(q, SITE_DRAW_ARRAYS, VP_SLOT_DRAW_ARRAYS, q->cmd, primitive,
              rd32(q, record + 0x18u), rd32(q, record + 0x14u));
    if (!q->m.error) post_draw(q);
}

static void draw_arrays_instanced(Queue *q, uint32_t record)
{
    uint32_t primitive = primitive_of(q, rd32(q, record + 0xcu));
    uint32_t stack[2];
    if (q->m.error) return;
    stack[0] = rd32(q, record + 0x20u);
    stack[1] = rd32(q, record + 0x1cu);
    emit_with_stack(q, SITE_DRAW_INDEXED, VP_SLOT_DRAW_ARRAYS_INSTANCED,
                    q->cmd, primitive, rd32(q, record + 0x18u),
                    rd32(q, record + 0x14u), stack, 2);
    if (!q->m.error) post_draw(q);
}

static void draw_elements_base_vertex(Queue *q, uint32_t record,
                                      uint32_t query_site)
{
    uint32_t buffer, primitive;
    uint64_t address, adjusted;
    uint32_t stack[3];

    buffer = rd32(q, rd32(q, record + 0x10u) + 0x28u);
    buffer = rd32(q, buffer + 0x20u);
    address = query(q, query_site, VP_SLOT_BUFFER_GET_ADDRESS, buffer);
    if (q->m.error) return;
    adjusted = add_u32_offset(address, rd32(q, record + 0x18u) << 1);
    primitive = primitive_of(q, rd32(q, record + 0xcu));
    if (q->m.error) return;
    stack[0] = (uint32_t)adjusted;
    stack[1] = (uint32_t)(adjusted >> 32);
    stack[2] = rd32(q, record + 0x1cu);
    emit_with_stack(q, SITE_DRAW_INDEXED, VP_SLOT_DRAW_ELEMENTS_BASE_VERTEX,
                    q->cmd, primitive, 1u, rd32(q, record + 0x14u),
                    stack, 3);
    if (!q->m.error) post_draw(q);
}

static void draw_elements_instanced(Queue *q, uint32_t record,
                                    uint32_t query_site)
{
    uint32_t buffer, primitive;
    uint64_t address, adjusted;
    uint32_t stack[5];

    buffer = rd32(q, rd32(q, record + 0x10u) + 0x28u);
    buffer = rd32(q, buffer + 0x20u);
    address = query(q, query_site, VP_SLOT_BUFFER_GET_ADDRESS, buffer);
    if (q->m.error) return;
    adjusted = add_u32_offset(address, rd32(q, record + 0x18u) << 1);
    primitive = primitive_of(q, rd32(q, record + 0xcu));
    if (q->m.error) return;
    stack[0] = (uint32_t)adjusted;
    stack[1] = (uint32_t)(adjusted >> 32);
    stack[2] = rd32(q, record + 0x1cu);
    /* 0xBBF438 STM sp,{r0,r1,r3,r5} stores FOUR consecutive words;
     * r5 was loaded from record+0x24, not an uninitialized stack slot. */
    stack[3] = rd32(q, record + 0x24u);
    stack[4] = rd32(q, record + 0x20u);
    emit_with_stack(q, SITE_DRAW_INDEXED, VP_SLOT_DRAW_ELEMENTS_INSTANCED,
                    q->cmd, primitive, 1u, rd32(q, record + 0x14u),
                    stack, 5);
    if (!q->m.error) post_draw(q);
}

static uint32_t deref_current(Queue *q, uint32_t base)
{
    uint32_t p = rd32(q, base);
    p = rd32(q, p + 0x204u);
    return rd32(q, p);
}

static uint32_t lru_find(Queue *q, uint32_t value, uint32_t *slot_out)
{
    unsigned i;
    for (i = 0; i < 32u; i++) {
        uint32_t slot = q->ctx + VP_ENGINE_QUEUE_NATIVE_CTX_LRU + i * 4u;
        if (rd32(q, slot) == value) {
            if (slot_out) *slot_out = slot;
            return 1;
        }
    }
    return 0;
}

static void draw_with_streams(Queue *q, uint32_t record, uint32_t query_site,
                              uint32_t bind_site, uint32_t bind_first)
{
    uint32_t vertex_state = rd32(q, record + 0x10u);
    if (rd32(q, q->state + 0x14u) != vertex_state) {
        bind_streams(q, vertex_state, query_site, bind_site, bind_first);
        if (q->m.error) return;
        wr32(q, q->state + 0x14u, vertex_state);
    }
    submit_material(q, rd32(q, record + 0xcu));
}

/* 0xBBFBE4 / 0xBBFEBC / 0xBC0000: the type-5 draw delegation refreshes the
 * sub-record's target and pipeline state, then runs the shared stream and
 * material preparation before DrawArrays. */
static void draw_sub_record(Queue *q, uint32_t sub, uint32_t query_site,
                            uint32_t bind_site, uint32_t bind_first)
{
    int rc = vp_engine_state_targets(q->m.env, q->ctx, q->state,
                                     rd32(q, sub + 0x4u));
    if (rc != VP_ENGINE_NATIVE_OK) {
        q->m.error = rc;
        return;
    }
    rc = vp_engine_state_pipeline(q->m.env, q->ctx, q->state,
                                  rd32(q, sub + 0x8u));
    if (rc != VP_ENGINE_NATIVE_OK) {
        q->m.error = rc;
        return;
    }
    draw_with_streams(q, sub, query_site, bind_site, bind_first);
    if (!q->m.error) draw_arrays(q, sub);
}

/* Build the six-word readback/clear descriptor used by type 5 and type 14:
 * three zero words, packed 16-bit channels at +0x1E/+0x20, and a trailing 1. */
static void build_color_descriptor(Queue *q, uint32_t scratch,
                                   uint32_t descriptor)
{
    uint32_t low = rd16(q, descriptor + 0x20u);
    uint32_t high = rd16(q, descriptor + 0x1eu);
    uint32_t word = high | (low << 16);
    wr32(q, scratch + 0x0u, 0u);
    wr32(q, scratch + 0x4u, 0u);
    wr32(q, scratch + 0x8u, 0u);
    wr32(q, scratch + 0xcu, word & 0xffffu);
    wr32(q, scratch + 0x10u, (word >> 16) & 0x3fffu);
    wr32(q, scratch + 0x14u, 1u);
}

static double double_from_bits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static float float_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void srgb_convert_word(Queue *q, uint32_t *word, uint32_t site)
{
    double value = (double)float_from_bits(*word);
    double threshold = double_from_bits(VP_D_SRGB_THRESHOLD);
    double result;
    if (value > threshold) {
        double base = (value + double_from_bits(VP_D_SRGB_OFFSET)) /
                      double_from_bits(VP_D_SRGB_SCALE);
        result = q->m.env->numeric_pow(q->m.env->opaque, site, base,
                                       double_from_bits(VP_D_SRGB_GAMMA));
    } else {
        result = value / double_from_bits(VP_D_SRGB_LINEAR);
    }
    *word = float_bits((float)result);
}

/* type 0: DrawArrays with material and stream preparation. */
static void handle_type0(Queue *q, uint32_t record)
{
    if (call_targets(q, rd32(q, record + 0x4u))) return;
    if (call_pipeline(q, rd32(q, record + 0x8u))) return;
    draw_with_streams(q, record, SITE_STREAM_BUF_H, SITE_STREAM_VB_H,
                      q->cmd);
    if (!q->m.error) draw_arrays(q, record);
}

/* type 1: DrawElementsBaseVertex, or the original pending merge. */
static int handle_type1(Queue *q, uint32_t record, uint32_t *pending_out,
                        uint32_t *merge_base, uint32_t *merge_count)
{
    if (call_targets(q, rd32(q, record + 0x4u))) return q->m.error;
    if (call_pipeline(q, rd32(q, record + 0x8u))) return q->m.error;
    draw_with_streams(q, record, SITE_STREAM_BUF_B, SITE_STREAM_VB_B,
                      q->cmd);
    if (q->m.error) return q->m.error;
    if (q->merge_enable) {
        *merge_base = rd32(q, record + 0x14u);
        *merge_count = rd32(q, record + 0x18u);
        *pending_out = record;
        return 0;
    }
    draw_elements_base_vertex(q, record, SITE_QUERY_T1);
    return q->m.error;
}

/* type 2: DrawArraysInstanced. */
static void handle_type2(Queue *q, uint32_t record)
{
    if (call_targets(q, rd32(q, record + 0x4u))) return;
    if (call_pipeline(q, rd32(q, record + 0x8u))) return;
    draw_with_streams(q, record, SITE_STREAM_BUF_A, SITE_STREAM_VB_A,
                      q->cmd);
    if (!q->m.error) draw_arrays_instanced(q, record);
}

/* type 3: DrawElementsInstanced. */
static void handle_type3(Queue *q, uint32_t record)
{
    if (call_targets(q, rd32(q, record + 0x4u))) return;
    if (call_pipeline(q, rd32(q, record + 0x8u))) return;
    draw_with_streams(q, record, SITE_STREAM_BUF_C, SITE_STREAM_VB_C,
                      q->cmd);
    if (!q->m.error) draw_elements_instanced(q, record, SITE_QUERY_T3);
}

/* type 4: clear when the rectangle is (0,0,dim.w,dim.h), otherwise draw the
 * sub-record at record+0x20. */
static void handle_type4(Queue *q, uint32_t record)
{
    uint32_t sub = rd32(q, record + 0x20u);
    uint32_t target = rd32(q, sub + 0x4u);
    uint32_t target_flags = rd32(q, target + 0x14u);
    uint32_t rect_address =
        target + ((target_flags & 0x200u) ? 0x28u : 0x18u);
    uint32_t x = rd32(q, rect_address + 0x0u);
    uint32_t y = rd32(q, rect_address + 0x4u);
    uint32_t width = rd32(q, rect_address + 0x8u);
    uint32_t height = rd32(q, rect_address + 0xcu);
    uint32_t dimensions = rd32(q, target + 0x58u);
    uint32_t clear_flags;

    if (call_targets(q, target)) return;
    if ((x | y) != 0u || (dimensions & 0xffffu) != width ||
        (dimensions >> 16) != height) {
        /* 0xBBF73C: draw path.  Only the pipeline state is refreshed here. */
        if (call_pipeline(q, rd32(q, sub + 0x8u))) return;
        draw_with_streams(q, sub, SITE_STREAM_BUF_D, SITE_STREAM_VB_D,
                          q->cmd);
        if (!q->m.error) draw_arrays(q, sub);
        return;
    }

    clear_flags = rd32(q, record + 0x4u);
    if (clear_flags & 0x0fu) {
        uint32_t color[4];
        unsigned i;
        color[0] = rd32(q, record + 0x10u);
        color[1] = rd32(q, record + 0x14u);
        color[2] = rd32(q, record + 0x18u);
        color[3] = rd32(q, record + 0x1cu);

        if (rd8(q, rd32(q, q->m.env->srgb_config) + 0x29u)) {
            static const uint32_t sites[3] = {
                0xbbf50cu, 0xbbf958u, 0xbbf98cu
            };
            for (i = 0; i < 3u; i++) {
                srgb_convert_word(q, &color[i], sites[i]);
                if (q->m.error) return;
            }
        }
        for (i = 0; i < 4u; i++)
            wr32(q, q->m.env->scratch + SCRATCH_CLEAR_A + i * 4u, color[i]);

        {
            uint32_t color_count = rd32(q, target + 0x14u) & 7u;
            for (i = 0; i < color_count && !q->m.error; i++) {
                if (!(clear_flags & (1u << i))) continue;
                emit_call(q, SITE_CLEAR_COLOR, VP_SLOT_CLEAR_COLOR, q->cmd,
                          i, q->m.env->scratch + SCRATCH_CLEAR_A, 0x0fu);
            }
        }
    }

    if ((clear_flags & 0x30u) && rd32(q, target + 0x10u) != 0u) {
        uint32_t descriptor = rd32(q, target + 0x10u);
        uint32_t kind = rd32(q, descriptor + 0x24u) - 0x32u;
        uint32_t enable = 0;
        uint32_t clear_value;
        if (!(clear_flags & 0x20u)) enable = 1u;
        if (kind < 3u) clear_value = 1u;
        else clear_value = 0u;
        if (clear_flags & 0x10u) clear_value = enable;
        clear_value = (clear_value != 0u) ? 0u : 0xffu;
        if (kind < 3u) clear_value = 0u;
        emit_float(q, SITE_CLEAR_DEPTH, VP_SLOT_CLEAR_DEPTH_STENCIL, q->cmd,
                   (clear_flags >> 4) & 1u, rd8(q, record + 0xcu),
                   clear_value, rd32(q, record + 0x8u));
    }
}

/* type 5: target/format decisions.  subtype<=3 resolves targets from the
 * record array; subtype>3 uses the aux target in state+0xA4. */
static void handle_type5(Queue *q, uint32_t record)
{
    uint32_t subtype = rd32(q, record + 0xcu) & 0xfu;
    uint32_t count;
    uint32_t frame;
    uint32_t node;
    uint32_t next_record;

    /* 0xBBEFDC..0xBC01FC: skip a duplicate type-5 node in the current range. */
    count = rd32(q, q->state + STATE_RANGE_COUNT);
    frame = q->state + STATE_RANGES + count * FRAME_BYTES;
    node = rd32(q, frame + FRAME_CURSOR_OFF);
    if (node != 0u && node < rd32(q, frame + FRAME_END_OFF)) {
        next_record = rd32(q, node + 0x4u);
        if (((rd32(q, next_record) >> 16) & 0xfu) == 5u &&
            rd32(q, next_record + 0x20u) == rd32(q, record + 0x20u) &&
            rd32(q, next_record + 0x24u) == rd32(q, record + 0x24u) &&
            rd32(q, next_record + 0x0cu) == rd32(q, record + 0x0cu) &&
            rd32(q, next_record + 0x18u) == rd32(q, record + 0x18u) &&
            rd32(q, next_record + 0x14u) == rd32(q, record + 0x14u) &&
            rd32(q, next_record + 0x10u) == rd32(q, record + 0x10u) &&
            rd32(q, next_record + 0x1cu) == rd32(q, record + 0x1cu) &&
            rd32(q, next_record + 0x4u) == rd32(q, record + 0x4u)) {
            /* 0xBC0198: consume the duplicate and pop exhausted ranges. */
            wr32(q, frame + FRAME_CURSOR_OFF, node + 8u);
            if (count >= 1u && node >= rd32(q, frame + FRAME_END_OFF)) {
                for (;;) {
                    uint32_t frame2;
                    count--;
                    wr32(q, q->state + STATE_RANGE_COUNT, count);
                    frame2 = q->state + 0x560u + count * FRAME_BYTES;
                    node = rd32(q, frame2);
                    if (node < rd32(q, frame2 + 4u)) {
                        wr32(q, frame2, node + 8u);
                        break;
                    }
                    if (count < 1u) break;
                }
            }
        }
    }

    if (subtype <= 3u) {
        uint32_t array = rd32(q, record + 0x4u);
        uint32_t index = rd32(q, record + 0x8u);
        /* 0xBBF0AC: the chosen object is indexed by record+0x8; the cache slot
         * is indexed by the subtype nibble. */
        uint32_t chosen = rd32(q, rd32(q, array + 0x204u) + index * 4u);
        uint32_t cached = rd32(q, q->state + 0x94u + subtype * 4u);
        uint32_t current_a = deref_current(
            q, q->ctx + VP_ENGINE_QUEUE_NATIVE_CTX_CURRENT_A);
        uint32_t descriptor_a, descriptor_b, descriptor_type;
        uint32_t out_a, out_b, slot;

        if (q->m.error) return;
        /* The locked code reads the B chain only inside these two branches. */
        if (current_a == cached) {
            cached = deref_current(
                q, q->ctx + VP_ENGINE_QUEUE_NATIVE_CTX_CURRENT_B);
            if (q->m.error) return;
        }
        if (current_a == chosen) {
            chosen = deref_current(
                q, q->ctx + VP_ENGINE_QUEUE_NATIVE_CTX_CURRENT_B);
            if (q->m.error) return;
        }

        descriptor_a = rd32(q, cached + 0x10u);
        descriptor_b = rd32(q, chosen + 0x10u);
        descriptor_type = rd32(q, descriptor_a + 0x10u);
        out_a = rd32(q, descriptor_a + 0x3cu);
        out_b = rd32(q, descriptor_b + 0x3cu);

        if (descriptor_type == 2u || descriptor_type == 5u) {
            if (lru_find(q, descriptor_a, &slot)) {
                barrier_sync(q, SITE_BARRIER_T5_LRU);
                if (q->m.error) return;
                wr32(q, slot, 0u);
            } else if (descriptor_a != 0u &&
                       (rd8(q, descriptor_a + 0x23u) & 0xfeu) != 0u) {
                barrier_sync(q, SITE_BARRIER_T5_FLAG);
                if (q->m.error) return;
            }
        }

        if ((rd32(q, chosen + 0x18u) ^ rd32(q, cached + 0x18u)) &
                0x7fffffffu ||
            rd32(q, chosen + 0x24u) != rd32(q, cached + 0x24u)) {
            uint32_t type = rd32(q, descriptor_a + 0x24u);
            uint32_t format = ((type >> 11) & 0xeu) | 1u;
            if (format == 9u) {
                emit_call(q, SITE_DOWNSAMPLE_B, VP_SLOT_DOWNSAMPLE, q->cmd,
                          out_a, out_b, 0);
                if (q->m.error) return;
                emit_call(q, SITE_DISCARD_COLOR, VP_SLOT_DISCARD_COLOR,
                          q->cmd, 0, 0, 0);
                return;
            }
            {
                uint32_t sub = rd32(q, record + 0x28u);
                draw_sub_record(q, sub, SITE_STREAM_BUF_E,
                                SITE_STREAM_VB_E, q->cmd);
            }
            return;
        }

        {
            uint32_t type = rd32(q, descriptor_a + 0x24u);
            uint32_t format = ((type >> 11) & 0xeu) | 1u;
            if (format == 9u) {
                emit_call(q, SITE_DOWNSAMPLE_A, VP_SLOT_DOWNSAMPLE, q->cmd,
                          out_a, out_b, 0);
                if (q->m.error) return;
                emit_call(q, SITE_DISCARD_COLOR, VP_SLOT_DISCARD_COLOR,
                          q->cmd, 0, 0, 0);
                return;
            }
            {
                uint32_t sub = rd32(q, record + 0x28u);
                draw_sub_record(q, sub, SITE_STREAM_BUF_G,
                                SITE_STREAM_VB_G, q->cmd);
            }
        }
        return;
    }

    {
        uint32_t array = rd32(q, record + 0x4u);
        uint32_t index = rd32(q, record + 0x8u);
        uint32_t chosen =
            rd32(q, rd32(q, array + 0x208u) + index * 4u);
        uint32_t aux = rd32(q, q->state + 0xa4u);
        uint32_t aux_descriptor = rd32(q, aux + 0x10u);
        uint32_t aux_type = rd32(q, aux_descriptor + 0x10u);
        uint32_t slot;

        if (q->m.error) return;
        if (aux_type == 2u || aux_type == 5u) {
            if (lru_find(q, aux_descriptor, &slot)) {
                barrier_sync(q, SITE_BARRIER_T5_AUX);
                if (q->m.error) return;
                wr32(q, slot, 0u);
            } else if (aux_descriptor != 0u &&
                       (rd8(q, aux_descriptor + 0x23u) & 0xfeu) != 0u) {
                barrier_sync(q, SITE_BARRIER_T5_ALT);
                if (q->m.error) return;
            }
        }

        if (chosen == q->snapshot_current) {
            uint32_t update = rd32(
                q, q->ctx + VP_ENGINE_QUEUE_NATIVE_CTX_PENDING_UPDATE);
            if (q->pending_snapshot == update) return;
        }

        if (rd32(q, aux + 0x18u) != rd32(q, chosen + 0x18u) ||
            rd32(q, aux + 0x24u) != rd32(q, chosen + 0x24u)) {
            uint32_t sub = rd32(q, record + 0x28u);
            draw_sub_record(q, sub, SITE_STREAM_BUF_F, SITE_STREAM_VB_F,
                            q->cmd);
            return;
        }

        {
            uint32_t chosen_descriptor = rd32(q, chosen + 0x10u);
            uint32_t type = rd32(q, aux_descriptor + 0x24u);
            uint32_t format = ((type >> 11) & 0xeu) | 1u;
            uint32_t desc_a = rd32(q, aux_descriptor + 0x3cu);
            uint32_t desc_b = rd32(q, chosen_descriptor + 0x3cu);

            build_color_descriptor(q, q->m.env->scratch + SCRATCH_CLEAR_A,
                                   aux_descriptor);
            build_color_descriptor(q, q->m.env->scratch + SCRATCH_CLEAR_B,
                                   chosen_descriptor);
            if (q->m.error) return;

            if (format != 9u) {
                uint32_t stack[4];
                stack[0] = desc_b;
                stack[1] = 0u;
                stack[2] = q->m.env->scratch + SCRATCH_CLEAR_B;
                stack[3] = 0u;
                emit_with_stack(q, SITE_COPY_T2T, VP_SLOT_COPY_TEXTURE_TO_TEXTURE,
                                q->cmd, desc_a, 0u,
                                q->m.env->scratch + SCRATCH_CLEAR_A,
                                stack, 4);
            } else {
                emit_call(q, SITE_DOWNSAMPLE_C, VP_SLOT_DOWNSAMPLE, q->cmd,
                          desc_a, desc_b, 0);
                if (q->m.error) return;
                emit_call(q, SITE_DISCARD_DEPTH, VP_SLOT_DISCARD_DEPTH_STENCIL,
                          q->cmd, 0, 0, 0);
            }
        }
    }
}

/* type 6: push a nested range. */
static void handle_type6(Queue *q, uint32_t record)
{
    uint32_t count = rd32(q, q->state + STATE_RANGE_COUNT) + 1u;
    uint32_t frame = q->state + STATE_RANGES + count * FRAME_BYTES;
    uint32_t cursor = rd32(q, record + 0x8u);
    uint32_t elements = rd32(q, record + 0xcu);
    wr32(q, q->state + STATE_RANGE_COUNT, count);
    wr32(q, frame, rd32(q, record + 0x4u));
    wr32(q, frame + FRAME_CURSOR_OFF, cursor);
    wr32(q, frame + FRAME_END_OFF, cursor + elements * 8u);
    q->pending_snapshot =
        rd32(q, q->ctx + VP_ENGINE_QUEUE_NATIVE_CTX_PENDING_UPDATE);
}

/* type 7: original external resource functions. */
static void handle_type7(Queue *q, uint32_t record)
{
    uint32_t arg0 = rd32(q, record + 0x8u);
    uint32_t selector = rd32(q, record + 0x4u);
    if (!arg0) return;
    if (!q->m.env->external_resource) {
        q->m.error = VP_ENGINE_NATIVE_ERR_CALLBACK;
        return;
    }
    q->m.env->external_resource(q->m.env->opaque,
                                selector ? SITE_EXT_A : SITE_EXT_B,
                                arg0, q->cmd);
}

/* type 14: copy the target color to a buffer. */
static void handle_type14(Queue *q, uint32_t record)
{
    uint32_t object = rd32(q, record + 0x4u);
    uint32_t descriptor_type;
    uint32_t slot;
    uint64_t address;

    if (!object) return;
    descriptor_type = rd32(q, rd32(q, object + 0x10u) + 0x10u);
    if (descriptor_type == 2u || descriptor_type == 5u) {
        if (lru_find(q, object, &slot)) {
            barrier_sync(q, SITE_BARRIER_T14_LRU);
            if (q->m.error) return;
            wr32(q, slot, 0u);
        } else if ((rd8(q, object + 0x23u) & 0xfeu) != 0u) {
            barrier_sync(q, SITE_BARRIER_T14_FLAG);
            if (q->m.error) return;
        }
    }

    build_color_descriptor(q, q->m.env->scratch + SCRATCH_CLEAR_A, object);
    if (q->m.error) return;
    address = query(q, SITE_QUERY_T4, VP_SLOT_BUFFER_GET_ADDRESS,
                    rd32(q, record + 0x8u));
    if (q->m.error) return;
    {
        uint32_t stack[3];
        stack[0] = (uint32_t)address;
        stack[1] = (uint32_t)(address >> 32);
        stack[2] = 0u;
        emit_with_stack(q, SITE_COPY_T2B, VP_SLOT_COPY_TEXTURE_TO_BUFFER,
                        q->cmd, rd32(q, object + 0x3cu), 0u,
                        q->m.env->scratch + SCRATCH_CLEAR_A, stack, 3);
    }
}

static void flush_pending(Queue *q, uint32_t pending, uint32_t merge_base,
                          uint32_t merge_count)
{
    uint32_t primitive;
    uint32_t buffer;
    uint64_t address, adjusted;
    uint32_t stack[3];
    uint32_t count = merge_base + merge_count - rd32(q, pending + 0x18u);

    if (q->m.error) return;
    primitive = primitive_of(q, rd32(q, pending + 0xcu));
    if (q->m.error) return;
    buffer = rd32(q, rd32(q, pending + 0x10u) + 0x28u);
    buffer = rd32(q, buffer + 0x20u);
    address = query(q, SITE_QUERY_FLUSH, VP_SLOT_BUFFER_GET_ADDRESS, buffer);
    if (q->m.error) return;
    adjusted = add_u32_offset(address, rd32(q, pending + 0x18u) << 1);
    stack[0] = (uint32_t)adjusted;
    stack[1] = (uint32_t)(adjusted >> 32);
    stack[2] = rd32(q, pending + 0x1cu);
    emit_with_stack(q, 0xbc039cu, VP_SLOT_DRAW_ELEMENTS_BASE_VERTEX, q->cmd,
                    primitive, 1u, count, stack, 3);
    if (q->m.error) return;
    {
        uint32_t update_address =
            q->ctx + VP_ENGINE_QUEUE_NATIVE_CTX_PENDING_UPDATE;
        wr32(q, update_address, rd32(q, update_address) + 1u);
    }
}

int vp_engine_queue_native_run(const VPEngineNvnEnv *env,
                               const VPEngineMaterialEnv *material_env,
                               uint32_t ctx, uint32_t state)
{
    Queue q;
    uint32_t selector, root_cursor, root_nodes;
    uint32_t pending = 0;
    uint32_t merge_base = 0;
    uint32_t merge_count = 0;

    if (!env || !material_env || !env->resolve || !env->emit ||
        !env->memclr || !env->external_resource || !env->numeric_pow ||
        !env->default_viewport || !env->srgb_config || !env->scratch ||
        env->scratch_size < VP_ENGINE_NVN_SCRATCH_MIN || !ctx || !state ||
        !material_env->resolve)
        return VP_ENGINE_NATIVE_ERR_ARGUMENT;

    memset(&q, 0, sizeof(q));
    q.m.env = env;
    q.m.error = 0;
    q.mat = material_env;
    q.ctx = ctx;
    q.state = state;
    q.cmd = rd32(&q, ctx + VP_ENGINE_QUEUE_NATIVE_CTX_CMD);
    if (q.m.error) return q.m.error;

    vpm_memclr(&q.m, SITE_STATE_RESET, state, 0x117cu);
    wr32(&q, state + VP_ENGINE_QUEUE_NATIVE_STATE_CMD, q.cmd);
    vpm_memclr(&q.m, SITE_LRU_RESET,
               ctx + VP_ENGINE_QUEUE_NATIVE_CTX_LRU, 0x88u);
    if (q.m.error) return q.m.error;

    selector = rd32(&q, ctx + VP_ENGINE_QUEUE_NATIVE_CTX_SELECTOR) ^
               rd8(&q, ctx + VP_ENGINE_QUEUE_NATIVE_CTX_SELECTOR_XOR);
    root_cursor = rd32(&q,
                       ctx + VP_ENGINE_QUEUE_NATIVE_CTX_ROOT_CURSORS +
                           selector * 4u);
    root_nodes = rd32(&q,
                      ctx + VP_ENGINE_QUEUE_NATIVE_CTX_ROOT_COUNTS +
                          selector * 4u);
    wr32(&q, state + STATE_RANGES + FRAME_CURSOR_OFF, root_cursor);
    wr32(&q, state + STATE_RANGES + FRAME_END_OFF,
         root_cursor + root_nodes * 8u);
    if (q.m.error) return q.m.error;

    /* 0xBBEA50..0xBBEAAC: initial NVN state. */
    emit_call(&q, SITE_SET_RESTART, VP_SLOT_SET_PRIMITIVE_RESTART, q.cmd, 1u,
              0, 0);
    emit_float(&q, SITE_SET_LINE_WIDTH, VP_SLOT_SET_LINE_WIDTH, q.cmd, 0, 0,
               0, 0x3f800000u);
    emit_float(&q, SITE_SET_POINT_SIZE, VP_SLOT_SET_POINT_SIZE, q.cmd, 0, 0,
               0, 0x3f800000u);
    emit_float(&q, SITE_SET_ALPHA_REF, VP_SLOT_SET_ALPHA_REF, q.cmd, 0, 0, 0,
               0x00000000u);
    if (q.m.error) return q.m.error;
    wr32(&q, state + STATE_MSAA_CACHE, 0x3eu);

    q.merge_enable = rd8(&q, ctx + 0x28u);
    {
        uint32_t wrapper = rd32(&q, ctx + VP_ENGINE_QUEUE_NATIVE_CTX_ROOT_STATE);
        if (wrapper) {
            wrapper = rd32(&q, wrapper + 0x280u);
            if (wrapper) {
                wrapper = rd32(&q, wrapper + 0x208u);
                q.snapshot_current = rd32(&q, wrapper);
            }
        }
    }
    if (q.m.error) return q.m.error;

    for (;;) {
        uint32_t count = rd32(&q, state + STATE_RANGE_COUNT);
        uint32_t frame = state + STATE_RANGES + count * FRAME_BYTES;
        uint32_t node = rd32(&q, frame + FRAME_CURSOR_OFF);
        uint32_t record;
        uint32_t type;

        wr32(&q, frame + FRAME_CURSOR_OFF, node + 8u);
        if (q.m.error) return q.m.error;
        if (node < rd32(&q, frame + FRAME_END_OFF))
            goto process_node;

        for (;;) {
            if (count < 1u) return VP_ENGINE_NATIVE_OK;
            count--;
            wr32(&q, state + STATE_RANGE_COUNT, count);
            {
                uint32_t cursor =
                    state + 0x56cu + count * FRAME_BYTES;
                node = rd32(&q, cursor);
                if (q.m.error) return q.m.error;
                if (node < rd32(&q, cursor + 4u)) {
                    wr32(&q, cursor, node + 8u);
                    goto process_node;
                }
            }
            if (count < 1u) return VP_ENGINE_NATIVE_OK;
        }

    process_node:
        if (!node) return VP_ENGINE_NATIVE_OK;
        record = rd32(&q, node + 0x4u);
        if (!record) return VP_ENGINE_NATIVE_ERR_MEMORY;
        if (q.m.error) return q.m.error;

        if (pending) {
            if (((rd32(&q, pending) ^ rd32(&q, record)) & 0xf0000u) == 0u &&
                rd32(&q, pending + 0xcu) == rd32(&q, record + 0xcu) &&
                rd32(&q, pending + 0x10u) == rd32(&q, record + 0x10u) &&
                rd32(&q, pending + 0x8u) == rd32(&q, record + 0x8u) &&
                rd32(&q, pending + 0x4u) == rd32(&q, record + 0x4u) &&
                rd32(&q, pending + 0x1cu) == rd32(&q, record + 0x1cu) &&
                merge_base + merge_count + 1u ==
                    rd32(&q, record + 0x18u)) {
                merge_count = merge_base + merge_count + 1u;
                merge_base = rd32(&q, record + 0x14u);
                continue;
            }
            flush_pending(&q, pending, merge_base, merge_count);
            if (q.m.error) return q.m.error;
            merge_base = 0;
            merge_count = 0;
        }

        pending = 0;
        type = (rd32(&q, record) >> 16) & 0xfu;
        switch (type) {
        case 0:
            handle_type0(&q, record);
            break;
        case 1:
            if (handle_type1(&q, record, &pending, &merge_base,
                             &merge_count) != 0)
                return q.m.error;
            break;
        case 2:
            handle_type2(&q, record);
            break;
        case 3:
            handle_type3(&q, record);
            break;
        case 4:
            handle_type4(&q, record);
            break;
        case 5:
            handle_type5(&q, record);
            break;
        case 6:
            handle_type6(&q, record);
            break;
        case 7:
            handle_type7(&q, record);
            break;
        case 14:
            handle_type14(&q, record);
            break;
        case 8:
        case 9:
        case 10:
        case 11:
        case 12:
        case 13:
        case 15:
            break;
        default:
            /* The locked jump table covers 0..14; 15 is special-cased. */
            return VP_ENGINE_NATIVE_ERR_UNRESOLVED;
        }
        if (q.m.error) return q.m.error;
    }
}

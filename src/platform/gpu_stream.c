#include "gpu_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int vp_gpu_command_buffer_reserve(vp_gpu_command_buffer *buffer, size_t words) {
    const size_t per_word=2u*sizeof(uint32_t)+sizeof(vp_gpu_write);
    if (!buffer || !words || words>0x1fffffu || words>SIZE_MAX/per_word) return -1;
    if (words<=buffer->capacity) return 0;
    void *storage=malloc(words*per_word);
    if (!storage) return -1;
    free(buffer->storage);
    buffer->storage=storage;
    buffer->capacity=words;
    buffer->words=storage;
    buffer->params=buffer->words+words;
    buffer->writes=(vp_gpu_write *)(buffer->params+words);
    return 0;
}

void vp_gpu_command_buffer_free(vp_gpu_command_buffer *buffer) {
    if (!buffer) return;
    free(buffer->storage);
    memset(buffer,0,sizeof(*buffer));
}

static int grow(void **buffer, size_t *capacity, size_t needed, size_t element)
{
    size_t next;
    void *grown;

    if (needed <= *capacity) return 0;
    next = *capacity ? *capacity : 16u;
    while (next < needed) {
        if (next > (SIZE_MAX / 2u)) return -1;
        next *= 2u;
    }
    if (next > (SIZE_MAX / element)) return -1;
    grown = realloc(*buffer, next * element);
    if (grown == NULL) return -1;
    *buffer = grown;
    *capacity = next;
    return 0;
}

int vp_gpu_decode(const uint32_t *words, size_t count, vp_gpu_write_fn fn,
                  void *ctx, vp_gpu_decode_stats *stats){
    size_t i = 0;

    if (words == NULL && count != 0) return -1;
    if (stats != NULL) {
        stats->writes = 0;
        stats->unparsed = 0;
    }
    while (i < count) {
        uint32_t word = words[i];
        uint32_t mode = word >> 29;
        uint16_t method = (uint16_t)(word & VP_GPU_METHOD_MASK);
        uint8_t sub = (uint8_t)((word >> 13) & 7u);
        uint32_t args = (word >> 16) & VP_GPU_METHOD_MASK;
        uint32_t j;

        if ((mode == 0u || mode == 1u) || (mode == 2u || mode == 3u)) {
            if (count - i - 1u < args) return -1;
            for (j = 0; j < args; j++) {
                vp_gpu_write write;
                write.method = (uint16_t)((mode <= 1u) ? (method + j) : method);
                write.sub = sub;
                write.value = words[i + 1u + j];
                if (fn != NULL) fn(ctx, &write);
                if (stats != NULL) stats->writes++;
            }
            i += (size_t)args + 1u;
        } else if (mode == 4u) {
            vp_gpu_write write;
            write.method = method;
            write.sub = sub;
            write.value = args;
            if (fn != NULL) fn(ctx, &write);
            if (stats != NULL) stats->writes++;
            i++;
        } else if (mode == 5u) {
            vp_gpu_write write;
            if (!args || count - i - 1u < args) return -1;
            write.method = method;
            write.sub = sub;
            write.value = words[i + 1u];
            if (fn != NULL) fn(ctx, &write);
            if (stats != NULL) stats->writes++;
            for (j = 1; j < args; j++) {
                write.method = (uint16_t)(method + 1u);
                write.value = words[i + 1u + j];
                if (fn != NULL) fn(ctx, &write);
                if (stats != NULL) stats->writes++;
            }
            i += (size_t)args + 1u;
        } else {
            if (stats != NULL) stats->unparsed++;
            i++;
        }
    }
    return 0;
}

int vp_gpu_decode_collect(const uint32_t *words, size_t count,
                          vp_gpu_write *writes, size_t capacity,
                          vp_gpu_decode_stats *stats){
    size_t i = 0;
    size_t written = 0;

    if ((words == NULL && count != 0) || (writes == NULL && capacity != 0))
        return -1;
    if (stats != NULL) {
        stats->writes = 0;
        stats->unparsed = 0;
    }
    while (i < count) {
        uint32_t word = words[i];
        uint32_t mode = word >> 29;
        uint16_t method = (uint16_t)(word & VP_GPU_METHOD_MASK);
        uint8_t sub = (uint8_t)((word >> 13) & 7u);
        uint32_t args = (word >> 16) & VP_GPU_METHOD_MASK;
        uint32_t j;

        if ((mode == 0u || mode == 1u) || (mode == 2u || mode == 3u)) {
            if (count - i - 1u < args) return -1;
            if (args > capacity - written) return -1;
            for (j = 0; j < args; j++) {
                writes[written].method =
                    (uint16_t)((mode <= 1u) ? (method + j) : method);
                writes[written].sub = sub;
                writes[written].value = words[i + 1u + j];
                written++;
            }
            if (stats != NULL) stats->writes += args;
            i += (size_t)args + 1u;
        } else if (mode == 4u) {
            if (capacity - written < 1u) return -1;
            writes[written].method = method;
            writes[written].sub = sub;
            writes[written].value = args;
            written++;
            if (stats != NULL) stats->writes++;
            i++;
        } else if (mode == 5u) {
            if (!args || count - i - 1u < args) return -1;
            if (args > capacity - written) return -1;
            writes[written].method = method;
            writes[written].sub = sub;
            writes[written].value = words[i + 1u];
            written++;
            for (j = 1; j < args; j++) {
                writes[written].method = (uint16_t)(method + 1u);
                writes[written].sub = sub;
                writes[written].value = words[i + 1u + j];
                written++;
            }
            if (stats != NULL) stats->writes += args;
            i += (size_t)args + 1u;
        } else {
            if (stats != NULL) stats->unparsed++;
            i++;
        }
    }
    return 0;
}

void vp_macro_table_init(vp_macro_table *table)
{
    if (table == NULL) return;
    memset(table, 0, sizeof(*table));
}

void vp_macro_table_free(vp_macro_table *table)
{
    size_t i;

    if (table == NULL) return;
    for (i = 0; i < table->program_count; i++) free(table->programs[i].code);
    free(table->programs);
    free(table->positions);
    vp_macro_table_init(table);
}

static vp_macro_program *macro_find(vp_macro_table *table, uint32_t ip)
{
    size_t i;

    for (i = 0; i < table->program_count; i++) {
        if (table->programs[i].ip == ip) return &table->programs[i];
    }
    return NULL;
}

static vp_macro_program *macro_create(vp_macro_table *table, uint32_t ip)
{
    vp_macro_program *program;

    if (grow((void **)&table->programs, &table->program_capacity,
             table->program_count + 1u, sizeof(*table->programs)) != 0) {
        return NULL;
    }
    program = &table->programs[table->program_count++];
    memset(program, 0, sizeof(*program));
    program->ip = ip;
    return program;
}

typedef struct {
    vp_macro_table *table;
    vp_macro_upload_state *state;
    int failed;
} vp_macro_upload_ctx;

void vp_macro_upload_state_init(vp_macro_upload_state *state)
{
    if (state == NULL) return;
    state->ip = 0;
    state->sp = 0;
}

int vp_macro_upload_apply(vp_macro_table *table, vp_macro_upload_state *state,
                          uint16_t method, uint32_t value)
{
    if (table == NULL || state == NULL) return -1;
    if (method == 0x45u) {
        state->ip = value;
        if (macro_find(table, state->ip) == NULL &&
            macro_create(table, state->ip) == NULL) {
            return -1;
        }
    } else if (method == 0x46u) {
        vp_macro_program *program = macro_find(table, state->ip);
        if (program != NULL) {
            if (grow((void **)&program->code, &program->code_capacity,
                     program->code_count + 1u, sizeof(uint32_t)) != 0) {
                return -1;
            }
            program->code[program->code_count++] = value;
        }
    } else if (method == 0x47u) {
        state->sp = value;
    } else if (method == 0x48u) {
        if (grow((void **)&table->positions, &table->position_capacity,
                 (size_t)state->sp + 1u, sizeof(uint32_t)) != 0) {
            return -1;
        }
        while (table->position_count <= (size_t)state->sp) {
            table->positions[table->position_count++] = 0;
        }
        table->positions[state->sp] = value;
        state->sp++;
    }
    return 0;
}

static void macro_upload_write(void *opaque, const vp_gpu_write *write)
{
    vp_macro_upload_ctx *ctx = opaque;

    if (ctx->failed) return;
    if (vp_macro_upload_apply(ctx->table, ctx->state, write->method, write->value) != 0) {
        ctx->failed = 1;
    }
}

int vp_macro_upload(const uint32_t *init_words, size_t init_count,
                    vp_macro_table *table)
{
    vp_macro_upload_state state;
    vp_macro_upload_ctx ctx;

    if (table == NULL || (init_words == NULL && init_count != 0)) return -1;
    vp_macro_upload_state_init(&state);
    memset(&ctx, 0, sizeof(ctx));
    ctx.table = table;
    ctx.state = &state;
    if (vp_gpu_decode(init_words, init_count, macro_upload_write, &ctx, NULL) != 0) {
        return -1;
    }
    return ctx.failed ? -1 : 0;
}

uint64_t vp_macro_hash(const uint32_t *code, size_t count)
{
    const uint64_t m = ((uint64_t)0xc6a4a793u << 32) + 0x5bd1e995u;
    uint64_t h = 0;
    size_t i;

    for (i = 0; i < count; i++) {
        uint64_t k = code[i];
        k *= m;
        k ^= k >> 47;
        k *= m;
        h ^= k;
        h *= m;
        h += 0xe6546b64u;
    }
    return h;
}

void vp_mme_shared_init(vp_mme_shared *shared)
{
    if (shared == NULL) return;
    memset(shared, 0, sizeof(*shared));
}

void vp_mme_shared_write(vp_mme_shared *shared, uint16_t method, uint32_t value)
{
    if (shared == NULL || method >= VP_GPU_REG_LIMIT) return;
    shared->regs[method] = value;
    shared->valid[method] = 1u;
}

static uint32_t mme_read(vp_mme_shared *shared, uint16_t method, int *hit)
{
    if (method >= VP_GPU_REG_LIMIT) {
        if (hit != NULL) *hit = 0;
        return 0;
    }
    if (hit != NULL) *hit = shared->valid[method] != 0;
    if (shared->valid[method] == 0) shared->read_misses++;
    return shared->regs[method];
}

static uint32_t send_value(vp_mme_shared *shared, vp_gpu_send_fn send_fn,
                           void *send_ctx, vp_mme_result *result,
                           uint32_t address, uint32_t increment, uint32_t value)
{
    uint16_t method = (uint16_t)(address & 0xfffu);

    vp_mme_shared_write(shared, method, value);
    result->sends++;
    if (method == 0x585u || method == 0x485u || method == 0x5f9u) {
        result->draws++;
    } else if (method == 0x674u) {
        result->clears++;
    }
    if (send_fn != NULL) send_fn(send_ctx, method, value);
    return (address + increment) & 0xfffu;
}

static void fail_mme(vp_mme_result *result, const char *text)
{
    result->supported = 0;
    snprintf(result->reason, sizeof(result->reason), "%s", text);
}

int vp_mme_execute(const vp_macro_table *table, uint32_t position,
                   const uint32_t *params, size_t param_count,
                   vp_mme_shared *shared, vp_gpu_send_fn send_fn, void *send_ctx,
                   vp_mme_result *result)
{
    const uint32_t *program = NULL;
    size_t program_count = 0;
    uint32_t regs[VP_MME_REG_COUNT] = {0};
    size_t next_param = 1;
    uint32_t address = 0;
    uint32_t increment = 0;
    int carry = 0;
    int64_t pc = 0;
    int has_delay = 0;
    int64_t delay_target = 0;
    int countdown = -1;
    size_t steps = 0;
    size_t i;

    if (table == NULL || result == NULL || shared == NULL) return -1;
    memset(result, 0, sizeof(*result));
    result->supported = 1;
    for (i = 0; i < table->program_count; i++) {
        if (table->programs[i].ip == position) {
            program = table->programs[i].code;
            program_count = table->programs[i].code_count;
            break;
        }
    }
    if (program == NULL) {
        fail_mme(result, "no program");
        return 0;
    }
    regs[1] = param_count > 0 ? params[0] : 0;

    while (pc < (int64_t)program_count) {
        uint32_t word;
        uint32_t op;
        uint32_t res;
        uint32_t dst;
        uint32_t a;
        uint32_t b;
        uint32_t alu;
        int32_t imm;
        uint32_t value = 0;
        int in_delay = has_delay;
        int64_t base = pc;

        if (++steps > VP_MME_MAX_STEPS) {
            fail_mme(result, "step limit");
            break;
        }
        word = program[pc];
        pc++;
        op = word & 7u;
        res = (word >> 4) & 7u;
        dst = (word >> 8) & 7u;
        a = (word >> 11) & 7u;
        b = (word >> 14) & 7u;
        alu = (word >> 17) & 0x1fu;
        imm = (int32_t)((word >> 14) & 0x3ffffu);
        if (imm >= 0x20000) imm -= 0x40000;

        if (op == 0u) {
            uint64_t left = regs[a];
            uint64_t right = regs[b];
            switch (alu) {
            case 0u: {
                uint64_t sum = left + right;
                carry = sum > 0xffffffffu;
                value = (uint32_t)sum;
                break;
            }
            case 1u: {
                uint64_t sum = left + right + (uint64_t)(carry ? 1u : 0u);
                carry = sum > 0xffffffffu;
                value = (uint32_t)sum;
                break;
            }
            case 2u: {
                int64_t diff = (int64_t)left - (int64_t)right;
                carry = diff >= 0;
                value = (uint32_t)diff;
                break;
            }
            case 3u: {
                int64_t diff = (int64_t)left - (int64_t)right - (carry ? 0 : 1);
                carry = diff >= 0;
                value = (uint32_t)diff;
                break;
            }
            case 8u: value = regs[a] ^ regs[b]; break;
            case 9u: value = regs[a] | regs[b]; break;
            case 10u: value = regs[a] & regs[b]; break;
            case 11u: value = regs[a] & ~regs[b]; break;
            case 12u: value = ~(regs[a] & regs[b]); break;
            default:
                snprintf(result->reason, sizeof(result->reason), "alu %u", alu);
                result->supported = 0;
                break;
            }
            if (!result->supported) break;
        } else if (op == 1u) {
            value = (uint32_t)((int64_t)regs[a] + (int64_t)imm);
        } else if (op == 2u) {
            uint32_t d = regs[a];
            uint32_t s = regs[b];
            uint32_t size = (word >> 22) & 0x1fu;
            uint32_t src_bit = (word >> 17) & 0x1fu;
            uint32_t dst_bit = (word >> 27) & 0x1fu;
            uint32_t mask = size == 0 ? 0u : ((1u << size) - 1u);
            s = (s >> src_bit) & mask;
            value = (d & ~(mask << dst_bit)) | (s << dst_bit);
        } else if (op == 3u || op == 4u) {
            uint32_t d = regs[a];
            uint32_t s = regs[b];
            uint32_t size = (word >> 22) & 0x1fu;
            uint32_t dst_bit = (word >> 27) & 0x1fu;
            uint32_t mask = size == 0 ? 0u : ((1u << size) - 1u);
            uint32_t shift = (op == 3u ? d : regs[a]) & 31u;
            value = (((s >> shift) & mask) << dst_bit);
        } else if (op == 5u) {
            uint16_t method = (uint16_t)(((uint64_t)regs[a] + (uint64_t)imm) & 0xfffu);
            int hit = 0;
            value = mme_read(shared, method, &hit);
        } else if (op == 7u) {
            uint32_t condition = (word >> 4) & 1u;
            int taken = condition ? (regs[a] != 0) : (regs[a] == 0);
            if (taken) {
                int64_t target = base + (int64_t)imm;
                if (target < 0 || target >= (int64_t)program_count) {
                    fail_mme(result, "branch out of range");
                    break;
                }
                if ((word >> 5) & 1u) {
                    pc = target;
                } else {
                    has_delay = 1;
                    delay_target = target;
                }
            }
        } else {
            snprintf(result->reason, sizeof(result->reason), "op %u", op);
            result->supported = 0;
            break;
        }

        if (op != 7u) {
            switch (res) {
            case 0u:
                if (dst != 0u && next_param < param_count) regs[dst] = params[next_param];
                next_param++;
                break;
            case 1u:
                if (dst != 0u) regs[dst] = value;
                break;
            case 2u:
                if (dst != 0u) regs[dst] = value;
                address = value & 0xfffu;
                increment = (value >> 12) & 0x3fu;
                break;
            case 3u: {
                uint32_t v = next_param < param_count ? params[next_param] : 0;
                next_param++;
                if (dst != 0u) regs[dst] = v;
                address = send_value(shared, send_fn, send_ctx, result, address, increment, value);
                break;
            }
            case 4u:
                if (dst != 0u) regs[dst] = value;
                address = send_value(shared, send_fn, send_ctx, result, address, increment, value);
                break;
            case 5u:
                if (dst != 0u && next_param < param_count) regs[dst] = params[next_param];
                next_param++;
                address = value & 0xfffu;
                increment = (value >> 12) & 0x3fu;
                break;
            case 6u: {
                uint32_t v;
                if (dst != 0u) regs[dst] = value;
                address = value & 0xfffu;
                increment = (value >> 12) & 0x3fu;
                v = next_param < param_count ? params[next_param] : 0;
                next_param++;
                address = send_value(shared, send_fn, send_ctx, result, address, increment, v);
                break;
            }
            case 7u:
                if (dst != 0u) regs[dst] = value;
                address = value & 0xfffu;
                increment = (value >> 12) & 0x3fu;
                address = send_value(shared, send_fn, send_ctx, result, address, increment, increment);
                break;
            default:
                break;
            }
        }

        if (in_delay) {
            pc = delay_target;
            has_delay = 0;
        }
        if ((word & 0x80u) != 0u && !in_delay) countdown = 1;
        if (countdown == 0) break;
        if (countdown > 0) countdown--;
    }
    return 0;
}

void vp_gpu_state_init(vp_gpu_state *state)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
}

void vp_gpu_state_apply(vp_gpu_state *state, uint8_t sub, uint16_t method,
                        uint32_t value)
{
    if (state == NULL || sub >= VP_GPU_SUBCHANNELS || method >= VP_GPU_REG_LIMIT) return;
    state->sub[sub].value[method] = value;
    state->sub[sub].valid[method] = 1u;
    if (method >= 0x8e4u && method <= 0x8f3u)
        state->sub[sub].value[0x8e3u] += 4u;
    /* CB_ADDRESS is only the selector. Each bind group latches it into a
     * stage/slot, including writes emitted by MME macros. */
    if (method >= 0x904u && method <= 0x924u && ((method - 0x904u) % 8u) == 0u) {
        const unsigned stage = (method - 0x904u) / 8u;
        const unsigned slot = (value >> 4) & 31u;
        vp_gpu_subchannel *s = &state->sub[sub];
        if (slot < VP_GPU_CBUF_SLOTS) {
            vp_gpu_cbuf_binding *binding = &s->cbuf[stage][slot];
            binding->enabled = (value & 1u) != 0u;
            binding->address = ((uint64_t)s->value[0x8e1u] << 32) | s->value[0x8e2u];
            binding->size = s->value[0x8e0u];
        }
    }
}

int vp_gpu_state_snapshot(const vp_gpu_state *state, uint8_t sub,
                          uint32_t index, int indexed, vp_gpu_draw_state *out)
{
    const vp_gpu_subchannel *s;
    uint32_t i;

    if (state == NULL || out == NULL || sub >= VP_GPU_SUBCHANNELS) return -1;
    memset(out, 0, sizeof(*out));
    s = &state->sub[sub];
    out->index = index;
    out->sub = sub;
    out->indexed = indexed;
    out->draw_begin = s->value[0x586u];
    out->vertex_start = s->value[0x35du];
    out->vertex_count = s->value[0x35eu];
    out->index_address = ((uint64_t)s->value[0x5f2u] << 32) | s->value[0x5f3u];
    out->index_format = s->value[0x5f6u];
    out->index_first = s->value[0x5f7u];
    out->index_count = s->value[0x5f8u];

    for (i = 0; i < VP_GPU_VERTEX_STREAMS; i++) {
        uint32_t base = 0x700u + 4u * i;
        if (!s->valid[base] && !s->valid[base + 1u] && !s->valid[base + 2u]) continue;
        out->streams[i].stride = s->value[base] & 0xfffu;
        out->streams[i].address = ((uint64_t)s->value[base + 1u] << 32) | s->value[base + 2u];
        out->streams[i].enabled = ((s->value[base] >> 12) & 1u) != 0u &&
                                  out->streams[i].address != 0u;
    }

    for (i = 0; i < VP_GPU_RENDER_TARGETS; i++) {
        uint32_t base = 0x200u + 16u * i;
        if (!s->valid[base] && !s->valid[base + 2u]) continue;
        out->targets[i].address = ((uint64_t)s->value[base] << 32) | s->value[base + 1u];
        out->targets[i].width = s->value[base + 2u];
        out->targets[i].height = s->value[base + 3u];
        out->targets[i].format = s->value[base + 4u];
        out->targets[i].tile_mode = s->value[base + 5u];
        if (out->targets[i].address != 0u && out->targets[i].width != 0u &&
            out->targets[i].height != 0u) {
            out->target_count++;
        }
    }

    out->rt_control = s->value[0x487u];
    out->rt_control_observed = s->valid[0x487u];
    out->color_mask_raw = s->value[0x680u];
    /* ColorMaskCommon is the byte register 0x0F90, decoded as word method
     * 0x3E4. 0xF90 is not its word-method index. */
    out->color_mask_common = s->value[0x3e4u];
    out->active_rt_index = 0u;
    out->color_attachment = 0u;
    out->color_mrt_unsupported = 0u;
    out->color_mapping_unsupported = 0u;
    /* Effective colour attachment. Reference semantics (yuzu
     * maxwell_3d.h RenderTargetConfig/RtControl + texture_cache
     * FindColorBuffer): an RT is active only when RT_CONTROL selects it and
     * its configuration has a nonzero address/extent and a format other than
     * NONE (0). A stale address and a nonzero color mask alone do not make a
     * colour target. RT_CONTROL count is bits 0..3; the RT index for draw
     * buffer 0 is bits 4..6. Only one colour target exists in this backend. */
    {
        unsigned count = 0u, map0 = 0u;
        int target_valid;
        if (out->rt_control_observed) {
            count = out->rt_control & 0xfu;
            map0 = (out->rt_control >> 4) & 7u;
            out->active_rt_index = map0;
        }
        target_valid = out->active_rt_index < VP_GPU_RENDER_TARGETS &&
            out->targets[out->active_rt_index].address != 0u &&
            out->targets[out->active_rt_index].width != 0u &&
            out->targets[out->active_rt_index].height != 0u &&
            out->targets[out->active_rt_index].format != 0u;
        if (out->rt_control_observed) {
            if (count == 0u) {
                /* No colour buffer is bound; the raw RT/mask fields are stale. */
            } else if (count > 1u) {
                out->color_mrt_unsupported = 1u;
            } else if (target_valid) {
                if (map0 != 0u && out->color_mask_common == 0u) {
                    /* Per-target masks are not decoded for mapped RTs, so this
                     * colour draw has no faithful colour-mask replay. */
                    out->color_mapping_unsupported = 1u;
                } else {
                    out->color_attachment = 1u;
                    out->color_mask = out->color_mask_raw;
                }
            }
        } else if (target_valid) {
            /* RT_CONTROL has not been observed in this subchannel yet. The
             * format-NONE rule still applies; otherwise retain the legacy
             * address/extent classification explicitly. */
            out->color_attachment = 1u;
            out->color_mask = out->color_mask_raw;
        }
    }

    out->viewport_x = s->value[0x300u] & 0xffffu;
    out->viewport_width = (s->value[0x300u] >> 16) & 0xffffu;
    out->viewport_y = s->value[0x301u] & 0xffffu;
    out->viewport_height = (s->value[0x301u] >> 16) & 0xffffu;
    out->viewport_scale_z = s->value[0x282u];
    out->viewport_translate_z = s->value[0x285u];
    out->viewport_z_valid = (uint8_t)(s->valid[0x282u] && s->valid[0x285u] &&
                                      (s->value[0x64bu] & 1u));
    out->depth_mode = s->value[0x35fu];

    out->depth_test_enable = s->value[0x4b3u];
    out->depth_write_enable = s->value[0x4bau];
    out->depth_func = s->value[0x4c3u];
    out->blend_per_target_enabled = s->value[0x4b9u];
    out->cull_enable = s->value[0x646u];
    out->front_face = s->value[0x647u];
    out->cull_face = s->value[0x648u];
    out->blend_color_op = s->value[0x781u];
    out->blend_color_src = s->value[0x782u];
    out->blend_color_dst = s->value[0x783u];
    out->blend_alpha_op = s->value[0x784u];
    out->blend_alpha_src = s->value[0x785u];
    out->blend_alpha_dst = s->value[0x786u];

    for (i = 0; i < VP_GPU_CONST_WORDS; i++) {
        uint16_t method = (uint16_t)(0x8e0u + i);
        out->const_buffer_valid[i] = s->valid[method];
        out->const_buffer[i] = s->value[method];
        if (s->valid[method]) out->const_buffer_count++;
    }
    return 0;
}

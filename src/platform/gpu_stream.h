/* Portable Maxwell command-stream and MME macro decoder.
 *
 * This is the target-side counterpart of tools/decode_maxwell_stream.ps1 and
 * tools/emulate_mme_calls.ps1. It only transforms captured words: nothing here
 * touches guest memory, the GPU or a host OS.
 */
#ifndef VP_GPU_STREAM_H
#define VP_GPU_STREAM_H

#include <stddef.h>
#include <stdint.h>

#define VP_GPU_METHOD_MASK 0x1fffu
#define VP_GPU_REG_LIMIT 0x1000u
#define VP_MME_REG_COUNT 8u
#define VP_MME_MAX_STEPS 2048u
#define VP_MME_REASON_BYTES 32u

typedef struct {
    uint16_t method;
    uint8_t sub;
    uint32_t value;
} vp_gpu_write;

/* One complete GPFIFO entry, with room for every decoded write and macro
 * argument. Storage grows together so a valid entry is never truncated. */
typedef struct {
    void *storage;
    size_t capacity;
    uint32_t *words;
    uint32_t *params;
    vp_gpu_write *writes;
} vp_gpu_command_buffer;
int vp_gpu_command_buffer_reserve(vp_gpu_command_buffer *buffer, size_t words);
void vp_gpu_command_buffer_free(vp_gpu_command_buffer *buffer);

typedef void (*vp_gpu_write_fn)(void *ctx, const vp_gpu_write *write);
typedef void (*vp_gpu_send_fn)(void *ctx, uint16_t address, uint32_t value);

typedef struct {
    size_t writes;
    size_t unparsed;
} vp_gpu_decode_stats;

/* Decode command headers into register writes, reporting each write through a
 * callback. Returns 0 on success. Kept as the streaming reference decoder. */
int vp_gpu_decode(const uint32_t *words, size_t count, vp_gpu_write_fn fn,
                  void *ctx, vp_gpu_decode_stats *stats);

/* Decode the complete stream into writes[0..capacity). The whole packet is
 * capacity-checked before any of its words are emitted, so a short buffer or a
 * truncated packet returns -1 without writing past capacity. On success the
 * caller may execute the collected writes; on failure no prefix is a complete
 * decode and the caller must not dispatch it. */
int vp_gpu_decode_collect(const uint32_t *words, size_t count,
                          vp_gpu_write *writes, size_t capacity,
                          vp_gpu_decode_stats *stats);

typedef struct {
    uint32_t ip;
    uint32_t *code;
    size_t code_count;
    size_t code_capacity;
} vp_macro_program;

typedef struct {
    vp_macro_program *programs;
    size_t program_count;
    size_t program_capacity;
    uint32_t *positions;
    size_t position_count;
    size_t position_capacity;
} vp_macro_table;

typedef struct {
    uint32_t ip;
    uint32_t sp;
} vp_macro_upload_state;

void vp_macro_table_init(vp_macro_table *table);
void vp_macro_table_free(vp_macro_table *table);
void vp_macro_upload_state_init(vp_macro_upload_state *state);

/* Apply one 0x45-0x48 upload word; other methods are ignored. */
int vp_macro_upload_apply(vp_macro_table *table, vp_macro_upload_state *state,
                          uint16_t method, uint32_t value);

/* Rebuild the macro programs and position table from a channel-init stream. */
int vp_macro_upload(const uint32_t *init_words, size_t init_count,
                    vp_macro_table *table);

/* 64-bit hash used by the reference runtime's macro HLE lookup. */
uint64_t vp_macro_hash(const uint32_t *code, size_t count);

typedef struct {
    uint32_t regs[VP_GPU_REG_LIMIT];
    uint8_t valid[VP_GPU_REG_LIMIT];
    size_t read_misses;
} vp_mme_shared;

void vp_mme_shared_init(vp_mme_shared *shared);
void vp_mme_shared_write(vp_mme_shared *shared, uint16_t method, uint32_t value);

typedef struct {
    int supported;
    char reason[VP_MME_REASON_BYTES];
    size_t sends;
    size_t draws;
    size_t clears;
} vp_mme_result;

/* Execute one macro call. shared is the stream-wide register file; sends are
 * reported through send_fn and counted as draws/clears by method address. */
int vp_mme_execute(const vp_macro_table *table, uint32_t position,
                   const uint32_t *params, size_t param_count,
                   vp_mme_shared *shared, vp_gpu_send_fn send_fn, void *send_ctx,
                   vp_mme_result *result);

/* Per-subchannel register file used to snapshot the state a draw sees. */
#define VP_GPU_SUBCHANNELS 8u
#define VP_GPU_VERTEX_STREAMS 32u
#define VP_GPU_RENDER_TARGETS 8u
#define VP_GPU_CONST_WORDS 6u
#define VP_GPU_SHADER_STAGES 5u
#define VP_GPU_CBUF_SLOTS 18u

typedef struct {
    uint64_t address;
    uint32_t size;
    int enabled;
} vp_gpu_cbuf_binding;

typedef struct {
    uint32_t value[VP_GPU_REG_LIMIT];
    uint8_t valid[VP_GPU_REG_LIMIT];
    vp_gpu_cbuf_binding cbuf[VP_GPU_SHADER_STAGES][VP_GPU_CBUF_SLOTS];
} vp_gpu_subchannel;

typedef struct {
    vp_gpu_subchannel sub[VP_GPU_SUBCHANNELS];
} vp_gpu_state;

void vp_gpu_state_init(vp_gpu_state *state);
void vp_gpu_state_apply(vp_gpu_state *state, uint8_t sub, uint16_t method,
                        uint32_t value);

typedef struct {
    int enabled;
    uint32_t stride;
    uint64_t address;
} vp_gpu_vertex_stream;

typedef struct {
    uint64_t address;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t tile_mode;
} vp_gpu_render_target;

typedef struct {
    uint32_t stage;
    uint32_t hash;
    uint64_t address;
} vp_gpu_shader_bind;

typedef struct {
    uint32_t index;
    uint8_t sub;
    int indexed;
    uint32_t draw_begin;   /* 0x586 */
    uint32_t vertex_start; /* 0x35d */
    uint32_t vertex_count; /* 0x35e */
    uint64_t index_address; /* 0x5f2/0x5f3 */
    uint32_t index_format;  /* 0x5f6 */
    uint32_t index_first;   /* 0x5f7 */
    uint32_t index_count;   /* 0x5f8 */
    vp_gpu_vertex_stream streams[VP_GPU_VERTEX_STREAMS];
    vp_gpu_render_target targets[VP_GPU_RENDER_TARGETS];
    uint32_t target_count;   /* decoded configurations, not active targets */
    /* Effective colour attachment 0. Maxwell selects the render target through
     * RT_CONTROL (0x487): count is bits 0..3 and target0 (the RT index used by
     * draw buffer 0) is bits 4..6. A configuration with format NONE (0) or a
     * zero address/extent is not an attachment, even if its stale fields and
     * the raw colour mask remain populated. */
    uint32_t rt_control;     /* 0x487 raw */
    uint32_t active_rt_index;
    uint32_t color_mask_raw; /* 0x680 raw */
    uint32_t color_mask_common; /* 0xf90 raw: nonzero selects color_mask[0] */
    uint8_t rt_control_observed;
    uint8_t color_attachment; /* 1 when active RT0 is a real colour target */
    uint8_t color_mrt_unsupported;     /* RT count > 1, no MRT path */
    uint8_t color_mapping_unsupported; /* mapped RT/mask cannot be honoured */
    uint32_t viewport_x;
    uint32_t viewport_y;
    uint32_t viewport_width;
    uint32_t viewport_height;
    /* Maxwell viewport_transform[0] z (0x282/0x285, float bits) and depth
     * mode (0x35f). The window-depth transform is the guest's own state; the
     * executor must not substitute the GXM/OpenGL default 0.5/0.5 range. */
    uint32_t viewport_scale_z;
    uint32_t viewport_translate_z;
    uint8_t viewport_z_valid;
    uint32_t depth_mode;
    uint32_t depth_test_enable;
    uint32_t depth_write_enable;
    uint32_t depth_func;
    uint32_t blend_per_target_enabled;
    uint32_t cull_enable;
    uint32_t front_face;    /* 0x647: 0x900 = CW, 0x901 = CCW */
    uint32_t cull_face;     /* 0x648: 0x404 = front, 0x405 = back, 0x408 = both */
    uint32_t color_mask;    /* 0x680: RT0 R/G/B/A at bits 0/4/8/12 */
    uint32_t blend_color_op;     /* 0x781 */
    uint32_t blend_color_src;    /* 0x782 */
    uint32_t blend_color_dst;    /* 0x783 */
    uint32_t blend_alpha_op;     /* 0x784 */
    uint32_t blend_alpha_src;    /* 0x785 */
    uint32_t blend_alpha_dst;    /* 0x786 */
    vp_gpu_shader_bind shaders[2];
    uint32_t shader_count;
    uint32_t const_buffer[VP_GPU_CONST_WORDS];
    uint8_t const_buffer_valid[VP_GPU_CONST_WORDS];
    uint8_t const_buffer_count;
} vp_gpu_draw_state;

/* Fill the typed fields of one draw from the current register file. */
int vp_gpu_state_snapshot(const vp_gpu_state *state, uint8_t sub,
                          uint32_t index, int indexed, vp_gpu_draw_state *out);

#endif

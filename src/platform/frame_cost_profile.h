#ifndef VP_FRAME_COST_PROFILE_H
#define VP_FRAME_COST_PROFILE_H

#include <stddef.h>
#include <stdint.h>

/* Bounded nested wall-time accounting for the single GXM worker.
 *
 * This module is pure: every timestamp is supplied by the caller, so host
 * tests can drive exact begin/end sequences. The runtime layer only calls it
 * from the GXM worker; it never adds a host clock call itself and never opens
 * a file. Categories are fixed so a snapshot has a deterministic row set.
 *
 * A category's inclusive_us contains all nested spans recorded on the stack.
 * self_us subtracts the inclusive time of direct children only, so nested and
 * recursive categories do not inflate the parent's self time. root_* count
 * spans that started with an empty stack; those are the only spans that may be
 * subtracted from sampled frame wall. */

#define VP_FRAME_COST_SCHEMA 3u
#define VP_FRAME_COST_STACK 32u
#define VP_FRAME_COST_CATEGORY_COUNT 22u

enum {
    VP_FRAME_COST_ENTRY = 0,
    VP_FRAME_COST_DISPATCH,
    VP_FRAME_COST_MME,
    VP_FRAME_COST_DRAW_SUBMIT,
    VP_FRAME_COST_CLEAR_SUBMIT,
    VP_FRAME_COST_TEX_LOOKUP,
    VP_FRAME_COST_TEX_STORE,
    VP_FRAME_COST_TEX_DECODE,
    VP_FRAME_COST_TEX_TAIL,
    VP_FRAME_COST_GXM_DRAW,
    VP_FRAME_COST_GXM_CLEAR,
    VP_FRAME_COST_GXM_FLUSH,
    VP_FRAME_COST_WAIT_NOTIFY,
    VP_FRAME_COST_WAIT_FINISH,
    VP_FRAME_COST_DRAW_STATE,
    VP_FRAME_COST_DRAW_INDEX,
    VP_FRAME_COST_DRAW_CBUF,
    VP_FRAME_COST_DRAW_RESOURCES,
    VP_FRAME_COST_DRAW_FINAL,
    VP_FRAME_COST_GXM_REQUEST,
    VP_FRAME_COST_WORKER_TAIL,
    VP_FRAME_COST_LOG
};

typedef struct {
    unsigned category;
    uint64_t started;
} VPFrameCostPhase;

extern const char *const vp_frame_cost_category_names[VP_FRAME_COST_CATEGORY_COUNT];

typedef struct {
    uint64_t calls;
    uint64_t inclusive_us;
    uint64_t self_us;
    uint64_t max_us;
    /* Spans that began with an empty stack. Root spans never overlap each
     * other, so their sum is the measured, non-duplicated worker time. */
    uint64_t root_calls;
    uint64_t root_inclusive_us;
} VPFrameCostCategory;

typedef struct {
    unsigned category;
    uint64_t start_us;
    uint64_t child_us;
} VPFrameCostSpan;

typedef struct {
    VPFrameCostCategory categories[VP_FRAME_COST_CATEGORY_COUNT];
    VPFrameCostSpan stack[VP_FRAME_COST_STACK];
    unsigned depth;
    /* Explicit accounting failures. A report with any non-zero value is not
     * valid closure data and must not be accepted as a measurement. */
    uint64_t mismatch;
    uint64_t overflow;
    uint64_t underflow;
    uint64_t clock_regression;
    uint64_t missing_time;
    /* A snapshot was requested while a span was still open. That snapshot is
     * never emitted as closure data. */
    uint64_t incomplete_snapshot;
    /* Timestamp calls made by the runtime for spans in this epoch. */
    uint64_t clock_calls;
} VPFrameCostProfile;

void vp_frame_cost_profile_reset(VPFrameCostProfile *profile);
/* Returns 1 when a span was pushed, 0 when now_us is zero (the runtime gate)
 * or the bounded stack overflowed. */
int vp_frame_cost_span_begin(VPFrameCostProfile *profile, unsigned category,
                             uint64_t now_us);
/* Returns 0 when the top span matched and was closed, -1 otherwise. A
 * mismatch does not pop the stack so the caller's error remains visible. */
int vp_frame_cost_span_end(VPFrameCostProfile *profile, unsigned category,
                           uint64_t now_us);
uint64_t vp_frame_cost_error_count(const VPFrameCostProfile *profile);

/* Strict control file reader: exactly one '0' or '1' followed only by ASCII
 * whitespace. Missing digits, extra digits and other bytes are invalid. */
int vp_frame_cost_parse_mode(const char *text, size_t length, unsigned *mode);

#endif

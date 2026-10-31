/* B112 host checks for the direct command-collector decoder.
 *
 * vp_gpu_decode remains the streaming oracle: for every well-formed stream the
 * collector must produce the same ordered writes and stats. Failure cases
 * prove that a truncated packet or a short output buffer never writes past
 * capacity and that a production-style caller dispatches nothing unless the
 * whole entry decoded. */
#include "gpu_stream.h"

#include <stdio.h>
#include <string.h>

#define MAX_WRITES 64u
#define GUARD_WRITES (MAX_WRITES + 4u)

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

typedef struct {
    vp_gpu_write items[MAX_WRITES];
    size_t count;
    size_t capacity;
} Sink;

static void sink_write(void *opaque, const vp_gpu_write *write) {
    Sink *sink = opaque;
    CHECK(sink->count < sink->capacity);
    if (sink->count < sink->capacity) sink->items[sink->count++] = *write;
}

static uint32_t packet(uint32_t mode, uint32_t method, uint32_t sub,
                       uint32_t args) {
    return (mode << 29) | ((args & 0x1fffu) << 16) |
           ((sub & 7u) << 13) | (method & 0x1fffu);
}

static void same_write(const vp_gpu_write *a, const vp_gpu_write *b) {
    CHECK(a->method == b->method);
    CHECK(a->sub == b->sub);
    CHECK(a->value == b->value);
}

/* Compare one successful stream against the streaming oracle. */
static void same_as_oracle(const uint32_t *words, size_t count) {
    Sink sink = {.count = 0u, .capacity = MAX_WRITES};
    vp_gpu_write collected[GUARD_WRITES];
    vp_gpu_decode_stats oracle_stats, collect_stats;
    size_t i;
    int oracle_rc, collect_rc;

    memset(collected, 0x5a, sizeof(collected));
    oracle_rc = vp_gpu_decode(words, count, sink_write, &sink, &oracle_stats);
    collect_rc = vp_gpu_decode_collect(words, count, collected, MAX_WRITES,
                                       &collect_stats);
    CHECK(oracle_rc == 0);
    CHECK(collect_rc == 0);
    CHECK(oracle_stats.writes == sink.count);
    CHECK(collect_stats.writes == sink.count);
    CHECK(collect_stats.unparsed == oracle_stats.unparsed);
    CHECK(collect_stats.writes <= MAX_WRITES);
    for (i = 0; i < sink.count; i++) same_write(&collected[i], &sink.items[i]);
    for (; i < GUARD_WRITES; i++)
        CHECK(collected[i].method == (uint16_t)0x5a5a);
}

static void test_modes_and_oracle(void) {
    const uint32_t words[] = {
        packet(0u, 0x360u, 2u, 3u),
        0x11u, 0x22u, 0x33u,
        packet(1u, 0x1fffu, 7u, 3u),          /* uint16 method arithmetic */
        0x44u, 0x55u, 0x66u,
        packet(2u, 0x8e0u, 1u, 2u),           /* non-incrementing */
        0x77u, 0x88u,
        packet(3u, 0x123u, 4u, 2u),           /* non-incrementing */
        0x99u, 0xaau,
        packet(4u, 0x585u, 5u, 0xfeedu),      /* immediate */
        packet(5u, 0xe00u, 3u, 4u),           /* first, then +1 */
        0x1u, 0x2u, 0x3u, 0x4u,
        packet(5u, 0x1ffeu, 1u, 3u),          /* increment past 0x1fff */
        0xbbu, 0xccu, 0xddu,
        packet(6u, 0x111u, 0u, 0u),           /* unparsed */
        packet(7u, 0x222u, 0u, 0u),
        packet(0u, 0x470u, 0u, 0u),           /* zero-argument packet */
    };
    same_as_oracle(words, sizeof(words) / sizeof(words[0]));
}

static void test_mixed_mme_args(void) {
    const uint32_t words[] = {
        packet(4u, 0xe00u, 2u, 0x300u),
        packet(5u, 0xe01u, 2u, 3u), 0xa1u, 0xa2u, 0xa3u,
        packet(0u, 0xe04u, 2u, 2u), 0xa4u, 0xa5u,
        packet(2u, 0xe06u, 2u, 2u), 0xa6u, 0xa7u,
    };
    same_as_oracle(words, sizeof(words) / sizeof(words[0]));
}

static void test_empty_and_unknown(void) {
    vp_gpu_decode_stats stats;
    const uint32_t unknown[] = {
        packet(6u, 0u, 0u, 0u), packet(7u, 0x1fffu, 7u, 0x1fffu),
    };

    CHECK(vp_gpu_decode_collect(NULL, 0u, NULL, 0u, &stats) == 0);
    CHECK(stats.writes == 0u && stats.unparsed == 0u);
    CHECK(vp_gpu_decode_collect(unknown, 2u, NULL, 0u, &stats) == 0);
    CHECK(stats.writes == 0u && stats.unparsed == 2u);
    CHECK(vp_gpu_decode_collect(NULL, 0u, NULL, 0u, NULL) == 0);
    CHECK(vp_gpu_decode_collect(NULL, 1u, NULL, 0u, &stats) < 0);
    CHECK(vp_gpu_decode_collect(unknown, 1u, NULL, 1u, &stats) < 0);
}

static void test_capacity(void) {
    const uint32_t words[] = {
        packet(0u, 0x300u, 0u, 3u), 1u, 2u, 3u,
        packet(4u, 0x585u, 0u, 1u),
    };
    const uint32_t first_packet_only[] = {
        packet(0u, 0x300u, 0u, 3u), 1u, 2u, 3u,
    };
    const uint32_t immediate[] = {
        packet(4u, 0x585u, 0u, 1u),
    };
    vp_gpu_write out[GUARD_WRITES];
    vp_gpu_write exact[GUARD_WRITES];
    vp_gpu_decode_stats stats, exact_stats;
    size_t i;

    memset(out, 0x5a, sizeof(out));
    CHECK(vp_gpu_decode_collect(words, 5u, out, 4u, &stats) == 0);
    CHECK(stats.writes == 4u && stats.unparsed == 0u);
    CHECK(vp_gpu_decode_collect(words, 5u, exact, 4u, &exact_stats) == 0);
    CHECK(exact_stats.writes == 4u);
    for (i = 0; i < 4u; i++) same_write(&out[i], &exact[i]);

    /* Short by one: the trailing immediate packet is refused before it is
     * written, while the first packet (three writes) still fits. */
    memset(out, 0x5a, sizeof(out));
    CHECK(vp_gpu_decode_collect(words, 5u, out, 3u, &stats) < 0);
    CHECK(stats.writes == 3u);
    for (i = 3u; i < GUARD_WRITES; i++)
        CHECK(out[i].method == (uint16_t)0x5a5a);

    /* A packet that cannot fit is refused before any of its words are
     * emitted, so the short buffer stays untouched. */
    memset(out, 0x5a, sizeof(out));
    CHECK(vp_gpu_decode_collect(first_packet_only, 4u, out, 2u, &stats) < 0);
    CHECK(stats.writes == 0u);
    for (i = 0; i < GUARD_WRITES; i++)
        CHECK(out[i].method == (uint16_t)0x5a5a);

    /* Capacity zero is valid only when the stream emits no writes. */
    CHECK(vp_gpu_decode_collect(NULL, 0u, NULL, 0u, &stats) == 0);
    CHECK(vp_gpu_decode_collect(immediate, 1u, NULL, 0u, &stats) < 0);
    CHECK(vp_gpu_decode_collect(words, 5u, out, 0u, &stats) < 0);
}

static int collect_and_dispatch(const uint32_t *words, size_t count,
                                size_t capacity, size_t *dispatched) {
    vp_gpu_write writes[MAX_WRITES];
    vp_gpu_decode_stats stats;
    int rc = vp_gpu_decode_collect(words, count, writes, capacity, &stats);
    *dispatched = rc == 0 ? stats.writes : 0u;
    return rc;
}

static void test_truncated_tail(void) {
    vp_gpu_write collected[GUARD_WRITES];
    vp_gpu_decode_stats oracle_stats, collect_stats;
    size_t dispatched = 0u;
    const uint32_t truncated_modes[] = {
        packet(0u, 0x300u, 0u, 2u), 0x31u,
        packet(5u, 0x400u, 0u, 2u), 0x41u,
        packet(5u, 0x500u, 0u, 0u),
        packet(3u, 0x600u, 0u, 0x1fffu), 0x61u,
    };
    const uint32_t prefix_then_tail[] = {
        packet(4u, 0x700u, 0u, 0x71u),
        packet(0u, 0x300u, 0u, 2u), 0x31u,
    };
    size_t i;

    /* Each malformed packet fails in both decoders after the same prefix. */
    memset(collected, 0, sizeof(collected));
    {
        Sink sink = {.count = 0u, .capacity = MAX_WRITES};
        CHECK(vp_gpu_decode(truncated_modes, 2u, sink_write, &sink,
                            &oracle_stats) < 0);
    }
    CHECK(vp_gpu_decode_collect(truncated_modes, 2u, collected, MAX_WRITES,
                                &collect_stats) < 0);
    CHECK(collect_stats.writes == oracle_stats.writes);
    CHECK(collect_and_dispatch(truncated_modes, 2u, MAX_WRITES,
                               &dispatched) < 0);
    CHECK(dispatched == 0u);
    CHECK(vp_gpu_decode_collect(truncated_modes + 2, 2u, collected,
                                MAX_WRITES, &collect_stats) < 0);
    CHECK(collect_stats.writes == 0u);
    CHECK(vp_gpu_decode_collect(truncated_modes + 2, 1u, collected,
                                MAX_WRITES, &collect_stats) < 0);
    CHECK(collect_stats.writes == 0u);
    CHECK(vp_gpu_decode_collect(truncated_modes + 5, 2u, collected,
                                MAX_WRITES, &collect_stats) < 0);
    CHECK(collect_stats.writes == 0u);

    /* A valid prefix followed by a malformed tail compares to the oracle and
     * still dispatches nothing as a whole entry. */
    {
        Sink sink = {.count = 0u, .capacity = MAX_WRITES};
        CHECK(vp_gpu_decode(prefix_then_tail, 3u, sink_write, &sink,
                            &oracle_stats) < 0);
        CHECK(vp_gpu_decode_collect(prefix_then_tail, 3u, collected,
                                    MAX_WRITES, &collect_stats) < 0);
        CHECK(collect_stats.writes == sink.count);
        for (i = 0; i < sink.count; i++)
            same_write(&collected[i], &sink.items[i]);
        CHECK(collect_and_dispatch(prefix_then_tail, 3u, MAX_WRITES,
                                   &dispatched) < 0);
        CHECK(dispatched == 0u);
    }
}

int main(void) {
    test_modes_and_oracle();
    test_mixed_mme_args();
    test_empty_and_unknown();
    test_capacity();
    test_truncated_tail();
    if (failures != 0) {
        fprintf(stderr, "gpu stream collect: %d failure(s)\n", failures);
        return 1;
    }
    puts("PASS gpu stream collect: modes/oracle/empty/truncation/capacity/"
         "mme-order/unparsed");
    return 0;
}

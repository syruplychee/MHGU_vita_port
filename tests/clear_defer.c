/* Host regression for the mode-6 deferred-clear contract.
 *
 * The production bodies are extracted verbatim from
 * src/platform/gxm_runtime.c by tests/CMakeLists.txt. This file supplies only
 * the GXM/log/completion stubs and fixtures; it does not reimplement the
 * admission, record, materialization or in-scene consumption decisions. */
#include "gxm_runtime.h"
#include "clear_fusion.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Minimal stand-ins for the VitaSDK surface handles stored in VPGxmSurface.
 * The extracted code never inspects them. */
typedef struct { unsigned opaque; } SceGxmColorSurface;
typedef struct { unsigned opaque; } SceGxmTexture;

#include "clear_surface_production.inc"

/* Counters and helpers the extracted production sections reference. */
static uint64_t gxm_clear_fusion_deferred;
static uint64_t gxm_clear_fusion_in_scene;
static uint64_t gxm_clear_fusion_materialized;
static uint64_t gxm_clear_defer_accepts;
static uint64_t gxm_clear_defer_scene_submits;
static uint64_t gxm_clear_defer_submit_failures;
static uint64_t gxm_clear_defer_coalesced;
static uint64_t gxm_clear_defer_materialize_calls;
static uint64_t gxm_clear_defer_materialize_failures;
static uint64_t gxm_clear_defer_in_scene;

static void glog(const char *format, ...) { (void)format; }

/* Ordered-event probes. Every stub appends its marker so the tests can prove
 * the submission-before-record and drain-before-write order. */
enum {
    EVENT_FLUSH = 1,
    EVENT_PENDING_WAIT = 2,
    EVENT_SURFACE_CLEAR = 3
};
static unsigned test_events[8];
static unsigned test_event_count;
static VPGxmSurface *test_flush_surface;
static unsigned test_flush_pending_before;
static uint32_t test_flush_color_before;
static int test_flush_rc;
static unsigned test_flush_reason;
static const char *test_pending_why;
static int test_pending_rc;

static void event_push(unsigned event) {
    assert(test_event_count < sizeof(test_events) / sizeof(test_events[0]));
    test_events[test_event_count++] = event;
}

static void events_reset(void) {
    test_event_count = 0u;
    test_flush_rc = 0;
    test_flush_reason = 0u;
    test_pending_why = NULL;
    test_pending_rc = 0;
    test_flush_surface = NULL;
    test_flush_pending_before = 0u;
    test_flush_color_before = 0u;
}

static int gxm_batch_flush_worker(unsigned reason) {
    event_push(EVENT_FLUSH);
    test_flush_reason = reason;
    if (test_flush_surface != NULL) {
        /* Snapshot the surface state at flush time. The tests compare it with
         * the state after the call: the scene submission must precede every
         * clear-metadata update of this request. */
        test_flush_pending_before = test_flush_surface->clear_pending;
        test_flush_color_before = test_flush_surface->clear_pending_color;
    }
    return test_flush_rc;
}

static int gxm_pending_scene_complete(const char *why) {
    event_push(EVENT_PENDING_WAIT);
    test_pending_why = why;
    return test_pending_rc;
}

static unsigned test_clear_mask;
static uint32_t test_clear_color;
static int test_surface_clear_calls;
static int surface_clear(VPGxmSurface *s, uint32_t mask, uint32_t color) {
    (void)s;
    event_push(EVENT_SURFACE_CLEAR);
    test_clear_mask = mask;
    test_clear_color = color;
    test_surface_clear_calls++;
    return 0;
}

#include "clear_defer_production.inc"

static VPGxmClearRequest request_full_colour(void) {
    VPGxmClearRequest req;
    memset(&req, 0, sizeof(req));
    req.target_va = 0x12345000ull;
    req.width = 640u;
    req.height = 360u;
    req.format = 0xd5u;
    req.mask = 0x3cu;
    req.color = 0xff112233u;
    return req;
}

static VPGxmSurface matching_surface(void) {
    VPGxmSurface s;
    memset(&s, 0, sizeof(s));
    s.used = 1;
    s.va = 0x12345000ull;
    s.width = 640u;
    s.height = 360u;
    s.format = 0xd5u;
    s.color_mem = &s; /* Non-NULL placeholder; never dereferenced here. */
    return s;
}

static void test_reason_and_sync_classification(void) {
    assert(GXM_BATCH_REASON_CLEAR_DEFER < GXM_BATCH_REASON_COUNT);
    assert(GXM_BATCH_REASON_CLEAR_DEFER != GXM_BATCH_REASON_CLEAR);
    assert(strcmp(gxm_batch_reason_name(GXM_BATCH_REASON_CLEAR_DEFER),
                  "clear-defer") == 0);
    assert(gxm_batch_reason_needs_sync(GXM_BATCH_REASON_CLEAR_DEFER) == 0);
    assert(gxm_batch_reason_needs_sync(GXM_BATCH_REASON_CLEAR) == 1);
    assert(gxm_batch_reason_needs_sync(GXM_BATCH_REASON_SUBMIT) == 1);
}

static void test_admission_accepts_and_rejects(void) {
    VPGxmClearRequest req = request_full_colour();
    const unsigned w = req.width, h = req.height;
    const uint32_t f = req.format;

    /* Full colour, depth-free, matching surface, no diagnostics. */
    assert(clear_defer_admit(&req, w, h, f, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0));
    /* Coalescing an older pending colour stays admissible. */
    assert(clear_defer_admit(&req, w, h, f, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0));
    /* Every refusal keeps the original synchronous path. */
    assert(!clear_defer_admit(&req, w, h, f, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0));
    assert(!clear_defer_admit(&req, w, h, f, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0));
    assert(!clear_defer_admit(&req, w, h, f, 1, 0, 1, 1, 1, 0, 0, 1, 1, 0));
    assert(!clear_defer_admit(&req, w, h, f, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0));
    assert(!clear_defer_admit(&req, w, h, f, 1, 0, 1, 1, 0, 0, 1, 1, 1, 0));
    assert(!clear_defer_admit(&req, w, h, f, 1, 0, 1, 1, 0, 0, 0, 0, 1, 0));
    assert(!clear_defer_admit(&req, w, h, f, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0));
    assert(!clear_defer_admit(&req, w, h, f, 1, 0, 1, 1, 0, 0, 0, 1, 1, 1));
    assert(!clear_defer_admit(&req, w, h, f, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0));
    assert(!clear_defer_admit(&req, w + 1u, h, f, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0));
    assert(!clear_defer_admit(&req, w, h + 1u, f, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0));
    assert(!clear_defer_admit(&req, w, h, f ^ 1u, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0));

    /* Partial colour, combined depth/stencil, and out-of-range mask bits. */
    {
        const uint32_t bad[] = {0x3du, 0x3eu, 0x3fu, 0x3bu, 0x1cu, 0x00u,
                                0x103cu};
        for (unsigned i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            req.mask = bad[i];
            assert(!clear_defer_admit(&req, w, h, f, 1, 0, 1, 1, 0, 0, 0,
                                      1, 1, 0));
        }
    }
}

static void test_submit_before_record_and_coalesce(void) {
    VPGxmClearRequest req = request_full_colour();
    VPGxmSurface s = matching_surface();
    unsigned events_before;

    events_reset();
    events_before = test_event_count;
    test_flush_surface = &s;
    test_flush_rc = 0;
    assert(clear_defer_submit_and_record(&req, &s, 1) == 0);
    /* The open scene was submitted through the dedicated non-sync reason
     * before any clear metadata changed. */
    assert(test_event_count == events_before + 1u);
    assert(test_events[events_before] == EVENT_FLUSH);
    assert(test_flush_reason == GXM_BATCH_REASON_CLEAR_DEFER);
    assert(test_flush_pending_before == 0u);
    assert(s.clear_pending == 1u);
    assert(s.clear_pending_async == 1u);
    assert(s.clear_pending_color == req.color);
    assert(gxm_clear_defer_scene_submits == 1u);
    assert(gxm_clear_defer_accepts == 1u);

    /* A second full clear coalesces; the newest colour wins and the async tag
     * remains until a materialization point consumes it. The flush still runs
     * before the metadata update and therefore sees the older value. */
    req.color = 0xff445566u;
    assert(clear_defer_submit_and_record(&req, &s, 1) == 0);
    assert(test_event_count == events_before + 2u);
    assert(test_events[events_before + 1u] == EVENT_FLUSH);
    assert(test_flush_pending_before == 1u);
    assert(test_flush_color_before == 0xff112233u);
    assert(s.clear_pending == 1u);
    assert(s.clear_pending_async == 1u);
    assert(s.clear_pending_color == req.color);
    assert(gxm_clear_defer_coalesced == 1u);
    assert(gxm_clear_defer_accepts == 2u);
    assert(gxm_clear_fusion_deferred == 2u);
}

static void test_submit_failure_keeps_original_state(void) {
    VPGxmClearRequest req = request_full_colour();
    VPGxmSurface s = matching_surface();
    uint64_t accepts = gxm_clear_defer_accepts;
    uint64_t deferred = gxm_clear_fusion_deferred;

    events_reset();
    test_flush_surface = &s;
    test_flush_rc = -7;
    assert(clear_defer_submit_and_record(&req, &s, 1) == -7);
    assert(s.clear_pending == 0u);
    assert(s.clear_pending_async == 0u);
    assert(gxm_clear_defer_accepts == accepts);
    assert(gxm_clear_fusion_deferred == deferred);
    assert(gxm_clear_defer_submit_failures == 1u);

    /* A coalescing record whose scene submission fails must not lose the older
     * pending colour or its protection either. */
    s.clear_pending = 1u;
    s.clear_pending_async = 1u;
    s.clear_pending_color = 0xff010203u;
    req.color = 0xffaabbccu;
    events_reset();
    test_flush_surface = &s;
    test_flush_rc = -8;
    assert(clear_defer_submit_and_record(&req, &s, 1) == -8);
    assert(s.clear_pending == 1u);
    assert(s.clear_pending_async == 1u);
    assert(s.clear_pending_color == 0xff010203u);
    assert(gxm_clear_defer_accepts == accepts);
    assert(gxm_clear_fusion_deferred == deferred);
    assert(gxm_clear_defer_submit_failures == 2u);
}

static void test_materialize_drains_before_write(void) {
    VPGxmSurface s = matching_surface();
    unsigned events_before;

    s.clear_pending = 1u;
    s.clear_pending_async = 1u;
    s.clear_pending_color = 0xff00ff00u;
    events_reset();
    events_before = test_event_count;
    test_pending_rc = 0;
    test_surface_clear_calls = 0;
    assert(surface_clear_materialize(&s, "unit") == 0);
    assert(test_event_count == events_before + 2u);
    assert(test_events[events_before] == EVENT_PENDING_WAIT);
    assert(test_events[events_before + 1u] == EVENT_SURFACE_CLEAR);
    assert(strcmp(test_pending_why, "clear-materialize") == 0);
    assert(test_surface_clear_calls == 1);
    assert(test_clear_mask == 0x3cu);
    assert(test_clear_color == 0xff00ff00u);
    assert(s.clear_pending == 0u);
    assert(s.clear_pending_async == 0u);
    assert(gxm_clear_defer_materialize_calls == 1u);
}

static void test_materialize_wait_failure_preserves_pending(void) {
    VPGxmSurface s = matching_surface();
    unsigned events_before;

    s.clear_pending = 1u;
    s.clear_pending_async = 1u;
    s.clear_pending_color = 0xff00ff00u;
    events_reset();
    events_before = test_event_count;
    test_pending_rc = -5;
    test_surface_clear_calls = 0;
    assert(surface_clear_materialize(&s, "unit") == -5);
    assert(test_event_count == events_before + 1u);
    assert(test_events[events_before] == EVENT_PENDING_WAIT);
    /* The failed drain proves nothing. The pending colour, the tag and the
     * backing contents stay untouched; no GPU reference is released here. */
    assert(s.clear_pending == 1u);
    assert(s.clear_pending_async == 1u);
    assert(s.clear_pending_color == 0xff00ff00u);
    assert(test_surface_clear_calls == 0);
    assert(gxm_clear_defer_materialize_failures == 1u);
}

static void test_original_path_materialize_does_not_wait(void) {
    VPGxmSurface s = matching_surface();

    s.clear_pending = 1u;
    s.clear_pending_async = 0u;
    s.clear_pending_color = 0xdeadbeefu;
    events_reset();
    test_surface_clear_calls = 0;
    assert(surface_clear_materialize(&s, "original") == 0);
    assert(test_event_count == 1u);
    assert(test_events[0] == EVENT_SURFACE_CLEAR);
    assert(test_surface_clear_calls == 1);
    assert(s.clear_pending == 0u);
    assert(s.clear_pending_async == 0u);

    /* No pending colour is a no-op. */
    events_reset();
    assert(surface_clear_materialize(&s, "none") == 0);
    assert(test_event_count == 0u);
}

static void test_mode_switch_keeps_existing_tag_protection(void) {
    VPGxmSurface s = matching_surface();
    unsigned events_before;

    /* A mode 6 -> 5 switch only stops admitting new fast-path records. The
     * surface tag written while mode 6 was active is consulted without the
     * mode switch, so the drain still happens. */
    s.clear_pending = 1u;
    s.clear_pending_async = 1u;
    s.clear_pending_color = 0x01020304u;
    events_reset();
    events_before = test_event_count;
    test_pending_rc = 0;
    assert(surface_clear_materialize(&s, "mode-switch") == 0);
    assert(test_event_count == events_before + 2u);
    assert(test_events[events_before] == EVENT_PENDING_WAIT);
    assert(test_events[events_before + 1u] == EVENT_SURFACE_CLEAR);
    assert(s.clear_pending == 0u);
    assert(s.clear_pending_async == 0u);
}

static void test_coalescing_normal_record_keeps_async_tag(void) {
    VPGxmSurface s = matching_surface();

    s.clear_pending = 1u;
    s.clear_pending_async = 1u;
    s.clear_pending_color = 0x11111111u;
    surface_clear_defer_record(&s, 0x22222222u, 0u);
    assert(s.clear_pending == 1u);
    assert(s.clear_pending_async == 1u);
    assert(s.clear_pending_color == 0x22222222u);
}

static void test_in_scene_consume_clears_both_tags(void) {
    VPGxmSurface s = matching_surface();

    s.clear_pending = 1u;
    s.clear_pending_async = 1u;
    s.color_clear_mask = 0u;
    s.cleared_count = 0u;
    surface_clear_consumed_in_scene(&s);
    assert(s.clear_pending == 0u);
    assert(s.clear_pending_async == 0u);
    assert(s.color_clear_mask == 0x3cu);
    assert(s.cleared_count == 1u);
    assert(gxm_clear_defer_in_scene == 1u);
    assert(gxm_clear_fusion_in_scene == 1u);

    surface_clear_consumed_in_scene(&s);
    assert(s.cleared_count == 2u);
    assert(gxm_clear_defer_in_scene == 1u);
}

int main(void) {
    test_reason_and_sync_classification();
    test_admission_accepts_and_rejects();
    test_submit_before_record_and_coalesce();
    test_submit_failure_keeps_original_state();
    test_materialize_drains_before_write();
    test_materialize_wait_failure_preserves_pending();
    test_original_path_materialize_does_not_wait();
    test_mode_switch_keeps_existing_tag_protection();
    test_coalescing_normal_record_keeps_async_tag();
    test_in_scene_consume_clears_both_tags();
    puts("clear_defer: all checks passed");
    return 0;
}

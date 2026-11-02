/* Host regression for the static-pair hint and pending-scene completion policy.
 *
 * The production function bodies are extracted verbatim from
 * src/platform/gxm_runtime.c by tests/CMakeLists.txt. This file supplies only
 * the GXM/upload/log stubs and fixtures; it does not reimplement either
 * algorithm. */
#include "gxm_runtime.h"

#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct SceGxmProgram SceGxmProgram;
typedef struct SceGxmVertexProgram SceGxmVertexProgram;
typedef struct SceGxmFragmentProgram SceGxmFragmentProgram;
typedef uint32_t SceGxmShaderPatcherId;

#define VP_UPLOAD_CACHE_SLOTS 4u
#define VP_GXM_BATCH_ARENAS 3u
#define VP_SCENE_CLASS_COUNT 2u
#define SCE_GXM_COLOR_MASK_R 0x1u
#define SCE_GXM_COLOR_MASK_G 0x2u
#define SCE_GXM_COLOR_MASK_B 0x4u
#define SCE_GXM_COLOR_MASK_A 0x8u

/* Pair fixtures need the counters the production probe updates. */
static unsigned static_clock;
static struct {
    unsigned pair_hits;
    unsigned pair_hint_hits;
} static_stats;

/* Completion stubs: one upload counter per cache slot, one notification
 * region, and a wait result the test controls. */
static struct {
    unsigned pending;
} gxm_uploads[VP_UPLOAD_CACHE_SLOTS];

static uint64_t gxm_notify_wait_calls, gxm_notify_wait_us;
static uint64_t gxm_pending_complete_calls, gxm_pending_wait_us;
static uint64_t gxm_pipeline_waits, gxm_pipeline_age_us, gxm_pipeline_age_max;
static struct {
    uint64_t notify_us, total_us, total_max;
} gxm_scene_classes[VP_SCENE_CLASS_COUNT];

typedef struct {
    volatile unsigned *address;
    uint32_t value;
} SceGxmNotification;

static volatile unsigned test_notification_region[VP_GXM_BATCH_ARENAS + 1u];
static uint64_t test_time_now;
static unsigned test_wait_calls;
static unsigned test_wait_arena;
static uint32_t test_wait_value;
static int test_wait_rc;
static unsigned test_poison_calls;

static uint64_t gxm_prof_now(void) {
    return ++test_time_now;
}

static volatile unsigned *sceGxmGetNotificationRegion(void) {
    return test_notification_region;
}

static int sceGxmNotificationWait(const SceGxmNotification *notification) {
    unsigned offset = (unsigned)(notification->address - test_notification_region);
    test_wait_calls++;
    test_wait_arena = offset >= 1u ? offset - 1u : 0xffffffffu;
    test_wait_value = notification->value;
    return test_wait_rc;
}

static int frame_poison_context(int rc) {
    test_poison_calls++;
    return rc;
}

static void glog(const char *format, ...) {
    (void)format;
}

#include "gxm_render_pipeline_production.inc"

typedef struct {
    VPStaticPair pair;
    uint32_t vs_token;
    uint32_t fs_token;
    unsigned viewport;
    uint32_t color_mask_input;
} VPPairFixture;

static void pair_fixture_init(VPPairFixture *fixture, uint32_t seed,
                              unsigned viewport) {
    unsigned i;
    VPStaticPair *pair = &fixture->pair;

    memset(fixture, 0, sizeof(*fixture));
    fixture->vs_token = 0x1000u + seed;
    fixture->fs_token = 0x2000u + seed;
    fixture->viewport = viewport;
    fixture->color_mask_input = 0x1111u;
    pair->used = 1;
    pair->index_source = 1u;
    pair->stride = 24u + seed;
    pair->color_mask = color_mask_to_gxm(fixture->color_mask_input);
    for (i = 0; i < 16u; ++i)
        pair->attributes[i] = seed * 0x100u + i;
    pair->blend[0] = 1u + seed;
    pair->blend[1] = 2u + seed;
    pair->blend[2] = 3u + seed;
    pair->blend[3] = 4u + seed;
    pair->blend[4] = 5u + seed;
    pair->blend[5] = 6u + seed;
}

static void request_from_fixture(VPGxmDrawRequest *req,
                                 const VPPairFixture *fixture) {
    memset(req, 0, sizeof(*req));
    req->stride = fixture->pair.stride;
    req->color_mask = fixture->color_mask_input;
    memcpy(req->attributes, fixture->pair.attributes, sizeof(req->attributes));
    req->blend_color_op = fixture->pair.blend[0];
    req->blend_color_src = fixture->pair.blend[1];
    req->blend_color_dst = fixture->pair.blend[2];
    req->blend_alpha_op = fixture->pair.blend[3];
    req->blend_alpha_src = fixture->pair.blend[4];
    req->blend_alpha_dst = fixture->pair.blend[5];
}

static int pair_probe(const VPPairFixture *fixture, const VPGxmDrawRequest *req,
                      VPStaticPair **owner) {
    VPGxmProgramPair *base = NULL;
    return static_pair_hint_probe(req, fixture->vs_token, fixture->fs_token,
                                  fixture->viewport, fixture->pair.index_source,
                                  &base, owner);
}

static void test_hint_alternating_keys(void) {
    enum { PAIR_COUNT = 4, ROUNDS = 3 };
    VPPairFixture fixtures[PAIR_COUNT];
    VPGxmDrawRequest requests[PAIR_COUNT];
    unsigned i, round;

    static_pair_hint_reset();
    static_stats.pair_hits = 0u;
    static_stats.pair_hint_hits = 0u;
    for (i = 0; i < PAIR_COUNT; ++i) {
        pair_fixture_init(&fixtures[i], i + 1u, i & 1u);
        request_from_fixture(&requests[i], &fixtures[i]);
        static_pair_hint_set(&fixtures[i].pair, fixtures[i].vs_token,
                             fixtures[i].fs_token, fixtures[i].viewport);
    }
    for (round = 0; round < ROUNDS; ++round) {
        for (i = 0; i < PAIR_COUNT; ++i) {
            VPStaticPair *owner = NULL;
            assert(pair_probe(&fixtures[i], &requests[i], &owner) == 1);
            assert(owner == &fixtures[i].pair);
        }
    }
    assert(static_stats.pair_hint_hits == PAIR_COUNT * ROUNDS);
    assert(static_stats.pair_hits == PAIR_COUNT * ROUNDS);
    puts("PASS pair hints retain multiple alternating keys");
}

static void test_hint_full_match_rejection(void) {
    VPPairFixture fixture;
    VPGxmDrawRequest req;
    VPStaticPair *owner = NULL;
    unsigned i;
    uint32_t last_use;
    unsigned hits;

    pair_fixture_init(&fixture, 7u, 1u);
    request_from_fixture(&req, &fixture);
    static_pair_hint_reset();
    static_pair_hint_set(&fixture.pair, fixture.vs_token, fixture.fs_token,
                         fixture.viewport);
    assert(pair_probe(&fixture, &req, &owner) == 1);
    assert(owner == &fixture.pair);
    last_use = fixture.pair.last_use;
    hits = static_stats.pair_hint_hits;

    fixture.pair.used = 0;
    assert(pair_probe(&fixture, &req, &owner) == 0);
    fixture.pair.used = 1;

    req.stride++;
    assert(pair_probe(&fixture, &req, &owner) == 0);
    req.stride--;
    req.attributes[5] ^= 0x1u;
    assert(pair_probe(&fixture, &req, &owner) == 0);
    req.attributes[5] ^= 0x1u;
    req.color_mask ^= 0x10u;
    assert(pair_probe(&fixture, &req, &owner) == 0);
    req.color_mask ^= 0x10u;
    for (i = 0; i < 6u; ++i) {
        uint32_t *field = i == 0u ? &req.blend_color_op :
                          i == 1u ? &req.blend_color_src :
                          i == 2u ? &req.blend_color_dst :
                          i == 3u ? &req.blend_alpha_op :
                          i == 4u ? &req.blend_alpha_src :
                                    &req.blend_alpha_dst;
        *field ^= 0x1u;
        assert(pair_probe(&fixture, &req, &owner) == 0);
        *field ^= 0x1u;
    }
    assert(pair_probe(&fixture, &req, &owner) == 1);
    assert(fixture.pair.last_use != last_use);
    assert(static_stats.pair_hint_hits == hits + 1u);

    owner = NULL;
    assert(static_pair_hint_probe(&req, fixture.vs_token, fixture.fs_token,
                                  fixture.viewport ^ 1u,
                                  fixture.pair.index_source, NULL, &owner) == 0);
    assert(static_pair_hint_probe(&req, fixture.vs_token, fixture.fs_token,
                                  fixture.viewport,
                                  fixture.pair.index_source ^ 1u,
                                  NULL, &owner) == 0);
    puts("PASS pair hint requires the full input comparison");
}

static void test_hint_lifecycle(void) {
    VPPairFixture fixture;
    VPGxmDrawRequest req;
    VPStaticPair *owner = NULL;
    uint32_t old_vs, old_fs;

    pair_fixture_init(&fixture, 11u, 0u);
    request_from_fixture(&req, &fixture);
    static_pair_hint_reset();
    static_pair_hint_set(&fixture.pair, fixture.vs_token, fixture.fs_token,
                         fixture.viewport);
    assert(pair_probe(&fixture, &req, &owner) == 1);

    static_pair_hint_forget(&fixture.pair);
    assert(pair_probe(&fixture, &req, &owner) == 0);
    static_pair_hint_set(&fixture.pair, fixture.vs_token, fixture.fs_token,
                         fixture.viewport);
    assert(pair_probe(&fixture, &req, &owner) == 1);

    old_vs = fixture.vs_token;
    old_fs = fixture.fs_token;
    static_pair_hint_forget(&fixture.pair);
    fixture.vs_token = 0x9001u;
    fixture.fs_token = 0xa001u;
    static_pair_hint_set(&fixture.pair, fixture.vs_token, fixture.fs_token,
                         fixture.viewport);
    owner = NULL;
    assert(static_pair_hint_probe(&req, old_vs, old_fs, fixture.viewport,
                                  fixture.pair.index_source, NULL, &owner) == 0);
    assert(pair_probe(&fixture, &req, &owner) == 1);

    static_pair_hint_reset();
    assert(pair_probe(&fixture, &req, &owner) == 0);
    puts("PASS pair hint invalidation and reuse do not leave stale keys");
}

static void test_hint_lru(void) {
    enum { PAIR_COUNT = 9 };
    VPPairFixture fixtures[PAIR_COUNT];
    VPGxmDrawRequest requests[PAIR_COUNT];
    VPStaticPair *owner = NULL;
    unsigned i;

    static_pair_hint_reset();
    for (i = 0; i < PAIR_COUNT; ++i) {
        pair_fixture_init(&fixtures[i], i + 20u, i & 1u);
        request_from_fixture(&requests[i], &fixtures[i]);
        static_pair_hint_set(&fixtures[i].pair, fixtures[i].vs_token,
                             fixtures[i].fs_token, fixtures[i].viewport);
    }
    assert(pair_probe(&fixtures[8], &requests[8], &owner) == 1);
    assert(owner == &fixtures[8].pair);
    assert(pair_probe(&fixtures[0], &requests[0], &owner) == 0);
    for (i = 1; i < PAIR_COUNT; ++i)
        assert(pair_probe(&fixtures[i], &requests[i], &owner) == 1);
    puts("PASS pair hint evicts the oldest of eight entries");
}

static void test_hint_refresh_after_hole(void) {
    VPPairFixture fixtures[3];
    VPGxmDrawRequest req;
    VPStaticPair *owner = NULL;
    unsigned entries = 0u;

    static_pair_hint_reset();
    for (unsigned i = 0u; i < 3u; ++i) {
        pair_fixture_init(&fixtures[i], i + 30u, 0u);
        static_pair_hint_set(&fixtures[i].pair, fixtures[i].vs_token,
                             fixtures[i].fs_token, fixtures[i].viewport);
    }
    static_pair_hint_forget(&fixtures[0].pair);
    static_pair_hint_set(&fixtures[2].pair, fixtures[2].vs_token,
                         fixtures[2].fs_token, fixtures[2].viewport);
    for (unsigned i = 0u; i < VP_STATIC_PAIR_HINTS; ++i)
        if (static_pair_hints[i].pair == &fixtures[2].pair) entries++;
    assert(entries == 1u);
    request_from_fixture(&req, &fixtures[2]);
    assert(pair_probe(&fixtures[2], &req, &owner) == 1);
    static_pair_hint_forget(&fixtures[2].pair);
    assert(pair_probe(&fixtures[2], &req, &owner) == 0);
    puts("PASS hint refresh finds an existing entry beyond a free hole");
}

static void pending_reset(void) {
    memset(gxm_pending_scenes, 0, sizeof(gxm_pending_scenes));
    gxm_pending_scene_count = 0u;
    memset(gxm_uploads, 0, sizeof(gxm_uploads));
    memset(gxm_scene_classes, 0, sizeof(gxm_scene_classes));
    gxm_notify_wait_calls = 0u;
    gxm_notify_wait_us = 0u;
    gxm_pending_complete_calls = 0u;
    gxm_pending_wait_us = 0u;
    gxm_pipeline_waits = 0u;
    gxm_pipeline_age_us = 0u;
    gxm_pipeline_age_max = 0u;
    test_time_now = 0u;
    test_wait_calls = 0u;
    test_wait_arena = 0xffffffffu;
    test_wait_value = 0u;
    test_wait_rc = 0;
    test_poison_calls = 0u;
}

static void pending_add(unsigned arena, uint32_t value, unsigned cls,
                        unsigned pin_mask) {
    VPGxmPendingScene *pending = &gxm_pending_scenes[arena];
    unsigned i;

    assert(arena < VP_GXM_BATCH_ARENAS);
    memset(pending, 0, sizeof(*pending));
    gxm_pending_scene_count++;
    pending->used = 1;
    pending->arena = arena;
    pending->notify_value = value;
    pending->submit_us = ++test_time_now;
    pending->draws = arena + 1u;
    pending->cls = cls;
    for (i = 0; i < VP_UPLOAD_CACHE_SLOTS; ++i) {
        if ((pin_mask & (1u << i)) == 0u) continue;
        pending->pins[i] = 1u;
        pending->pin_count++;
        gxm_uploads[i].pending++;
    }
}

static void test_completion_no_pending(void) {
    pending_reset();
    assert(gxm_pending_scene_complete("none") == 0);
    assert(test_wait_calls == 0u);
    assert(gxm_pending_scene_count == 0u);
    puts("PASS completion without pending scenes performs no wait");
}

static void test_completion_newest_arena(void) {
    pending_reset();
    pending_add(2u, 10u, 0u, 0x1u);
    pending_add(0u, 20u, 1u, 0x3u);
    assert(gxm_pending_scene_count == 2u);

    assert(gxm_pending_scene_complete("newest") == 0);
    assert(test_wait_calls == 1u);
    assert(test_wait_arena == 0u);
    assert(test_wait_value == 20u);
    assert(gxm_pending_scene_count == 0u);
    assert(!gxm_pending_scenes[0].used);
    assert(!gxm_pending_scenes[2].used);
    assert(gxm_uploads[0].pending == 0u);
    assert(gxm_uploads[1].pending == 0u);
    puts("PASS completion waits once for the newest value, not the highest arena");
}

static void test_completion_failure_keeps_resources(void) {
    pending_reset();
    pending_add(0u, UINT32_MAX, 0u, 0x1u);
    pending_add(1u, 1u, 1u, 0x2u);
    test_wait_rc = -7;

    assert(gxm_pending_scene_complete("fail") == -7);
    assert(test_wait_calls == 1u);
    assert(test_wait_arena == 1u);
    assert(test_wait_value == 1u);
    assert(test_poison_calls == 1u);
    assert(gxm_pending_scene_count == 2u);
    assert(gxm_pending_scenes[0].used);
    assert(gxm_pending_scenes[1].used);
    assert(gxm_pending_scenes[0].pin_count == 1u);
    assert(gxm_pending_scenes[1].pin_count == 1u);
    assert(gxm_uploads[0].pending == 1u);
    assert(gxm_uploads[1].pending == 1u);

    test_wait_rc = 0;
    assert(gxm_pending_scene_complete("retry") == 0);
    assert(test_wait_calls == 2u);
    assert(test_wait_value == 1u);
    assert(gxm_pending_scene_count == 0u);
    assert(gxm_uploads[0].pending == 0u);
    assert(gxm_uploads[1].pending == 0u);
    puts("PASS failed completion poisons without retiring scenes or pins");
}

static void test_completion_wrap_releases_old_scene(void) {
    pending_reset();
    pending_add(1u, UINT32_MAX, 0u, 0x1u);
    pending_add(2u, 1u, 1u, 0x1u);

    assert(gxm_pending_scene_complete("wrap") == 0);
    assert(test_wait_calls == 1u);
    assert(test_wait_arena == 2u);
    assert(test_wait_value == 1u);
    assert(gxm_pending_scene_count == 0u);
    assert(gxm_uploads[0].pending == 0u);
    puts("PASS completion order survives the 32-bit notification wrap");
}

static void test_completion_arena_wait(void) {
    pending_reset();
    pending_add(1u, 30u, 0u, 0x1u);
    pending_add(2u, 40u, 1u, 0x3u);

    assert(gxm_pending_scene_complete_arena(1u, "reuse") == 0);
    assert(test_wait_calls == 1u);
    assert(test_wait_arena == 1u);
    assert(test_wait_value == 30u);
    assert(!gxm_pending_scenes[1].used);
    assert(gxm_pending_scenes[2].used);
    assert(gxm_pending_scene_count == 1u);
    assert(gxm_uploads[0].pending == 1u);
    assert(gxm_uploads[1].pending == 1u);

    assert(gxm_pending_scene_complete_arena(0u, "empty") == 0);
    assert(test_wait_calls == 1u);
    puts("PASS per-arena reuse wait retires only that arena");
}

static void test_retire_upto_partial(void) {
    pending_reset();
    pending_add(0u, 10u, 0u, 0x1u);
    pending_add(1u, 20u, 0u, 0x1u);
    pending_add(2u, 30u, 1u, 0x1u);
    assert(gxm_uploads[0].pending == 3u);

    gxm_pending_scene_retire_upto(20u);
    assert(!gxm_pending_scenes[0].used);
    assert(!gxm_pending_scenes[1].used);
    assert(gxm_pending_scenes[2].used);
    assert(gxm_pending_scene_count == 1u);
    assert(gxm_uploads[0].pending == 1u);

    gxm_pending_scene_retire_upto(30u);
    assert(gxm_pending_scene_count == 0u);
    assert(gxm_uploads[0].pending == 0u);
    puts("PASS retirement releases exactly one pin per retired scene");
}

static void test_notify_order_helpers(void) {
    assert(gxm_notify_next(0u) == 1u);
    assert(gxm_notify_next(UINT32_MAX - 1u) == UINT32_MAX);
    assert(gxm_notify_next(UINT32_MAX) == 1u);
    assert(gxm_notify_newer(2u, 1u));
    assert(!gxm_notify_newer(1u, 2u));
    assert(gxm_notify_newer(1u, UINT32_MAX));
    assert(!gxm_notify_newer(UINT32_MAX, 1u));
    assert(gxm_notify_reached(1u, UINT32_MAX));
    assert(!gxm_notify_reached(UINT32_MAX, 1u));
    assert(gxm_notify_reached(7u, 7u));
    puts("PASS notification ordering skips zero and handles wrap");
}

int main(void) {
    test_hint_alternating_keys();
    test_hint_full_match_rejection();
    test_hint_lifecycle();
    test_hint_lru();
    test_hint_refresh_after_hole();
    test_completion_no_pending();
    test_completion_newest_arena();
    test_completion_failure_keeps_resources();
    test_completion_wrap_releases_old_scene();
    test_completion_arena_wait();
    test_retire_upto_partial();
    test_notify_order_helpers();
    puts("gxm_render_pipeline: all checks passed");
    return 0;
}

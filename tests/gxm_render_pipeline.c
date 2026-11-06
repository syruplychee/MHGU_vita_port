/* Host regression for the static-pair hint and pending-scene completion policy.
 *
 * The production function bodies are extracted verbatim from
 * src/platform/gxm_runtime.c by tests/CMakeLists.txt. This file supplies only
 * the GXM/upload/log stubs and fixtures; it does not reimplement either
 * algorithm. */
#include "gxm_runtime.h"
#include "texture_pipeline.h"
#include "gpu_wait_profile.h"
#include "frame_cost_profile.h"
#define glog_work glog

#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct SceGxmProgram SceGxmProgram;
typedef struct SceGxmVertexProgram SceGxmVertexProgram;
typedef struct SceGxmFragmentProgram SceGxmFragmentProgram;
typedef uint32_t SceGxmShaderPatcherId;

/* VitaSDK sampler/blend value stubs for the extracted blend planner. The
 * numeric order matches psp2/gxm.h; the test checks named fields, never the
 * bitfield packing. */
typedef enum {
    SCE_GXM_BLEND_FUNC_NONE = 0,
    SCE_GXM_BLEND_FUNC_ADD = 1,
    SCE_GXM_BLEND_FUNC_SUBTRACT = 2,
    SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT = 3,
    SCE_GXM_BLEND_FUNC_MIN = 4,
    SCE_GXM_BLEND_FUNC_MAX = 5
} SceGxmBlendFunc;
typedef enum {
    SCE_GXM_BLEND_FACTOR_ZERO = 0,
    SCE_GXM_BLEND_FACTOR_ONE = 1,
    SCE_GXM_BLEND_FACTOR_SRC_COLOR = 2,
    SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR = 3,
    SCE_GXM_BLEND_FACTOR_SRC_ALPHA = 4,
    SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 5,
    SCE_GXM_BLEND_FACTOR_DST_COLOR = 6,
    SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR = 7,
    SCE_GXM_BLEND_FACTOR_DST_ALPHA = 8,
    SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA = 9,
    SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE = 10,
    SCE_GXM_BLEND_FACTOR_DST_ALPHA_SATURATE = 11
} SceGxmBlendFactor;
typedef struct SceGxmBlendInfo {
    uint8_t colorMask;
    uint8_t colorFunc : 4;
    uint8_t alphaFunc : 4;
    uint8_t colorSrc : 4;
    uint8_t colorDst : 4;
    uint8_t alphaSrc : 4;
    uint8_t alphaDst : 4;
} SceGxmBlendInfo;

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
static uint64_t gxm_notify_poll_hits, gxm_notify_real_waits;
static uint64_t gxm_pending_complete_calls, gxm_pending_wait_us;
static uint64_t gxm_pipeline_waits, gxm_pipeline_age_us, gxm_pipeline_age_max;
static struct {
    uint64_t notify_us, total_us, total_max;
} gxm_scene_classes[VP_SCENE_CLASS_COUNT];

/* Direct-lease release stub used by the extracted pending/direct sections. */
static unsigned test_direct_releases;
static VPTexturePoolToken test_last_released;
static void vp_texture_direct_release_gpu(VPTexturePoolToken token) {
    test_direct_releases++;
    test_last_released = token;
}
static int test_deep_active = 1;
static int vp_texture_direct_active(void) { return test_deep_active; }
static int vp_texture_direct_compatible(const void *rgba, size_t bytes) {
    (void)rgba; (void)bytes; return 0;
}
static int vp_texture_direct_retain_gpu(const void *rgba,
                                        VPTexturePoolToken *token) {
    (void)rgba;
    (void)token;
    return 0;
}
static unsigned vp_texture_direct_live_refs(void) { return 0u; }

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

/* The extracted pending-scene wait now contains a frame-cost span. This host
 * fixture exercises completion/lease semantics, not the profiler, so the span
 * hooks stay inert. */
uint64_t vp_gxm_frame_cost_category_begin(unsigned category) {
    (void)category;
    return 0u;
}

void vp_gxm_frame_cost_category_end(unsigned category, uint64_t started_us) {
    (void)category;
    (void)started_us;
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

static VPGpuWaitProfile gxm_wait_profile;
static unsigned gxm_wait_reclaim_active;
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
    /* Prepared blend output is not an input identity: changing it must not
     * make a pair match (or stop matching) on its own. */
    memset(&fixture.pair.blend_info, 0xff, sizeof(fixture.pair.blend_info));
    assert(pair_probe(&fixture, &req, &owner) == 1);

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
    memset(&gxm_wait_profile, 0, sizeof(gxm_wait_profile));
    gxm_wait_reclaim_active = 0u;
    memset(gxm_pending_scenes, 0, sizeof(gxm_pending_scenes));
    gxm_pending_scene_count = 0u;
    memset(gxm_uploads, 0, sizeof(gxm_uploads));
    memset(gxm_scene_classes, 0, sizeof(gxm_scene_classes));
    gxm_notify_wait_calls = 0u;
    gxm_notify_wait_us = 0u;
    gxm_notify_poll_hits = 0u;
    gxm_notify_real_waits = 0u;
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
    test_direct_releases = 0u;
    memset(&test_last_released, 0, sizeof(test_last_released));
    memset(test_notification_region, 0, sizeof(test_notification_region));
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

static void test_completion_poll_skips_duplicate_wait(void) {
    pending_reset();
    pending_add(1u, 20u, 0u, 0x1u);
    gxm_pending_scenes[1].direct[0] = (VPTexturePoolToken){ 3u, 7u };
    gxm_pending_scenes[1].direct[1] = (VPTexturePoolToken){ 4u, 9u };
    gxm_pending_scenes[1].direct_count = 2u;
    /* Address is region + 1 + arena. The acquire observation already equals
     * the pending value, so NotificationWait must not be called again. */
    test_notification_region[1u + 1u] = 20u;
    assert(gxm_pending_scene_complete("poll-hit") == 0);
    assert(test_wait_calls == 0u);
    assert(gxm_notify_poll_hits == 1u);
    assert(gxm_notify_real_waits == 0u);
    assert(gxm_wait_profile.causes[vp_gpu_wait_cause("other")].polls == 1u);
    assert(gxm_wait_profile.causes[vp_gpu_wait_cause("other")].block_us == 0u);
    assert(gxm_pending_scene_count == 0u);
    assert(test_direct_releases == 2u);
    assert(test_last_released.slot == 4u && test_last_released.generation == 9u);
    puts("PASS completion polls the acquire value and skips an equal wait");
}

static void test_completion_baseline_still_waits(void) {
    pending_reset();
    pending_add(1u, 20u, 0u, 0u);
    test_notification_region[2u] = 20u;
    test_deep_active = 0;
    assert(gxm_pending_scene_complete("baseline") == 0);
    assert(test_wait_calls == 1u);
    assert(gxm_notify_poll_hits == 0u);
    test_deep_active = 1;
}

static void test_completion_failure_keeps_direct_refs(void) {
    pending_reset();
    pending_add(0u, 10u, 0u, 0x1u);
    gxm_pending_scenes[0].direct[0] = (VPTexturePoolToken){ 5u, 2u };
    gxm_pending_scenes[0].direct_count = 1u;
    test_wait_rc = -7;
    assert(gxm_pending_scene_complete("fail") == -7);
    assert(test_wait_calls == 1u);
    assert(gxm_notify_real_waits == 1u);
    assert(test_direct_releases == 0u);
    assert(gxm_pending_scenes[0].direct_count == 1u);
    assert(gxm_wait_profile.causes[vp_gpu_wait_cause("other")].failures == 1u);
    assert(gxm_wait_profile.causes[vp_gpu_wait_cause("other")].block_us ==
           gxm_notify_wait_us);

    /* A later observation that already equals the pending value proves
     * completion without the SVC; only then is the lease released. */
    test_wait_rc = 0;
    test_notification_region[1u + 0u] = 10u;
    assert(gxm_pending_scene_complete("retry-poll") == 0);
    assert(test_wait_calls == 1u);
    assert(gxm_notify_poll_hits == 1u);
    assert(test_direct_releases == 1u);
    assert(gxm_pending_scene_count == 0u);
    puts("PASS a failed wait retains direct leases until a proven completion");
}

static void test_direct_refs_limit_release_quarantine(void) {
    VPGxmDirectRefs refs;
    memset(&refs, 0, sizeof(refs));
    gxm_direct_quarantined = 0u;
    gxm_direct_quarantine_count = 0u;
    test_direct_releases = 0u;
    for (unsigned i = 0u; i < VP_GXM_DIRECT_TEXTURE_MAX; ++i)
        assert(gxm_direct_refs_add(&refs,
            (VPTexturePoolToken){ i, i + 1u }) == 0);
    assert(gxm_direct_refs_add(&refs,
        (VPTexturePoolToken){ 9u, 9u }) < 0);
    assert(gxm_direct_refs_release(&refs) == VP_GXM_DIRECT_TEXTURE_MAX);
    assert(refs.count == 0u);
    assert(test_direct_releases == VP_GXM_DIRECT_TEXTURE_MAX);

    assert(gxm_direct_refs_add(&refs,
        (VPTexturePoolToken){ 1u, 1u }) == 0);
    assert(gxm_direct_refs_add(&refs,
        (VPTexturePoolToken){ 2u, 2u }) == 0);
    assert(gxm_direct_refs_quarantine(&refs) == 2u);
    assert(refs.count == 0u);
    assert(gxm_direct_quarantined == 2u);
    assert(test_direct_releases == VP_GXM_DIRECT_TEXTURE_MAX);
    puts("PASS direct token limit, release and unknown-state quarantine");
}

static void test_three_arena_direct_release(void) {
    pending_reset();
    pending_add(0u, 10u, 0u, 0u);
    pending_add(1u, 20u, 0u, 0u);
    pending_add(2u, 30u, 0u, 0u);
    gxm_pending_scenes[0].direct[0] = (VPTexturePoolToken){ 0u, 1u };
    gxm_pending_scenes[0].direct_count = 1u;
    gxm_pending_scenes[1].direct[0] = (VPTexturePoolToken){ 1u, 1u };
    gxm_pending_scenes[1].direct_count = 1u;
    gxm_pending_scenes[2].direct[0] = (VPTexturePoolToken){ 2u, 1u };
    gxm_pending_scenes[2].direct_count = 1u;
    test_notification_region[1u + 2u] = 30u;
    assert(gxm_pending_scene_complete("three-arena") == 0);
    assert(test_wait_calls == 0u);
    assert(gxm_notify_poll_hits == 1u);
    assert(gxm_pending_scene_count == 0u);
    assert(test_direct_releases == 3u);
    puts("PASS one completion retires direct leases from all three arenas");
}

static void test_direct_rollback_policy(void) {
    VPGxmDirectRefs refs;
    memset(&refs, 0, sizeof(refs));
    test_direct_releases = 0u;
    gxm_direct_quarantined = 0u;
    gxm_direct_quarantine_count = 0u;
    assert(gxm_direct_rollback_decision(0, 0) ==
           VP_GXM_DIRECT_ROLLBACK_RELEASE);
    assert(gxm_direct_rollback_decision(0, 1) ==
           VP_GXM_DIRECT_ROLLBACK_RELEASE);
    assert(gxm_direct_rollback_decision(1, 1) ==
           VP_GXM_DIRECT_ROLLBACK_RELEASE);
    assert(gxm_direct_rollback_decision(1, 0) ==
           VP_GXM_DIRECT_ROLLBACK_QUARANTINE);
    assert(gxm_direct_refs_add(&refs,
        (VPTexturePoolToken){ 7u, 3u }) == 0);
    gxm_direct_rollback_refs(&refs,
        gxm_direct_rollback_decision(1, 0));
    assert(refs.count == 0u);
    assert(test_direct_releases == 0u);
    assert(gxm_direct_quarantined == 1u);
    assert(gxm_direct_refs_add(&refs,
        (VPTexturePoolToken){ 8u, 4u }) == 0);
    gxm_direct_rollback_refs(&refs,
        gxm_direct_rollback_decision(1, 1));
    assert(test_direct_releases == 1u);
    assert(gxm_direct_quarantined == 1u);
    puts("PASS draw rollback releases only proven or never-issued leases");
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

static void test_blend_plan_enums_unchanged(void) {
    static const struct {
        uint32_t raw, alias, expected;
    } factors[] = {
        {1u, 0x4000u, SCE_GXM_BLEND_FACTOR_ZERO},
        {2u, 0x4001u, SCE_GXM_BLEND_FACTOR_ONE},
        {3u, 0x4300u, SCE_GXM_BLEND_FACTOR_SRC_COLOR},
        {4u, 0x4301u, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR},
        {5u, 0x4302u, SCE_GXM_BLEND_FACTOR_SRC_ALPHA},
        {6u, 0x4303u, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA},
        {7u, 0x4304u, SCE_GXM_BLEND_FACTOR_DST_ALPHA},
        {8u, 0x4305u, SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA},
        {9u, 0x4306u, SCE_GXM_BLEND_FACTOR_DST_COLOR},
        {0xau, 0x4307u, SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR},
        {0xbu, 0x4308u, SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE},
    };
    static const struct {
        uint32_t raw, alias, expected;
    } ops[] = {
        {1u, 0x8006u, SCE_GXM_BLEND_FUNC_ADD},
        {2u, 0x800au, SCE_GXM_BLEND_FUNC_SUBTRACT},
        {3u, 0x800bu, SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT},
        {4u, 0x8007u, SCE_GXM_BLEND_FUNC_MIN},
        {5u, 0x8008u, SCE_GXM_BLEND_FUNC_MAX},
    };
    unsigned i;
    VPGxmDrawRequest req;
    SceGxmBlendInfo info;
    memset(&req, 0, sizeof(req));
    req.color_mask = 0x1111u;
    for (i = 0; i < sizeof(factors) / sizeof(factors[0]); ++i) {
        req.blend_color_op = ops[0].raw;
        req.blend_alpha_op = ops[0].alias;
        req.blend_color_src = factors[i].raw;
        req.blend_color_dst = factors[i].alias;
        req.blend_alpha_src = factors[i].alias;
        req.blend_alpha_dst = factors[i].raw;
        assert(build_blend_info(&req, &info) == 0);
        assert(info.colorFunc == SCE_GXM_BLEND_FUNC_ADD);
        assert(info.alphaFunc == SCE_GXM_BLEND_FUNC_ADD);
        assert(info.colorSrc == factors[i].expected);
        assert(info.colorDst == factors[i].expected);
        assert(info.alphaSrc == factors[i].expected);
        assert(info.alphaDst == factors[i].expected);
    }
    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
        req.blend_color_op = ops[i].raw;
        req.blend_alpha_op = ops[i].alias;
        req.blend_color_src = 2u;
        req.blend_color_dst = 1u;
        req.blend_alpha_src = 2u;
        req.blend_alpha_dst = 1u;
        assert(build_blend_info(&req, &info) == 0);
        assert(info.colorFunc == ops[i].expected);
        assert(info.alphaFunc == ops[i].expected);
    }
    req.blend_color_op = 0u;
    assert(build_blend_info(&req, &info) < 0);
    req.blend_color_op = 6u;
    assert(build_blend_info(&req, &info) < 0);
    req.blend_color_op = 1u;
    req.blend_color_src = 0u;
    assert(build_blend_info(&req, &info) < 0);
    req.blend_color_src = 0xcu;
    assert(build_blend_info(&req, &info) < 0);
    puts("PASS blend planner keeps every accepted enum and rejects the rest");
}

static void test_pair_uniform_budget(void) {
    static const SceGxmProgram *dummy_program =
        (const SceGxmProgram *)(uintptr_t)1u;
    VPStaticPair pair;
    VPGxmDrawRequest req;
    size_t vs = gxm_batch_align_size(100u);
    size_t fs = gxm_batch_align_size(200u);

    memset(&pair, 0, sizeof(pair));
    memset(&req, 0, sizeof(req));
    pair.vs_program = dummy_program;
    pair.fs_program = dummy_program;
    pair.vs_default_uniform_bytes = 100u;
    pair.fs_default_uniform_bytes = 200u;

    /* Two-face budget requires: face uniform present, culling disabled and one
     * of the three two-sided face selectors on this draw. */
    assert(static_pair_uniform_bytes(&pair, &req) == vs + fs);
    pair.fs_face_state = 1u;
    req.cull_enable = 0u;
    req.cull_face = 0x404u;
    assert(static_pair_uniform_bytes(&pair, &req) == vs + fs + fs);
    req.cull_face = 0x405u;
    assert(static_pair_uniform_bytes(&pair, &req) == vs + fs + fs);
    req.cull_face = 0x408u;
    assert(static_pair_uniform_bytes(&pair, &req) == vs + fs + fs);
    req.cull_face = 0x409u;
    assert(static_pair_uniform_bytes(&pair, &req) == vs + fs);
    req.cull_face = 0x404u;
    req.cull_enable = 1u;
    assert(static_pair_uniform_bytes(&pair, &req) == vs + fs);
    req.cull_enable = 0u;
    pair.fs_face_state = 0u;
    assert(static_pair_uniform_bytes(&pair, &req) == vs + fs);

    /* Saturating addition keeps the existing size-overflow semantics. On a
     * 64-bit host the three uint32_t sizes cannot overflow size_t, so the
     * helper sums exactly; the saturation itself is checked directly. */
    pair.vs_default_uniform_bytes = UINT32_MAX;
    pair.fs_default_uniform_bytes = UINT32_MAX;
    pair.fs_face_state = 1u;
    req.cull_enable = 0u;
    req.cull_face = 0x404u;
    {
        size_t expected = gxm_batch_add_size(0u, UINT32_MAX);
        expected = gxm_batch_add_size(expected, UINT32_MAX);
        expected = gxm_batch_add_size(expected, UINT32_MAX);
        assert(static_pair_uniform_bytes(&pair, &req) == expected);
    }
    assert(gxm_batch_add_size((size_t)-1, 1u) == (size_t)-1);
    assert(gxm_batch_add_size((size_t)-1 - 8u, 16u) == (size_t)-1);
    pair.fs_program = NULL;
    assert(static_pair_uniform_bytes(&pair, &req) ==
           gxm_batch_add_size(0u, UINT32_MAX));
    /* A released pair has no prepared size; callers still see zero. */
    assert(static_pair_uniform_bytes(NULL, &req) == 0u);
    puts("PASS pair uniform plan keeps live face and overflow bounds");
}

int main(void) {
    test_hint_alternating_keys();
    test_hint_full_match_rejection();
    test_hint_lifecycle();
    test_hint_lru();
    test_hint_refresh_after_hole();
    test_blend_plan_enums_unchanged();
    test_pair_uniform_budget();
    test_completion_no_pending();
    test_completion_newest_arena();
    test_completion_failure_keeps_resources();
    test_completion_wrap_releases_old_scene();
    test_completion_arena_wait();
    test_retire_upto_partial();
    test_completion_baseline_still_waits();
    test_completion_poll_skips_duplicate_wait();
    test_completion_failure_keeps_direct_refs();
    test_direct_refs_limit_release_quarantine();
    test_three_arena_direct_release();
    test_direct_rollback_policy();
    test_notify_order_helpers();
    puts("gxm_render_pipeline: all checks passed");
    return 0;
}

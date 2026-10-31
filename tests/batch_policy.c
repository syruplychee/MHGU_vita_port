/* Host regression for bounded batch admission/lifetime policy. */
#include <stdint.h>
#include <stdio.h>

#include "batch_policy.h"

static unsigned failures;

#define CHECK(condition, name)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("BATCH_POLICY FAIL case=%s line=%d\n", (name), __LINE__);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static int add(VPBatchPolicy *p, VPBatchPolicyStats *s, uint64_t scope,
               int depth_only, uint64_t target, uint64_t depth, size_t bytes) {
    unsigned reason = vp_batch_policy_admit(p, scope, depth_only, target,
                                            1920u, 1080u, 0xd5u, 1,
                                            depth, 0xa9u, 1920u, 1080u,
                                            bytes, VP_BATCH_MAX_DRAWS,
                                            VP_BATCH_STAGING_BUDGET);
    if (reason != VP_BATCH_ACCEPT) return (int)reason;
    return vp_batch_policy_push(p, s, scope, depth_only, target, 1920u,
                                1080u, 0xd5u, 1, depth, 0xa9u, 1920u, 1080u,
                                bytes, VP_BATCH_MAX_DRAWS,
                                VP_BATCH_STAGING_BUDGET);
}

int main(void) {
    /* Empty admission, push, then commit publishes exactly the pending count. */
    {
        VPBatchPolicy p;
        VPBatchPolicyStats s;
        vp_batch_policy_init(&p, &s);
        CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 1024u) == 0, "empty_push");
        CHECK(p.open == 1 && p.draws == 1 && p.resources_pinned == 1,
              "push_state");
        CHECK(vp_batch_policy_commit(&p, &s) == 1u, "commit_count");
        CHECK(s.completed == 1u && s.failed == 0u && s.flushes == 1u,
              "commit_stats");
        CHECK(p.open == 0 && p.resources_pinned == 0, "commit_reset");
    }

    /* Scope isolation: a different submission must not join. */
    {
        VPBatchPolicy p;
        VPBatchPolicyStats s;
        vp_batch_policy_init(&p, &s);
        CHECK(add(&p, &s, 7u, 0, 0x1000u, 0x2000u, 64u) == 0, "scope_first");
        CHECK(add(&p, &s, 8u, 0, 0x1000u, 0x2000u, 64u) ==
              VP_BATCH_FLUSH_SCOPE, "scope_flush");
        CHECK(p.draws == 1, "scope_no_join");
        CHECK(vp_batch_policy_commit(&p, &s) == 1u, "scope_commit");
    }

    /* RT identity change flushes, including extent/format. */
    {
        VPBatchPolicy p;
        VPBatchPolicyStats s;
        vp_batch_policy_init(&p, &s);
        CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 64u) == 0, "rt_first");
        CHECK(add(&p, &s, 1u, 0, 0x3000u, 0x2000u, 64u) ==
              VP_BATCH_FLUSH_RT, "rt_va_change");
        CHECK(add(&p, &s, 1u, 1, 0u, 0x2000u, 64u) ==
              VP_BATCH_FLUSH_MODE, "mode_change");
        CHECK(vp_batch_policy_commit(&p, &s) == 1u, "rt_commit");
    }

    /* Depth attachment identity change flushes; attachment disable differs. */
    {
        VPBatchPolicy p;
        VPBatchPolicyStats s;
        vp_batch_policy_init(&p, &s);
        CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 64u) == 0, "depth_first");
        CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 64u) == 0, "depth_join");
        CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2001u, 64u) ==
              VP_BATCH_FLUSH_DEPTH, "depth_change");
        {
            VPBatchPolicy q;
            VPBatchPolicyStats t;
            vp_batch_policy_init(&q, &t);
            unsigned reason = vp_batch_policy_admit(&q, 1u, 0, 0x1000u, 1920u,
                1080u, 0xd5u, 0, 0u, 0u, 0u, 0u, 64u, VP_BATCH_MAX_DRAWS,
                VP_BATCH_STAGING_BUDGET);
            CHECK(reason == VP_BATCH_ACCEPT, "depth_disabled_first");
            vp_batch_policy_push(&q, &t, 1u, 0, 0x1000u, 1920u, 1080u, 0xd5u,
                                 0, 0u, 0u, 0u, 0u, 64u,
                                 VP_BATCH_MAX_DRAWS, VP_BATCH_STAGING_BUDGET);
            reason = vp_batch_policy_admit(&q, 1u, 0, 0x1000u, 1920u, 1080u,
                0xd5u, 1, 0x2000u, 0xa9u, 1920u, 1080u, 64u,
                VP_BATCH_MAX_DRAWS, VP_BATCH_STAGING_BUDGET);
            CHECK(reason == VP_BATCH_FLUSH_DEPTH, "attachment_enable_change");
        }
        CHECK(vp_batch_policy_commit(&p, &s) == 2u, "depth_commit");
    }

    /* Draw-count exhaustion at exactly VP_BATCH_MAX_DRAWS. */
    {
        VPBatchPolicy p;
        VPBatchPolicyStats s;
        unsigned i;
        vp_batch_policy_init(&p, &s);
        for (i = 0; i < VP_BATCH_MAX_DRAWS; ++i)
            CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 1u) == 0, "limit_push");
        CHECK(p.draws == VP_BATCH_MAX_DRAWS, "limit_draws");
        CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 1u) ==
              VP_BATCH_FLUSH_LIMIT, "limit_flush");
        CHECK(vp_batch_policy_commit(&p, &s) == VP_BATCH_MAX_DRAWS, "limit_commit");
        CHECK(s.max_batch == VP_BATCH_MAX_DRAWS, "limit_max");
    }

    /* Byte-budget exhaustion and a single draw too large for batching. */
    {
        VPBatchPolicy p;
        VPBatchPolicyStats s;
        vp_batch_policy_init(&p, &s);
        CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u,
                  VP_BATCH_STAGING_BUDGET - 8u) == 0, "budget_first");
        CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 16u) ==
              VP_BATCH_FLUSH_BUDGET, "budget_flush");
        CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u,
                  VP_BATCH_STAGING_BUDGET + 1u) ==
              VP_BATCH_REFUSE_TOO_LARGE, "budget_oversize");
        CHECK(vp_batch_policy_commit(&p, &s) == 1u, "budget_commit");
        CHECK(s.refused == 0u, "budget_not_refused");
    }

    /* Corrupted accounting must refuse/flush instead of wrapping the byte
     * subtraction into an apparently valid reservation. */
    {
        VPBatchPolicy p;
        VPBatchPolicyStats s;
        vp_batch_policy_init(&p, &s);
        CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 64u) == 0,
              "overflow_first");
        p.bytes_used = (size_t)-1;
        CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 64u) ==
              VP_BATCH_FLUSH_BUDGET, "overflow_admit_flush");
        CHECK(vp_batch_policy_push(&p, &s, 1u, 0, 0x1000u, 1920u, 1080u,
                                   0xd5u, 1, 0x2000u, 0xa9u, 1920u, 1080u,
                                   64u, VP_BATCH_MAX_DRAWS,
                                   VP_BATCH_STAGING_BUDGET) == -1,
              "overflow_push_refuse");
        CHECK(p.bytes_used == (size_t)-1 && p.draws == 1u,
              "overflow_no_wrap");
        CHECK(s.refused == 1u, "overflow_refused_stat");
    }

    /* Every API boundary reports the pending set but does not drop it before
     * the actual GPU flush. Failure drops without publishing. */
    {
        static const unsigned boundaries[] = {
            VP_BATCH_BOUNDARY_END_SUBMIT, VP_BATCH_BOUNDARY_CLEAR,
            VP_BATCH_BOUNDARY_READBACK, VP_BATCH_BOUNDARY_PRESENT,
            VP_BATCH_BOUNDARY_RESOURCE, VP_BATCH_BOUNDARY_DEADLINE
        };
        for (unsigned i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]);
             ++i) {
            VPBatchPolicy p;
            VPBatchPolicyStats s;
            vp_batch_policy_init(&p, &s);
            CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 32u) == 0,
                  "boundary_first");
            CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 32u) == 0,
                  "boundary_second");
            CHECK(vp_batch_policy_boundary(&p, boundaries[i], &s) == 2u,
                  "boundary_pending");
            CHECK(p.open == 1 && p.draws == 2u, "boundary_not_reset");
            CHECK(vp_batch_policy_commit(&p, &s) == 2u, "boundary_commit");
            CHECK(s.sync_fallbacks == 1u, "boundary_stat");
        }
        {
            VPBatchPolicy p;
            VPBatchPolicyStats s;
            vp_batch_policy_init(&p, &s);
            CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 32u) == 0,
                  "failure_first");
            CHECK(vp_batch_policy_boundary(&p, VP_BATCH_BOUNDARY_FAILURE,
                                           &s) == 1u, "failure_pending");
            CHECK(p.open == 0 && p.resources_pinned == 0, "failure_release");
            CHECK(s.failed == 1u && s.completed == 0u &&
                  s.commits_dropped == 1u, "failure_no_publish");
        }
    }

    /* Abort releases pins and never publishes. Repeated exhaustion stays
     * bounded. */
    {
        VPBatchPolicy p;
        VPBatchPolicyStats s;
        vp_batch_policy_init(&p, &s);
        for (unsigned round = 0; round < 8u; ++round) {
            for (unsigned i = 0; i < VP_BATCH_MAX_DRAWS; ++i)
                CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 1024u) == 0,
                      "round_push");
            CHECK(p.draws == VP_BATCH_MAX_DRAWS &&
                  p.resources_pinned == VP_BATCH_MAX_DRAWS,
                  "round_bounded");
            CHECK(vp_batch_policy_abort(&p, &s) == VP_BATCH_MAX_DRAWS,
                  "round_abort");
            CHECK(p.open == 0 && p.resources_pinned == 0, "round_release");
        }
        CHECK(s.encoded == 8u * VP_BATCH_MAX_DRAWS &&
              s.failed == 8u * VP_BATCH_MAX_DRAWS && s.completed == 0u,
              "round_stats");
        CHECK(s.commits_dropped == 8u * VP_BATCH_MAX_DRAWS, "round_drop");
    }

    /* Commit then reuse with a different identity from an empty policy. */
    {
        VPBatchPolicy p;
        VPBatchPolicyStats s;
        vp_batch_policy_init(&p, &s);
        CHECK(add(&p, &s, 1u, 0, 0x1000u, 0x2000u, 64u) == 0, "reuse_first");
        vp_batch_policy_commit(&p, &s);
        CHECK(add(&p, &s, 2u, 1, 0u, 0x3000u, 64u) == 0, "reuse_different");
        CHECK(p.open == 1 && p.depth_only == 1 && p.scope == 2u,
              "reuse_identity");
        CHECK(vp_batch_policy_commit(&p, &s) == 1u, "reuse_commit");
        CHECK(s.completed == 2u, "reuse_completed");
    }

    if (failures != 0) {
        printf("BATCH_POLICY failures=%u\n", failures);
        return 1;
    }
    printf("BATCH_POLICY failures=0 cases=11\n");
    return 0;
}

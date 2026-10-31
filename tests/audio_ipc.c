/* Host regression for the M6-B6 audio IPC classification and cadence helper.
 * Pure C11, no platform headers: it compiles on the host with ASan/UBSan and
 * exercises the exact functions the production service calls. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_ipc.h"

static int failures;

#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL %s (line %d)\n", what, __LINE__); ++failures; } \
} while (0)

static void test_classification(void) {
    struct {
        const char *name;
        uint32_t type;
        uint32_t command;
        VPAudioIpcClass expected;
    } cases[] = {
        /* The real renderer RequestUpdate: session "renderer", CMIF command 4. */
        {"renderer", 4, 4, VP_AUDIO_IPC_RENDERER_UPDATE},
        {"renderer", 6, 4, VP_AUDIO_IPC_RENDERER_UPDATE},
        {"renderer", 4, 5, VP_AUDIO_IPC_RENDERER_START},
        {"renderer", 4, 6, VP_AUDIO_IPC_RENDERER_STOP},
        {"renderer", 4, 0, VP_AUDIO_IPC_RENDERER_GET},
        {"renderer", 4, 13, VP_AUDIO_IPC_RENDERER_GET},
        {"renderer", 4, 99, VP_AUDIO_IPC_RENDERER_OTHER},
        /* audren:u command 4 is GetAudioDeviceServiceWithRevisionInfo. It must
         * never be classified as the renderer update. */
        {"audren:u", 4, 4, VP_AUDIO_IPC_MANAGER_GET_DEVICE},
        {"audren:u", 4, 0, VP_AUDIO_IPC_MANAGER_OPEN},
        {"audren:u", 4, 1, VP_AUDIO_IPC_MANAGER_GET_WORK_SIZE},
        {"audren:u", 4, 7, VP_AUDIO_IPC_MANAGER_OTHER},
        {"aud-dev", 4, 4, VP_AUDIO_IPC_DEVICE},
        {"aud-dev", 4, 14, VP_AUDIO_IPC_DEVICE},
        /* Transport control and session close override any command number. */
        {"renderer", 5, 4, VP_AUDIO_IPC_CONTROL},
        {"audren:u", 5, 0, VP_AUDIO_IPC_CONTROL},
        {"renderer", 2, 4, VP_AUDIO_IPC_SESSION_CLOSE},
        {"aud-dev", 2, 0, VP_AUDIO_IPC_SESSION_CLOSE},
        /* Responses and unrelated services are not audio CMIF requests. */
        {"renderer", 0, 4, VP_AUDIO_IPC_NONE},
        {"renderer", 1, 4, VP_AUDIO_IPC_NONE},
        {"fsp-srv", 4, 4, VP_AUDIO_IPC_NONE},
        {"", 4, 4, VP_AUDIO_IPC_NONE},
        {NULL, 4, 4, VP_AUDIO_IPC_NONE},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        VPAudioIpcClass got = vp_audio_ipc_classify(cases[i].name,
                                                    cases[i].type,
                                                    cases[i].command);
        if (got != cases[i].expected) {
            printf("FAIL classify[%u] name=%s type=%u command=%u got=%s expected=%s\n",
                   i, cases[i].name ? cases[i].name : "(null)", cases[i].type,
                   cases[i].command, vp_audio_ipc_class_name(got),
                   vp_audio_ipc_class_name(cases[i].expected));
            ++failures;
        }
    }
    CHECK(vp_audio_ipc_is_session("audren:u"), "audren:u is an audio session");
    CHECK(vp_audio_ipc_is_session("aud-dev"), "aud-dev is an audio session");
    CHECK(vp_audio_ipc_is_session("renderer"), "renderer is an audio session");
    CHECK(!vp_audio_ipc_is_session("fsp-srv"), "fsp-srv is not an audio session");
    CHECK(!vp_audio_ipc_is_session(NULL), "NULL is not an audio session");
    CHECK(strcmp(vp_audio_ipc_class_name(VP_AUDIO_IPC_RENDERER_UPDATE),
                 "renderer-update") == 0, "class name is stable");
}

static void test_cadence(void) {
    VPAudioLogCadence cadence = {0};
    for (uint32_t n = 1; n <= 200; ++n) {
        int emit = vp_audio_log_cadence_should_emit(&cadence, 4, 64);
        int expected = (n <= 4 || (n % 64) == 0);
        CHECK(emit == expected, "cadence first=4 interval=64");
    }
    CHECK(cadence.seen == 200, "cadence counts every event");
    CHECK(cadence.emitted == 7, "cadence emits 1..4,64,128,192");

    vp_audio_log_cadence_reset(&cadence);
    CHECK(cadence.seen == 0 && cadence.emitted == 0, "cadence reset");

    for (uint32_t n = 1; n <= 10; ++n)
        vp_audio_log_cadence_should_emit(&cadence, 2, 0);
    CHECK(cadence.seen == 10 && cadence.emitted == 2,
          "interval 0 emits only the first events");

    /* Managers and device sessions keep every request; callers simply do not
     * consult the cadence for them. The helper itself must never emit when
     * there is no state. */
    CHECK(!vp_audio_log_cadence_should_emit(NULL, 4, 64),
          "NULL cadence never emits");
}

static void test_timing(void) {
    VPAudioTimingWindow window = {0};
    VPAudioTimingSummary summary;

    vp_audio_timing_note(&window, 1000, 10, 20, 50, 240);
    vp_audio_timing_note(&window, 11000, 30, 40, 90, 240);
    vp_audio_timing_note(&window, 21000, 20, 10, 60, 240);
    vp_audio_timing_note(&window, 33000, 40, 30, 100, 240);

    vp_audio_timing_summary(&window, &summary);
    CHECK(summary.updates == 4, "timing update count");
    CHECK(summary.interval_samples == 3, "first update has no interval");
    CHECK(summary.interval_mean_us == (10000u + 10000u + 12000u) / 3u,
          "interval mean");
    CHECK(summary.interval_max_us == 12000, "interval max");
    CHECK(summary.render_mean_us == (10u + 30u + 20u + 40u) / 4u, "render mean");
    CHECK(summary.render_max_us == 40, "render max");
    CHECK(summary.wait_mean_us == (20u + 40u + 10u + 30u) / 4u, "wait mean");
    CHECK(summary.wait_max_us == 40, "wait max");
    CHECK(summary.service_mean_us == (50u + 90u + 60u + 100u) / 4u, "service mean");
    CHECK(summary.service_max_us == 100, "service max");
    CHECK(summary.accepted_frames == 960, "accepted frames accumulate");

    /* Reset keeps the last request timestamp: the next window's first
     * interval spans the boundary, matching the production sparse cadence. */
    vp_audio_timing_reset(&window);
    vp_audio_timing_summary(&window, &summary);
    CHECK(summary.updates == 0 && summary.accepted_frames == 0, "timing reset");
    vp_audio_timing_note(&window, 45000, 5, 6, 7, 240);
    vp_audio_timing_summary(&window, &summary);
    CHECK(summary.updates == 1 && summary.interval_samples == 1,
          "interval continues across a reset");
    CHECK(summary.interval_mean_us == 12000, "cross-window interval value");

    /* A backward/non-monotonic timestamp must not produce a huge interval. */
    vp_audio_timing_reset(&window);
    vp_audio_timing_note(&window, 1000, 0, 0, 0, 0);
    vp_audio_timing_note(&window, 500, 0, 0, 0, 0);
    vp_audio_timing_note(&window, 600, 0, 0, 0, 0);
    vp_audio_timing_summary(&window, &summary);
    CHECK(summary.interval_samples == 1 && summary.interval_mean_us == 100,
          "backward clock sample is skipped");
}

static void test_log_policy(void) {
    /* Real renderer update is recognized by session+CMIF command, while the
     * manager's command 4 is a different call. */
    CHECK(vp_audio_ipc_classify("renderer", 4, 4) == VP_AUDIO_IPC_RENDERER_UPDATE,
          "renderer cmd4 is update");
    CHECK(vp_audio_ipc_classify("audren:u", 4, 4)
              == VP_AUDIO_IPC_MANAGER_GET_DEVICE,
          "audren cmd4 is manager get-device");
    CHECK(!vp_audio_ipc_known_fast("renderer", 4, 4, 1),
          "renderer update uses its own path, not known_fast");
    CHECK(vp_audio_ipc_known_fast("audren:u", 4, 4, 1),
          "manager get-device is known fast");
    CHECK(!vp_audio_ipc_known_fast("audren:u", 4, 1, 1),
          "manager work-size is not known fast");
    CHECK(vp_audio_ipc_known_fast("aud-dev", 4, 3, 1), "device name cmd3");
    CHECK(vp_audio_ipc_known_fast("aud-dev", 4, 10, 1), "device name cmd10");
    CHECK(vp_audio_ipc_known_fast("aud-dev", 4, 13, 1), "device name cmd13");
    CHECK(!vp_audio_ipc_known_fast("aud-dev", 4, 0, 1), "device list cmd0 detailed");
    CHECK(!vp_audio_ipc_known_fast("aud-dev", 4, 14, 1), "device list cmd14 detailed");
    CHECK(!vp_audio_ipc_known_fast("aud-dev", 5, 3, 1),
          "transport control is never known fast");
    CHECK(!vp_audio_ipc_known_fast("aud-dev", 4, 3, 0),
          "malformed CMIF is never known fast");
    CHECK(!vp_audio_ipc_known_fast("fsp-srv", 4, 3, 1),
          "unrelated service is never known fast");
    CHECK(vp_audio_ipc_classify("renderer", 5, 4) == VP_AUDIO_IPC_CONTROL,
          "HIPC control overrides command number");
    CHECK(vp_audio_ipc_classify("renderer", 2, 4) == VP_AUDIO_IPC_SESSION_CLOSE,
          "HIPC close overrides command number");
    CHECK(vp_audio_ipc_classify("fsp-srv", 4, 4) == VP_AUDIO_IPC_NONE,
          "unrelated CMIF is not audio");

    /* Thread ownership is required and never implied by zero handles. */
    CHECK(!vp_audio_ipc_thread_owned(0, 0), "no owner and no update thread");
    CHECK(!vp_audio_ipc_thread_owned(0x12d, 0), "no update thread");
    CHECK(vp_audio_ipc_thread_owned(0x12d, 0x12d), "same renderer thread");
    CHECK(!vp_audio_ipc_thread_owned(0x12e, 0x12d), "different thread unowned");

    /* Model of the production suppression gate: no bound without a
     * trustworthy CMIF header, regardless of command contents. */
    int cmif_ready = 0;
    int bound = cmif_ready
        && (vp_audio_ipc_classify("renderer", 4, 4)
                == VP_AUDIO_IPC_RENDERER_UPDATE
            || vp_audio_ipc_known_fast("aud-dev", 4, 3, 1));
    CHECK(!bound, "malformed CMIF keeps the generic line");
}

int main(void) {
    test_classification();
    test_cadence();
    test_timing();
    test_log_policy();
    if (failures) {
        printf("FAIL audio ipc: %d checks\n", failures);
        return 1;
    }
    puts("PASS audio ipc: classification, cadence and timing windows");
    return 0;
}

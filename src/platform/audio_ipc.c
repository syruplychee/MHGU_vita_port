#include "audio_ipc.h"

#include <string.h>

int vp_audio_ipc_is_session(const char *session_name) {
    if (!session_name) return 0;
    return strcmp(session_name, "audren:u") == 0
        || strcmp(session_name, "aud-dev") == 0
        || strcmp(session_name, "renderer") == 0;
}

VPAudioIpcClass vp_audio_ipc_classify(const char *session_name,
                                      uint32_t message_type,
                                      uint32_t command) {
    if (!vp_audio_ipc_is_session(session_name)) return VP_AUDIO_IPC_NONE;
    /* Transport control and session close are HIPC messages, not CMIF
     * requests. Their payload word at the SFCI command offset is a transport
     * opcode (or stale data) and must never select an audio command. */
    if (message_type == VP_AUDIO_IPC_MESSAGE_CONTROL) return VP_AUDIO_IPC_CONTROL;
    if (message_type == VP_AUDIO_IPC_MESSAGE_CLOSE) return VP_AUDIO_IPC_SESSION_CLOSE;
    if (message_type != VP_AUDIO_IPC_MESSAGE_CMIF
        && message_type != VP_AUDIO_IPC_MESSAGE_CMIF_ALT)
        return VP_AUDIO_IPC_NONE;

    if (strcmp(session_name, "audren:u") == 0) {
        if (command == 0u) return VP_AUDIO_IPC_MANAGER_OPEN;
        if (command == 1u) return VP_AUDIO_IPC_MANAGER_GET_WORK_SIZE;
        if (command == 4u) return VP_AUDIO_IPC_MANAGER_GET_DEVICE;
        return VP_AUDIO_IPC_MANAGER_OTHER;
    }
    if (strcmp(session_name, "renderer") == 0) {
        if (command == 4u) return VP_AUDIO_IPC_RENDERER_UPDATE;
        if (command == 5u) return VP_AUDIO_IPC_RENDERER_START;
        if (command == 6u) return VP_AUDIO_IPC_RENDERER_STOP;
        if (command <= 3u || command == 9u || command == 13u)
            return VP_AUDIO_IPC_RENDERER_GET;
        return VP_AUDIO_IPC_RENDERER_OTHER;
    }
    return VP_AUDIO_IPC_DEVICE;
}

int vp_audio_ipc_known_fast(const char *session_name, uint32_t message_type,
                            uint32_t command, uint32_t cmif_ready) {
    if (!cmif_ready || !session_name) return 0;
    if (message_type != VP_AUDIO_IPC_MESSAGE_CMIF
        && message_type != VP_AUDIO_IPC_MESSAGE_CMIF_ALT)
        return 0;
    if (strcmp(session_name, "audren:u") == 0)
        return command == 4u;
    if (strcmp(session_name, "aud-dev") == 0)
        return command == 3u || command == 10u || command == 13u;
    return 0;
}

int vp_audio_ipc_thread_owned(uint32_t current_thread, uint32_t update_thread) {
    return update_thread != 0u && current_thread == update_thread;
}

const char *vp_audio_ipc_class_name(VPAudioIpcClass value) {
    switch (value) {
        case VP_AUDIO_IPC_MANAGER_OPEN: return "manager-open";
        case VP_AUDIO_IPC_MANAGER_GET_WORK_SIZE: return "manager-work-size";
        case VP_AUDIO_IPC_MANAGER_GET_DEVICE: return "manager-get-device";
        case VP_AUDIO_IPC_MANAGER_OTHER: return "manager-other";
        case VP_AUDIO_IPC_RENDERER_GET: return "renderer-get";
        case VP_AUDIO_IPC_RENDERER_UPDATE: return "renderer-update";
        case VP_AUDIO_IPC_RENDERER_START: return "renderer-start";
        case VP_AUDIO_IPC_RENDERER_STOP: return "renderer-stop";
        case VP_AUDIO_IPC_RENDERER_OTHER: return "renderer-other";
        case VP_AUDIO_IPC_DEVICE: return "device";
        case VP_AUDIO_IPC_CONTROL: return "control";
        case VP_AUDIO_IPC_SESSION_CLOSE: return "session-close";
        default: return "none";
    }
}

int vp_audio_log_cadence_should_emit(VPAudioLogCadence *cadence,
                                     uint32_t first, uint32_t interval) {
    if (!cadence) return 0;
    ++cadence->seen;
    int emit = cadence->seen <= first
        || (interval && (cadence->seen % interval) == 0u);
    if (emit) ++cadence->emitted;
    return emit;
}

void vp_audio_log_cadence_reset(VPAudioLogCadence *cadence) {
    if (!cadence) return;
    memset(cadence, 0, sizeof(*cadence));
}

void vp_audio_timing_note_periods(VPAudioTimingWindow *window,
                                  uint64_t request_us, uint64_t render_us,
                                  uint64_t wait_us, uint64_t service_us,
                                  uint32_t accepted_frames, uint32_t periods,
                                  uint32_t source_frames, uint32_t debt_frames,
                                  uint32_t starvation_periods,
                                  uint32_t cap_hits) {
    if (!window) return;
    ++window->updates;
    if (window->have_prev && request_us >= window->prev_request_us) {
        uint64_t interval = request_us - window->prev_request_us;
        window->interval_sum_us += interval;
        ++window->interval_samples;
        if (interval > window->interval_max_us) window->interval_max_us = interval;
    }
    window->prev_request_us = request_us;
    window->have_prev = 1;
    window->render_sum_us += render_us;
    if (render_us > window->render_max_us) window->render_max_us = render_us;
    window->wait_sum_us += wait_us;
    if (wait_us > window->wait_max_us) window->wait_max_us = wait_us;
    window->service_sum_us += service_us;
    if (service_us > window->service_max_us) window->service_max_us = service_us;
    window->accepted_frames += accepted_frames;
    window->periods += periods;
    if (periods > window->periods_max) window->periods_max = periods;
    window->source_frames += source_frames;
    window->debt_frames += debt_frames;
    window->starvation_periods += starvation_periods;
    window->cap_hits += cap_hits;
}

void vp_audio_timing_note(VPAudioTimingWindow *window, uint64_t request_us,
                          uint64_t render_us, uint64_t wait_us,
                          uint64_t service_us, uint32_t accepted_frames) {
    vp_audio_timing_note_periods(window, request_us, render_us, wait_us,
                                 service_us, accepted_frames, 1u,
                                 accepted_frames, 0u, 0u, 0u);
}

void vp_audio_timing_summary(const VPAudioTimingWindow *window,
                             VPAudioTimingSummary *summary) {
    if (!summary) return;
    memset(summary, 0, sizeof(*summary));
    if (!window) return;
    summary->updates = window->updates;
    summary->interval_samples = window->interval_samples;
    summary->interval_max_us = window->interval_max_us;
    summary->render_max_us = window->render_max_us;
    summary->wait_max_us = window->wait_max_us;
    summary->service_max_us = window->service_max_us;
    summary->accepted_frames = window->accepted_frames;
    summary->periods = window->periods;
    summary->periods_max = window->periods_max;
    summary->source_frames = window->source_frames;
    summary->debt_frames = window->debt_frames;
    summary->starvation_periods = window->starvation_periods;
    summary->cap_hits = window->cap_hits;
    if (window->updates) {
        summary->render_mean_us = window->render_sum_us / window->updates;
        summary->wait_mean_us = window->wait_sum_us / window->updates;
        summary->service_mean_us = window->service_sum_us / window->updates;
    }
    if (window->interval_samples)
        summary->interval_mean_us = window->interval_sum_us / window->interval_samples;
}

void vp_audio_timing_reset(VPAudioTimingWindow *window) {
    if (!window) return;
    /* Keep the previous request timestamp so the first interval of the next
     * window spans the gap between windows instead of becoming undefined. */
    uint64_t prev = window->prev_request_us;
    uint32_t have_prev = window->have_prev;
    memset(window, 0, sizeof(*window));
    window->prev_request_us = prev;
    window->have_prev = have_prev;
}

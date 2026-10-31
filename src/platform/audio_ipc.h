/* M6-B6: audio IPC classification, bounded log cadence and timing windows.
 *
 * The audren service has three distinct HIPC objects:
 *   - "audren:u": IAudioRendererManager. Command 0 OpenAudioRenderer,
 *     command 1 GetWorkBufferSize, command 4
 *     GetAudioDeviceServiceWithRevisionInfo. Command 4 is a manager call that
 *     returns an "aud-dev" session; it is not IAudioRenderer::RequestUpdate.
 *   - "aud-dev": IAudioDevice, whose calls are control traffic.
 *   - "renderer": IAudioRenderer. Command 4 is RequestUpdate, the only audio
 *     IPC that runs at the device cadence (~65-200 calls per second).
 *
 * The classification is deliberately independent of the HIPC message type:
 * transport control (type 5) and session close (type 2) carry numbers that
 * must not be read as CMIF commands, and only CMIF request types (4 and 6)
 * carry a command at the SFCI header. This module has no platform
 * dependencies so the classification and the bounded cadence can be tested
 * on the host. */
#ifndef VP_AUDIO_IPC_H
#define VP_AUDIO_IPC_H

#include <stdint.h>

#define VP_AUDIO_IPC_MESSAGE_CLOSE 2u
#define VP_AUDIO_IPC_MESSAGE_CMIF 4u
#define VP_AUDIO_IPC_MESSAGE_CONTROL 5u
#define VP_AUDIO_IPC_MESSAGE_CMIF_ALT 6u

typedef enum {
    VP_AUDIO_IPC_NONE = 0,
    VP_AUDIO_IPC_MANAGER_OPEN,
    VP_AUDIO_IPC_MANAGER_GET_WORK_SIZE,
    VP_AUDIO_IPC_MANAGER_GET_DEVICE,
    VP_AUDIO_IPC_MANAGER_OTHER,
    VP_AUDIO_IPC_RENDERER_GET,
    VP_AUDIO_IPC_RENDERER_UPDATE,
    VP_AUDIO_IPC_RENDERER_START,
    VP_AUDIO_IPC_RENDERER_STOP,
    VP_AUDIO_IPC_RENDERER_OTHER,
    VP_AUDIO_IPC_DEVICE,
    VP_AUDIO_IPC_CONTROL,
    VP_AUDIO_IPC_SESSION_CLOSE,
    VP_AUDIO_IPC_COUNT
} VPAudioIpcClass;

/* True for the three sessions this loader models for nn::audio. */
int vp_audio_ipc_is_session(const char *session_name);

/* Classify one received HIPC message. `command` is only consumed for CMIF
 * request messages; callers must still pass the value decoded from the SFCI
 * header, but transport/close messages override it. */
VPAudioIpcClass vp_audio_ipc_classify(const char *session_name,
                                      uint32_t message_type,
                                      uint32_t command);

/* The two reviewed manager/device calls that run at the audio update cadence:
 * audren:u GetAudioDeviceServiceWithRevisionInfo (command 4) and aud-dev
 * GetActiveAudioDeviceName (commands 3/10/13). Only a CMIF-ready request has
 * a trustworthy command; transport/close never qualifies. */
int vp_audio_ipc_known_fast(const char *session_name, uint32_t message_type,
                            uint32_t command, uint32_t cmif_ready);

/* True only when the caller is the thread that last issued an actual renderer
 * RequestUpdate. Both zero (no owner, no update thread) is not owned. */
int vp_audio_ipc_thread_owned(uint32_t current_thread, uint32_t update_thread);

const char *vp_audio_ipc_class_name(VPAudioIpcClass value);

/* Bounded log cadence: emit the first `first` events and then every
 * `interval`-th event. A negative return is not used; NULL means "never". */
typedef struct {
    uint32_t seen;
    uint32_t emitted;
} VPAudioLogCadence;

int vp_audio_log_cadence_should_emit(VPAudioLogCadence *cadence,
                                     uint32_t first, uint32_t interval);
void vp_audio_log_cadence_reset(VPAudioLogCadence *cadence);

/* Per-renderer RequestUpdate timing window. `request_us` is the entry
 * timestamp of each update, so the interval is the real client cadence and
 * not the time spent inside the service. All accumulators are reset by the
 * caller after it emits the sparse summary. */
typedef struct {
    uint32_t updates;
    uint32_t interval_samples;
    uint64_t prev_request_us;
    uint32_t have_prev;
    uint64_t interval_sum_us;
    uint64_t interval_max_us;
    uint64_t render_sum_us;
    uint64_t render_max_us;
    uint64_t wait_sum_us;
    uint64_t wait_max_us;
    uint64_t service_sum_us;
    uint64_t service_max_us;
    uint64_t accepted_frames;
    /* M6-B11 automatic execution clock: fresh processing periods rendered
     * from the applied snapshot. `periods` sums all periods in the window;
     * `periods_max` is the largest number for one RequestUpdate. */
    uint32_t periods;
    uint32_t periods_max;
    uint32_t source_frames;      /* output frames covered by a real voice */
    uint32_t debt_frames;        /* pace shortfall after the bounded catch-up */
    uint32_t starvation_periods; /* periods with an input-starved voice */
    uint32_t cap_hits;           /* requests stopped by the period cap */
} VPAudioTimingWindow;

typedef struct {
    uint32_t updates;
    uint32_t interval_samples;
    uint64_t interval_mean_us;
    uint64_t interval_max_us;
    uint64_t render_mean_us;
    uint64_t render_max_us;
    uint64_t wait_mean_us;
    uint64_t wait_max_us;
    uint64_t service_mean_us;
    uint64_t service_max_us;
    uint64_t accepted_frames;
    uint32_t periods;
    uint32_t periods_max;
    uint32_t source_frames;
    uint32_t debt_frames;
    uint32_t starvation_periods;
    uint32_t cap_hits;
} VPAudioTimingSummary;

void vp_audio_timing_note(VPAudioTimingWindow *window, uint64_t request_us,
                          uint64_t render_us, uint64_t wait_us,
                          uint64_t service_us, uint32_t accepted_frames);
/* Same accounting, plus the automatic-clock period accounting. The service
 * calls this once per RequestUpdate after all fresh periods were rendered and
 * accepted; `accepted_frames` must therefore be the sum over all periods. */
void vp_audio_timing_note_periods(VPAudioTimingWindow *window,
                                  uint64_t request_us, uint64_t render_us,
                                  uint64_t wait_us, uint64_t service_us,
                                  uint32_t accepted_frames, uint32_t periods,
                                  uint32_t source_frames, uint32_t debt_frames,
                                  uint32_t starvation_periods,
                                  uint32_t cap_hits);
void vp_audio_timing_summary(const VPAudioTimingWindow *window,
                             VPAudioTimingSummary *summary);
void vp_audio_timing_reset(VPAudioTimingWindow *window);

#endif

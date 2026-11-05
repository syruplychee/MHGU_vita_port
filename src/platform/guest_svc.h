#ifndef VP_GUEST_SVC_H
#define VP_GUEST_SVC_H
#include <stdint.h>
/* Set once by the loader before guest threads start; diagnostic only. */
void vp_guest_memory_audit_set(unsigned enabled);
#include <stdio.h>
#include "host_bridge.h"
#include "guest_storage.h"
#include "guest_save.h"
#include "nvmap_table.h"
#include "memory_attributes.h"
#include "audio_ipc.h"

/* Native implementation of the AArch32 Horizon user SVC ABI observed at the
 * locked SDK's veneer table. The loader keeps the original wrapper bodies and
 * resumes them at the instruction after each SVC; only the kernel side is
 * replaced. Unsupported ids return VP_BRIDGE_UNSUPPORTED instead of a
 * fabricated success. */
#define VP_GUEST_SVC_BASE 0x300u
#define VP_GUEST_SVC_MAX 0x380u
/* Not an SVC: the loader's MRRC p15,c14 islands use the same bridge frame.
 * frame.reserved carries the destination even register number. */
#define VP_GUEST_TIMER_SERVICE 0x37Fu
#define VP_GUEST_REGION_LIMIT 32u
/* Kernel handle table. A real Horizon process may hold far more than 64
 * handles, and the title's movie stack (NuPlayer, NuCachedSource2,
 * NuMediaExtractor, the software AVC decoder and their event/thread pairs)
 * legitimately keeps ~90 live at once. The image's observed peak was a full
 * 64-entry table whose last slot was a decoder thread handle; the table is
 * therefore sized well above the working set instead of reproducing an
 * arbitrary low cap. Handle values stay in the opaque 0x100.. range. */
#define VP_GUEST_HANDLE_LIMIT 256u
/* Server sessions are a narrower resource: each VPGuestSession carries the
 * per-service state of one HIPC connection (save objects, binder slots, GPU
 * address space), so the array is not multiplied by the handle-table growth.
 * Only kind-2 handles consume a session entry. */
#define VP_GUEST_SESSION_LIMIT 64u
#define VP_GUEST_HANDLE_FIRST 0x100u
#define VP_GUEST_DOMAIN_OBJECT_LIMIT 64u
/* Device descriptors opened on the nvdrv service. Boot opens six (ctrl,
 * ctrl-gpu, as-gpu, two GPU channels, nvmap); movie playback then opens two
 * NVDEC channels and a VIC channel, and close does not compact the table. */
#define VP_GUEST_NVDRV_DEVICE_LIMIT 32u
#define VP_GUEST_NVDRV_NAME_MAX 32u
#define VP_GUEST_NVDRV_IOCTL_TRACE 64u
#define VP_GUEST_NVCTRL_EVENT_LIMIT 64u
#define VP_GUEST_GPU_AS_ALLOC_LIMIT 8u
/* nvmap user-handle capacity lives in nvmap_table.h. It bounds simultaneously
 * live allocations (the GPU working set alone reaches 16 and the movie
 * decoder adds more), while Free retires records so a session may create far
 * more than 128 allocations over its lifetime. */
/* svcSetMemoryAttribute state. The guest runs natively on the Vita CPU, so
 * the attributes have no cache effect on ordinary loads and stores here; they
 * are recorded for svcQueryMemory and the future graphics boundary, which
 * must know which buffers the title designated device-shared/uncached. The
 * table is effective state, not an event log: masked sets and clears update
 * the represented pages and unmapping drops them. VP_GUEST_ATTRIBUTE_LIMIT
 * bounds simultaneously distinct nonzero intervals, not the session's number
 * of calls, so repeated resource reloads do not exhaust it. */
#define VP_GUEST_ATTRIBUTE_LIMIT VP_MEMORY_ATTRIBUTE_LIMIT
/* MapBufferEx records live for the whole session because the locked title
 * keeps its GPU buffers mapped: the first boot phase alone creates 17
 * mappings before the render loop starts, and each later buffer adds more.
 * UnmapBuffer/FreeSpace are not yet part of the live request set, so the
 * table is sized for a full session's working set instead of being
 * reclaimed. */
#define VP_GUEST_GPU_AS_MAP_LIMIT 64u
#define VP_GUEST_BINDER_SLOT_LIMIT 16u
/* svcMapMemory copies instead of aliasing, but the destination range is still
 * owned by the mapping until svcUnmapMemory. The table is bounded; exceeding
 * it rejects the map rather than letting the allocator reuse live pages. */
#define VP_GUEST_MAP_COPY_LIMIT 128u
/* Queued inline-keyboard replies. The title polls the interactive channel at
 * frame rate and drains replies in order; four entries overflowed as soon as
 * a text change and a state change landed in the same poll window. */
#define VP_SWKBD_REPLY_QUEUE 16u
/* Inline software-keyboard protocol values. These are part of the reviewed
 * Horizon library-applet ABI and are used by the platform layer's applet
 * model and by its gamepad focus check. */
#define VP_SWKBD_REPLY_FINISHED_INIT 0u
#define VP_SWKBD_REPLY_DEFAULT 1u
#define VP_SWKBD_REPLY_CHANGED_STRING 2u
#define VP_SWKBD_REPLY_DECIDED_ENTER 5u
#define VP_SWKBD_REPLY_DECIDED_CANCEL 6u
#define VP_SWKBD_REPLY_CHANGED_STRING_UTF8 7u
#define VP_SWKBD_REPLY_DECIDED_ENTER_UTF8 9u
#define VP_SWKBD_REPLY_UNSET_CUSTOMIZE_DIC 10u
#define VP_SWKBD_REPLY_RELEASED_USER_WORD_INFO 11u
#define VP_SWKBD_REPLY_CHANGED_STRING_V2 13u
#define VP_SWKBD_STATE_NOT_INITIALIZED 0u
#define VP_SWKBD_STATE_HIDDEN 1u
#define VP_SWKBD_STATE_APPEARING 2u
#define VP_SWKBD_STATE_SHOWN 3u
#define VP_SWKBD_STATE_DISAPPEARING 4u
#define VP_SWKBD_UTF16_REPLY_BYTES 0x3ec

/* CMIF domain object interfaces modeled by this service layer. The ids are
 * server-assigned opaque values; the client's domain proxy sends the target
 * id in its 16-byte domain request header. */
enum {
    VP_GUEST_OBJECT_NONE = 0,
    VP_GUEST_OBJECT_APPLICATION_PROXY_SERVICE = 1,
    VP_GUEST_OBJECT_APPLICATION_PROXY = 2,
    VP_GUEST_OBJECT_APPLICATION_FUNCTIONS = 3,
    VP_GUEST_OBJECT_LIBRARY_APPLET_CREATOR = 4,
    VP_GUEST_OBJECT_COMMON_STATE_GETTER = 5,
    VP_GUEST_OBJECT_SELF_CONTROLLER = 6,
    VP_GUEST_OBJECT_WINDOW_CONTROLLER = 7,
    VP_GUEST_OBJECT_AUDIO_CONTROLLER = 8,
    VP_GUEST_OBJECT_DISPLAY_CONTROLLER = 9,
    VP_GUEST_OBJECT_PROCESS_WINDING_CONTROLLER = 10,
    VP_GUEST_OBJECT_DEBUG_FUNCTIONS = 11,
    VP_GUEST_OBJECT_PARENTAL_CONTROL_SERVICE_FACTORY = 12,
    VP_GUEST_OBJECT_PARENTAL_CONTROL_SERVICE = 13,
    VP_GUEST_OBJECT_AOC_SERVICE = 14,
    VP_GUEST_OBJECT_TIME_SERVICE = 15,
    VP_GUEST_OBJECT_TIME_SYSTEM_CLOCK = 16,
    VP_GUEST_OBJECT_TIME_STEADY_CLOCK = 17,
    VP_GUEST_OBJECT_TIME_ZONE_SERVICE = 18,
    VP_GUEST_OBJECT_FILE_SYSTEM_PROXY = 19,
    VP_GUEST_OBJECT_FS_STORAGE = 20,
    /* BuiltInSystem data titles (for example NgWord2 0x0100000000000823) are
     * firmware content that this loader must not ship. The object exists so
     * the guest's mount succeeds, but it is empty and every read reports the
     * missing content; it is never treated as the title's RomFS. */
    VP_GUEST_OBJECT_FS_STORAGE_EMPTY = 21,
    VP_GUEST_OBJECT_FRIEND_SERVICE = 22,
    VP_GUEST_OBJECT_BCAT_SERVICE_CREATOR = 23,
    VP_GUEST_OBJECT_BCAT_SERVICE = 24,
    VP_GUEST_OBJECT_BCAT_PROGRESS = 25,
    VP_GUEST_OBJECT_LIBRARY_APPLET_ACCESSOR = 26,
    VP_GUEST_OBJECT_STORAGE = 27,
    VP_GUEST_OBJECT_STORAGE_ACCESSOR = 28,
    VP_GUEST_OBJECT_ACC_MANAGER_FOR_APP = 29,
    VP_GUEST_OBJECT_ACC_ASYNC_CONTEXT = 30,
    VP_GUEST_OBJECT_ACC_PROFILE = 31,
    /* IFileSystem returned by fsp-srv OpenSaveDataFileSystem. Its root is a
     * real directory below ux0:data/mhgu-vita-port/savedata; commands are
     * backed by guest_save.c and return nn::fs result codes. */
    VP_GUEST_OBJECT_FS_SAVE = 32,
    /* IFile and IDirectory returned by that file system's OpenFile command 8
     * and OpenDirectory command 9. */
    VP_GUEST_OBJECT_FS_SAVE_FILE = 33,
    VP_GUEST_OBJECT_FS_SAVE_DIR = 34,
};

/* nn::visrv display objects reachable from IApplicationDisplayService. The
 * root vi:m session returns the application display service; its commands
 * 100..103 return the narrower relay/system/manager/indirect services. */
enum {
    VP_GUEST_VI_APPLICATION = 1,
    VP_GUEST_VI_SYSTEM = 2,
    VP_GUEST_VI_MANAGER = 3,
    VP_GUEST_VI_RELAY = 4,
    VP_GUEST_VI_INDIRECT = 5,
};

typedef struct {
    uint32_t base, size;
    uint32_t state, permission;
    const char *name;
} VPGuestRegion;

/* Software model of the nvhost-as-gpu GPU address space. The kernel ioctls
 * program the GMMU page tables; this loader instead keeps the same virtual
 * range layout as bookkeeping. Nothing here aliases or translates memory:
 * GPU VA allocation is answered with the layout the guest requested, while
 * any operation that would need real GPU mappings is an explicit unsupported
 * path until its model exists. */
typedef struct {
    uint32_t initialised;
    uint32_t big_page_size;
    uint64_t va_start;
    uint64_t va_split;
    uint64_t va_end;
    uint64_t small_next; /* next unallocated small-page byte offset */
    uint64_t big_next;   /* next unallocated big-page byte offset */
    int32_t bound_channel; /* nvhost-gpu fd bound by BindChannel, 0 = none */
    uint32_t bound_channel_set;
    /* Reservations created by AllocateSpace. MapBufferEx with a fixed
     * offset is only accepted inside one of these ranges. */
    uint32_t allocation_count;
    struct {
        uint64_t offset;
        uint64_t size;
        uint32_t page_size;
    } allocations[VP_GUEST_GPU_AS_ALLOC_LIMIT];
    /* GPU VA mappings created by MapBufferEx. device_address is the nvmap
     * CPU address plus the buffer offset, i.e. the memory a GPU access would
     * have to read; offset is the GPU virtual address. */
    uint32_t mapping_count;
    struct {
        uint32_t handle;
        uint32_t kind;
        uint32_t fixed;
        uint32_t big_page;
        uint32_t device_address;
        uint64_t offset;
        uint64_t size;
    } mappings[VP_GUEST_GPU_AS_MAP_LIMIT];
    /* MapBufferEx with the Remap flag (0x100) re-points a subrange of an
     * existing mapping at a device address inside the same nvmap allocation.
     * The loader validates the range against the base mapping and records
     * how many such PTE rewrites were requested; no PTE is programmed. */
    uint32_t remap_count;
} VPGuestGpuAs;

/* nvmap object records and their table operations are declared in
 * nvmap_table.h so the host regression exercises the same code as the
 * dispatcher. */

/* P0 media command-stream instrumentation. NVDEC and VIC receive host1x
 * command buffers whose headers use the same ChCommandHeader layout as the
 * GPU channel: {u16 value, u12 method, u4 submission mode}. This record
 * mirrors the CDMA state machine so the title's media methods can be decoded
 * from evidence; it executes nothing and never changes a submit result.
 * Enabled only by ux0:data/mhgu-vita-port/media-instrument.enable. */
typedef struct {
    uint32_t instrument_checked;
    uint32_t instrument;
    /* Per-engine CDMA parser state, persisted across submits like the
     * hardware channel. */
    uint32_t current_class;
    uint32_t count;
    uint32_t offset;
    uint32_t mask;
    uint32_t incrementing;
    uint32_t method0;
    uint32_t commands;
    uint32_t executes;
    /* Address registers as the engine stores them: the THI argument is
     * shifted left by 8, so these are byte GPU addresses. */
    uint32_t nvdec_codec;
    uint64_t nvdec_picture_info;
    uint64_t nvdec_bitstream;
    uint64_t nvdec_control;
    uint64_t vic_config;
    uint64_t vic_luma;
    uint64_t vic_chroma;
} VPGuestMediaParse;

/* /dev/nvhost-gpu channel control state. The kernel programs the channel and
 * its GPFIFO; this loader has no GPU, so each request it models is recorded
 * here for a future device model. Submission endpoints are only accepted
 * once their completion semantics can be stated honestly. */
typedef struct {
    /* Device descriptor this channel state belongs to. nvhost gives every
     * open of /dev/nvhost-gpu its own kernel channel object, so the loader
     * keeps one state record per fd; a second open must not inherit the
     * first channel's GPFIFO allocation or syncpoint. */
    int32_t fd;
    int32_t nvmap_fd;
    uint32_t nvmap_set;
    /* AllocGPFIFOEx2 bookkeeping. The guest's command FIFO is not executed
     * by any device here; the model records its size and associates the
     * channel with a modeled syncpoint so later submissions have a fence
     * the loader can honestly complete only when it executes them. */
    uint32_t gpfifo_entries;
    uint32_t gpfifo_flags;
    uint32_t gpfifo_allocated;
    uint32_t syncpoint;
    uint32_t fence_value;
    /* AllocateObjectContext: the kernel allocates a GPU object for a class.
     * No device object exists here, so the loader hands out a virtual,
     * monotonically increasing id and records the class the guest asked
     * for. The id is opaque to the driver; it only appears in command
     * streams, which this loader does not execute. */
    uint32_t object_count;
    uint32_t next_object_id;
    struct {
        uint32_t class_num, flags, id;
    } objects[8];
    /* Channel control settings: CHANNEL_SET_TIMEOUT, CHANNEL_ZCULL_BIND,
     * CHANNEL_SET_ERROR_NOTIFIER, CHANNEL_SET_PRIORITY and
     * CHANNEL_SET_TIMESLICE. The loader owns no kernel channel object, so
     * these are recorded as evidence of what the guest asked a real GM20B
     * channel to do; the logs say they were not programmed. */
    uint32_t timeout;
    uint64_t zcull_gpu_va;
    uint32_t zcull_mode;
    uint64_t error_notifier_offset;
    uint64_t error_notifier_size;
    uint32_t error_notifier_mem;
    uint32_t priority;
    uint32_t timeslice;
    /* CHANNEL_SET_CLIENT_DATA: opaque driver user data round-tripped by the
     * kernel. It has no meaning to this loader, but GetClientData must return
     * exactly what was set. */
    uint64_t client_data;
    /* Inlined GPFIFO submissions (nvdrv Ioctl2 ioctl 0xc018481b). The guest
     * hands the kernel a 24-byte submission struct plus the GPFIFO entries
     * themselves in the second send buffer. No device executes them here: the
     * loader records the submission and advances the channel's submitted
     * fence counter exactly as the kernel's syncpoint bookkeeping would, but
     * it signals nothing. A later syncpoint wait therefore has no completion
     * source until a command-list consumer exists. */
    uint32_t submit_count;
    uint64_t submit_address;      /* params.address (unused for inlined form) */
    uint32_t submit_entries;
    uint32_t submit_flags;
    uint32_t submit_fence_id;
    uint32_t submit_fence_value;
    uint32_t submit_inline_size;
    uint64_t submit_inline_address;
    /* Portable Maxwell/MME decode state for this channel, allocated on the
     * first submission that carries readable command words. */
    void *gpu_decode;
    /* /dev/nvhost-nvdec channel state. The device is opened by the title's
     * media stack and has its own syncpoint, nvmap binding and submit
     * ioctl. The transport model still retires the guest fence; when
     * media-video.enable is present, supported H.264 picture-info/AU state is
     * synchronously decoded by the media-video hook. Without that marker the
     * channel remains fence-only. */
    uint32_t nvdec_device;
    /* /dev/nvhost-vic channel state. VIC shares NVDEC's nvhost command
     * channel ABI (group 0 cmd 1/2/3/9/0xa and group 'H' cmd 1); the media
     * player opens it to scale/convert decoded frames onto the display
     * surface. With media-video.enable the supported pitch-linear YUV420
     * output is synchronously copied by the VIC hook; unsupported layouts
     * remain explicit misses rather than guessed surfaces. */
    uint32_t vic_device;
    uint32_t nvdec_syncpoint_valid;
    int32_t nvdec_nvmap_fd;
    uint32_t nvdec_nvmap_set;
    uint32_t nvdec_submit_timeout;
    uint32_t nvdec_submit_count;
    uint32_t nvdec_cmd_buffers;
    uint32_t nvdec_cmd_words;
    uint32_t nvdec_syncpoints;
    /* P0 observation state for this media channel's command buffers. */
    VPGuestMediaParse media;
} VPGuestGpuChannel;

/* Server-side state of a service session. HIPC CloneCurrentObject creates a
 * second client handle bound to the same server session, so this state is
 * reference-counted and shared by every handle that names it. */
#define VP_GUEST_NPAD_ID_LIMIT 16u
/* IHidServer StartSixAxisSensor/StopSixAxisSensor state. The 4-byte handle
 * names npad style index, npad id and device index; enabling only gates
 * whether samples would be produced, and no controller exists here to produce
 * them, so the exact handle and its enabled flag are recorded. */
#define VP_GUEST_SIXAXIS_LIMIT 8u
typedef struct {
    uint32_t npad_type;
    uint32_t npad_id;
    uint32_t device_index;
    uint64_t aruid;
    uint32_t enabled;
} VPGuestSixAxisSensor;
/* IHidServer CreateActiveVibrationDeviceList returns an
 * IActiveVibrationDeviceList; its command 0 ActivateVibrationDevice appends a
 * {npad_type, npad_id, device_index} handle to the server-side list after
 * validating it. No haptic device exists here, so the list is the whole
 * effect and the request values are preserved exactly. */
#define VP_GUEST_VIBRATION_LIMIT 16u
typedef struct {
    uint32_t npad_type;
    uint32_t npad_id;
    uint32_t device_index;
} VPGuestVibrationDevice;
/* IHidServer AcquireNpadStyleSetUpdateEventHandle returns an auto-clear
 * readable event per {aruid, npad id}; the reference creates it on first use
 * and signals it on every acquire. No controller exists to change a style
 * set later, so an acquired event is signalled exactly once per acquire. */
#define VP_GUEST_STYLE_EVENT_LIMIT 10u
typedef struct {
    uint64_t aruid;
    uint32_t npad_id;
    uint32_t handle;
} VPGuestStyleEvent;
/* nn::mmnv::IRequest ("mm:u") request slots. See VPGuestSvc.media_clock. */
#define VP_GUEST_MEDIA_CLOCK_LIMIT 8u
typedef struct {
    uint32_t active;
    uint32_t request_id;
    uint32_t module;      /* nn::mmnv::Module; 5 = Nvdec */
    uint32_t priority;
    uint32_t clear_mode;
    uint32_t min_setting;
    uint32_t max_setting;
    uint32_t current;
} VPGuestMediaClock;
typedef struct {
    uint32_t domain_state;    /* 0 before query, 1 query seen, 2 converted */
    uint32_t next_object_id;
    uint8_t objects[VP_GUEST_DOMAIN_OBJECT_LIMIT];
    /* Per-domain-object state for save file system objects, indexed by the
     * same id as `objects`. Allocated when OpenSaveDataFileSystem, OpenFile
     * or OpenDirectory returns an object and freed by the CMIF domain close
     * message or when the session is released. */
    VPGuestSaveObject *save_objects[VP_GUEST_DOMAIN_OBJECT_LIMIT];
    /* Applet notification queue. The native platform models an application
     * starting in focus, which queues one FocusStateChanged message. The queue
     * carries explicit message types so a platform-initiated Exit request can
     * be delivered to the title's own message loop; it is not a count-only
     * approximation. applet_event is the notification event handle. */
    uint32_t applet_event;
    uint32_t applet_queue[8];
    uint32_t applet_queue_head;
    uint32_t applet_messages;
    uint32_t foreground_rights;
    uint8_t focus_handling[3]; /* SetFocusHandlingMode flags (no focus changes here) */
    uint32_t out_of_focus_suspending;
    uint32_t parental_initialized;
    uint64_t parental_process;
    uint64_t fs_process; /* fsp-srv SetCurrentProcess argument */
    uint32_t fs_access_log_mode;
    /* acc:u0 InitializeApplicationInfo (command 100). The loader runs the
     * launched title, so the application exists; the storage identity the
     * reference copies from ARP is not available here and is recorded as an
     * explicit loader default rather than silently fabricated per call. */
    uint32_t acc_initialized;
    /* IApplicationFunctions PopLaunchParameter(AccountPreselectedUser). The
     * loader launches the title itself, so it supplies the one launch
     * parameter the launcher would queue: the 0x88-byte preselected-user
     * record the SDK's TryPopPreselectedUser parses. The channel yields that
     * storage exactly once, like the reference's one-element deque. */
    uint32_t preselected_user_popped;
    /* acc:u0 GetBaasAccountManagerForApplication (command 101) returns an
     * IManagerForApplication proxy over the selected user. There is no
     * Nintendo Account backend, so the manager reports the loader's
     * preselected user as the last opened account and an empty ID-token
     * cache: EnsureIdTokenCacheAsync hands out one already-complete
     * IAsyncContext per session with one signaled completion event. */
    uint32_t acc_async_event;
    /* Per-IProfile Uid recorded by GetProfile (command 5). The reference
     * returns a profile interface for any Uid and only fails when that
     * interface is asked for data the profile database does not contain;
     * the loader models exactly one user, so other Uids are answered with
     * the reference's unknown-user result instead of a fabricated profile. */
    uint8_t acc_profile_uid[VP_GUEST_DOMAIN_OBJECT_LIMIT][16];
    /* friend:u IFriendService::GetCompletionEvent. The reference creates one
     * event per service instance and returns it on every call. No friend
     * operation is issued here, so the event stays unsignaled: that is the
     * exact state of a console whose friend operations never complete. */
    uint32_t friend_completion_event;
    /* audren:u GetWorkBufferSize result for this session's renderer
     * parameter; the client allocates exactly this many bytes of transfer
     * memory before OpenAudioRenderer. */
    uint32_t audio_work_size;
    /* IAudioRenderer (session created by audren:u command 0). The renderer
     * is modeled as a silent consumer: it owns no DSP or audio device, so
     * RequestUpdate zero-fills the client's output buffer (the same initial
     * state the reference applies) and completes the render cycle
     * immediately. Every command is answered from these recorded parameters;
     * the model is logged on each call and documented in the ADR. */
    uint32_t audio_sample_rate;
    uint32_t audio_sample_count;
    uint32_t audio_mix_buffer_count;
    uint32_t audio_voice_count;
    uint32_t audio_sink_count;
    uint32_t audio_effect_count;
    uint32_t audio_revision;
    /* ExecutionMode from OpenAudioRenderer: 0=Auto, 1=Manual in the reviewed
     * reference parameter (mode byte at +0x23). Only Auto enables the
     * service-paced catch-up periods; Manual keeps the one-period update. */
    uint32_t audio_execution_mode;
    uint32_t audio_active;
    uint32_t audio_rendering_time_limit;
    uint32_t audio_system_event;
    uint32_t audio_tmem_handle;
    uint32_t audio_tmem_size;
    uint32_t audio_voice_drop_bits; /* f32 bit pattern */
    uint32_t audio_update_count;
    /* M6-B6/M6-B7 per-session audio IPC observability. The renderer's
     * RequestUpdate is the only audio request at the device cadence; its
     * request line and its timing window are bounded here instead of sharing
     * a process-global counter with other sessions. */
    VPAudioLogCadence audio_request_log;
    VPAudioLogCadence audio_manager_get_device_log;
    VPAudioLogCadence audio_device_name_log;
    VPAudioLogCadence audio_reset_log;
    VPAudioLogCadence audio_close_log;
    VPAudioLogCadence audio_control_log;
    VPAudioLogCadence audio_meta_log;
    VPAudioTimingWindow audio_timing;
    uint32_t audio_ipc_class_counts[VP_AUDIO_IPC_COUNT];
    uint32_t audio_manager_cmd_counts[8];
    uint32_t audio_device_cmd_counts[16];
    uint32_t audio_reset_count;
    uint32_t audio_close_count;
    uint32_t audio_control_count;
    uint32_t audio_meta_count;
    uint32_t audio_probe_block_n;
    uint64_t audio_probe_prev_us;
    uint64_t audio_probe_block_us;
    uint64_t audio_probe_render_us;
    uint64_t audio_probe_wait_us;
    uint64_t audio_timing_underrun_base;
    uint64_t audio_timing_silence_base;
    uint64_t audio_timing_overrun_base;
    uint32_t audio_timing_sink_base_valid;
    /* M5 renderer process state. The renderer session owns one mixer; the
     * SceAudioOut sink is a process-wide resource handed to the first
     * renderer that starts (see audio_sink.h). */
    void *audio_mixer;            /* VPAudioRenderer*, created by Start */
    uint32_t audio_sink_owned;    /* this renderer owns the process sink */
    uint32_t audio_sink_refused;  /* another renderer owns the sink */
    uint32_t audio_sink_submits;
    uint32_t audio_sink_dropped;
    uint32_t audio_render_errors;
    uint64_t audio_render_frames;
    uint32_t audio_last_peak;
    uint32_t audio_last_rms;
    uint64_t audio_zero_left;
    uint64_t audio_zero_right;
    uint32_t audio_last_voices;
    uint32_t audio_last_unsupported;
    /* IAudioDevice (session created by audren:u command 2/4). The device
     * model reports the console's output device names for the requested
     * revision, keeps the AudioTvOutput volume, and owns one event. The
     * platform has no audio sink wired yet, so the active channel count is
     * the stereo layout the names describe. */
    uint32_t audio_device_revision;
    uint64_t audio_device_aruid;
    uint32_t audio_device_volume_bits; /* f32 bit pattern, default 1.0f */
    uint32_t audio_device_event;
    uint32_t audio_device_event_created;
    /* bcat:u progress: the null delivery backend finishes every sync
     * immediately, so the one progress event is signalled at request time
     * and the reported DeliveryCacheProgressImpl is the Done state. */
    uint32_t bcat_progress_event;
    /* ILibraryAppletAccessor state, indexed by the appletOE domain object id.
     * CreateLibraryApplet makes the applet object; no library-applet process
     * runs on this platform, so its state-changed event never fires and
     * IsCompleted stays false. Launching is refused explicitly. */
    uint32_t lapplet_event[VP_GUEST_DOMAIN_OBJECT_LIMIT];
    uint32_t lapplet_id[VP_GUEST_DOMAIN_OBJECT_LIMIT];
    uint32_t lapplet_mode[VP_GUEST_DOMAIN_OBJECT_LIMIT];
    /* ISelfController GetLibraryAppletLaunchableEvent returns one event per
     * applet service instance: the reference signals it on the query and again
     * when ILibraryAppletCreator creates an applet, and the SDK's
     * StartLibraryApplet waits on it before issuing the accessor Start. One
     * event is created on first use and reused for every later query. */
    uint32_t lapplet_launch_event;
    /* ILibraryAppletAccessor lifecycle and in/out data events. Start records
     * that the SDK asked the applet to run; without an applet process the
     * state-changed event is never signalled, IsCompleted stays false and the
     * pop events never fire. PushInData retains the passed IStorage as the
     * applet's pending input instead of consuming it. */
    uint32_t lapplet_started[VP_GUEST_DOMAIN_OBJECT_LIMIT];
    uint32_t lapplet_in_data[VP_GUEST_DOMAIN_OBJECT_LIMIT];
    uint32_t lapplet_pop_event[VP_GUEST_DOMAIN_OBJECT_LIMIT];
    uint32_t lapplet_interactive_event[VP_GUEST_DOMAIN_OBJECT_LIMIT];
    uint32_t lapplet_complete[VP_GUEST_DOMAIN_OBJECT_LIMIT];
    /* Inline software keyboard (library applet 0x11) model. The applet is not
     * a guest process here: the loader parses the Calc requests the title
     * pushes on the accessor's interactive channel and queues the replies the
     * swkbd protocol defines, which PopInteractiveOutData then returns as an
     * IStorage. One keyboard is active per applet session, which is the only
     * shape the locked title uses. `state` is the SwkbdState reported in every
     * reply (0 not initialized, 1 hidden, 2 appearing, 3 shown, 4
     * disappearing); `calc_size` selects the 0x4a0 or 0x4e8 request layout. */
    struct {
        uint32_t object;      /* accessor object id, 0 = no keyboard */
        uint32_t state;
        uint32_t calc_size;
        uint32_t text_len;    /* bytes of UTF-16LE text, <= 0x3ec */
        uint32_t cursor;
        uint32_t changed_v2;  /* SetChangedStringV2Flag was requested */
        uint32_t moved_v2;    /* SetMovedCursorV2Flag was requested */
          uint32_t utf8_mode;   /* Calc request selected UTF-8 replies */
          uint32_t max_chars, min_chars;
        uint32_t selection;   /* selected key in the rendered inline keyboard */
        uint32_t input_last;  /* previous pad buttons, for press edges */
        uint32_t decided;     /* DecidedEnter already sent for this show */
        uint32_t render_addr; /* guest image last rendered, 0 = never */
        uint32_t render_width, render_height;
        uint32_t render_dirty;
        uint8_t text[0x3ec];
        struct {
            uint8_t *data;
            uint32_t size;
        } replies[VP_SWKBD_REPLY_QUEUE];
        uint32_t reply_count;
    } swkbd;
    /* IStorage / IStorageAccessor. The bytes live in a host-side buffer owned
     * by this server session; the accessor copies them to and from the
     * client's HIPC buffer descriptors. storage_parent maps an accessor object
     * id to its IStorage object id. A storage retained by PushInData is not
     * released when the client closes its domain handle. */
    struct {
        uint8_t *data;
        uint32_t size;
        uint32_t pushed;
        uint32_t source_address; /* temporary: transfer memory a handle storage copied */
    } storages[VP_GUEST_DOMAIN_OBJECT_LIMIT];
    uint32_t storage_parent[VP_GUEST_DOMAIN_OBJECT_LIMIT];
    uint32_t terminate_result; /* IApplicationFunctions SetTerminateResult */
    uint32_t nvdrv_initialized;    /* nvdrv command 3 accepted */
    uint32_t nvdrv_process_handle;
    uint32_t nvdrv_transfer_handle;
    uint32_t nvdrv_transfer_size;  /* Size argument of nvdrv Initialize */
    uint64_t nvdrv_pid;            /* PID descriptor seen by SetAruid */
    uint64_t nvdrv_aruid;          /* AppletResourceUserId bound to the session */
    uint32_t nvdrv_firmware_margin; /* command 13 SetGraphicsFirmwareMemoryMarginEnabled */
    /* nn::nvdrv device table. Open allocates one descriptor per reviewed
     * device node and binds it to the path the guest requested; a name this
     * loader has no model for is refused instead of being registered. The
     * per-device ioctl models are added only as their semantics are
     * established, so an unmodeled ioctl on a registered fd fails loudly. */
    uint32_t nvdrv_devices;
    int32_t nvdrv_device_fd[VP_GUEST_NVDRV_DEVICE_LIMIT];
    char nvdrv_device_name[VP_GUEST_NVDRV_DEVICE_LIMIT][VP_GUEST_NVDRV_NAME_MAX];
    /* First-seen (device fd, ioctl word) pairs for this session. VP01.5.3
     * needs the live device/request-code set; each pair is recorded once so
     * repeated frame traffic does not flood the log. */
    uint32_t nvdrv_ioctl_trace[VP_GUEST_NVDRV_IOCTL_TRACE];
    uint32_t nvdrv_ioctl_trace_count;
    /* /dev/nvhost-ctrl event slots (ioctls 0x1f/0x20/0x21). The kernel owns a
     * KEvent per registered user event id and nvdrv QueryEvent hands its handle
     * to the guest. Registration is pure bookkeeping here; the handle the
     * guest waits on is signalled only when the matching channel fence
     * bookkeeping actually completes. */
    uint64_t nvctrl_events;
    /* Completed syncpoint counter per nvhost event slot's fence id. A
     * submission retires only into this model: the command stream is never
     * executed, so the counter records the fence value the guest declared for
     * the submission at the moment the submission is accepted. */
    uint32_t nvctrl_syncpt_value[VP_GUEST_NVCTRL_EVENT_LIMIT];
    /* Async event waits (ioctl 0x1d allocation form / 0x1e). The kernel parks
     * the thread until the fence is signalled; this model records the pending
     * wait instead of returning a fabricated completion. */
    struct {
        uint32_t armed;
        uint32_t fence_id;
        uint32_t fence_value;
        uint32_t event_id;
    } nvctrl_waits[VP_GUEST_NVCTRL_EVENT_LIMIT];
    VPGuestGpuAs gpu_as; /* nvhost-as-gpu address space model */
    /* One /dev/nvhost-gpu channel per open descriptor. The real kernel
     * creates a channel object per open and allocates it its own syncpoint;
     * the model mirrors that by keying state on the device fd and handing
     * out a distinct syncpoint id per channel. */
    VPGuestGpuChannel gpu_channels[VP_GUEST_NVDRV_DEVICE_LIMIT];
    uint32_t gpu_channel_count;
    uint32_t gpu_next_syncpoint;
    /* Live nvmap handles, compacted on Free; ids are monotonic and never
     * reused. nvmap_next_id is the last issued id (0 before the first). */
    uint32_t nvmap_handle_count;
    uint32_t nvmap_next_id;
    VPGuestNvMapHandle nvmap_handles[VP_GUEST_NVMAP_HANDLE_LIMIT];
    /* NVGPU_IOCTL_ZBC_SET_TABLE on nvhost-ctrl-gpu. This loader programs no
     * GPU; the request is recorded so a future device model can apply the
     * Z-buffer compression table instead of the service pretending it did. */
    uint32_t zbc_set_count;
    struct {
        uint32_t color_ds[4];
        uint32_t color_l2[4];
        uint32_t depth, format, type;
    } zbc_table;
    uint32_t display_service;      /* vi:m GetDisplayService type; 0 = not a display session */
    uint32_t vi_role;              /* VP_GUEST_VI_* role of a returned display object */
    /* IHOSBinderDriver object state. The NativeWindow Parcel returned by
     * OpenLayer names binder handle 1 under the "dispdrv" service; the relay
     * session tracks the guest's strong/weak reference adjustments for it. */
    uint32_t binder_handle;
    int32_t binder_strong_refs;
    int32_t binder_weak_refs;
    /* IGraphicBufferProducer connect (IHOSBinderDriver transaction 10)
     * accepted for the binder handle above. The guest sends a null token,
     * NATIVE_WINDOW_API_EGL and producerControlledByApp; the reply carries
     * the modeled display's QueueBufferOutput. */
    uint32_t binder_connected;
    uint32_t binder_api;
    /* IGraphicBufferProducer SetPreallocatedBuffer (transaction 14): the
     * guest hands the composer one nvmap-backed buffer per slot. This
     * platform has no compositor and does not display those pixels, but the
     * slot table is recorded so Query answers the producer's real geometry
     * and a future display path knows exactly which allocation each slot
     * names. */
    uint32_t binder_slot_count;
    struct {
        int32_t slot;
        uint32_t width, height, stride, format, usage, buffer_id, handle, offset;
        /* 0 = preallocated/ready, 1 = dequeued by the guest, 2 = queued to
         * the modeled composer. A queued buffer is not scanned out and no
         * pixels are kept; the consumer model presents it (releases the slot)
         * only when the producer's next blocking DequeueBuffer needs a free
         * slot, which keeps the ring live without inventing vsync timing. */
        uint32_t state;
        uint32_t queue_seq; /* submission order for the release model */
        uint32_t fence_id, fence_value;
        uint32_t has_buffer;
        /* GPU VA of this slot's pixels, resolved from (handle, offset) through
         * the nvmap allocation and the nvhost-as-gpu mapping table. The same
         * nvmap allocation can be mapped at several GPU VAs; every candidate
         * is kept so the VA the guest actually renders through is the one the
         * bridge presents. Filled the first time the slot is needed; 0 means
         * the mapping was not established and the bridge must report a miss. */
        uint64_t gpu_va;
        uint64_t gpu_va_candidates[VP_GUEST_GPU_AS_MAP_LIMIT];
        uint32_t gpu_va_candidate_count;
        uint32_t gpu_va_resolved;
        uint32_t gpu_va_valid;
        uint32_t buffer[0x16cu / 4u]; /* guest NvGraphicBuffer image */
    } binder_slots[VP_GUEST_BINDER_SLOT_LIMIT];
    uint32_t binder_width, binder_height, binder_format; /* producer defaults */
    uint32_t binder_dequeues, binder_queues; /* modeled ring progress */
    uint32_t binder_queue_seq;   /* last QueueBuffer submission number */
    uint32_t binder_presents;    /* queued slots released by the consumer model */
    /* IHidServer CreateAppletResource created this session as the
     * IAppletResource for `hid_aruid`. Its command 0 GetSharedMemoryHandle
     * lazily creates the hid shared memory object (0x40000 bytes, zeroed)
     * and returns the same handle on every subsequent call. */
    uint64_t hid_aruid;
    uint32_t hid_shared_handle;
    uint32_t hid_shared_addr;
    /* IHidServer ActivateDebugPad/ActivateTouchScreen/ActivateMouse/
     * ActivateKeyboard. The device objects exist and are marked active, but
     * no input source is wired to the platform yet, so the shared memory
     * stays zero: an active device that has never produced state. */
    uint32_t debug_pad_activated;
    uint32_t touch_screen_activated;
    uint32_t mouse_activated;
    uint32_t keyboard_activated;
    /* IHidServer ActivateNpad/DeactivateNpad. This platform has no Switch
     * controllers, so activation only records which aruid became active; the
     * hid shared memory stays zero, which is the exact state of a console
     * with no input devices attached. Physical input is a later device
     * model, not a fabricated npad report. */
    uint64_t npad_aruid;
    uint32_t npad_activated;
    /* IHidServer SetSupportedNpadStyleSet. The client declares which styles
     * it can use; with no controllers attached the shared memory reports
     * none of them until a device model exists. */
    uint64_t npad_style_aruid;
    uint32_t npad_style_set;
    uint32_t npad_style_set_valid;
    /* IHidServer SetSupportedNpadIdType. The id list travels as an InArray in
     * the HIPC pointer buffer (X descriptor); it selects which npad ids the
     * client expects to report once controllers exist. With none attached the
     * list is recorded and the hid shared memory stays zero. */
    uint64_t npad_id_aruid;
    uint32_t npad_id_count;
    uint32_t npad_ids[VP_GUEST_NPAD_ID_LIMIT];
    /* IHidServer SetNpadJoyHoldType: 0 vertical, 1 horizontal. It changes how
     * a detached Joy-Con pair is reported; with no controllers attached the
     * value is recorded and nothing in the shared memory changes. */
    uint64_t npad_joy_hold_aruid;
    uint32_t npad_joy_hold_type;
    uint32_t npad_joy_hold_valid;
    /* StartSixAxisSensor/StopSixAxisSensor records, one per handle. */
    uint32_t sixaxis_count;
    VPGuestSixAxisSensor sixaxis[VP_GUEST_SIXAXIS_LIMIT];
    /* IActiveVibrationDeviceList command 0 records. */
    uint32_t vibration_count;
    VPGuestVibrationDevice vibration[VP_GUEST_VIBRATION_LIMIT];
    /* Npad style-set update events, one per {aruid, npad id}. */
    uint32_t style_event_count;
    VPGuestStyleEvent style_events[VP_GUEST_STYLE_EVENT_LIMIT];
    /* vi application display layer opened through OpenLayer and the scaling
     * mode the guest requested for it (SetLayerScalingMode). Kept for
     * evidence; this platform has no compositor to apply it. */
    uint32_t layer_opened;
    uint64_t layer_id;
    uint32_t layer_scaling_mode;
    uint32_t exit_lock_depth;  /* ISelfController LockExit nesting */
    /* Applet lifecycle: the platform requested termination (the Exit message
     * is queued) and the title released its exit lock, so the reviewed
     * ISelfController::UnlockExit model terminates the process. */
    uint32_t exit_requested;
    uint32_t process_exit_requested;
    uint32_t operation_mode_notification; /* ISelfController command 11 flag */
    uint32_t performance_mode_notification; /* ISelfController command 12 flag */
    uint32_t restart_message_enabled; /* ISelfController command 14 flag */
    uint32_t screenshot_permission; /* ISelfController command 10 value */
    /* apm ISession SetPerformanceConfiguration, indexed by PerformanceMode
     * (0 Normal, 1 Boost). The loader records what the application requested
     * but controls no clocks; GetPerformanceConfiguration answers from these
     * fields, defaulting to Config7 (0x00020003) like the reference controller,
     * whose clock hook is a no-op as well. */
    uint32_t apm_config_normal;
    uint32_t apm_config_boost;
    uint32_t refs;
} VPGuestSession;

/* Kernel handles are opaque to the guest. Only handles this service layer
 * created are accepted; the pseudo handles stay separate. `session` is a
 * one-based index into VPGuestSvc.sessions for kind 2, zero otherwise.
 * Kind 4 threads are created by the native SDK loader (svc 0x08); their
 * scheduling attributes live here so the generic kernel services can answer
 * queries for any guest thread, not only the calling one. */
#define VP_GUEST_HANDLE_KIND_THREAD 4u
/* Kind 5 transfer memory: pages of the calling process that svc 0x15 handed
 * to a transfer-memory object. The source range stays owned by the process;
 * the handle records what a later MapTransferMemory would have to expose. */
#define VP_GUEST_HANDLE_KIND_TRANSFER 5u
/* Kind 6 shared memory: the server-created object the client maps with svc
 * 0x13. The only object this platform creates is the hid applet-resource
 * block, which Horizon initializes to zero and maps read-only into the
 * client. There is no host backing buffer: the mapping is modeled as a copy
 * of the object's (zeroed) pages into the range the client chose. */
#define VP_GUEST_HANDLE_KIND_SHARED 6u
/* sizeof(nn::hid::detail::SharedMemoryFormat), the hid applet-resource
 * shared memory the server creates and the client maps read-only. The
 * kernel zero-fills the object at creation; the client passes this exact
 * size to svcMapSharedMemory (the kernel requires size == object size). */
#define VP_GUEST_HID_SHARED_BYTES 0x40000u
/* Pointer-buffer capacity declared to the 32-bit SDK by
 * QueryPointerBufferSize. The writer uses this region for HipcPointer
 * arguments (InArray/OutArray) and, when a MapAlias buffer's declared size
 * fits, moves that buffer's descriptor into the same region. The descriptor
 * encoding for the moved MapAlias case is decoded per service. The reference
 * service manager answers 0x8000 regardless of the specific server; the
 * loader keeps the same value because the SDK also checks nn::fs paths
 * declared as 0x301-byte MapAlias buffers before it will send them at all.
 * The declared size gates which buffers the client relocates; the loader
 * resolves A/B/X/C descriptors directly in the client's address space, so it
 * does not need to back a copy of that region. A client array that does not
 * fit fails its own HIPC size check visibly (result 0x11A0B) instead of being
 * truncated. */
#define VP_GUEST_POINTER_BUFFER_BYTES 0x8000u

typedef struct {
    uint32_t kind; /* 0 free, 1 named-port session, 2 service session, 3 event, 4 thread, 5 transfer memory, 6 shared memory */
    char name[9];
    uint32_t arg;
    uint32_t signaled; /* event state (kind 3); manual-clear, reset by SVC 0x17 */
    /* Host-monotonic deadline for a paced event signal (microseconds,
     * 0 = none). The modeled audio sink consumes one buffer per nominal
     * sample period, so RequestUpdate arms this instead of signaling
     * instantly; event queries and waits observe the signal once it passes. */
    uint64_t signal_at_us;
    uint32_t session;
    /* 1-based session index that created this audio event, 0 for non-audio
     * events. Cleared when that session is freed so a stale event can never
     * borrow a reused session's cadence state. */
    uint32_t audio_owner_session;
    uint32_t thread_priority;
    int32_t thread_core;       /* -2 = no affinity requested */
    uint64_t thread_affinity;  /* bits 0..3, kernel core mask */
    uint32_t transfer_address;
    uint32_t transfer_size;
    uint32_t transfer_permission;
    /* Kind 6 shared memory object: size fixed at creation, plus the single
     * client mapping MapSharedMemory created for it. */
    uint32_t shared_size;
    uint32_t shared_address;
    uint32_t shared_permission;
    uint32_t shared_mapped;
} VPGuestHandle;

typedef struct {
    VPGuestRegion regions[VP_GUEST_REGION_LIMIT];
    unsigned region_count;
    /* svcSetMemoryAttribute effective ranges, sorted and disjoint. */
    VPMemoryAttributes attributes;
    VPGuestHandle handles[VP_GUEST_HANDLE_LIMIT];
    VPGuestSession sessions[VP_GUEST_SESSION_LIMIT];
    uint32_t hid_shared_addr;
    /* The reference AppletResourceManager owns the vibration device list for
     * the whole service (keyed by aruid), not per client session. The list is
     * built through the IActiveVibrationDeviceList sub-session but consumed by
     * IHidServer::SendVibrationValues on the parent session, so it lives above
     * both. No haptic device exists on this platform; the list is the
     * validated set the server accepted. */
    uint32_t vibration_count;
    VPGuestVibrationDevice vibration[VP_GUEST_VIBRATION_LIMIT];
    /* nn::mmnv::IRequest ("mm:u"). The media player initializes the Nvdec
     * module of the multimedia clock manager before it submits decode work:
     * Initialize(Module, Priority, EventClearMode) hands out a RequestId,
     * SetAndWait(RequestId, Min, Max) asks for a clock setting and waits for
     * it, Get returns the current setting. This platform has no media clock,
     * so the service records the request and reports the requested minimum as
     * current, the same explicit no-clock model as the apm configuration
     * fields below. No frequency, power or decode behavior is claimed. */
    uint32_t media_clock_next;
    VPGuestMediaClock media_clock[VP_GUEST_MEDIA_CLOCK_LIMIT];
    /* Module image bases, recorded by the loader probes. The hid writer uses
     * module 3's base to read the title's npad pointer table for diagnostics;
     * no game address is guessed. Zero means not recorded. */
    uint32_t module_bases[4];
    uint32_t tls_buffer; /* modeled TPIDRURO: the per-thread IPC command buffer */
    /* Original-SDK computation hooks. Some service responses are pure
     * functions of their request (the audio work-buffer size is defined by
     * the locked SDK's own server implementation). The service layer asks the
     * loader to execute that original function on a spare executor instead
     * of reimplementing its constants. guest_call returns 0 and the guest r0
     * on success. */
    int (*guest_call)(void *user, uint32_t module, uint32_t address,
                      uint32_t arg, uint32_t *result);
    void *guest_call_user;
    /* A synchronous device wait can yield the service-state lock without
     * changing caller-owned guest resource lifetime. The token restores the
     * calling executor's ambient handle/TLS after other services have run. */
    void (*device_wait_begin)(void *user, uint32_t token[2]);
    void (*device_wait_end)(void *user, const uint32_t token[2]);
    void *device_wait_user;
    /* Multi-argument form used to run the original SDK's thread API on a
     * spare executor (M5 audio probe). */
    int (*guest_call_args)(void *user, uint32_t module, uint32_t address,
                           const uint32_t *args, uint32_t count,
                           uint32_t *result);
    uint32_t audio_work_size_fn;
    /* M5 audio probe: the loader owns the marker check, the guest buffer and
     * the SDK thread; the service only asks for it once, when the title's
     * first OpenAudioRenderer proves the audio library is initialized. */
    void (*audio_probe_request)(void *user);
    void *audio_probe_request_user;
    uint32_t audio_probe_requested;
    uint32_t audio_probe_args;          /* guest address of VPAudioProbeArgs */
    uint32_t audio_probe_pending_field; /* args->sink_pending_frames */
    uint32_t audio_probe_done_field;    /* args->done */
    void *audio_sink_owner;             /* VPGuestSession* owning SceAudioOut */
    /* M6-B7: actual renderer RequestUpdate thread and its 1-based renderer
     * session. Set on a real renderer update, cleared on Stop and on session
     * close. Used only to bound successful ResetSignal of audio-owned events
     * and sm:GetService("audren:u") on the same thread. */
    uint32_t audio_update_thread_handle;
    uint32_t audio_update_session;
    uint32_t audio_log_unowned_reset;
    uint32_t audio_log_unowned_sm_getservice;
    /* sm: is a named port (kind 1), not a VPGuestSession; its GetService
     * cadence is a single service-level slot. */
    VPAudioLogCadence audio_sm_getservice_log;
    uint32_t audio_sm_getservice_count;
    uint32_t process_id;
    uint32_t current_thread_handle;
    /* M5 diagnosis: handle of the SDK thread the loader creates for the audio
     * probe (set by the loader around its nn::os::CreateThread call). While
     * it is set, every SVC dispatched on that thread is traced so a fault
     * after a service reply can be tied to the last client operation. */
    uint32_t audio_probe_expect_thread;
    uint32_t audio_probe_thread_handle;
    /* Number of probe-thread SVCs still traced through a close-per-line file.
     * The emulated file layer loses its tail on abort, and the first calls
     * after OpenAudioRenderer are exactly where a fault must be localized. */
    uint32_t audio_probe_trace_budget;
    uint32_t total_memory, used_memory;
    uint32_t heap_base, heap_size;
    uint32_t alias_base, alias_size;
    uint32_t stack_base, stack_size;
    uint32_t aslr_base, aslr_size;
    uint32_t plr_base, plr_size;
    uint32_t free_threads;
    uint32_t thread_priority, thread_core, thread_affinity;
    uint32_t key_signals;
    /* Applet-side managed display layer (ISelfController command 40). The id
     * is the platform's layer registry key that vi OpenLayer later opens; the
     * host has no compositor, so the id is allocated but no pixels exist. */
    uint32_t managed_layers;
    uint64_t managed_layer_id;
    uint64_t tick_origin_us;
    VPGuestStorage storage; /* lazily opened RomFS index/map/tree backing */
    unsigned calls, unsupported, ipc_requests, ipc_unsupported, timer_reads;
    /* Guest-initiated svcBreak calls. They are handled (state dump + stop),
     * so they are reported apart from unimplemented SVC dispatch. */
    unsigned breaks;
    unsigned map_copies; /* modeled svcMapMemory calls (copy, not page alias) */
    /* Destination ranges of live svcMapMemory copies. The SDK address-space
     * allocator picks free ranges from QueryMemory, so these ranges must be
     * reported as Shared until the matching svcUnmapMemory. Without this,
     * two alias stacks were handed the same VA and one overwrote the other's
     * thread entry. */
    uint32_t map_copy_count;
    uint32_t map_copy_untracked;
    struct {
        uint32_t dst;
        uint32_t src;
        uint32_t size;
        uint32_t owner; /* thread index+1 when this copy is an alias stack */
    } map_copy_ranges[VP_GUEST_MAP_COPY_LIMIT];
    unsigned shared_maps; /* modeled svcMapSharedMemory calls (zeroed object copy) */
    unsigned wait_trace_118; /* temporary: one stack dump for the error-event poll */
    /* Serializes save file system operations. Guest threads run natively and
     * can issue fsp-srv calls concurrently; the save backend's object tables
     * and Vita file handles are not re-entrant. */
    volatile int save_lock;
    FILE *log;
    /* Duplicate service trace closed and reopened per event: the emulator's
     * file layer buffers ~4 KiB, so a guest fault would otherwise lose the
     * last events. Diagnostic only; the primary log keeps the full order. */
    FILE *trace;
} VPGuestSvc;

void vp_guest_svc_init(VPGuestSvc *, FILE *);
void vp_guest_svc_set_module_base(VPGuestSvc *, unsigned index, uint32_t base);
void vp_guest_svc_tls(VPGuestSvc *, uint32_t tls_buffer);
void vp_guest_svc_log(VPGuestSvc *, const char *format, ...);
/* Resolve a GPU virtual address through the MapBufferEx table recorded for
 * this session. Returns 1 and stores the guest CPU address on success; 0 means
 * no recorded mapping backs the range. */
int vp_guest_svc_gpu_va_to_cpu(const VPGuestSession *, uint64_t va,
                               uint64_t size, uint32_t *cpu);
int vp_guest_svc_region(VPGuestSvc *, uint32_t, uint32_t, uint32_t, uint32_t, const char *);
int vp_guest_svc_writable(const VPGuestSvc *, uint32_t, uint32_t);
int vp_guest_svc_readable(const VPGuestSvc *, uint32_t, uint32_t);
/* Event-handle state probed by the SDK blocking-wait path, which must poll
 * while the session state lock is dropped. event_state returns 1 and stores
 * the signal state when the handle is a loader event; consume clears a
 * signaled event and returns 1. */
int vp_guest_svc_event_state(VPGuestSvc *, uint32_t handle, int *signaled);
int vp_guest_svc_event_consume(VPGuestSvc *, uint32_t handle);
/* Queue an AppletMessage for the title's own message loop and signal the
 * applet notification event. Returns 0 when the message was queued. An Exit
 * message (0x4) raises the platform exit request that ISelfController Exit,
 * LockExit and UnlockExit act on. */
int vp_guest_svc_queue_applet_message(VPGuestSvc *, uint32_t message, const char *why);
/* 1 when the title released its exit lock after an applet exit request and
 * the session must end as a process termination, not as a loader stop. */
int vp_guest_svc_process_exit_requested(VPGuestSvc *);
/* Returns 0 when the original veneer may resume, or VP_BRIDGE_UNSUPPORTED. */
int vp_guest_svc_dispatch(VPGuestSvc *, VPBridgeFrame *);
/* Original rTexture lifecycle hooks also identify the private R8 font pack. */
int vp_guest_pixel_font_enabled(void);
int vp_guest_pixel_font_event(VPGuestSvc *, unsigned event, uint32_t object);
#endif

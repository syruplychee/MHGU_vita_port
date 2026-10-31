/* Bounded, lifetime-owned append sink for runtime diagnostics.
 *
 * The sink exists because diagnostic lines were previously written with one
 * fopen/fwrite/fclose per line. That churns host file descriptors for the
 * whole process lifetime and makes every diagnostic line a new io lifetime.
 * This helper opens the append target once, serializes optional callers, and
 * keeps explicit counters for every condition that would otherwise lose a
 * line silently.
 *
 * Lifetime contract:
 *   - vp_log_sink_init configures and opens the sink. A failed open returns -1
 *     and is recorded in stats.open_failures. The sink never retries on its
 *     own; call vp_log_sink_init again for an explicit retry.
 *   - vp_log_sink_printf/vprintf append one complete line per call. They never
 *     log an error through the sink (or anywhere else).
 *   - vp_log_sink_close is idempotent and preserves stats.
 *   - ops->lock/unlock are optional. When present, the owner supplies them
 *     when the sink is shared between threads; they must protect the whole
 *     write+flush operation. Init/close remain owner-only operations (call
 *     them before sharing or after joining writers).
 *   - ops callbacks must not call back into the same sink.
 */
#ifndef VP_LOG_SINK_H
#define VP_LOG_SINK_H

#include <stdarg.h>
#include <stddef.h>

#define VP_LOG_SINK_LINE_BYTES 1024u
#define VP_LOG_SINK_PATH_BYTES 192u

enum {
    VP_LOG_SINK_OK = 0,
    VP_LOG_SINK_INVALID = -1,     /* null sink/format or invalid configuration */
    VP_LOG_SINK_NOT_READY = -2,   /* not initialized, closed, failed, or busy */
    VP_LOG_SINK_WRITE_FAILED = -3,/* negative or short write; sink is now failed */
    VP_LOG_SINK_FLUSH_FAILED = -4,/* flush failed; sink is now failed */
};

typedef struct {
    void *user;
    /* Returns an opaque handle, or NULL on failure. */
    void *(*open)(void *user, const char *path);
    /* Returns the number of bytes written (0..length) or a negative error.
     * A return value other than `length` is a failed line. */
    int (*write)(void *user, void *handle, const char *data, size_t length);
    /* Optional. Returns 0 on success. Called after every complete line so a
     * later process abort cannot discard buffered diagnostic text. */
    int (*flush)(void *user, void *handle);
    int (*close)(void *user, void *handle);
    /* Optional serialization pair. Provide both or neither. */
    void (*lock)(void *user);
    void (*unlock)(void *user);
} VPLogSinkOps;

typedef struct {
    unsigned open_attempts;
    unsigned open_failures;
    unsigned lines_attempted;
    unsigned lines_written;
    unsigned lines_failed;
    unsigned short_writes;
    unsigned flush_failures;
    unsigned truncated_lines;
    unsigned suppressed_lines;
    unsigned close_calls;
    unsigned close_failures;
    unsigned long long bytes_written;
    unsigned long long bytes_dropped;
} VPLogSinkStats;

typedef struct {
    const VPLogSinkOps *ops;
    void *handle;
    char path[VP_LOG_SINK_PATH_BYTES];
    unsigned initialized;
    unsigned open;
    unsigned failed;
    unsigned in_write;
    VPLogSinkStats stats;
} VPLogSink;

/* Zero-initialize the structure before the first use. Returns 0 when the
 * append handle is open. Calling init again while open is a no-op (no second
 * handle). After a failed open, init retries explicitly and preserves stats. */
int vp_log_sink_init(VPLogSink *sink, const VPLogSinkOps *ops, const char *path);

/* Append one formatted line. Returns the bytes appended (excluding the NUL),
 * 0 for an empty formatted line, or a negative VP_LOG_SINK_* value. A line
 * longer than VP_LOG_SINK_LINE_BYTES is written truncated to
 * VP_LOG_SINK_LINE_BYTES-1 bytes and counted in stats.truncated_lines;
 * a short/error write fails closed for the rest of this sink lifetime. */
int vp_log_sink_vprintf(VPLogSink *sink, const char *format, va_list args);
int vp_log_sink_printf(VPLogSink *sink, const char *format, ...);

/* Close the current lifetime. Idempotent; stats are preserved across close.
 * The helper flushes after every successfully written line, so close does not
 * call ops->flush: the close callback owns the final buffered flush (the
 * stdio adapter relies on fclose). A closed sink rejects writes until a new
 * vp_log_sink_init starts a fresh lifetime (which resets stats).
 * init/close are owner-only operations and must not race writers. */
int vp_log_sink_close(VPLogSink *sink);

/* Snapshot counters. Returns 0 on success, -1 for a null argument. */
int vp_log_sink_get_stats(VPLogSink *sink, VPLogSinkStats *out);

/* Production adapter: fopen(path, "ab"), fwrite, fflush, fclose. */
extern const VPLogSinkOps vp_log_sink_stdio_ops;

#endif

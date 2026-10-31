#include "log_sink.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static void sink_lock(VPLogSink *sink) {
    if (sink->ops != NULL && sink->ops->lock != NULL)
        sink->ops->lock(sink->ops->user);
}

static void sink_unlock(VPLogSink *sink) {
    if (sink->ops != NULL && sink->ops->unlock != NULL)
        sink->ops->unlock(sink->ops->user);
}

int vp_log_sink_init(VPLogSink *sink, const VPLogSinkOps *ops, const char *path) {
    size_t path_length;
    void *handle;

    if (sink == NULL || ops == NULL || path == NULL
        || ops->open == NULL || ops->write == NULL || ops->close == NULL
        || ((ops->lock == NULL) != (ops->unlock == NULL)))
        return VP_LOG_SINK_INVALID;
    path_length = strlen(path);
    if (path_length + 1u > sizeof(sink->path)) return VP_LOG_SINK_INVALID;

    if (!sink->initialized) {
        memset(sink, 0, sizeof(*sink));
    } else if (sink->open) {
        /* Deterministic init: one sink owns at most one open handle. */
        return VP_LOG_SINK_OK;
    } else if (sink->ops != ops || strcmp(sink->path, path) != 0) {
        /* An explicit retry must target the same sink configuration; mixing
         * configurations here would hide an ownership error. */
        return VP_LOG_SINK_INVALID;
    }

    sink->ops = ops;
    memcpy(sink->path, path, path_length + 1u);
    sink->initialized = 1u;
    sink->failed = 0u;
    sink->in_write = 0u;
    ++sink->stats.open_attempts;
    handle = ops->open(ops->user, sink->path);
    if (handle == NULL) {
        ++sink->stats.open_failures;
        sink->handle = NULL;
        sink->open = 0u;
        sink->failed = 1u;
        return VP_LOG_SINK_NOT_READY;
    }
    sink->handle = handle;
    sink->open = 1u;
    return VP_LOG_SINK_OK;
}

static int sink_append(VPLogSink *sink, const char *data, size_t length) {
    int written;

    if (!sink->initialized || !sink->open || sink->failed || sink->in_write) {
        ++sink->stats.suppressed_lines;
        return VP_LOG_SINK_NOT_READY;
    }
    ++sink->stats.lines_attempted;
    sink->in_write = 1u;
    written = sink->ops->write(sink->ops->user, sink->handle, data, length);
    if (written < 0 || (size_t)written != length) {
        ++sink->stats.lines_failed;
        if (written > 0 && (size_t)written < length) {
            ++sink->stats.short_writes;
            sink->stats.bytes_dropped += (unsigned long long)(length - (size_t)written);
        } else {
            sink->stats.bytes_dropped += (unsigned long long)length;
        }
        /* Fail closed: a partially written file cannot be repaired by
         * appending the next line, and implicit retries would multiply an
         * unbounded io failure into a log storm. The counters keep the loss
         * observable without logging through this sink. */
        sink->failed = 1u;
        sink->in_write = 0u;
        return VP_LOG_SINK_WRITE_FAILED;
    }
    sink->stats.bytes_written += (unsigned long long)length;
    if (sink->ops->flush != NULL) {
        if (sink->ops->flush(sink->ops->user, sink->handle) != 0) {
            ++sink->stats.lines_failed;
            ++sink->stats.flush_failures;
            sink->failed = 1u;
            sink->in_write = 0u;
            return VP_LOG_SINK_FLUSH_FAILED;
        }
    }
    ++sink->stats.lines_written;
    sink->in_write = 0u;
    return (int)length;
}

int vp_log_sink_vprintf(VPLogSink *sink, const char *format, va_list args) {
    char line[VP_LOG_SINK_LINE_BYTES];
    int required;
    size_t length;
    int result;

    if (sink == NULL || format == NULL) return VP_LOG_SINK_INVALID;
    sink_lock(sink);
    if (!sink->initialized || !sink->open || sink->failed || sink->in_write) {
        if (sink->initialized)
            ++sink->stats.suppressed_lines;
        sink_unlock(sink);
        return sink->initialized ? VP_LOG_SINK_NOT_READY : VP_LOG_SINK_INVALID;
    }
    required = vsnprintf(line, sizeof(line), format, args);
    if (required <= 0) {
        sink_unlock(sink);
        return 0;
    }
    length = (size_t)required;
    if (length >= sizeof(line)) {
        length = sizeof(line) - 1u;
        ++sink->stats.truncated_lines;
    }
    if (length == 0u) {
        sink_unlock(sink);
        return 0;
    }
    result = sink_append(sink, line, length);
    sink_unlock(sink);
    return result;
}

int vp_log_sink_printf(VPLogSink *sink, const char *format, ...) {
    va_list args;
    int result;

    va_start(args, format);
    result = vp_log_sink_vprintf(sink, format, args);
    va_end(args);
    return result;
}

int vp_log_sink_close(VPLogSink *sink) {
    int result = VP_LOG_SINK_OK;

    if (sink == NULL) return VP_LOG_SINK_INVALID;
    if (!sink->initialized) return VP_LOG_SINK_OK;
    /* Every successful line was already flushed by sink_append. The close
     * callback is responsible for any final buffering flush (fclose does it
     * for the stdio adapter); no extra ops->flush call is made here. */
    if (sink->open) {
        ++sink->stats.close_calls;
        if (sink->ops->close(sink->ops->user, sink->handle) != 0) {
            ++sink->stats.close_failures;
            result = VP_LOG_SINK_WRITE_FAILED;
        }
    }
    sink->handle = NULL;
    sink->ops = NULL;
    sink->open = 0u;
    sink->failed = 0u;
    sink->in_write = 0u;
    sink->initialized = 0u;
    return result;
}

int vp_log_sink_get_stats(VPLogSink *sink, VPLogSinkStats *out) {
    if (sink == NULL || out == NULL) return -1;
    sink_lock(sink);
    *out = sink->stats;
    sink_unlock(sink);
    return 0;
}

static void *stdio_open(void *user, const char *path) {
    (void)user;
    return (void *)fopen(path, "ab");
}

static int stdio_write(void *user, void *handle, const char *data, size_t length) {
    FILE *file = (FILE *)handle;
    size_t written;
    (void)user;

    if (file == NULL || length > (size_t)INT_MAX) return -1;
    written = fwrite(data, 1, length, file);
    return (int)written;
}

static int stdio_flush(void *user, void *handle) {
    (void)user;
    return fflush((FILE *)handle) == 0 ? 0 : -1;
}

static int stdio_close(void *user, void *handle) {
    (void)user;
    return fclose((FILE *)handle) == 0 ? 0 : -1;
}

const VPLogSinkOps vp_log_sink_stdio_ops = {
    NULL,
    stdio_open,
    stdio_write,
    stdio_flush,
    stdio_close,
    NULL,
    NULL,
};

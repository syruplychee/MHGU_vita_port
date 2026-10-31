/* Host regression for the bounded diagnostic append sink.
 *
 * Build (WSL):
 *   gcc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *       -Isrc/platform tests/log_sink.c src/platform/log_sink.c -pthread \
 *       -o artifacts/m6b1-log-lifetime-20261026/log-sink-test
 */
#include "log_sink.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned open_calls;
    unsigned close_calls;
    unsigned write_calls;
    unsigned flush_calls;
    unsigned lines;
    unsigned bad_lines;
    unsigned long long bytes;
    int fail_open;
    int partial_at_write;   /* 1-based write call number, 0 = disabled */
    size_t partial_bytes;
    int error_at_write;     /* 1-based write call number, 0 = disabled */
    int flush_error_at;     /* 1-based flush call number, 0 = disabled */
    char last[VP_LOG_SINK_LINE_BYTES + 8u];
    size_t last_length;
    pthread_mutex_t mutex;
} Fake;

static void *fake_open(void *user, const char *path) {
    Fake *fake = (Fake *)user;
    (void)path;
    ++fake->open_calls;
    if (fake->fail_open) return NULL;
    return fake;
}

static int fake_write(void *user, void *handle, const char *data, size_t length) {
    Fake *fake = (Fake *)user;
    (void)handle;
    ++fake->write_calls;
    if (fake->error_at_write && fake->write_calls == (unsigned)fake->error_at_write)
        return -7;
    if (fake->partial_at_write && fake->write_calls == (unsigned)fake->partial_at_write) {
        size_t partial = fake->partial_bytes < length ? fake->partial_bytes : length;
        return (int)partial;
    }
    if (length == 0u || data[length - 1u] != '\n') ++fake->bad_lines;
    if (data[0] != 'T' && data[0] != 'L') ++fake->bad_lines;
    fake->bytes += (unsigned long long)length;
    ++fake->lines;
    fake->last_length = length < sizeof(fake->last) - 1u ? length : sizeof(fake->last) - 1u;
    memcpy(fake->last, data, fake->last_length);
    fake->last[fake->last_length] = 0;
    return (int)length;
}

static int fake_flush(void *user, void *handle) {
    Fake *fake = (Fake *)user;
    (void)handle;
    ++fake->flush_calls;
    if (fake->flush_error_at && fake->flush_calls == (unsigned)fake->flush_error_at)
        return -1;
    return 0;
}

static int fake_close(void *user, void *handle) {
    Fake *fake = (Fake *)user;
    (void)handle;
    ++fake->close_calls;
    return 0;
}

static VPLogSinkOps no_flush_ops = {
    NULL, fake_open, fake_write, NULL, fake_close, NULL, NULL,
};

static void fake_lock(void *user) {
    Fake *fake = (Fake *)user;
    pthread_mutex_lock(&fake->mutex);
}

static void fake_unlock(void *user) {
    Fake *fake = (Fake *)user;
    pthread_mutex_unlock(&fake->mutex);
}

static VPLogSinkOps single_ops = {
    NULL, fake_open, fake_write, fake_flush, fake_close, NULL, NULL,
};

static VPLogSinkOps locked_ops = {
    NULL, fake_open, fake_write, fake_flush, fake_close, fake_lock, fake_unlock,
};

static unsigned failures;

static void check(int condition, const char *name) {
    if (condition) return;
    fprintf(stderr, "FAIL %s\n", name);
    ++failures;
}

static void fake_init(Fake *fake, int shared) {
    memset(fake, 0, sizeof(*fake));
    if (shared) {
        pthread_mutex_init(&fake->mutex, NULL);
        locked_ops.user = fake;
    } else {
        single_ops.user = fake;
    }
}

static const VPLogSinkOps *ops_for(int shared) {
    return shared ? &locked_ops : &single_ops;
}

static void test_many_writes_one_lifetime(void) {
    enum { LINES = 100001 };
    Fake fake;
    VPLogSink sink;
    VPLogSinkStats stats;
    unsigned i;

    fake_init(&fake, 0);
    memset(&sink, 0, sizeof(sink));
    check(vp_log_sink_init(&sink, ops_for(0), "fake.log") == 0, "many/init");
    check(vp_log_sink_init(&sink, ops_for(0), "fake.log") == 0, "many/init idempotent");
    check(fake.open_calls == 1u, "many/init does not duplicate open");
    for (i = 0; i < LINES; ++i) {
        int rc = vp_log_sink_printf(&sink, "T line %06u\n", i);
        if (rc <= 0) break;
    }
    check(i == LINES, "many/all writes attempted");
    check(fake.open_calls == 1u, "many/one open");
    check(fake.close_calls == 0u, "many/open until close");
    check(fake.write_calls == LINES, "many/one write per line");
    check(fake.flush_calls == LINES, "many/flush per line");
    check(fake.lines == LINES, "many/all lines complete");
    check(fake.bad_lines == 0u, "many/no malformed line");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "many/stats");
    check(stats.lines_written == LINES, "many/lines_written");
    check(stats.bytes_written == fake.bytes, "many/bytes");
    check(vp_log_sink_close(&sink) == 0, "many/close");
    check(fake.close_calls == 1u, "many/one close");
    check(fake.flush_calls == LINES, "many/close owns no extra flush");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "many/stats after close");
    check(stats.lines_written == LINES, "many/stats preserved after close");
    check(vp_log_sink_close(&sink) == 0, "many/close idempotent");
    check(fake.close_calls == 1u, "many/still one close");
    check(vp_log_sink_printf(&sink, "T after close\n") < 0, "many/write after close rejected");
    check(fake.write_calls == LINES, "many/no io after close");
}

static void test_reinit_after_close(void) {
    Fake fake;
    VPLogSink sink;
    VPLogSinkStats stats;

    fake_init(&fake, 0);
    memset(&sink, 0, sizeof(sink));
    check(vp_log_sink_init(&sink, ops_for(0), "fake.log") == 0, "reinit/first init");
    check(vp_log_sink_printf(&sink, "T first lifetime\n") > 0, "reinit/first write");
    check(vp_log_sink_printf(&sink, "T second line\n") > 0, "reinit/second write");
    check(vp_log_sink_close(&sink) == 0, "reinit/first close");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "reinit/stats preserved");
    check(stats.lines_written == 2u, "reinit/first lifetime lines");
    check(stats.close_calls == 1u, "reinit/first close counted");
    check(vp_log_sink_printf(&sink, "T rejected\n") < 0, "reinit/closed rejects");
    check(fake.write_calls == 2u, "reinit/no io while closed");

    check(vp_log_sink_init(&sink, ops_for(0), "fake.log") == 0, "reinit/new lifetime");
    check(fake.open_calls == 2u, "reinit/second open");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "reinit/new stats");
    check(stats.open_attempts == 1u, "reinit/new lifetime counters reset");
    check(stats.lines_written == 0u, "reinit/new lifetime starts empty");
    check(vp_log_sink_printf(&sink, "T third lifetime\n") > 0, "reinit/new write");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "reinit/new stats2");
    check(stats.lines_written == 1u, "reinit/new lifetime line counted");
    check(vp_log_sink_close(&sink) == 0, "reinit/second close");
    check(fake.close_calls == 2u, "reinit/two closes");
    check(fake.flush_calls == 3u, "reinit/one flush per written line");
}


static void test_short_and_error_writes(void) {
    Fake fake;
    VPLogSink sink;
    VPLogSinkStats stats;
    int rc;

    fake_init(&fake, 0);
    fake.partial_at_write = 3;
    fake.partial_bytes = 4;
    memset(&sink, 0, sizeof(sink));
    check(vp_log_sink_init(&sink, ops_for(0), "fake.log") == 0, "short/init");
    check(vp_log_sink_printf(&sink, "T first\n") > 0, "short/first");
    check(vp_log_sink_printf(&sink, "T second\n") > 0, "short/second");
    rc = vp_log_sink_printf(&sink, "T third\n");
    check(rc == VP_LOG_SINK_WRITE_FAILED, "short/partial fails line");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "short/stats");
    check(stats.short_writes == 1u, "short/count");
    check(stats.lines_failed == 1u, "short/failed line");
    check(stats.bytes_dropped == 8u - 4u, "short/dropped remainder");
    check(vp_log_sink_printf(&sink, "T fourth\n") < 0, "short/fail closed");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "short/stats2");
    check(stats.suppressed_lines == 1u, "short/suppressed visible");
    check(fake.write_calls == 3u, "short/no implicit retry");
    check(vp_log_sink_close(&sink) == 0, "short/close");
    check(fake.close_calls == 1u, "short/close once");

    fake_init(&fake, 0);
    fake.error_at_write = 2;
    memset(&sink, 0, sizeof(sink));
    check(vp_log_sink_init(&sink, ops_for(0), "fake.log") == 0, "error/init");
    check(vp_log_sink_printf(&sink, "T first\n") > 0, "error/first");
    check(vp_log_sink_printf(&sink, "T second\n") == VP_LOG_SINK_WRITE_FAILED,
          "error/negative fails line");
    check(vp_log_sink_printf(&sink, "T third\n") < 0, "error/fail closed");
    check(fake.write_calls == 2u, "error/no implicit retry");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "error/stats");
    check(stats.lines_failed == 1u, "error/failed line");
    check(stats.bytes_dropped == 9u, "error/whole line dropped");
    vp_log_sink_close(&sink);
    check(fake.close_calls == 1u, "error/close once");
}

static void test_flush_failure(void) {
    Fake fake;
    VPLogSink sink;
    VPLogSinkStats stats;

    fake_init(&fake, 0);
    fake.flush_error_at = 2;
    memset(&sink, 0, sizeof(sink));
    check(vp_log_sink_init(&sink, ops_for(0), "fake.log") == 0, "flush/init");
    check(vp_log_sink_printf(&sink, "T first\n") > 0, "flush/first");
    check(vp_log_sink_printf(&sink, "T second\n") == VP_LOG_SINK_FLUSH_FAILED,
          "flush/failure reported");
    check(vp_log_sink_printf(&sink, "T third\n") < 0, "flush/fail closed");
    check(fake.write_calls == 2u, "flush/no further write");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "flush/stats");
    check(stats.flush_failures == 1u, "flush/count");
    check(stats.lines_failed == 1u, "flush/failed line");
    vp_log_sink_close(&sink);
    check(fake.close_calls == 1u, "flush/close once");
}

static void test_failed_open_and_explicit_retry(void) {
    Fake fake;
    VPLogSink sink;
    VPLogSinkStats stats;

    fake_init(&fake, 0);
    fake.fail_open = 1;
    memset(&sink, 0, sizeof(sink));
    check(vp_log_sink_init(&sink, ops_for(0), "fake.log") == VP_LOG_SINK_NOT_READY,
          "open/failure returned");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "open/stats");
    check(stats.open_attempts == 1u, "open/attempt counted");
    check(stats.open_failures == 1u, "open/failure counted");
    check(vp_log_sink_printf(&sink, "T suppressed\n") < 0, "open/write rejected");
    check(fake.write_calls == 0u, "open/no write attempted");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "open/stats2");
    check(stats.suppressed_lines == 1u, "open/suppression counted");

    fake.fail_open = 0;
    check(vp_log_sink_init(&sink, ops_for(0), "fake.log") == 0, "open/explicit retry");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "open/stats3");
    check(stats.open_attempts == 2u, "open/retry counted");
    check(stats.open_failures == 1u, "open/failure history kept");
    check(vp_log_sink_printf(&sink, "T recovered\n") > 0, "open/write after retry");
    check(fake.open_calls == 2u, "open/two attempts one success");
    check(fake.close_calls == 0u, "open/still open");
    vp_log_sink_close(&sink);
    check(fake.close_calls == 1u, "open/close once");
}

static void test_truncation(void) {
    char long_line[1101];
    Fake fake;
    VPLogSink sink;
    VPLogSinkStats stats;

    memset(long_line, 'x', sizeof(long_line) - 1u);
    long_line[sizeof(long_line) - 1u] = 0;
    fake_init(&fake, 0);
    memset(&sink, 0, sizeof(sink));
    check(vp_log_sink_init(&sink, ops_for(0), "fake.log") == 0, "trunc/init");
    check(vp_log_sink_printf(&sink, "L%s\n", long_line) == (int)(VP_LOG_SINK_LINE_BYTES - 1u),
          "trunc/bounded return");
    check(fake.last_length == VP_LOG_SINK_LINE_BYTES - 1u, "trunc/bounded write");
    check(fake.last[0] == 'L', "trunc/prefix kept");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "trunc/stats");
    check(stats.truncated_lines == 1u, "trunc/counted");
    check(stats.lines_written == 1u, "trunc/line counted");
    check(stats.bytes_written == VP_LOG_SINK_LINE_BYTES - 1u, "trunc/bytes counted");
    vp_log_sink_close(&sink);
}

typedef struct {
    VPLogSink *sink;
    unsigned id;
    unsigned lines;
    unsigned failures;
} ThreadArg;

static void *thread_writer(void *user) {
    ThreadArg *arg = (ThreadArg *)user;
    unsigned i;
    for (i = 0; i < arg->lines; ++i) {
        if (vp_log_sink_printf(arg->sink, "T%u %08u\n", arg->id, i) <= 0)
            ++arg->failures;
    }
    return NULL;
}

static void test_concurrent_ownership(void) {
    enum { THREADS = 4, LINES = 25000 };
    Fake fake;
    VPLogSink sink;
    VPLogSinkStats stats;
    ThreadArg args[THREADS];
    pthread_t threads[THREADS];
    unsigned i;

    fake_init(&fake, 1);
    memset(&sink, 0, sizeof(sink));
    check(vp_log_sink_init(&sink, ops_for(1), "fake.log") == 0, "threads/init");
    for (i = 0; i < THREADS; ++i) {
        args[i].sink = &sink;
        args[i].id = i;
        args[i].lines = LINES;
        args[i].failures = 0;
        check(pthread_create(&threads[i], NULL, thread_writer, &args[i]) == 0,
              "threads/create");
    }
    for (i = 0; i < THREADS; ++i) pthread_join(threads[i], NULL);
    for (i = 0; i < THREADS; ++i)
        check(args[i].failures == 0u, "threads/no failed write");
    check(fake.open_calls == 1u, "threads/one open");
    check(fake.write_calls == THREADS * LINES, "threads/all lines written");
    check(fake.lines == THREADS * LINES, "threads/all complete");
    check(fake.bad_lines == 0u, "threads/no interleaved line");
    check(vp_log_sink_get_stats(&sink, &stats) == 0, "threads/stats");
    check(stats.lines_written == THREADS * LINES, "threads/stats lines");
    check(stats.suppressed_lines == 0u, "threads/no suppression");
    check(vp_log_sink_close(&sink) == 0, "threads/close");
    check(fake.close_calls == 1u, "threads/one close");
    pthread_mutex_destroy(&fake.mutex);
}

static void test_no_flush_adapter_and_invalid_config(void) {
    Fake fake;
    VPLogSink sink;
    VPLogSink fresh;
    VPLogSinkStats stats;

    fake_init(&fake, 0);
    no_flush_ops.user = &fake;
    memset(&sink, 0, sizeof(sink));
    check(vp_log_sink_init(&sink, &no_flush_ops, "fake.log") == 0, "noflush/init");
    check(vp_log_sink_printf(&sink, "T line\n") > 0, "noflush/write");
    check(fake.flush_calls == 0u, "noflush/no flush op");
    vp_log_sink_close(&sink);
    check(fake.close_calls == 1u, "noflush/close");
    memset(&fresh, 0, sizeof(fresh));
    check(vp_log_sink_init(NULL, &no_flush_ops, "fake.log") == VP_LOG_SINK_INVALID,
          "invalid/null sink");
    check(vp_log_sink_init(&fresh, NULL, "fake.log") == VP_LOG_SINK_INVALID,
          "invalid/null ops");
    check(vp_log_sink_init(&fresh, &no_flush_ops, NULL) == VP_LOG_SINK_INVALID,
          "invalid/null path");
    check(vp_log_sink_get_stats(&fresh, &stats) == 0, "invalid/stats");
    check(stats.open_attempts == 0u, "invalid/no open attempt");
}

int main(void) {
    test_many_writes_one_lifetime();
    test_reinit_after_close();
    test_short_and_error_writes();
    test_flush_failure();
    test_failed_open_and_explicit_retry();
    test_truncation();
    test_concurrent_ownership();
    test_no_flush_adapter_and_invalid_config();
    if (failures) {
        fprintf(stderr, "log_sink: %u failure(s)\n", failures);
        return 1;
    }
    printf("log_sink: PASS\n");
    return 0;
}

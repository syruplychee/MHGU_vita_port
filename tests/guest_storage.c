#define _GNU_SOURCE
/* Host regression for src/platform/guest_storage.c tree-file diagnostics.
 *
 * The production translation unit is included so the static open_tree_file()
 * can be called directly for the zero-size success case. Only the three
 * sceIo* entry points used by that unit are mocked; there is no filesystem or
 * framework. Compile with -Itests/stubs so <psp2/...> resolves to the local
 * minimal ABI stubs.
 *
 * The guest read address is a 32-bit field in the production ABI. On a 64-bit
 * host a stack buffer would be truncated, so the test allocates a page with
 * MAP_32BIT and asserts that its address fits. The production ABI is not
 * widened for the host.
 */
#include "../src/platform/guest_storage.c"

#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

#define TREE_ROOT "root"
#define TREE_FILE "root/bgm.opus"
#define RECORD_OFFSET 0x1000u
#define MAP_BYTES (VP_GUEST_STORAGE_MAP_HEADER_BYTES + VP_GUEST_STORAGE_MAP_RECORD_BYTES + 16u)

typedef struct Mock {
    int open_rc;
    int stat_rc;
    int64_t stat_size;
    int probe_rc;        /* sceIoGetstatByFd after the first open-time stat */
    int64_t probe_size;
    int pread_error;
    uint32_t short_first;
    const uint8_t *data;
    uint64_t data_size;
    unsigned open_calls;
    unsigned close_calls;
    unsigned stat_calls;
    unsigned pread_calls;
    int nested_read_rc;  /* result of one re-entrant read issued by pread */
    unsigned nested_reads;
} Mock;

static Mock mock;
static uint8_t map_bytes[MAP_BYTES];
static uint8_t tree_data[64];
static uint8_t *out;
static VPGuestStorage *active_storage;

static void put32(uint8_t *p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static void put64(uint8_t *p, uint64_t value) {
    memcpy(p, &value, sizeof(value));
}

static void mock_reset(void) {
    memset(&mock, 0, sizeof(mock));
    mock.open_rc = 42;
    mock.stat_rc = 0;
    mock.stat_size = 8;
    mock.probe_rc = 0;
    mock.probe_size = 8;
    mock.data = tree_data;
    mock.data_size = sizeof(tree_data);
    active_storage = NULL;
}

static void setup_storage(VPGuestStorage *st, uint64_t record_size) {
    memset(map_bytes, 0, sizeof(map_bytes));
    put64(map_bytes + VP_GUEST_STORAGE_MAP_HEADER_BYTES, RECORD_OFFSET);
    put64(map_bytes + VP_GUEST_STORAGE_MAP_HEADER_BYTES + 8, record_size);
    put32(map_bytes + VP_GUEST_STORAGE_MAP_HEADER_BYTES + 16, 0);
    memcpy(map_bytes + VP_GUEST_STORAGE_MAP_HEADER_BYTES
               + VP_GUEST_STORAGE_MAP_RECORD_BYTES,
           "bgm.opus", sizeof("bgm.opus"));

    vp_guest_storage_init(st);
    st->map = map_bytes;
    st->map_bytes = (uint32_t)sizeof(map_bytes);
    st->file_count = 1;
    st->path_bytes = (uint32_t)sizeof("bgm.opus");
    st->tree_bytes = record_size;
    st->data_offset = RECORD_OFFSET;
    st->image_size = RECORD_OFFSET + record_size;
    strcpy(st->tree_root, TREE_ROOT);
    st->open = 1;
}

static uint32_t guest_address(const void *p) {
    uintptr_t address = (uintptr_t)p;
    assert(address <= UINT32_MAX);
    return (uint32_t)address;
}

static int read_at(VPGuestStorage *st, uint64_t offset, uint32_t size) {
    return vp_guest_storage_read(st, offset, guest_address(out), size);
}

static int read_bytes(VPGuestStorage *st, uint32_t size) {
    return read_at(st, RECORD_OFFSET, size);
}

static int expect_read_at(VPGuestStorage *st, uint64_t offset, uint32_t size,
                          int expected) {
    int rc = read_at(st, offset, size);
    if (rc != expected) {
        fprintf(stderr, "read offset=%llu size=%u rc=%d expected=%d diag=%u error=%s\n",
                (unsigned long long)offset, size, rc, expected,
                st->diagnostic.operation, st->error);
    }
    return rc;
}

SceUID sceIoOpen(const char *file, int flags, SceMode mode) {
    (void)flags;
    (void)mode;
    ++mock.open_calls;
    assert(strcmp(file, TREE_FILE) == 0 || strcmp(file, "root/a.opus") == 0
           || strcmp(file, "root/b.opus") == 0);
    return mock.open_rc + (int)mock.open_calls - 1;
}

static int mock_fd_valid(SceUID fd) {
    return fd >= mock.open_rc
        && fd < mock.open_rc + (int)mock.open_calls;
}

int sceIoClose(SceUID fd) {
    ++mock.close_calls;
    assert(mock_fd_valid(fd));
    return 0;
}

int sceIoGetstat(const char *file, SceIoStat *stat) {
    (void)file;
    (void)stat;
    return -1;
}

int sceIoGetstatByFd(SceUID fd, SceIoStat *stat) {
    ++mock.stat_calls;
    assert(mock_fd_valid(fd));
    if (mock.stat_calls == 1) {
        if (mock.stat_rc < 0) return mock.stat_rc;
        stat->st_size = mock.stat_size;
        return 0;
    }
    if (mock.probe_rc < 0) return mock.probe_rc;
    stat->st_size = mock.probe_size;
    return 0;
}

int sceIoPread(SceUID fd, void *data, SceSize size, SceOff offset) {
    ++mock.pread_calls;
    assert(mock_fd_valid(fd));
    if (active_storage != NULL && mock.nested_reads == 0) {
        mock.nested_reads = 1;
        mock.nested_read_rc = vp_guest_storage_read(
            active_storage, RECORD_OFFSET, guest_address(out), 8);
    }
    if (mock.pread_error) return mock.pread_error;
    if (mock.short_first && mock.pread_calls > 1) return 0;
    if (offset < 0) return -1;
    uint64_t position = (uint64_t)offset;
    if (position >= mock.data_size) return 0;
    uint64_t available = mock.data_size - position;
    uint32_t amount = size;
    if (mock.short_first && mock.pread_calls == 1) amount = mock.short_first;
    if ((uint64_t)amount > available) amount = (uint32_t)available;
    if (amount) memcpy(data, mock.data + position, amount);
    return (int)amount;
}

static void test_open_failure(void) {
    VPGuestStorage st;
    mock_reset();
    mock.open_rc = -13;
    setup_storage(&st, 16);

    assert(expect_read_at(&st, RECORD_OFFSET + 5, 8,
                          VP_GUEST_STORAGE_BACKING)
           == VP_GUEST_STORAGE_BACKING);
    const VPGuestStorageDiagnostic *d = &st.diagnostic;
    assert(d->operation == VP_GUEST_STORAGE_DIAG_OPEN);
    assert(strcmp(vp_guest_storage_diag_operation_name(d->operation), "open") == 0);
    assert(d->open_attempted == 1 && d->open_rc == -13 && d->fd == -1);
    assert(d->stat_attempted == 0 && d->stat_rc == 0);
    assert(d->observed_valid == 0 && d->observed_size == 0);
    assert(d->expected_size == 16 && d->read_attempted == 0);
    assert(d->file_offset == 5 && d->read_length == 8);
    assert(strcmp(st.error, "tree file is missing") == 0);
    assert(strcmp(st.last_path, "bgm.opus") == 0);
    assert(mock.open_calls == 1 && mock.stat_calls == 0 && mock.pread_calls == 0);
    puts("PASS open failure with requested range");
}

static void test_fd_stat_failure(void) {
    VPGuestStorage st;
    mock_reset();
    mock.stat_rc = -5;
    setup_storage(&st, 16);

    assert(expect_read_at(&st, RECORD_OFFSET + 5, 8,
                          VP_GUEST_STORAGE_BACKING)
           == VP_GUEST_STORAGE_BACKING);
    const VPGuestStorageDiagnostic *d = &st.diagnostic;
    assert(d->operation == VP_GUEST_STORAGE_DIAG_FD_STAT);
    assert(strcmp(vp_guest_storage_diag_operation_name(d->operation), "fd_stat") == 0);
    assert(d->open_attempted == 1 && d->open_rc == 42 && d->fd == 42);
    assert(d->stat_attempted == 1 && d->stat_rc == -5);
    assert(d->observed_valid == 0 && d->observed_size == 0);
    assert(d->expected_size == 16 && d->read_attempted == 0);
    assert(d->file_offset == 5 && d->read_length == 8);
    assert(d->open_seq == 1 && d->close_attempted == 1 && d->close_rc == 0);
    assert(st.tree_opens == 1 && st.tree_closes == 1 && st.tree_retires == 0);
    assert(strcmp(st.error, "tree file fd stat failed") == 0);
    assert(mock.open_calls == 1 && mock.stat_calls == 1 && mock.pread_calls == 0);
    assert(mock.close_calls == 1);
    assert(st.tree_fd == -1 && st.tree_fd_path == UINT32_MAX);
    puts("PASS fd-stat failure is distinct from a size mismatch");
}

static void test_size_mismatch(void) {
    VPGuestStorage st;
    mock_reset();
    mock.stat_size = 17;
    setup_storage(&st, 16);

    assert(expect_read_at(&st, RECORD_OFFSET + 5, 8,
                          VP_GUEST_STORAGE_BACKING)
           == VP_GUEST_STORAGE_BACKING);
    const VPGuestStorageDiagnostic *d = &st.diagnostic;
    assert(d->operation == VP_GUEST_STORAGE_DIAG_SIZE);
    assert(strcmp(vp_guest_storage_diag_operation_name(d->operation), "size_mismatch") == 0);
    assert(d->open_attempted == 1 && d->open_rc == 42 && d->fd == 42);
    assert(d->stat_attempted == 1 && d->stat_rc == 0 && d->observed_valid == 1);
    assert(d->expected_size == 16 && d->observed_size == 17);
    assert(d->read_attempted == 0);
    assert(d->file_offset == 5 && d->read_length == 8);
    assert(d->open_seq == 1 && d->close_attempted == 1 && d->close_rc == 0);
    assert(st.tree_opens == 1 && st.tree_closes == 1 && st.tree_retires == 0);
    assert(strcmp(st.error, "tree file size differs from the RomFS map") == 0);
    /* Fail-closed guard: exactly one open and one stat, no retry, no read. */
    assert(mock.open_calls == 1 && mock.stat_calls == 1 && mock.pread_calls == 0);
    assert(mock.close_calls == 1);
    assert(st.tree_fd == -1 && st.tree_fd_path == UINT32_MAX);
    puts("PASS true size mismatch, fail-closed with no retry");
}

static void test_zero_size_valid_success(void) {
    VPGuestStorage st;
    mock_reset();
    mock.stat_size = 0;
    setup_storage(&st, 0);

    assert(open_tree_file(&st, 0, 0) == 0);
    assert(st.diagnostic.operation == VP_GUEST_STORAGE_DIAG_NONE);
    assert(st.diagnostic.observed_valid == 0 && st.diagnostic.observed_size == 0);
    assert(st.file_opens == 1 && st.tree_fd == 42 && st.tree_fd_path == 0);
    assert(st.fd_open_seq == 1 && st.fd_close_seq == 0
           && st.fd_generation == 1 && st.tree_fd_open_seq == 1);
    assert(mock.open_calls == 1 && mock.stat_calls == 1);
    puts("PASS observed size 0 is valid when the map expects 0");
}

static void test_cached_success_clears_stale_diagnostic(void) {
    VPGuestStorage st;
    mock_reset();
    mock.pread_error = -5;
    setup_storage(&st, 8);

    assert(read_bytes(&st, 8) == VP_GUEST_STORAGE_IO);
    assert(st.diagnostic.operation == VP_GUEST_STORAGE_DIAG_READ);
    assert(st.tree_fd == 42 && st.tree_fd_path == 0);
    assert(st.diagnostic.fd_probe_attempted == 1);
    assert(st.diagnostic.fd_probe_rc == 0 && st.diagnostic.fd_probe_valid == 1);
    assert(st.diagnostic.fd_probe_size == 8);
    assert(st.diagnostic.open_seq == 1 && st.diagnostic.close_seq == 0);
    assert(st.diagnostic.closes_since_open == 0);

    mock.pread_error = 0;
    assert(read_bytes(&st, 8) == VP_GUEST_STORAGE_OK);
    assert(st.diagnostic.operation == VP_GUEST_STORAGE_DIAG_NONE);
    assert(memcmp(out, tree_data, 8) == 0);
    /* The second read reused the cached fd; it did not reopen or restat at
     * open time (the only later stat was the one read-failure probe). */
    assert(mock.open_calls == 1 && mock.stat_calls == 2);
    assert(mock.pread_calls == 2);
    puts("PASS cached-fd success clears stale diagnostic");
}

static void test_read_error_and_short_read(void) {
    VPGuestStorage st;
    mock_reset();
    mock.stat_size = 16;
    mock.pread_error = -5;
    setup_storage(&st, 16);

    assert(expect_read_at(&st, RECORD_OFFSET + 5, 8, VP_GUEST_STORAGE_IO)
           == VP_GUEST_STORAGE_IO);
    const VPGuestStorageDiagnostic *d = &st.diagnostic;
    assert(d->operation == VP_GUEST_STORAGE_DIAG_READ);
    assert(strcmp(vp_guest_storage_diag_operation_name(d->operation), "read") == 0);
    assert(d->read_attempted == 1 && d->io_rc == -5);
    assert(d->fd == 42 && d->file_offset == 5 && d->read_length == 8);
    assert(d->expected_size == 16 && strcmp(st.last_path, "bgm.opus") == 0);
    assert(st.tree_fd == 42);
    /* A failed read leaves the fd cached; it is not retried or substituted.
     * The one additional stat is the failure probe, which found the handle
     * still present with the expected size. */
    assert(mock.open_calls == 1 && mock.stat_calls == 2 && mock.pread_calls == 1);
    assert(mock.close_calls == 0);

    mock_reset();
    mock.stat_size = 16;
    mock.short_first = 2;
    setup_storage(&st, 16);
    assert(expect_read_at(&st, RECORD_OFFSET + 5, 8, VP_GUEST_STORAGE_IO)
           == VP_GUEST_STORAGE_IO);
    d = &st.diagnostic;
    assert(d->operation == VP_GUEST_STORAGE_DIAG_READ);
    assert(d->io_rc == 0 && d->read_length == 8 && d->file_offset == 5);
    assert(mock.pread_calls == 2 && mock.stat_calls == 2);
    puts("PASS read error and short/EOF read are categorized");
}

#define MAP2_BYTES (VP_GUEST_STORAGE_MAP_HEADER_BYTES + 2u * VP_GUEST_STORAGE_MAP_RECORD_BYTES + 16u)
static uint8_t map2_bytes[MAP2_BYTES];

static void setup_two_files(VPGuestStorage *st) {
    memset(map2_bytes, 0, sizeof(map2_bytes));
    const uint32_t records = VP_GUEST_STORAGE_MAP_HEADER_BYTES;
    put64(map2_bytes + records, 0x1000u);
    put64(map2_bytes + records + 8, 8u);
    put32(map2_bytes + records + 16, 0u);
    put64(map2_bytes + records + VP_GUEST_STORAGE_MAP_RECORD_BYTES, 0x1008u);
    put64(map2_bytes + records + VP_GUEST_STORAGE_MAP_RECORD_BYTES + 8, 8u);
    put32(map2_bytes + records + VP_GUEST_STORAGE_MAP_RECORD_BYTES + 16, 7u);
    memcpy(map2_bytes + records + 2u * VP_GUEST_STORAGE_MAP_RECORD_BYTES,
           "a.opus", sizeof("a.opus"));
    memcpy(map2_bytes + records + 2u * VP_GUEST_STORAGE_MAP_RECORD_BYTES + 7u,
           "b.opus", sizeof("b.opus"));

    vp_guest_storage_init(st);
    st->map = map2_bytes;
    st->map_bytes = (uint32_t)sizeof(map2_bytes);
    st->file_count = 2;
    st->path_bytes = 14;
    st->tree_bytes = 16;
    st->data_offset = 0x1000u;
    st->image_size = 0x1010u;
    strcpy(st->tree_root, TREE_ROOT);
    st->open = 1;
}

static void test_invalidated_cache_retired(void) {
    VPGuestStorage st;
    mock_reset();
    mock.stat_size = 8;
    mock.probe_size = 8;
    setup_storage(&st, 8);
    active_storage = &st;
    vp_guest_storage_set_owner(&st, 0x1234u);

    assert(read_bytes(&st, 8) == VP_GUEST_STORAGE_OK);
    assert(st.tree_fd == 42 && st.fd_open_seq == 1 && st.fd_generation == 1);

    /* EBADFD is what the emulator returns when tell_file()/stat cannot find
     * the descriptor at all. The failure probe must confirm that before the
     * cache entry is retired. */
    mock.pread_error = (int)0x80010051u;
    mock.probe_rc = (int)0x80010051u;
    assert(read_bytes(&st, 8) == VP_GUEST_STORAGE_IO);
    const VPGuestStorageDiagnostic *d = &st.diagnostic;
    assert(d->operation == VP_GUEST_STORAGE_DIAG_READ);
    assert(d->io_rc == (int)0x80010051u);
    assert(d->owner == 0x1234u);
    assert(d->fd == 42 && d->open_seq == 1 && d->close_seq == 0);
    assert(d->closes_since_open == 0 && d->retires_since_open == 0);
    assert(d->cache_generation == 2 && d->retire_seq == 1);
    assert(d->fd_probe_attempted == 1 && d->fd_probe_rc == (int)0x80010051u);
    assert(d->fd_probe_valid == 0 && d->fd_probe_size == 0);
    assert(d->fd_probe_ebadfd == 1 && d->retire_attempted == 1);
    assert(strstr(st.error, "removed from the emulator fd table") != NULL);
    /* Retired without a second close on the dead descriptor and without any
     * retry inside the failing request. */
    assert(st.tree_fd == -1 && st.tree_fd_path == UINT32_MAX);
    assert(st.fd_close_seq == 0 && st.fd_retire_seq == 1);
    assert(st.fd_generation == 2);
    assert(st.tree_opens == 1 && st.tree_closes == 0 && st.tree_retires == 1);
    assert(mock.open_calls == 1 && mock.pread_calls == 2 && mock.stat_calls == 2);
    assert(mock.close_calls == 0);

    /* The next independent request takes the normal cache-miss path. This is
     * not a retry of the failed request. */
    mock.pread_error = 0;
    mock.probe_rc = 0;
    assert(read_bytes(&st, 8) == VP_GUEST_STORAGE_OK);
    assert(st.tree_fd == 43 && st.fd_open_seq == 2);
    assert(st.file_opens == 2 && st.tree_opens == 2);
    assert(st.tree_closes == 0 && st.tree_retires == 1);
    assert(mock.open_calls == 2 && mock.pread_calls == 3 && mock.stat_calls == 3);
    puts("PASS invalidated cached fd is probed and retired without retry");
}

static void test_non_ebadfd_probe_keeps_cache(void) {
    VPGuestStorage st;
    mock_reset();
    mock.stat_size = 8;
    mock.probe_size = 8;
    setup_storage(&st, 8);
    active_storage = &st;

    assert(read_bytes(&st, 8) == VP_GUEST_STORAGE_OK);
    mock.pread_error = -5;             /* EIO on the read */
    mock.probe_rc = (int)0x80010005u;  /* EIO probe, not EBADFD */
    assert(read_bytes(&st, 8) == VP_GUEST_STORAGE_IO);
    const VPGuestStorageDiagnostic *d = &st.diagnostic;
    assert(d->operation == VP_GUEST_STORAGE_DIAG_READ && d->io_rc == -5);
    assert(d->fd_probe_attempted == 1);
    assert(d->fd_probe_rc == (int)0x80010005u);
    assert(d->fd_probe_valid == 0 && d->fd_probe_ebadfd == 0);
    assert(d->retire_attempted == 0);
    assert(strstr(st.error, "ownership unknown") != NULL);
    /* A non-EBADFD probe cannot prove the handle is absent: keep the cache
     * entry, close nothing, and do not retry the failing request. */
    assert(st.tree_fd == 42 && st.fd_close_seq == 0 && st.fd_retire_seq == 0);
    assert(st.tree_closes == 0 && st.tree_retires == 0);
    assert(mock.open_calls == 1 && mock.pread_calls == 2 && mock.close_calls == 0);

    /* The same cached descriptor still works after the transient probe error. */
    mock.pread_error = 0;
    mock.probe_rc = 0;
    assert(read_bytes(&st, 8) == VP_GUEST_STORAGE_OK);
    assert(st.tree_fd == 42 && mock.open_calls == 1 && mock.pread_calls == 3);
    puts("PASS non-EBADFD probe leaves cache retained and ownership unknown");
}

static void test_cache_switch_sequences(void) {
    VPGuestStorage st;
    mock_reset();
    mock.stat_size = 8;
    mock.probe_size = 8;
    setup_two_files(&st);
    active_storage = &st;

    assert(read_at(&st, 0x1000u, 8) == VP_GUEST_STORAGE_OK);
    assert(st.tree_fd == 42 && st.fd_open_seq == 1 && st.fd_close_seq == 0
           && st.fd_generation == 1 && st.tree_fd_path == 0);
    assert(read_at(&st, 0x1008u, 8) == VP_GUEST_STORAGE_OK);
    assert(st.tree_fd == 43 && st.tree_fd_path == 7);
    assert(st.fd_open_seq == 2 && st.fd_close_seq == 1 && st.fd_generation == 3);
    assert(st.tree_opens == 2 && st.tree_closes == 1 && st.tree_retires == 0);
    assert(mock.close_calls == 1);

    mock.pread_error = -5;
    assert(read_at(&st, 0x1008u, 8) == VP_GUEST_STORAGE_IO);
    const VPGuestStorageDiagnostic *d = &st.diagnostic;
    assert(d->fd == 43 && d->open_seq == 2 && d->close_seq == 1);
    assert(d->retire_seq == 0);
    assert(d->closes_since_open == 0 && d->retires_since_open == 0);
    assert(d->cache_generation == 3);
    assert(d->fd_probe_valid == 1 && d->fd_probe_size == 8);
    /* A still-valid handle stays cached; the failure was an operation error. */
    assert(st.tree_fd == 43 && mock.close_calls == 1);
    puts("PASS cache switch and failure lifecycle sequences");
}

static void test_reentrant_read_refused(void) {
    VPGuestStorage st;
    mock_reset();
    setup_storage(&st, 8);
    active_storage = &st;

    assert(read_bytes(&st, 8) == VP_GUEST_STORAGE_OK);
    assert(mock.nested_reads == 1);
    assert(mock.nested_read_rc == VP_GUEST_STORAGE_BUSY);
    assert(st.concurrent_reads == 1);
    assert(st.reads == 1 && st.active == 0);
    /* The refused request did not open, close, read or clear state. */
    assert(mock.open_calls == 1 && mock.pread_calls == 1 && mock.close_calls == 0);
    puts("PASS re-entrant read refused without touching the cache");
}

int main(void) {
    _Static_assert(sizeof(SceIoStat) == 0x58, "SceIoStat ABI must match VitaSDK");
    for (unsigned i = 0; i < sizeof(tree_data); ++i) tree_data[i] = (uint8_t)i;

    out = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(out != MAP_FAILED);
    assert((uintptr_t)out <= UINT32_MAX);

    test_open_failure();
    test_fd_stat_failure();
    test_size_mismatch();
    test_zero_size_valid_success();
    test_cached_success_clears_stale_diagnostic();
    test_read_error_and_short_read();
    test_invalidated_cache_retired();
    test_non_ebadfd_probe_keeps_cache();
    test_cache_switch_sequences();
    test_reentrant_read_refused();
    puts("ALL GUEST STORAGE DIAGNOSTIC TESTS PASSED");
    return 0;
}

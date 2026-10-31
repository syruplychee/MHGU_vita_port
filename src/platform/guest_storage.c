#include "guest_storage.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdlib.h>
#include <string.h>

#define STORAGE_MAP_LIMIT (8u * 1024u * 1024u)
/* Vita error returned by the emulator when its fd table has no entry for the
 * descriptor. Only this exact value proves a cached handle is absent. */
#define VP_GUEST_STORAGE_EBADFD ((int)0x80010051u)

static uint32_t rd32(const uint8_t *p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint64_t rd64(const uint8_t *p) {
    uint64_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static void set_error(VPGuestStorage *st, const char *text) {
    size_t i = 0;
    for (; text[i] && i + 1 < sizeof(st->error); ++i) st->error[i] = text[i];
    st->error[i] = 0;
}

static void clear_diagnostic(VPGuestStorage *st) {
    memset(&st->diagnostic, 0, sizeof(st->diagnostic));
}

const char *vp_guest_storage_diag_operation_name(uint32_t operation) {
    switch (operation) {
    case VP_GUEST_STORAGE_DIAG_PATH_LIMIT: return "path_limit";
    case VP_GUEST_STORAGE_DIAG_OPEN: return "open";
    case VP_GUEST_STORAGE_DIAG_FD_STAT: return "fd_stat";
    case VP_GUEST_STORAGE_DIAG_SIZE: return "size_mismatch";
    case VP_GUEST_STORAGE_DIAG_READ: return "read";
    default: return "none";
    }
}

/* Close the cached tree descriptor and record the actual close. The return
 * code belongs in the caller's diagnostic; this helper never suppresses it. */
static int close_tree_fd(VPGuestStorage *st) {
    int rc = 0;
    if (st->tree_fd >= 0) rc = sceIoClose(st->tree_fd);
    st->tree_fd = -1;
    st->tree_fd_path = UINT32_MAX;
    ++st->tree_closes;
    ++st->fd_close_seq;
    ++st->fd_generation;
    return rc;
}

/* Retire a cache entry whose descriptor is already known to be absent. No
 * sceIoClose is attempted; the next independent request takes the normal
 * cache-miss path. */
static void retire_tree_fd(VPGuestStorage *st) {
    st->tree_fd = -1;
    st->tree_fd_path = UINT32_MAX;
    ++st->tree_retires;
    ++st->fd_retire_seq;
    ++st->fd_generation;
}

static void close_all(VPGuestStorage *st) {
    if (st->tree_fd >= 0) close_tree_fd(st);
    if (st->map_fd >= 0) sceIoClose(st->map_fd);
    if (st->index_fd >= 0) sceIoClose(st->index_fd);
    st->tree_fd = st->map_fd = st->index_fd = -1;
}

void vp_guest_storage_set_owner(VPGuestStorage *st, uint32_t owner) {
    if (st) st->owner = owner;
}

void vp_guest_storage_init(VPGuestStorage *st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->index_fd = st->map_fd = st->tree_fd = -1;
    st->tree_fd_path = UINT32_MAX;
}

static int read_exact(int fd, void *buffer, uint32_t bytes, uint64_t offset,
                      int *io_rc) {
    uint8_t *cursor = (uint8_t *)buffer;
    uint32_t done = 0;
    if (io_rc) *io_rc = 0;
    while (done < bytes) {
        int got = sceIoPread(fd, cursor + done, bytes - done, (SceOff)(offset + done));
        if (got <= 0) {
            if (io_rc) *io_rc = got;
            return -1;
        }
        done += (uint32_t)got;
    }
    return 0;
}

int vp_guest_storage_open(VPGuestStorage *st, const char *index_path,
                          const char *map_path, const char *tree_root) {
    if (!st || !index_path || !map_path || !tree_root) return -1;
    if (st->open) vp_guest_storage_close(st);
    vp_guest_storage_init(st);
    size_t root_len = strlen(tree_root);
    if (!root_len || root_len >= sizeof(st->tree_root)) {
        set_error(st, "tree root path too long");
        return -1;
    }
    memcpy(st->tree_root, tree_root, root_len + 1);

    st->index_fd = sceIoOpen(index_path, SCE_O_RDONLY, 0);
    if (st->index_fd < 0) {
        set_error(st, "cannot open romfs.index");
        st->index_fd = -1;
        return -1;
    }
    uint8_t header[0x50];
    if (read_exact(st->index_fd, header, sizeof(header), 0, NULL) != 0) {
        set_error(st, "cannot read the RomFS header");
        vp_guest_storage_close(st);
        return -1;
    }
    uint64_t header_size = rd64(header + 0x00);
    uint64_t dir_hash_offset = rd64(header + 0x08);
    uint64_t dir_hash_size = rd64(header + 0x10);
    uint64_t dir_meta_offset = rd64(header + 0x18);
    uint64_t dir_meta_size = rd64(header + 0x20);
    uint64_t file_hash_offset = rd64(header + 0x28);
    uint64_t file_hash_size = rd64(header + 0x30);
    uint64_t file_meta_offset = rd64(header + 0x38);
    uint64_t file_meta_size = rd64(header + 0x40);
    uint64_t data_offset = rd64(header + 0x48);
    if (header_size != 0x50 || !dir_hash_size || !file_hash_size
        || dir_hash_size % 4 || file_hash_size % 4
        || dir_hash_offset > data_offset || dir_hash_size > data_offset - dir_hash_offset
        || dir_meta_offset > data_offset || dir_meta_size > data_offset - dir_meta_offset
        || file_hash_offset > data_offset || file_hash_size > data_offset - file_hash_offset
        || file_meta_offset > data_offset || file_meta_size > data_offset - file_meta_offset) {
        set_error(st, "RomFS header is inconsistent");
        vp_guest_storage_close(st);
        return -1;
    }
    SceIoStat stat;
    if (sceIoGetstat(index_path, &stat) < 0 || (uint64_t)stat.st_size != data_offset) {
        set_error(st, "romfs.index size does not match its header");
        vp_guest_storage_close(st);
        return -1;
    }
    st->data_offset = data_offset;

    st->map_fd = sceIoOpen(map_path, SCE_O_RDONLY, 0);
    if (st->map_fd < 0) {
        set_error(st, "cannot open romfs.map");
        st->map_fd = -1;
        vp_guest_storage_close(st);
        return -1;
    }
    if (sceIoGetstat(map_path, &stat) < 0 || stat.st_size < (SceOff)VP_GUEST_STORAGE_MAP_HEADER_BYTES
        || (uint64_t)stat.st_size > STORAGE_MAP_LIMIT) {
        set_error(st, "romfs.map size is unusable");
        vp_guest_storage_close(st);
        return -1;
    }
    st->map_bytes = (uint32_t)stat.st_size;
    st->map = (uint8_t *)malloc(st->map_bytes);
    if (!st->map) {
        set_error(st, "cannot allocate the map buffer");
        vp_guest_storage_close(st);
        return -1;
    }
    if (read_exact(st->map_fd, st->map, st->map_bytes, 0, NULL) != 0) {
        set_error(st, "cannot read romfs.map");
        vp_guest_storage_close(st);
        return -1;
    }
    /* The map header is packed, so read it at explicit offsets rather than
     * through a C struct whose u64 alignment padding would differ. */
    uint32_t map_magic = rd32(st->map);
    uint32_t map_version = rd32(st->map + 4);
    uint32_t map_count = rd32(st->map + 8);
    uint32_t map_paths = rd32(st->map + 12);
    uint32_t map_reserved = rd32(st->map + 16);
    uint64_t map_tree_bytes = rd64(st->map + 20);
    uint64_t map_data_offset = rd64(st->map + 28);
    uint64_t map_image_size = rd64(st->map + 36);
    if (map_magic != VP_GUEST_STORAGE_MAP_MAGIC
        || map_version != VP_GUEST_STORAGE_MAP_VERSION
        || map_reserved != 0) {
        set_error(st, "romfs.map header is not a VPRM map");
        vp_guest_storage_close(st);
        return -1;
    }
    uint64_t records_bytes = (uint64_t)map_count * VP_GUEST_STORAGE_MAP_RECORD_BYTES;
    if (records_bytes > st->map_bytes - VP_GUEST_STORAGE_MAP_HEADER_BYTES
        || map_paths > st->map_bytes - VP_GUEST_STORAGE_MAP_HEADER_BYTES - records_bytes
        || (uint64_t)map_count * 0x20u > file_meta_size
        || map_data_offset != data_offset
        || map_image_size != data_offset + map_tree_bytes) {
        set_error(st, "romfs.map does not match the RomFS header");
        vp_guest_storage_close(st);
        return -1;
    }
    const uint8_t *records = st->map + VP_GUEST_STORAGE_MAP_HEADER_BYTES;
    const char *paths = (const char *)(records + records_bytes);
    uint64_t previous_end = data_offset;
    for (uint32_t i = 0; i < map_count; ++i) {
        const uint8_t *record = records + (uint64_t)i * VP_GUEST_STORAGE_MAP_RECORD_BYTES;
        uint64_t image_offset = rd64(record);
        uint64_t size = rd64(record + 8);
        uint32_t path_offset = rd32(record + 16);
        if (image_offset < previous_end || image_offset > map_image_size
            || size > map_image_size - image_offset
            || path_offset >= map_paths
            || !memchr(paths + path_offset, 0, map_paths - path_offset)) {
            set_error(st, "romfs.map record is inconsistent");
            vp_guest_storage_close(st);
            return -1;
        }
        previous_end = image_offset + size;
    }
    st->file_count = map_count;
    st->path_bytes = map_paths;
    st->tree_bytes = map_tree_bytes;
    st->image_size = map_image_size;
    st->open = 1;
    st->error[0] = 0;
    return 0;
}

void vp_guest_storage_close(VPGuestStorage *st) {
    if (!st) return;
    close_all(st);
    free(st->map);
    st->map = NULL;
    st->map_bytes = 0;
    st->open = 0;
    st->tree_fd_path = UINT32_MAX;
    clear_diagnostic(st);
}

uint64_t vp_guest_storage_size(const VPGuestStorage *st) {
    return st ? st->image_size : 0;
}

/* Binary search for the record containing `offset`. Zero-size records share
 * an offset with the following file, so the last record not greater than the
 * requested offset is the one whose bytes start there. */
static const uint8_t *find_record(const VPGuestStorage *st, uint64_t offset) {
    const uint8_t *records = st->map + VP_GUEST_STORAGE_MAP_HEADER_BYTES;
    uint32_t low = 0, high = st->file_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        uint64_t image_offset = rd64(records + (uint64_t)middle * VP_GUEST_STORAGE_MAP_RECORD_BYTES);
        if (image_offset <= offset) low = middle + 1;
        else high = middle;
    }
    if (!low) return NULL;
    return records + (uint64_t)(low - 1) * VP_GUEST_STORAGE_MAP_RECORD_BYTES;
}

static int open_tree_file(VPGuestStorage *st, uint32_t path_offset, uint64_t expected_size) {
    const char *relative = (const char *)(st->map + VP_GUEST_STORAGE_MAP_HEADER_BYTES
        + (uint64_t)st->file_count * VP_GUEST_STORAGE_MAP_RECORD_BYTES + path_offset);
    size_t relative_len = strlen(relative);

    size_t shown = 0;
    for (; relative[shown] && shown + 1 < sizeof(st->last_path); ++shown)
        st->last_path[shown] = relative[shown];
    st->last_path[shown] = 0;

    if (st->tree_fd >= 0 && st->tree_fd_path == path_offset) {
        clear_diagnostic(st);
        return 0;
    }
    if (st->tree_fd >= 0) close_tree_fd(st);
    clear_diagnostic(st);
    VPGuestStorageDiagnostic *diag = &st->diagnostic;
    diag->path_offset = path_offset;
    diag->fd = -1;
    diag->expected_size = expected_size;
    diag->owner = st->owner;
    diag->cache_generation = st->fd_generation;
    diag->close_seq = st->fd_close_seq;
    diag->retire_seq = st->fd_retire_seq;
    diag->concurrent = st->concurrent_reads != 0;

    size_t root_len = strlen(st->tree_root);
    if (root_len + 1 + relative_len + 1 > VP_GUEST_STORAGE_PATH_LIMIT) {
        diag->operation = VP_GUEST_STORAGE_DIAG_PATH_LIMIT;
        set_error(st, "tree path exceeds the loader path limit");
        return -1;
    }
    char full[VP_GUEST_STORAGE_PATH_LIMIT];
    memcpy(full, st->tree_root, root_len);
    full[root_len] = '/';
    memcpy(full + root_len + 1, relative, relative_len + 1);

    int open_rc = sceIoOpen(full, SCE_O_RDONLY, 0);
    diag->open_attempted = 1;
    diag->open_rc = open_rc;
    if (open_rc < 0) {
        diag->operation = VP_GUEST_STORAGE_DIAG_OPEN;
        set_error(st, "tree file is missing");
        st->tree_fd = -1;
        return -1;
    }
    st->tree_fd = open_rc;
    diag->fd = open_rc;
    /* Count the descriptor as soon as sceIoOpen returned it, before stat. An
     * open that later fails stat/size is still an actual handle that this
     * layer must close, so the ownership counters must include it. */
    ++st->tree_opens;
    ++st->fd_open_seq;
    ++st->fd_generation;
    st->tree_fd_open_seq = st->fd_open_seq;
    st->tree_fd_closes_at_open = st->fd_close_seq;
    st->tree_fd_retires_at_open = st->fd_retire_seq;
    diag->open_seq = st->fd_open_seq;
    diag->cache_generation = st->fd_generation;

    /* Keep the stat call separate from the size comparison: a failed stat and
     * a successful stat reporting a different size are distinct failures. The
     * structure is zeroed before the call so a failing implementation cannot
     * leave an observed value that this code might treat as valid. */
    SceIoStat stat;
    memset(&stat, 0, sizeof(stat));
    int stat_rc = sceIoGetstatByFd(open_rc, &stat);
    diag->stat_attempted = 1;
    diag->stat_rc = stat_rc;
    if (stat_rc < 0) {
        diag->operation = VP_GUEST_STORAGE_DIAG_FD_STAT;
        set_error(st, "tree file fd stat failed");
        diag->close_attempted = 1;
        diag->close_rc = close_tree_fd(st);
        diag->close_seq = st->fd_close_seq;
        diag->cache_generation = st->fd_generation;
        return -1;
    }
    diag->observed_valid = 1;
    diag->observed_size = (uint64_t)stat.st_size;
    if ((uint64_t)stat.st_size != expected_size) {
        diag->operation = VP_GUEST_STORAGE_DIAG_SIZE;
        set_error(st, "tree file size differs from the RomFS map");
        diag->close_attempted = 1;
        diag->close_rc = close_tree_fd(st);
        diag->close_seq = st->fd_close_seq;
        diag->cache_generation = st->fd_generation;
        return -1;
    }
    ++st->file_opens;
    st->tree_fd_path = path_offset;
    clear_diagnostic(st);
    return 0;
}

static int storage_read_locked(VPGuestStorage *st, uint64_t offset,
                               uint32_t address, uint32_t size) {
    if (!st->open) {
        set_error(st, "storage is not open");
        return VP_GUEST_STORAGE_NOT_OPEN;
    }
    st->error[0] = 0;
    st->last_path[0] = 0;
    clear_diagnostic(st);
    if (offset > st->image_size || size > st->image_size - offset) {
        set_error(st, "read is outside the RomFS image");
        ++st->errors;
        return VP_GUEST_STORAGE_RANGE;
    }
    uint8_t *destination = (uint8_t *)(uintptr_t)address;
    uint64_t position = offset;
    uint64_t end = offset + size;
    while (position < end) {
        if (position < st->data_offset) {
            uint64_t chunk = st->data_offset - position;
            if (chunk > end - position) chunk = end - position;
            if (read_exact(st->index_fd, destination, (uint32_t)chunk, position,
                           NULL) != 0) {
                set_error(st, "cannot read romfs.index");
                ++st->errors;
                return VP_GUEST_STORAGE_IO;
            }
            destination += chunk;
            position += chunk;
            continue;
        }
        const uint8_t *record = find_record(st, position);
        if (!record) {
            set_error(st, "read is outside every mapped file");
            ++st->errors;
            return VP_GUEST_STORAGE_RANGE;
        }
        uint64_t image_offset = rd64(record);
        uint64_t record_size = rd64(record + 8);
        uint32_t path_offset = rd32(record + 16);
        if (position < image_offset || position - image_offset >= record_size) {
            set_error(st, "read falls in an alignment gap");
            ++st->errors;
            return VP_GUEST_STORAGE_RANGE;
        }
        uint64_t file_offset = position - image_offset;
        uint64_t chunk = record_size - file_offset;
        if (chunk > end - position) chunk = end - position;
        if (open_tree_file(st, path_offset, record_size) != 0) {
            /* The requested file range is known even though no read was
             * attempted; keep it in the diagnostic for open/stat/size
             * failures without setting read_attempted. */
            st->diagnostic.file_offset = file_offset;
            st->diagnostic.read_length = (uint32_t)chunk;
            ++st->errors;
            return VP_GUEST_STORAGE_BACKING;
        }
        int io_rc = 0;
        if (read_exact(st->tree_fd, destination, (uint32_t)chunk, file_offset,
                       &io_rc) != 0) {
            VPGuestStorageDiagnostic *diag = &st->diagnostic;
            diag->operation = VP_GUEST_STORAGE_DIAG_READ;
            diag->path_offset = path_offset;
            diag->fd = st->tree_fd;
            diag->read_attempted = 1;
            diag->expected_size = record_size;
            diag->file_offset = file_offset;
            diag->read_length = (uint32_t)chunk;
            diag->io_rc = io_rc;
            diag->owner = st->owner;
            diag->cache_generation = st->fd_generation;
            diag->open_seq = st->tree_fd_open_seq;
            diag->close_seq = st->fd_close_seq;
            diag->retire_seq = st->fd_retire_seq;
            diag->closes_since_open =
                st->fd_close_seq - st->tree_fd_closes_at_open;
            diag->retires_since_open =
                st->fd_retire_seq - st->tree_fd_retires_at_open;
            diag->concurrent = st->concurrent_reads != 0;
            /* One probe on the failing descriptor separates "the emulator no
             * longer has this fd" from every other outcome. Only an exact
             * EBADFD proves the handle is absent; a different negative probe
             * result (or a successful one) leaves the ownership question
             * explicitly unknown and keeps the cache entry. The failing
             * request is never retried. */
            diag->fd_probe_attempted = 1;
            SceIoStat probe;
            memset(&probe, 0, sizeof(probe));
            int probe_rc = sceIoGetstatByFd(st->tree_fd, &probe);
            diag->fd_probe_rc = probe_rc;
            if (probe_rc >= 0) {
                diag->fd_probe_valid = 1;
                diag->fd_probe_size = (uint64_t)probe.st_size;
                set_error(st, "tree file read failed");
            } else if (probe_rc == VP_GUEST_STORAGE_EBADFD) {
                diag->fd_probe_ebadfd = 1;
                set_error(st,
                          "tree file read failed; cached descriptor was removed from the emulator fd table");
                diag->retire_attempted = 1;
                retire_tree_fd(st);
                diag->retire_seq = st->fd_retire_seq;
                diag->cache_generation = st->fd_generation;
            } else {
                set_error(st,
                          "tree file read failed; descriptor ownership unknown (probe did not return EBADFD)");
            }
            ++st->errors;
            return VP_GUEST_STORAGE_IO;
        }
        destination += chunk;
        position += chunk;
    }
    clear_diagnostic(st);
    ++st->reads;
    st->bytes += size;
    return VP_GUEST_STORAGE_OK;
}

/* One storage read owns the cache at a time. The caller already serializes
 * guest SVCs, but this local invariant is enforced with an atomic exchange so
 * that a future re-entrant or concurrently released path fails explicitly
 * instead of closing a descriptor another request is using. A refused request
 * does not clear diagnostics or mutate the cache. */
int vp_guest_storage_read(VPGuestStorage *st, uint64_t offset, uint32_t address,
                          uint32_t size) {
    if (!st) return VP_GUEST_STORAGE_NOT_OPEN;
    if (__atomic_exchange_n(&st->active, 1u, __ATOMIC_ACQ_REL) != 0u) {
        __atomic_add_fetch(&st->concurrent_reads, 1u, __ATOMIC_RELAXED);
        return VP_GUEST_STORAGE_BUSY;
    }    int rc = storage_read_locked(st, offset, address, size);
    __atomic_store_n(&st->active, 0u, __ATOMIC_RELEASE);
    return rc;
}

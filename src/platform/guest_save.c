#include "guest_save.h"
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/devctl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Vita-side implementation of the save-data file operations. Every result is
 * an nn::fs result code; the SDK client returns it to the original title
 * code unchanged. Nothing here fabricates an entry: the guest sees exactly
 * the files and directories present below the mount root, and every host
 * failure is reported instead of being hidden behind a success. */

#define SAVE_COPY_CHUNK 0x10000u
#define SAVE_ZERO_CHUNK 0x1000u
#define SAVE_RAW_INITIAL 64u

static FILE *save_log;

typedef struct {
    char name[VP_GUEST_SAVE_NAME_LIMIT];
    uint8_t type;
    uint64_t size;
} SaveRawEntry;

void vp_guest_save_set_log(FILE *log) { save_log = log; }

static void save_note(const char *what, const char *path, uint32_t result) {
    if (!save_log) return;
    fprintf(save_log, "SVC_SAVE_IO op=%s path=%s result=0x%08x\n",
            what, path ? path : "", result);
}

void vp_guest_save_release(VPGuestSaveObject *object) {
    if (!object) return;
    free(object->entries);
    object->entries = NULL;
    object->entry_count = 0;
    object->cursor = 0;
}

static int host_stat(const char *path, SceIoStat *stat) {
    return sceIoGetstat(path, stat);
}

static int host_is_dir(const char *path) {
    SceIoStat stat;
    return host_stat(path, &stat) == 0 && SCE_S_ISDIR(stat.st_mode);
}

static int host_is_file(const char *path) {
    SceIoStat stat;
    return host_stat(path, &stat) == 0 && SCE_S_ISREG(stat.st_mode);
}

/* Returns 1 when the directory has no visible entries, 0 when it has at
 * least one, and -1 when it cannot be read. */
static int host_dir_is_empty(const char *path) {
    SceUID dfd = sceIoDopen(path);
    if (dfd < 0) return -1;
    int empty = 1;
    SceIoDirent entry;
    for (;;) {
        int got = sceIoDread(dfd, &entry);
        if (got < 0) { empty = -1; break; }
        if (!got) break;
        if (!entry.d_name[0] || !strcmp(entry.d_name, ".") ||
            !strcmp(entry.d_name, ".."))
            continue;
        empty = 0;
        break;
    }
    sceIoDclose(dfd);
    return empty;
}

static int host_join(char *out, size_t capacity, const char *base,
                     const char *name) {
    size_t base_len = strlen(base);
    size_t name_len = strlen(name);
    if (base_len + 1 + name_len + 1 > capacity) return -1;
    memcpy(out, base, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1, name, name_len + 1);
    return 0;
}

/* Creates every component of an absolute Vita path below its device. The
 * device prefix ("ux0:") is never passed to sceIoMkdir. */
static int host_mkdir_p(const char *path) {
    char work[VP_GUEST_SAVE_PATH_LIMIT];
    size_t length = strlen(path);
    if (length + 1 > sizeof(work)) return -1;
    memcpy(work, path, length + 1);
    size_t start = 0;
    while (start < length && work[start] != ':') ++start;
    if (start < length) ++start;
    for (size_t i = start; i <= length; ++i) {
        if (work[i] != '/' && work[i] != 0) continue;
        int last = work[i] == 0;
        work[i] = 0;
        if (work[start] && !host_is_dir(work)) {
            if (sceIoMkdir(work, 0777) < 0 && !host_is_dir(work)) return -1;
        }
        if (!last) work[i] = '/';
    }
    return host_is_dir(path) ? 0 : -1;
}

static uint32_t resolve_path(const char *root, const char *guest, char *out,
                             size_t capacity) {
    if (!guest) return VP_GUEST_SAVE_NULL_ARGUMENT;
    size_t length = strnlen(guest, 0x301u);
    if (length > 0x2ffu) return VP_GUEST_SAVE_TOO_LONG_PATH;
    size_t root_len = strlen(root);
    if (root_len + 1 > capacity) return VP_GUEST_SAVE_TOO_LONG_PATH;
    memcpy(out, root, root_len);
    size_t used = root_len;
    size_t i = 0;
    while (i < length) {
        while (i < length && (guest[i] == '/' || guest[i] == '\\')) ++i;
        size_t start = i;
        while (i < length && guest[i] != '/' && guest[i] != '\\') {
            if (guest[i] == ':') return VP_GUEST_SAVE_INVALID_PATH;
            ++i;
        }
        size_t part = i - start;
        if (!part) continue;
        if (part == 1 && guest[start] == '.') continue;
        if (part == 2 && guest[start] == '.' && guest[start + 1] == '.')
            return VP_GUEST_SAVE_INVALID_PATH;
        /* The guest allows file names up to 0x300 bytes; the Vita filesystem
         * exposes 255-byte names. Rejecting is the only non-lossy answer. */
        if (part > 0xffu) return VP_GUEST_SAVE_TOO_LONG_PATH;
        if (used + 1 + part + 1 > capacity)
            return VP_GUEST_SAVE_TOO_LONG_PATH;
        out[used++] = '/';
        memcpy(out + used, guest + start, part);
        used += part;
    }
    out[used] = 0;
    return VP_GUEST_SAVE_OK;
}

/* Appends 32 lowercase hex digits without depending on printf width
 * modifiers the Vita libc might truncate. */
static void hex32(char *out, const uint8_t bytes[16]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0xfu];
    }
    out[32] = '\0';
}

uint32_t vp_guest_save_mount(VPGuestSaveObject *object, uint64_t program,
                             const uint8_t uid[16], uint32_t type,
                             uint32_t rank, uint32_t index) {
    if (!object) return VP_GUEST_SAVE_NULL_ARGUMENT;
    memset(object, 0, sizeof(*object));
    char uid_hex[33];
    hex32(uid_hex, uid);
    char name[96];
    int written = snprintf(name, sizeof(name),
                           "%016llx_%s_t%u_r%u_i%u",
                           (unsigned long long)program, uid_hex, type, rank, index);
    if (written <= 0 || (size_t)written >= sizeof(name))
        return VP_GUEST_SAVE_TOO_LONG_PATH;
    int root_written = snprintf(object->path, sizeof(object->path), "%s/%s",
                                VP_GUEST_SAVE_ROOT_PATH, name);
    if (root_written <= 0 || (size_t)root_written >= sizeof(object->path))
        return VP_GUEST_SAVE_TOO_LONG_PATH;
    if (host_mkdir_p(object->path) != 0) {
        save_note("mount", object->path, VP_GUEST_SAVE_IO_ERROR);
        object->path[0] = 0;
        return VP_GUEST_SAVE_IO_ERROR;
    }
    return VP_GUEST_SAVE_OK;
}

uint32_t vp_guest_save_entry_type(const VPGuestSaveObject *mount,
                                  const char *guest_path, uint32_t *type) {
    if (!mount || !mount->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (!type) return VP_GUEST_SAVE_NULL_ARGUMENT;
    char host[VP_GUEST_SAVE_PATH_LIMIT];
    uint32_t result = resolve_path(mount->path, guest_path, host, sizeof(host));
    if (result) return result;
    SceIoStat stat;
    if (host_stat(host, &stat) < 0) return VP_GUEST_SAVE_PATH_NOT_FOUND;
    if (SCE_S_ISDIR(stat.st_mode)) { *type = 0; return VP_GUEST_SAVE_OK; }
    if (SCE_S_ISREG(stat.st_mode)) { *type = 1; return VP_GUEST_SAVE_OK; }
    return VP_GUEST_SAVE_PATH_NOT_FOUND;
}

static uint32_t host_extend_file(const char *host, uint64_t size) {
    if (!size) return VP_GUEST_SAVE_OK;
    /* exFAT extends a file when it is seeked past the end and written. The
     * size is verified afterwards; if the device does not implement that,
     * zeros are written explicitly rather than leaving a wrong size. */
    SceUID fd = sceIoOpen(host, SCE_O_WRONLY, 0);
    if (fd < 0) return VP_GUEST_SAVE_IO_ERROR;
    int extended = 0;
    if (sceIoLseek(fd, (SceOff)(size - 1), SCE_SEEK_SET) == (SceOff)(size - 1)) {
        char zero = 0;
        extended = sceIoWrite(fd, &zero, 1) == 1;
    }
    sceIoClose(fd);
    if (extended) {
        SceIoStat stat;
        if (host_stat(host, &stat) == 0 && (uint64_t)stat.st_size == size)
            return VP_GUEST_SAVE_OK;
    }
    fd = sceIoOpen(host, SCE_O_WRONLY | SCE_O_TRUNC, 0);
    if (fd < 0) return VP_GUEST_SAVE_IO_ERROR;
    uint8_t zeros[SAVE_ZERO_CHUNK];
    memset(zeros, 0, sizeof(zeros));
    uint64_t left = size;
    while (left) {
        uint32_t chunk = left > sizeof(zeros) ? (uint32_t)sizeof(zeros)
                                              : (uint32_t)left;
        if (sceIoWrite(fd, zeros, chunk) != (int)chunk) {
            sceIoClose(fd);
            return VP_GUEST_SAVE_IO_ERROR;
        }
        left -= chunk;
    }
    return sceIoClose(fd) < 0 ? VP_GUEST_SAVE_IO_ERROR : VP_GUEST_SAVE_OK;
}

static uint32_t host_shrink_file(const char *host, uint64_t size) {
    char temporary[VP_GUEST_SAVE_PATH_LIMIT];
    if (strlen(host) + 7 > sizeof(temporary)) return VP_GUEST_SAVE_TOO_LONG_PATH;
    strcpy(temporary, host);
    strcat(temporary, ".vptmp");
    sceIoRemove(temporary);
    SceUID source = sceIoOpen(host, SCE_O_RDONLY, 0);
    if (source < 0) return VP_GUEST_SAVE_IO_ERROR;
    SceUID target = sceIoOpen(temporary, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                              0666);
    if (target < 0) {
        sceIoClose(source);
        return VP_GUEST_SAVE_IO_ERROR;
    }
    uint8_t buffer[SAVE_COPY_CHUNK];
    uint64_t left = size;
    int failed = 0;
    while (left) {
        uint32_t chunk = left > sizeof(buffer) ? (uint32_t)sizeof(buffer)
                                               : (uint32_t)left;
        int got = sceIoRead(source, buffer, chunk);
        if (got <= 0 || sceIoWrite(target, buffer, (uint32_t)got) != got) {
            failed = 1;
            break;
        }
        left -= (uint32_t)got;
    }
    if (sceIoClose(source) < 0) failed = 1;
    if (sceIoClose(target) < 0) failed = 1;
    if (!failed) {
        /* The replacement is complete before the old file is removed. If the
         * rename fails the old tree is left untouched except for the temp
         * file, which is removed. */
        if (sceIoRemove(host) < 0) failed = 1;
        else if (sceIoRename(temporary, host) < 0) failed = 1;
    }
    if (failed) {
        sceIoRemove(temporary);
        save_note("shrink", host, VP_GUEST_SAVE_IO_ERROR);
        return VP_GUEST_SAVE_IO_ERROR;
    }
    return VP_GUEST_SAVE_OK;
}

static uint32_t host_set_file_size(const char *host, uint64_t size) {
    SceIoStat stat;
    if (host_stat(host, &stat) < 0) return VP_GUEST_SAVE_PATH_NOT_FOUND;
    uint64_t old = (uint64_t)stat.st_size;
    if (size == old) return VP_GUEST_SAVE_OK;
    if (size > old) return host_extend_file(host, size);
    return host_shrink_file(host, size);
}

uint32_t vp_guest_save_create_file(const VPGuestSaveObject *mount,
                                   const char *guest_path, int64_t size) {
    if (!mount || !mount->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (size < 0) return VP_GUEST_SAVE_OUT_OF_RANGE;
    char host[VP_GUEST_SAVE_PATH_LIMIT];
    uint32_t result = resolve_path(mount->path, guest_path, host, sizeof(host));
    if (result) return result;
    char parent[VP_GUEST_SAVE_PATH_LIMIT];
    size_t host_len = strlen(host);
    if (host_len + 1 > sizeof(parent)) return VP_GUEST_SAVE_TOO_LONG_PATH;
    memcpy(parent, host, host_len + 1);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return VP_GUEST_SAVE_INVALID_PATH;
    *slash = 0;
    if (!host_is_dir(parent)) return VP_GUEST_SAVE_PATH_NOT_FOUND;
    SceIoStat existing;
    if (host_stat(host, &existing) == 0)
        return VP_GUEST_SAVE_PATH_ALREADY_EXISTS;
    SceUID fd = sceIoOpen(host, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        save_note("create", host, VP_GUEST_SAVE_IO_ERROR);
        return VP_GUEST_SAVE_IO_ERROR;
    }
    if (sceIoClose(fd) < 0) return VP_GUEST_SAVE_IO_ERROR;
    return host_set_file_size(host, (uint64_t)size);
}

uint32_t vp_guest_save_delete_file(const VPGuestSaveObject *mount,
                                   const char *guest_path) {
    if (!mount || !mount->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    char host[VP_GUEST_SAVE_PATH_LIMIT];
    uint32_t result = resolve_path(mount->path, guest_path, host, sizeof(host));
    if (result) return result;
    if (!host_is_file(host)) return VP_GUEST_SAVE_PATH_NOT_FOUND;
    if (sceIoRemove(host) < 0) {
        save_note("delete-file", host, VP_GUEST_SAVE_IO_ERROR);
        return VP_GUEST_SAVE_IO_ERROR;
    }
    return VP_GUEST_SAVE_OK;
}

uint32_t vp_guest_save_create_directory(const VPGuestSaveObject *mount,
                                        const char *guest_path) {
    if (!mount || !mount->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    char host[VP_GUEST_SAVE_PATH_LIMIT];
    uint32_t result = resolve_path(mount->path, guest_path, host, sizeof(host));
    if (result) return result;
    SceIoStat stat;
    if (host_stat(host, &stat) == 0) {
        if (SCE_S_ISDIR(stat.st_mode)) return VP_GUEST_SAVE_OK;
        return VP_GUEST_SAVE_PATH_ALREADY_EXISTS;
    }
    if (host_mkdir_p(host) != 0) {
        save_note("mkdir", host, VP_GUEST_SAVE_IO_ERROR);
        return VP_GUEST_SAVE_IO_ERROR;
    }
    return VP_GUEST_SAVE_OK;
}

static int host_remove_tree(const char *host) {
    SceUID dfd = sceIoDopen(host);
    if (dfd >= 0) {
        SceIoDirent entry;
        for (;;) {
            int got = sceIoDread(dfd, &entry);
            if (got <= 0) {
                if (got < 0) { sceIoDclose(dfd); return -1; }
                break;
            }
            if (!entry.d_name[0] || !strcmp(entry.d_name, ".") ||
                !strcmp(entry.d_name, ".."))
                continue;
            char child[VP_GUEST_SAVE_PATH_LIMIT];
            if (host_join(child, sizeof(child), host, entry.d_name) != 0) {
                sceIoDclose(dfd);
                return -1;
            }
            if (SCE_S_ISDIR(entry.d_stat.st_mode)) {
                if (host_remove_tree(child) != 0) { sceIoDclose(dfd); return -1; }
            } else if (sceIoRemove(child) < 0) {
                sceIoDclose(dfd);
                return -1;
            }
        }
        sceIoDclose(dfd);
    }
    return sceIoRmdir(host) == 0 ? 0 : -1;
}

static int host_clean_directory(const char *host) {
    SceUID dfd = sceIoDopen(host);
    if (dfd < 0) return -1;
    SceIoDirent entry;
    int failed = 0;
    for (;;) {
        int got = sceIoDread(dfd, &entry);
        if (got <= 0) {
            if (got < 0) failed = 1;
            break;
        }
        if (!entry.d_name[0] || !strcmp(entry.d_name, ".") ||
            !strcmp(entry.d_name, ".."))
            continue;
        char child[VP_GUEST_SAVE_PATH_LIMIT];
        if (host_join(child, sizeof(child), host, entry.d_name) != 0) {
            failed = 1;
            break;
        }
        int status = SCE_S_ISDIR(entry.d_stat.st_mode)
                         ? host_remove_tree(child) : sceIoRemove(child);
        if (status != 0) { failed = 1; break; }
    }
    sceIoDclose(dfd);
    return failed ? -1 : 0;
}

uint32_t vp_guest_save_delete_directory(const VPGuestSaveObject *mount,
                                        const char *guest_path, int recursive) {
    if (!mount || !mount->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    char host[VP_GUEST_SAVE_PATH_LIMIT];
    uint32_t result = resolve_path(mount->path, guest_path, host, sizeof(host));
    if (result) return result;
    if (!strcmp(host, mount->path)) return VP_GUEST_SAVE_INVALID_PATH;
    if (!host_is_dir(host)) return VP_GUEST_SAVE_PATH_NOT_FOUND;
    if (!recursive) {
        int empty = host_dir_is_empty(host);
        if (empty < 0) return VP_GUEST_SAVE_IO_ERROR;
        if (!empty) return VP_GUEST_SAVE_DIRECTORY_NOT_EMPTY;
    }
    int status = recursive ? host_remove_tree(host) : sceIoRmdir(host);
    if (status != 0) {
        save_note(recursive ? "delete-tree" : "delete-dir", host,
                  VP_GUEST_SAVE_IO_ERROR);
        return VP_GUEST_SAVE_IO_ERROR;
    }
    return VP_GUEST_SAVE_OK;
}

uint32_t vp_guest_save_clean_directory(const VPGuestSaveObject *mount,
                                       const char *guest_path) {
    if (!mount || !mount->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    char host[VP_GUEST_SAVE_PATH_LIMIT];
    uint32_t result = resolve_path(mount->path, guest_path, host, sizeof(host));
    if (result) return result;
    if (!host_is_dir(host)) return VP_GUEST_SAVE_PATH_NOT_FOUND;
    if (host_clean_directory(host) != 0) {
        save_note("clean-dir", host, VP_GUEST_SAVE_IO_ERROR);
        return VP_GUEST_SAVE_IO_ERROR;
    }
    return VP_GUEST_SAVE_OK;
}

uint32_t vp_guest_save_rename(const VPGuestSaveObject *mount,
                              const char *from_path, const char *to_path,
                              int directory) {
    if (!mount || !mount->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    char from[VP_GUEST_SAVE_PATH_LIMIT];
    char to[VP_GUEST_SAVE_PATH_LIMIT];
    uint32_t result = resolve_path(mount->path, from_path, from, sizeof(from));
    if (result) return result;
    result = resolve_path(mount->path, to_path, to, sizeof(to));
    if (result) return result;
    if (!strcmp(to, mount->path)) return VP_GUEST_SAVE_INVALID_PATH;
    if (directory ? !host_is_dir(from) : !host_is_file(from))
        return VP_GUEST_SAVE_PATH_NOT_FOUND;
    SceIoStat existing;
    if (host_stat(to, &existing) == 0)
        return VP_GUEST_SAVE_PATH_ALREADY_EXISTS;
    char parent[VP_GUEST_SAVE_PATH_LIMIT];
    size_t to_len = strlen(to);
    if (to_len + 1 > sizeof(parent)) return VP_GUEST_SAVE_TOO_LONG_PATH;
    memcpy(parent, to, to_len + 1);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return VP_GUEST_SAVE_INVALID_PATH;
    *slash = 0;
    if (!host_is_dir(parent)) return VP_GUEST_SAVE_PATH_NOT_FOUND;
    if (sceIoRename(from, to) < 0) {
        save_note("rename", from, VP_GUEST_SAVE_IO_ERROR);
        return VP_GUEST_SAVE_IO_ERROR;
    }
    return VP_GUEST_SAVE_OK;
}

uint32_t vp_guest_save_open_file(const VPGuestSaveObject *mount,
                                 const char *guest_path, uint32_t mode,
                                 VPGuestSaveObject *file) {
    if (!mount || !mount->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (!file) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (!(mode & 3u) || (mode & ~7u)) return VP_GUEST_SAVE_INVALID_MODE;
    char host[VP_GUEST_SAVE_PATH_LIMIT];
    uint32_t result = resolve_path(mount->path, guest_path, host, sizeof(host));
    if (result) return result;
    if (!host_is_file(host)) return VP_GUEST_SAVE_PATH_NOT_FOUND;
    memset(file, 0, sizeof(*file));
    memcpy(file->path, host, strlen(host) + 1);
    file->mode = mode;
    return VP_GUEST_SAVE_OK;
}

uint32_t vp_guest_save_read_file(const VPGuestSaveObject *file, int64_t offset,
                                 void *destination, uint32_t size,
                                 uint64_t *returned) {
    if (!file || !file->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (!returned) return VP_GUEST_SAVE_NULL_ARGUMENT;
    *returned = 0;
    if (size && !destination) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (offset < 0 || (uint64_t)size > (uint64_t)INT64_MAX - (uint64_t)offset)
        return VP_GUEST_SAVE_OUT_OF_RANGE;
    if (!size) return VP_GUEST_SAVE_OK;
    if (!(file->mode & 1u)) return VP_GUEST_SAVE_READ_NOT_PERMITTED;
    SceUID fd = sceIoOpen(file->path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        save_note("read-open", file->path, VP_GUEST_SAVE_IO_ERROR);
        return VP_GUEST_SAVE_IO_ERROR;
    }
    uint8_t *cursor = (uint8_t *)destination;
    uint64_t done = 0;
    uint32_t result = VP_GUEST_SAVE_OK;
    while (done < size) {
        int got = sceIoPread(fd, cursor + done, size - (uint32_t)done,
                             (SceOff)((uint64_t)offset + done));
        if (got < 0) {
            save_note("read", file->path, VP_GUEST_SAVE_IO_ERROR);
            result = VP_GUEST_SAVE_IO_ERROR;
            break;
        }
        if (!got) break;
        done += (uint32_t)got;
    }
    sceIoClose(fd);
    if (result == VP_GUEST_SAVE_OK) *returned = done;
    return result;
}

uint32_t vp_guest_save_write_file(const VPGuestSaveObject *file, int64_t offset,
                                  const void *source, uint32_t size) {
    if (!file || !file->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (size && !source) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (offset < 0 || (uint64_t)size > (uint64_t)INT64_MAX - (uint64_t)offset)
        return VP_GUEST_SAVE_OUT_OF_RANGE;
    if (!size) return VP_GUEST_SAVE_OK;
    if (!(file->mode & 2u)) return VP_GUEST_SAVE_WRITE_NOT_PERMITTED;
    SceUID fd = sceIoOpen(file->path, SCE_O_WRONLY, 0);
    if (fd < 0) {
        save_note("write-open", file->path, VP_GUEST_SAVE_IO_ERROR);
        return VP_GUEST_SAVE_IO_ERROR;
    }
    const uint8_t *cursor = (const uint8_t *)source;
    uint64_t done = 0;
    uint32_t result = VP_GUEST_SAVE_OK;
    while (done < size) {
        int put = sceIoPwrite(fd, cursor + done, size - (uint32_t)done,
                              (SceOff)((uint64_t)offset + done));
        if (put <= 0) {
            save_note("write", file->path, VP_GUEST_SAVE_IO_ERROR);
            result = VP_GUEST_SAVE_IO_ERROR;
            break;
        }
        done += (uint32_t)put;
    }
    if (sceIoClose(fd) < 0 && result == VP_GUEST_SAVE_OK)
        result = VP_GUEST_SAVE_IO_ERROR;
    return result;
}

uint32_t vp_guest_save_set_file_size(const VPGuestSaveObject *file,
                                     int64_t size) {
    if (!file || !file->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (size < 0) return VP_GUEST_SAVE_OUT_OF_RANGE;
    if (!(file->mode & 2u)) return VP_GUEST_SAVE_WRITE_NOT_PERMITTED;
    return host_set_file_size(file->path, (uint64_t)size);
}

uint32_t vp_guest_save_file_size(const VPGuestSaveObject *file, uint64_t *size) {
    if (!file || !file->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (!size) return VP_GUEST_SAVE_NULL_ARGUMENT;
    SceIoStat stat;
    if (host_stat(file->path, &stat) < 0) return VP_GUEST_SAVE_PATH_NOT_FOUND;
    *size = (uint64_t)stat.st_size;
    return VP_GUEST_SAVE_OK;
}

uint32_t vp_guest_save_flush_file(const VPGuestSaveObject *file) {
    if (!file || !file->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    /* Each write opens, writes and closes the host file, so no buffered
     * data survives the request that produced it. */
    return VP_GUEST_SAVE_OK;
}

static uint64_t sce_time_to_unix(const SceDateTime *value) {
    if (!value->year) return 0;
    struct tm broken;
    memset(&broken, 0, sizeof(broken));
    broken.tm_year = (int)value->year - 1900;
    broken.tm_mon = (int)value->month - 1;
    broken.tm_mday = (int)value->day;
    broken.tm_hour = (int)value->hour;
    broken.tm_min = (int)value->minute;
    broken.tm_sec = (int)value->second;
    broken.tm_isdst = -1;
    time_t seconds = mktime(&broken);
    return seconds < 0 ? 0 : (uint64_t)seconds;
}

uint32_t vp_guest_save_file_timestamp(const VPGuestSaveObject *mount,
                                      const char *guest_path,
                                      uint64_t timestamp[3]) {
    if (!mount || !mount->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (!timestamp) return VP_GUEST_SAVE_NULL_ARGUMENT;
    char host[VP_GUEST_SAVE_PATH_LIMIT];
    uint32_t result = resolve_path(mount->path, guest_path, host, sizeof(host));
    if (result) return result;
    SceIoStat stat;
    if (host_stat(host, &stat) < 0) return VP_GUEST_SAVE_PATH_NOT_FOUND;
    timestamp[0] = sce_time_to_unix(&stat.st_ctime);
    timestamp[1] = sce_time_to_unix(&stat.st_atime);
    timestamp[2] = sce_time_to_unix(&stat.st_mtime);
    return VP_GUEST_SAVE_OK;
}

uint32_t vp_guest_save_list_directory(const VPGuestSaveObject *mount,
                                      const char *guest_path, uint32_t mode,
                                      VPGuestSaveObject *directory) {
    if (!mount || !mount->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (!directory) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (!(mode & 3u) || (mode & ~0x80000003u))
        return VP_GUEST_SAVE_INVALID_MODE;
    char host[VP_GUEST_SAVE_PATH_LIMIT];
    uint32_t result = resolve_path(mount->path, guest_path, host, sizeof(host));
    if (result) return result;
    if (!host_is_dir(host)) return VP_GUEST_SAVE_PATH_NOT_FOUND;
    SceUID dfd = sceIoDopen(host);
    if (dfd < 0) {
        save_note("opendir", host, VP_GUEST_SAVE_IO_ERROR);
        return VP_GUEST_SAVE_IO_ERROR;
    }
    uint32_t capacity = SAVE_RAW_INITIAL;
    uint32_t count = 0;
    SaveRawEntry *raw = (SaveRawEntry *)malloc(capacity * sizeof(*raw));
    if (!raw) { sceIoDclose(dfd); return VP_GUEST_SAVE_IO_ERROR; }
    SceIoDirent entry;
    for (;;) {
        int got = sceIoDread(dfd, &entry);
        if (got < 0) { result = VP_GUEST_SAVE_IO_ERROR; goto done; }
        if (!got) break;
        if (!entry.d_name[0] || !strcmp(entry.d_name, ".") ||
            !strcmp(entry.d_name, ".."))
            continue;
        int is_dir = SCE_S_ISDIR(entry.d_stat.st_mode);
        int is_file = SCE_S_ISREG(entry.d_stat.st_mode);
        if (!is_dir && !is_file) continue;
        if (is_dir && !(mode & 1u)) continue;
        if (is_file && !(mode & 2u)) continue;
        size_t name_len = strnlen(entry.d_name, sizeof(entry.d_name));
        if (name_len >= sizeof(entry.d_name)) {
            result = VP_GUEST_SAVE_TOO_LONG_PATH;
            goto done;
        }
        if (count == VP_GUEST_SAVE_DIRECTORY_LIMIT) {
            result = VP_GUEST_SAVE_OUT_OF_RANGE;
            goto done;
        }
        if (count == capacity) {
            uint32_t grown = capacity * 2u;
            if (grown > VP_GUEST_SAVE_DIRECTORY_LIMIT)
                grown = VP_GUEST_SAVE_DIRECTORY_LIMIT;
            SaveRawEntry *larger = (SaveRawEntry *)realloc(raw, grown * sizeof(*raw));
            if (!larger) { result = VP_GUEST_SAVE_IO_ERROR; goto done; }
            raw = larger;
            capacity = grown;
        }
        memset(&raw[count], 0, sizeof(raw[count]));
        memcpy(raw[count].name, entry.d_name, name_len + 1);
        raw[count].type = (uint8_t)(is_dir ? 0 : 1);
        raw[count].size = is_file && entry.d_stat.st_size > 0
                              ? (uint64_t)entry.d_stat.st_size : 0;
        ++count;
    }
    sceIoDclose(dfd);
    dfd = -1;
    {
        uint32_t total = count;
        uint8_t *entries = total ? (uint8_t *)calloc(total, VP_GUEST_SAVE_ENTRY_BYTES)
                                 : NULL;
        if (total && !entries) { result = VP_GUEST_SAVE_IO_ERROR; goto done; }
        uint32_t at = 0;
        for (unsigned pass = 0; pass < 2; ++pass) {
            for (uint32_t i = 0; i < count; ++i) {
                if (raw[i].type != (uint8_t)pass) continue;
                uint8_t *slot = entries + (uint64_t)at * VP_GUEST_SAVE_ENTRY_BYTES;
                memcpy(slot, raw[i].name, strnlen(raw[i].name, 0x300u));
                slot[0x304] = raw[i].type;
                for (unsigned b = 0; b < 8; ++b)
                    slot[0x308 + b] = (uint8_t)(raw[i].size >> (8u * b));
                ++at;
            }
        }
        free(directory->entries);
        directory->entries = entries;
        directory->entry_count = at;
        directory->cursor = 0;
        directory->mode = mode;
    }
    result = VP_GUEST_SAVE_OK;
done:
    if (dfd >= 0) sceIoDclose(dfd);
    free(raw);
    if (result) save_note("listdir", host, result);
    return result;
}

uint32_t vp_guest_save_directory_read(VPGuestSaveObject *directory,
                                      void *destination, uint32_t capacity,
                                      uint64_t *count) {
    if (!directory || !count) return VP_GUEST_SAVE_NULL_ARGUMENT;
    *count = 0;
    if (directory->cursor > directory->entry_count)
        return VP_GUEST_SAVE_INVALID_ARGUMENT;
    uint32_t fits = capacity / VP_GUEST_SAVE_ENTRY_BYTES;
    uint32_t remaining = directory->entry_count - directory->cursor;
    uint32_t take = fits < remaining ? fits : remaining;
    if (take && !destination) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (take)
        memcpy(destination,
               directory->entries + (uint64_t)directory->cursor * VP_GUEST_SAVE_ENTRY_BYTES,
               (size_t)take * VP_GUEST_SAVE_ENTRY_BYTES);
    directory->cursor += take;
    *count = take;
    return VP_GUEST_SAVE_OK;
}

uint32_t vp_guest_save_directory_count(const VPGuestSaveObject *directory,
                                       uint64_t *count) {
    if (!directory || !count) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (directory->cursor > directory->entry_count)
        return VP_GUEST_SAVE_INVALID_ARGUMENT;
    *count = directory->entry_count - directory->cursor;
    return VP_GUEST_SAVE_OK;
}

uint32_t vp_guest_save_free_space(const VPGuestSaveObject *mount,
                                  uint64_t *free_bytes, uint64_t *total_bytes) {
    if (!mount || !mount->path[0]) return VP_GUEST_SAVE_NULL_ARGUMENT;
    if (!free_bytes || !total_bytes) return VP_GUEST_SAVE_NULL_ARGUMENT;
    SceIoDevInfo info;
    memset(&info, 0, sizeof(info));
    if (sceIoDevctl("ux0:", 0x3001, NULL, 0, &info, sizeof(info)) < 0) {
        save_note("devctl-space", mount->path, VP_GUEST_SAVE_NOT_IMPLEMENTED);
        return VP_GUEST_SAVE_NOT_IMPLEMENTED;
    }
    *free_bytes = info.free_size > 0 ? (uint64_t)info.free_size : 0;
    *total_bytes = info.max_size > 0 ? (uint64_t)info.max_size : 0;
    return VP_GUEST_SAVE_OK;
}

#ifdef VP_SAVE_SELFTEST
/* Device-side verification of the save backend. It exercises the same public
 * entry points the title's fsp-srv commands call, below a dedicated root that
 * cannot collide with a real save. The build option is off by default; this
 * exists so the file operations are proven on the Vita filesystem instead of
 * only compiled. */
static int save_selftest_check(FILE *log, const char *name, int ok,
                               unsigned value) {
    fprintf(log, "SAVE_SELFTEST check=%s ok=%d value=0x%x\n", name, ok ? 1 : 0,
            value);
    fflush(log);
    return ok ? 0 : 1;
}

int vp_guest_save_selftest(FILE *log) {
    if (!log) return -1;
    int failures = 0, checks = 0;
    static const uint8_t uid[16] = {
        0xa5, 0xa4, 0xa3, 0xa2, 0xa1, 0xa0, 0x9f, 0x9e,
        0x9d, 0x9c, 0x9b, 0x9a, 0x99, 0x98, 0x97, 0x96,
    };
    VPGuestSaveObject mount;
    memset(&mount, 0, sizeof(mount));
    uint32_t result = vp_guest_save_mount(&mount, 0x5650544553540001ull, uid,
                                          0xf0, 0xf1, 0xf2f3);
    ++checks;
    failures += save_selftest_check(log, "mount", result == 0, result);
    uint32_t type = 0xffffffffu;
    result = vp_guest_save_entry_type(&mount, "/", &type);
    ++checks;
    failures += save_selftest_check(log, "root-is-directory",
                                    result == 0 && type == 0, result);

    result = vp_guest_save_create_directory(&mount, "/dir");
    ++checks;
    failures += save_selftest_check(log, "create-directory", result == 0,
                                    result);
    result = vp_guest_save_create_file(&mount, "/dir/a.bin", 0x1234);
    ++checks;
    failures += save_selftest_check(log, "create-file-size", result == 0,
                                    result);
    type = 0xffffffffu;
    result = vp_guest_save_entry_type(&mount, "/dir/a.bin", &type);
    ++checks;
    failures += save_selftest_check(log, "file-entry-type",
                                    result == 0 && type == 1, result);

    VPGuestSaveObject file;
    memset(&file, 0, sizeof(file));
    result = vp_guest_save_open_file(&mount, "/dir/a.bin", 3, &file);
    ++checks;
    failures += save_selftest_check(log, "open-readwrite", result == 0, result);
    uint64_t size = 0;
    result = vp_guest_save_file_size(&file, &size);
    ++checks;
    failures += save_selftest_check(log, "file-size",
                                    result == 0 && size == 0x1234,
                                    (unsigned)size);

    uint8_t pattern[32], readback[32];
    for (unsigned i = 0; i < sizeof(pattern); ++i)
        pattern[i] = (uint8_t)(i * 7 + 3);
    result = vp_guest_save_write_file(&file, 0x100, pattern, sizeof(pattern));
    ++checks;
    failures += save_selftest_check(log, "write-at-offset", result == 0,
                                    result);
    memset(readback, 0, sizeof(readback));
    uint64_t returned = 0;
    result = vp_guest_save_read_file(&file, 0x100, readback, sizeof(readback),
                                     &returned);
    ++checks;
    failures += save_selftest_check(log, "read-back",
                                    result == 0 && returned == sizeof(pattern)
                                        && !memcmp(pattern, readback, sizeof(pattern)),
                                    (unsigned)returned);
    uint8_t tail[0x100];
    returned = 0;
    result = vp_guest_save_read_file(&file, 0x1200, tail, sizeof(tail),
                                     &returned);
    ++checks;
    failures += save_selftest_check(log, "short-read-at-eof",
                                    result == 0 && returned == 0x34,
                                    (unsigned)returned);
    result = vp_guest_save_set_file_size(&file, 0x800);
    ++checks;
    failures += save_selftest_check(log, "shrink-file", result == 0, result);
    result = vp_guest_save_file_size(&file, &size);
    ++checks;
    failures += save_selftest_check(log, "size-after-shrink",
                                    result == 0 && size == 0x800,
                                    (unsigned)size);
    vp_guest_save_release(&file);

    VPGuestSaveObject readonly;
    memset(&readonly, 0, sizeof(readonly));
    result = vp_guest_save_open_file(&mount, "/dir/a.bin", 1, &readonly);
    ++checks;
    failures += save_selftest_check(log, "open-readonly", result == 0, result);
    result = vp_guest_save_write_file(&readonly, 0, pattern, sizeof(pattern));
    ++checks;
    failures += save_selftest_check(log, "write-denied",
                                    result == VP_GUEST_SAVE_WRITE_NOT_PERMITTED,
                                    result);
    returned = 0;
    result = vp_guest_save_read_file(&readonly, 0, readback, 4, &returned);
    ++checks;
    failures += save_selftest_check(log, "read-allowed",
                                    result == 0 && returned == 4, (unsigned)returned);
    vp_guest_save_release(&readonly);

    result = vp_guest_save_create_file(&mount, "/dir/a.bin", 0);
    ++checks;
    failures += save_selftest_check(log, "create-existing-rejected",
                                    result == VP_GUEST_SAVE_PATH_ALREADY_EXISTS,
                                    result);
    result = vp_guest_save_create_directory(&mount, "/dir/dir2");
    ++checks;
    failures += save_selftest_check(log, "create-subdirectory", result == 0,
                                    result);
    result = vp_guest_save_create_file(&mount, "/dir/b.bin", 0);
    ++checks;
    failures += save_selftest_check(log, "create-empty-file", result == 0,
                                    result);
    result = vp_guest_save_rename(&mount, "/dir/b.bin", "/dir/c.bin", 0);
    ++checks;
    failures += save_selftest_check(log, "rename-file", result == 0, result);
    type = 0xffffffffu;
    result = vp_guest_save_entry_type(&mount, "/dir/b.bin", &type);
    ++checks;
    failures += save_selftest_check(log, "renamed-source-gone",
                                    result == VP_GUEST_SAVE_PATH_NOT_FOUND,
                                    result);

    VPGuestSaveObject dir;
    memset(&dir, 0, sizeof(dir));
    result = vp_guest_save_list_directory(&mount, "/dir", 3, &dir);
    ++checks;
    failures += save_selftest_check(log, "list-directory",
                                    result == 0 && dir.entry_count == 3,
                                    dir.entry_count);
    uint8_t entries[3 * VP_GUEST_SAVE_ENTRY_BYTES];
    memset(entries, 0, sizeof(entries));
    uint64_t count = 0;
    result = vp_guest_save_directory_read(&dir, entries, sizeof(entries),
                                          &count);
    int files_ok = 0;
    if (result == 0 && count == 3) {
        files_ok = 1;
        for (unsigned i = 1; i < 3; ++i) {
            const uint8_t *slot = entries + i * VP_GUEST_SAVE_ENTRY_BYTES;
            uint64_t size = 0;
            for (unsigned b = 0; b < 8; ++b)
                size |= (uint64_t)slot[0x308 + b] << (8u * b);
            if (slot[0x304] != 1) { files_ok = 0; continue; }
            if (!memcmp(slot, "a.bin", 6) && size == 0x800) continue;
            if (!memcmp(slot, "c.bin", 6) && size == 0) continue;
            files_ok = 0;
        }
    }
    ++checks;
    failures += save_selftest_check(
        log, "directory-order",
        result == 0 && count == 3 && entries[0x304] == 0
            && !memcmp(entries, "dir2", 5) && files_ok,
        (unsigned)count);
    result = vp_guest_save_directory_read(&dir, entries, sizeof(entries),
                                          &count);
    ++checks;
    failures += save_selftest_check(log, "directory-read-end",
                                    result == 0 && count == 0, (unsigned)count);
    vp_guest_save_release(&dir);

    result = vp_guest_save_delete_file(&mount, "/dir/c.bin");
    ++checks;
    failures += save_selftest_check(log, "delete-file", result == 0, result);
    result = vp_guest_save_delete_directory(&mount, "/dir", 0);
    ++checks;
    failures += save_selftest_check(log, "delete-nonempty-rejected",
                                    result == VP_GUEST_SAVE_DIRECTORY_NOT_EMPTY,
                                    result);
    result = vp_guest_save_delete_directory(&mount, "/dir", 1);
    ++checks;
    failures += save_selftest_check(log, "delete-recursive", result == 0,
                                    result);
    result = vp_guest_save_entry_type(&mount, "/dir", &type);
    ++checks;
    failures += save_selftest_check(log, "deleted-directory-gone",
                                    result == VP_GUEST_SAVE_PATH_NOT_FOUND,
                                    result);

    result = vp_guest_save_create_file(&mount, "/ts", 0);
    uint64_t timestamp[3] = {0, 0, 0};
    if (result == 0)
        result = vp_guest_save_file_timestamp(&mount, "/ts", timestamp);
    ++checks;
    failures += save_selftest_check(log, "file-timestamp", result == 0,
                                    result);
    if (timestamp[2]) {
        ++checks;
        failures += save_selftest_check(log, "timestamp-nonzero", 1,
                                        (unsigned)timestamp[2]);
    }
    result = vp_guest_save_delete_file(&mount, "/ts");
    ++checks;
    failures += save_selftest_check(log, "delete-timestamp-file",
                                    result == 0, result);

    uint64_t free_bytes = 0, total_bytes = 0;
    result = vp_guest_save_free_space(&mount, &free_bytes, &total_bytes);
    ++checks;
    failures += save_selftest_check(log, "free-space",
                                    result == 0 && total_bytes > 0
                                        && free_bytes <= total_bytes,
                                    (unsigned)total_bytes);

    result = vp_guest_save_entry_type(&mount, "../escape", &type);
    ++checks;
    failures += save_selftest_check(log, "parent-traversal-rejected",
                                    result == VP_GUEST_SAVE_INVALID_PATH,
                                    result);
    result = vp_guest_save_entry_type(&mount, "bad:name", &type);
    ++checks;
    failures += save_selftest_check(log, "colon-rejected",
                                    result == VP_GUEST_SAVE_INVALID_PATH,
                                    result);
    result = vp_guest_save_clean_directory(&mount, "/");
    ++checks;
    failures += save_selftest_check(log, "clean-root", result == 0, result);
    vp_guest_save_release(&mount);

    VPGuestSaveObject remount;
    memset(&remount, 0, sizeof(remount));
    result = vp_guest_save_mount(&remount, 0x5650544553540001ull, uid, 0xf0,
                                 0xf1, 0xf2f3);
    ++checks;
    failures += save_selftest_check(log, "remount", result == 0, result);
    vp_guest_save_release(&remount);

    fprintf(log, "SAVE_SELFTEST RESULT checks=%d failures=%d\n", checks,
            failures);
    fflush(log);
    return failures ? -1 : 0;
}
#endif

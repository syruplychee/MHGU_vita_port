#ifndef VP_GUEST_SAVE_H
#define VP_GUEST_SAVE_H
#include <stdint.h>
#include <stdio.h>

/* Writable save-data backing for the title's fsp-srv IFileSystem.
 *
 * IFileSystemProxy OpenSaveDataFileSystem returns an IFileSystem domain
 * object whose root lives under the loader's own data directory:
 *
 *   ux0:data/mhgu-vita-port/savedata/<program>_<uid>_t<type>_r<rank>_i<index>
 *
 * The backend performs the operation the guest asked for; it does not
 * synthesize a file tree. Paths are resolved inside the root and every
 * result is an nn::fs Result code (module 2) so the original SDK wrapper
 * returns exactly what a filesystem service would have returned.
 *
 * File operation semantics mirror the reference save backend that this
 * title was validated against:
 *   - Read/Write take an explicit offset and size; a short read at end of
 *     file succeeds and reports the returned byte count.
 *   - Open modes are the Horizon bits (1 Read, 2 Write, 4 AllowAppend).
 *   - Directory entries use the 0x310-byte Horizon layout: name[0x300],
 *     type at 0x304 (0 directory, 1 file), size u64 at 0x308.
 *   - Directories are listed with directory entries first, then files.
 *   - Commit is a success because every write is completed by close; the
 *     backend does not keep a write-back cache to flush.
 */

/* nn::fs result construction: module 2 (FS), 13-bit description. */
#define VP_GUEST_SAVE_ERROR(description) (2u | ((uint32_t)(description) << 9))

enum {
    VP_GUEST_SAVE_OK = 0,
    VP_GUEST_SAVE_PATH_NOT_FOUND = VP_GUEST_SAVE_ERROR(1),
    VP_GUEST_SAVE_PATH_ALREADY_EXISTS = VP_GUEST_SAVE_ERROR(2),
    /* ResultFsDirectoryNotEmpty (description 8 in the FS result namespace). */
    VP_GUEST_SAVE_DIRECTORY_NOT_EMPTY = VP_GUEST_SAVE_ERROR(8),
    VP_GUEST_SAVE_NOT_IMPLEMENTED = VP_GUEST_SAVE_ERROR(3001),
    VP_GUEST_SAVE_OUT_OF_RANGE = VP_GUEST_SAVE_ERROR(3005),
    VP_GUEST_SAVE_INVALID_ARGUMENT = VP_GUEST_SAVE_ERROR(6001),
    VP_GUEST_SAVE_INVALID_PATH = VP_GUEST_SAVE_ERROR(6002),
    VP_GUEST_SAVE_TOO_LONG_PATH = VP_GUEST_SAVE_ERROR(6003),
    VP_GUEST_SAVE_INVALID_OFFSET = VP_GUEST_SAVE_ERROR(6061),
    VP_GUEST_SAVE_INVALID_SIZE = VP_GUEST_SAVE_ERROR(6062),
    VP_GUEST_SAVE_NULL_ARGUMENT = VP_GUEST_SAVE_ERROR(6063),
    VP_GUEST_SAVE_IO_ERROR = VP_GUEST_SAVE_ERROR(6065),
    VP_GUEST_SAVE_INVALID_MODE = VP_GUEST_SAVE_ERROR(6072),
    VP_GUEST_SAVE_READ_NOT_PERMITTED = VP_GUEST_SAVE_ERROR(6202),
    VP_GUEST_SAVE_WRITE_NOT_PERMITTED = VP_GUEST_SAVE_ERROR(6203),
    VP_GUEST_SAVE_PERMISSION_DENIED = VP_GUEST_SAVE_ERROR(6400),
};

#define VP_GUEST_SAVE_ROOT_PATH "ux0:data/mhgu-vita-port/savedata"
/* Root path plus a full 0x2ff-byte guest path and separators. The guest
 * path string itself is capped at 0x300 by the SDK. */
#define VP_GUEST_SAVE_PATH_LIMIT 1024u
/* Vita's sceIoDirent.d_name is 256 bytes including the NUL. A guest file or
 * directory name that cannot be represented is rejected instead of being
 * truncated. */
#define VP_GUEST_SAVE_NAME_LIMIT 256u
/* sizeof(nn::fs::DirectoryEntry). */
#define VP_GUEST_SAVE_ENTRY_BYTES 0x310u
/* One OpenDirectory snapshot is bounded so a malformed directory cannot
 * exhaust the loader's heap. The save tree contains a handful of entries. */
#define VP_GUEST_SAVE_DIRECTORY_LIMIT 4096u

/* Per-domain-object state. FS_SAVE objects use `path` as the mount root;
 * FS_SAVE_FILE objects use it as the resolved host file path; FS_SAVE_DIR
 * objects own a snapshot of the directory's entries. */
typedef struct VPGuestSaveObject {
    uint32_t interface;
    uint32_t mode;                /* file open mode or directory listing mode */
    char path[VP_GUEST_SAVE_PATH_LIMIT];
    uint8_t *entries;             /* directory snapshot, count * 0x310 bytes */
    uint32_t entry_count;
    uint32_t cursor;              /* next snapshot entry IDirectory::Read returns */
} VPGuestSaveObject;

void vp_guest_save_set_log(FILE *log);
void vp_guest_save_release(VPGuestSaveObject *object);

/* Creates the mount root and stores it in `object`. */
uint32_t vp_guest_save_mount(VPGuestSaveObject *object, uint64_t program,
                             const uint8_t uid[16], uint32_t type,
                             uint32_t rank, uint32_t index);

uint32_t vp_guest_save_entry_type(const VPGuestSaveObject *mount,
                                  const char *guest_path, uint32_t *type);
uint32_t vp_guest_save_create_file(const VPGuestSaveObject *mount,
                                   const char *guest_path, int64_t size);
uint32_t vp_guest_save_delete_file(const VPGuestSaveObject *mount,
                                   const char *guest_path);
uint32_t vp_guest_save_create_directory(const VPGuestSaveObject *mount,
                                        const char *guest_path);
uint32_t vp_guest_save_delete_directory(const VPGuestSaveObject *mount,
                                        const char *guest_path, int recursive);
uint32_t vp_guest_save_clean_directory(const VPGuestSaveObject *mount,
                                       const char *guest_path);
uint32_t vp_guest_save_rename(const VPGuestSaveObject *mount,
                              const char *from_path, const char *to_path,
                              int directory);
uint32_t vp_guest_save_open_file(const VPGuestSaveObject *mount,
                                 const char *guest_path, uint32_t mode,
                                 VPGuestSaveObject *file);
uint32_t vp_guest_save_list_directory(const VPGuestSaveObject *mount,
                                      const char *guest_path, uint32_t mode,
                                      VPGuestSaveObject *directory);
uint32_t vp_guest_save_read_file(const VPGuestSaveObject *file, int64_t offset,
                                 void *destination, uint32_t size,
                                 uint64_t *returned);
uint32_t vp_guest_save_write_file(const VPGuestSaveObject *file, int64_t offset,
                                  const void *source, uint32_t size);
uint32_t vp_guest_save_set_file_size(const VPGuestSaveObject *file,
                                     int64_t size);
uint32_t vp_guest_save_file_size(const VPGuestSaveObject *file,
                                 uint64_t *size);
uint32_t vp_guest_save_flush_file(const VPGuestSaveObject *file);
uint32_t vp_guest_save_file_timestamp(const VPGuestSaveObject *mount,
                                      const char *guest_path,
                                      uint64_t timestamp[3]);
uint32_t vp_guest_save_directory_read(VPGuestSaveObject *directory,
                                      void *destination, uint32_t capacity,
                                      uint64_t *count);
uint32_t vp_guest_save_directory_count(const VPGuestSaveObject *directory,
                                       uint64_t *count);
uint32_t vp_guest_save_free_space(const VPGuestSaveObject *mount,
                                  uint64_t *free_bytes, uint64_t *total_bytes);

#ifdef VP_SAVE_SELFTEST
/* Device-side backend verification, compiled only for the probe build. */
int vp_guest_save_selftest(FILE *log);
#endif

#endif

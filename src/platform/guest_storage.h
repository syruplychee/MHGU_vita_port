#ifndef VP_GUEST_STORAGE_H
#define VP_GUEST_STORAGE_H
#include <stdint.h>

/* Read-only storage backing for the game's fsp-srv data storage.
 *
 * IStorage reads are served from two generated artefacts plus the extracted
 * RomFS tree:
 *   romfs.index  bytes [0, data_offset) of the virtual RomFS image, produced
 *                by tools/build_romfs_index.py in the locked SDK's on-disk
 *                format (header, name hash tables, directory/file metadata);
 *   romfs.map    a sorted image-offset -> tree-file map with the relative
 *                paths of the extracted tree;
 *   romfs-tree/  the tree itself, byte-for-byte file content.
 *
 * Only file content lives in the tree; every metadata byte the game parses
 * comes from the index, so the original RomFS reader runs unmodified. */

#define VP_GUEST_STORAGE_INDEX_PATH "ux0:data/mhgu-vita-port/romfs.index"
#define VP_GUEST_STORAGE_MAP_PATH   "ux0:data/mhgu-vita-port/romfs.map"
#define VP_GUEST_STORAGE_TREE_PATH  "ux0:data/mhgu-vita-port/romfs-tree"

#define VP_GUEST_STORAGE_MAP_MAGIC 0x4D525056u /* "VPRM" */
#define VP_GUEST_STORAGE_MAP_VERSION 1u
#define VP_GUEST_STORAGE_MAP_HEADER_BYTES 44u
#define VP_GUEST_STORAGE_MAP_RECORD_BYTES 24u
#define VP_GUEST_STORAGE_PATH_LIMIT 192u

/* Return codes from vp_guest_storage_read; the caller logs st->error. */
enum {
    VP_GUEST_STORAGE_OK = 0,
    VP_GUEST_STORAGE_NOT_OPEN = -1,
    VP_GUEST_STORAGE_RANGE = -2,
    VP_GUEST_STORAGE_BACKING = -3,
    VP_GUEST_STORAGE_IO = -4,
    /* A second read entered while the first one still owns the cache. The
     * request is refused without touching the cached handle. */
    VP_GUEST_STORAGE_BUSY = -5,
};

/* Exact tree-file failure categories. The diagnostic is filled only on the
 * failing open/stat/size/read path and cleared by every successful read, so a
 * stale record cannot be attributed to a later failure. */
typedef enum {
    VP_GUEST_STORAGE_DIAG_NONE = 0,
    VP_GUEST_STORAGE_DIAG_PATH_LIMIT = 1,
    VP_GUEST_STORAGE_DIAG_OPEN = 2,
    VP_GUEST_STORAGE_DIAG_FD_STAT = 3,
    VP_GUEST_STORAGE_DIAG_SIZE = 4,
    VP_GUEST_STORAGE_DIAG_READ = 5,
} VPGuestStorageDiagOperation;

typedef struct {
    uint32_t operation;       /* VPGuestStorageDiagOperation */
    uint32_t path_offset;     /* map path-table offset of the tree file */
    int fd;                   /* opened fd, or -1 when open failed */
    int open_rc;              /* sceIoOpen return: fd, or the negative error */
    int stat_rc;              /* sceIoGetstatByFd return; 0 if not attempted */
    int io_rc;                /* failing/short sceIoPread return; 0 otherwise */
    uint32_t open_attempted;
    uint32_t stat_attempted;
    uint32_t observed_valid;  /* sceIoGetstatByFd returned >= 0 */
    uint32_t read_attempted;
    uint64_t expected_size;   /* RomFS map record size */
    uint64_t observed_size;   /* only meaningful when observed_valid != 0 */
    uint64_t file_offset;     /* file offset passed to the failing read */
    uint32_t read_length;     /* byte count passed to the failing read */
    /* Handle-lifecycle evidence. A cached read failure is followed by one
     * sceIoGetstatByFd probe on the same descriptor. Only an exact EBADFD
     * probe proves the handle is absent from the emulator's table; any other
     * probe result is recorded as unknown and the cache entry is retained. */
    uint32_t owner;           /* guest thread token supplied by the caller */
    uint32_t concurrent;      /* this request observed another active read */
    uint32_t fd_probe_attempted;
    uint32_t fd_probe_valid;
    uint32_t fd_probe_ebadfd; /* probe returned exactly 0x80010051 */
    int fd_probe_rc;
    uint64_t fd_probe_size;
    uint32_t close_attempted; /* open/stat/size failure path only */
    int close_rc;             /* sceIoClose return for that path, 0 otherwise */
    uint32_t retire_attempted; /* invalid-cache retirement without close */
    uint64_t cache_generation; /* increments on every cache identity change */
    uint64_t open_seq;         /* actual sceIoOpen sequence of this descriptor */
    uint64_t close_seq;        /* actual sceIoClose sequence at failure time */
    uint64_t retire_seq;       /* no-close retirement sequence at failure time */
    uint64_t closes_since_open;/* actual storage closes after this open */
    uint64_t retires_since_open;/* no-close retirements after this open */
} VPGuestStorageDiagnostic;

typedef struct {
    int index_fd;
    int map_fd;
    int tree_fd;          /* cached file for the last read */
    uint32_t tree_fd_path; /* path offset cached in tree_fd, or UINT32_MAX */
    uint8_t *map;
    uint32_t map_bytes;
    uint32_t file_count;
    uint32_t path_bytes;
    uint64_t tree_bytes;   /* data region size, including alignment gaps */
    uint64_t data_offset;
    uint64_t image_size;
    char tree_root[VP_GUEST_STORAGE_PATH_LIMIT];
    uint32_t open;
    uint64_t reads;
    uint64_t bytes;
    uint64_t file_opens;
    uint64_t errors;
    char last_path[VP_GUEST_STORAGE_PATH_LIMIT];
    char error[96];
    VPGuestStorageDiagnostic diagnostic;
    /* Bounded lifecycle bookkeeping. These counters are cheap and are only
     * reported in the periodic summary and on a failed read. tree_opens and
     * tree_closes count actual sceIoOpen (>=0) and sceIoClose calls; a
     * retirement without close is counted separately. */
    uint64_t fd_generation;   /* increments on every cache identity change */
    uint64_t fd_open_seq;     /* last actual tree open sequence (>=0 return) */
    uint64_t fd_close_seq;    /* last actual tree sceIoClose sequence */
    uint64_t fd_retire_seq;   /* last no-close retirement sequence */
    uint64_t tree_opens;      /* actual sceIoOpen returns >= 0 */
    uint64_t tree_closes;     /* actual sceIoClose calls on tree descriptors */
    uint64_t tree_retires;    /* cache entries retired without close */
    uint64_t tree_fd_open_seq;/* open sequence of the cached descriptor */
    uint64_t tree_fd_closes_at_open; /* fd_close_seq when it was opened */
    uint64_t tree_fd_retires_at_open;/* fd_retire_seq when it was opened */
    uint32_t concurrent_reads;/* requests that hit an active read */
    uint32_t owner;           /* guest thread token for diagnostics */
    uint32_t active;          /* one storage read owns the cache at a time */
} VPGuestStorage;

void vp_guest_storage_init(VPGuestStorage *);
/* Records the guest thread token used in failure diagnostics. */
void vp_guest_storage_set_owner(VPGuestStorage *, uint32_t owner);
int vp_guest_storage_open(VPGuestStorage *, const char *index_path,
                          const char *map_path, const char *tree_root);
void vp_guest_storage_close(VPGuestStorage *);
uint64_t vp_guest_storage_size(const VPGuestStorage *);
const char *vp_guest_storage_diag_operation_name(uint32_t operation);
/* Writes exactly `size` bytes to the guest address, which must already have
 * been accepted by vp_guest_svc_writable(). Guest and host addresses are the
 * same 32-bit space on the native ARM target. */
int vp_guest_storage_read(VPGuestStorage *, uint64_t offset, uint32_t address,
                          uint32_t size);
#endif

#ifndef VP_NVMAP_TABLE_H
#define VP_NVMAP_TABLE_H

#include <stdint.h>

/* Maximum number of simultaneously live nvmap user handles in one service
 * session. This bounds live kernel objects, not the number of allocations a
 * session may create over its lifetime: Free retires an entry and its slot is
 * reused by a later Create. */
#define VP_GUEST_NVMAP_HANDLE_LIMIT 128u

/* nvmap object table. The client supplies the CPU address of memory it owns
 * in nvmap Alloc, so the loader only registers and validates the range; it
 * never allocates or aliases memory on the client's behalf. A record is live
 * from Create until Free: `allocated` only says whether Alloc registered a
 * range, so it must not be used as a liveness flag. */
typedef struct {
    uint32_t id;
    uint32_t size;      /* requested size, unaligned */
    uint32_t align;     /* allocation alignment, at least page size */
    uint32_t kind;
    uint32_t flags;
    uint32_t address;   /* client CPU address registered by Alloc */
    uint32_t allocated;
} VPGuestNvMapHandle;

/* Table operations shared by the service dispatcher and the host regression.
 * Entries stay compacted (count == live entries); ids are monotonic and are
 * never reused, so a retained id from a freed handle cannot alias a later
 * allocation (no ABA). */
typedef enum {
    VP_NVMAP_OK = 0,
    VP_NVMAP_INVALID_ARGUMENT,
    VP_NVMAP_FULL,
    VP_NVMAP_ID_EXHAUSTED,
    VP_NVMAP_NOT_FOUND,      /* unknown or already retired handle */
} VPNvMapResult;

/* Create a live handle without an allocation. Fails without changing the
 * table when capacity is exhausted or the monotonic id space is used up. */
VPNvMapResult vp_nvmap_table_create(VPGuestNvMapHandle *table, uint32_t capacity,
                                    uint32_t *count, uint32_t *next_id,
                                    uint32_t size, uint32_t *id_out);
/* Look up a live handle. Returns NULL for unknown/freed ids. */
VPGuestNvMapHandle *vp_nvmap_table_find(VPGuestNvMapHandle *table,
                                        uint32_t count, uint32_t id);
/* Retire a live handle, optionally copying its final record to `retired` for
 * the Free reply. The freed slot is compacted away and becomes available to a
 * later Create, which always issues a new id. */
VPNvMapResult vp_nvmap_table_retire(VPGuestNvMapHandle *table, uint32_t *count,
                                    uint32_t id, VPGuestNvMapHandle *retired);

#endif

#ifndef VP_PATCH_LOADER_H
#define VP_PATCH_LOADER_H
#include <stdint.h>

/* Patch side of the loader: the only writer of relocation words, import slots,
 * patch site words, island payloads and the deferred directory word.
 *
 * Every module-image write carries the word the caller expects to find there
 * (the linked addend, the slot preimage, the site original), so a module whose
 * bytes differ from the locked input is rejected before anything is written.
 * Import slots and patch sites are recorded in a ledger, so a second writer of
 * the same word is rejected instead of silently overwriting the first: the
 * "single fixer" rule is a runtime invariant here, not a convention.
 *
 * Bounded and self-authored inputs only; segment permissions, page mapping and
 * cache maintenance stay outside this unit.
 */
#define VP_PATCH_LEDGER 128u
#define VP_PATCH_BRANCH_REACH 0x2000000u /* A32 B, +-32 MiB */
enum {
    VP_PATCH_OK = 0,
    VP_PATCH_ARGUMENT = -1,
    VP_PATCH_RANGE = -2,
    VP_PATCH_BINDING = -3,
    VP_PATCH_DOUBLE_WRITE = -4,
    VP_PATCH_TEMPLATE = -5,
    VP_PATCH_REACH = -6,
    VP_PATCH_LEDGER_FULL = -7,
};
typedef struct {
    uint32_t offset, before, after;
} VPPatchRecord;
typedef struct {
    unsigned char *image;
    uint32_t bytes, base;
    uint32_t words, slots, sites, rejected;
    uint32_t ledger_count;
    VPPatchRecord ledger[VP_PATCH_LEDGER];
} VPPatchTarget;

int vp_patch_open(VPPatchTarget *, void *image, uint32_t bytes, uint32_t base);
/* Relocation or deferred word: bound, but not ledgered (the plan already
 * proves every relocation target is unique). */
int vp_patch_word(VPPatchTarget *, uint32_t offset, uint32_t expected, uint32_t value);
/* Import slot: resolved provider address, ledgered. */
int vp_patch_slot(VPPatchTarget *, uint32_t offset, uint32_t expected, uint32_t provider);
/* Patch site: the loader replaces the bound original word with `b island`. */
int vp_patch_site(VPPatchTarget *, uint32_t offset, uint32_t expected_original, uint32_t island);
/* Copy one island template, fill its deferred movw/movt pair with the runtime
 * directory address (keeping the template's destination register) and write the
 * return branch back to the site it serves. */
int vp_patch_island(uint32_t destination, const unsigned char *template, uint32_t template_bytes,
                    uint32_t return_offset, uint32_t resume, uint32_t directory_offset, uint32_t directory);
uint32_t vp_patch_ledger_after(const VPPatchTarget *, uint32_t offset);
#endif

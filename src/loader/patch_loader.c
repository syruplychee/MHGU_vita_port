#include "patch_loader.h"
#include <string.h>

static int read_word(const VPPatchTarget *target, uint32_t offset, uint32_t *value) {
    if (!target || !target->image) return VP_PATCH_ARGUMENT;
    if (offset & 3u) return VP_PATCH_ARGUMENT;
    if (offset > target->bytes || target->bytes - offset < 4) return VP_PATCH_RANGE;
    memcpy(value, target->image + offset, 4);
    return VP_PATCH_OK;
}

static int write_word(VPPatchTarget *target, uint32_t offset, uint32_t value) {
    memcpy(target->image + offset, &value, 4);
    return VP_PATCH_OK;
}

static int encode_branch(uint32_t source, uint32_t target, uint32_t *word) {
    int64_t displacement = (int64_t)target - ((int64_t)source + 8);
    if (source & 3u || target & 3u) return VP_PATCH_ARGUMENT;
    if (displacement < -(int64_t)VP_PATCH_BRANCH_REACH || displacement > (int64_t)VP_PATCH_BRANCH_REACH - 4
        || (displacement & 3)) return VP_PATCH_REACH;
    *word = 0xea000000u | ((uint32_t)(displacement >> 2) & 0xffffffu);
    return VP_PATCH_OK;
}

static uint32_t mov_pair_word(uint32_t base_opcode, uint32_t destination_register, uint32_t immediate) {
    return base_opcode | ((immediate >> 12) << 16) | ((destination_register & 0xfu) << 12) | (immediate & 0xfffu);
}

static VPPatchRecord *ledger_entry(VPPatchTarget *target, uint32_t offset) {
    for (uint32_t index = 0; index < target->ledger_count; ++index)
        if (target->ledger[index].offset == offset) return &target->ledger[index];
    return 0;
}

static int ledger_add(VPPatchTarget *target, uint32_t offset, uint32_t before, uint32_t after) {
    if (ledger_entry(target, offset)) return VP_PATCH_DOUBLE_WRITE;
    if (target->ledger_count >= VP_PATCH_LEDGER) return VP_PATCH_LEDGER_FULL;
    target->ledger[target->ledger_count].offset = offset;
    target->ledger[target->ledger_count].before = before;
    target->ledger[target->ledger_count].after = after;
    ++target->ledger_count;
    return VP_PATCH_OK;
}

int vp_patch_open(VPPatchTarget *target, void *image, uint32_t bytes, uint32_t base) {
    if (!target || !image || bytes < 4 || (bytes & 3u) || (base & 3u)) return VP_PATCH_ARGUMENT;
    memset(target, 0, sizeof(*target));
    target->image = image;
    target->bytes = bytes;
    target->base = base;
    return VP_PATCH_OK;
}

int vp_patch_word(VPPatchTarget *target, uint32_t offset, uint32_t expected, uint32_t value) {
    if (!target) return VP_PATCH_ARGUMENT;
    uint32_t current = 0;
    int status = read_word(target, offset, &current);
    if (status != VP_PATCH_OK) { ++target->rejected; return status; }
    if (current != expected) { ++target->rejected; return VP_PATCH_BINDING; }
    write_word(target, offset, value);
    ++target->words;
    return VP_PATCH_OK;
}

int vp_patch_slot(VPPatchTarget *target, uint32_t offset, uint32_t expected, uint32_t provider) {
    if (!target) return VP_PATCH_ARGUMENT;
    /* Single-writer rule first: a word this loader already owns is refused no
     * matter what it now contains. */
    VPPatchRecord *owned = ledger_entry(target, offset);
    if (owned) { ++target->rejected; return VP_PATCH_DOUBLE_WRITE; }
    if (target->ledger_count >= VP_PATCH_LEDGER) { ++target->rejected; return VP_PATCH_LEDGER_FULL; }
    uint32_t current = 0;
    int status = read_word(target, offset, &current);
    if (status != VP_PATCH_OK) { ++target->rejected; return status; }
    if (current != expected) { ++target->rejected; return VP_PATCH_BINDING; }
    ledger_add(target, offset, current, provider);
    write_word(target, offset, provider);
    ++target->slots;
    ++target->words;
    return VP_PATCH_OK;
}

int vp_patch_site(VPPatchTarget *target, uint32_t offset, uint32_t expected_original, uint32_t island) {
    if (!target) return VP_PATCH_ARGUMENT;
    VPPatchRecord *owned = ledger_entry(target, offset);
    if (owned) { ++target->rejected; return VP_PATCH_DOUBLE_WRITE; }
    if (target->ledger_count >= VP_PATCH_LEDGER) { ++target->rejected; return VP_PATCH_LEDGER_FULL; }
    uint32_t current = 0;
    int status = read_word(target, offset, &current);
    if (status != VP_PATCH_OK) { ++target->rejected; return status; }
    if (current != expected_original) { ++target->rejected; return VP_PATCH_BINDING; }
    uint32_t branch = 0;
    status = encode_branch(target->base + offset, island, &branch);
    if (status != VP_PATCH_OK) { ++target->rejected; return status; }
    ledger_add(target, offset, current, branch);
    write_word(target, offset, branch);
    ++target->sites;
    ++target->words;
    return VP_PATCH_OK;
}

int vp_patch_island(uint32_t destination, const unsigned char *template, uint32_t template_bytes,
                    uint32_t return_offset, uint32_t resume, uint32_t directory_offset, uint32_t directory) {
    if (!template || template_bytes < 16 || (template_bytes & 3u) || (destination & 3u)) return VP_PATCH_ARGUMENT;
    if (return_offset > template_bytes || template_bytes - return_offset < 4) return VP_PATCH_RANGE;
    if (directory_offset > template_bytes || template_bytes - directory_offset < 8) return VP_PATCH_RANGE;
    unsigned char *image = (unsigned char *)(uintptr_t)destination;
    uint32_t low = 0, high = 0;
    memcpy(&low, template + directory_offset, 4);
    memcpy(&high, template + directory_offset + 4, 4);
    /* Keep the template's destination register, replace only the immediate. */
    if ((low & 0xfff00000u) != 0xe3000000u || (high & 0xfff00000u) != 0xe3400000u) return VP_PATCH_TEMPLATE;
    if ((low & 0xf000u) != (high & 0xf000u)) return VP_PATCH_TEMPLATE;
    uint32_t branch = 0;
    int status = encode_branch(destination + return_offset, resume, &branch);
    if (status != VP_PATCH_OK) return status;
    memcpy(image, template, template_bytes);
    uint32_t register_field = low & 0xf000u;
    uint32_t lower = mov_pair_word(0xe3000000u, register_field >> 12, directory & 0xffffu);
    uint32_t upper = mov_pair_word(0xe3400000u, register_field >> 12, (directory >> 16) & 0xffffu);
    memcpy(image + directory_offset, &lower, 4);
    memcpy(image + directory_offset + 4, &upper, 4);
    memcpy(image + return_offset, &branch, 4);
    return VP_PATCH_OK;
}

uint32_t vp_patch_ledger_after(const VPPatchTarget *target, uint32_t offset) {
    if (!target) return 0;
    for (uint32_t index = 0; index < target->ledger_count; ++index)
        if (target->ledger[index].offset == offset) return target->ledger[index].after;
    return 0;
}

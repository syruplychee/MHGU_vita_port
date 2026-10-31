#include <stdio.h>
#include <string.h>
#include "gxm_sampler_slots.h"

static int check_offset(unsigned index, unsigned expected) {
    unsigned offset = 0xffffffffu;
    if (vp_gxm_fragment_sampler_offset(index, &offset) != 0) return 1;
    if (offset != expected) return 1;
    return 0;
}

static int check_slot(const char *name, unsigned expected) {
    unsigned slot = 0xffffffffu;
    if (vp_gxm_fragment_sampler_slot(name, &slot) != 0) return 1;
    if (slot != expected) return 1;
    return 0;
}

static int reject_offset(unsigned index) {
    unsigned offset = 0x12345678u;
    if (vp_gxm_fragment_sampler_offset(index, &offset) == 0) return 1;
    if (offset != 0x12345678u) return 1;
    return 0;
}

static int reject_slot(const char *name) {
    unsigned slot = 0x12345678u;
    if (vp_gxm_fragment_sampler_slot(name, &slot) == 0) return 1;
    if (slot != 0x12345678u) return 1;
    return 0;
}

static int check_stage_slot(const char *name, unsigned vertex, unsigned expected) {
    unsigned slot = 0xffffffffu;
    if (vp_gxm_sampler_name_slot(name, vertex, &slot) != 0) return 1;
    return slot == expected ? 0 : 1;
}

static int reject_stage_slot(const char *name, unsigned vertex) {
    unsigned slot = 0x12345678u;
    if (vp_gxm_sampler_name_slot(name, vertex, &slot) == 0) return 1;
    return slot == 0x12345678u ? 0 : 1;
}

int main(void) {
    /* The five fragment TIC handles are bank0 records at these exact byte
     * offsets (count 1, shift 0, size_shift 3). */
    if (check_offset(0, 32)) return 1;
    if (check_offset(1, 40)) return 2;
    if (check_offset(2, 48)) return 3;
    if (check_offset(3, 56)) return 4;
    if (check_offset(4, 64)) return 5;
    if (reject_offset(5)) return 6;
    if (reject_offset(6)) return 7;
    if (vp_gxm_fragment_sampler_offset(0, NULL) == 0) return 8;

    if (check_slot("tex0", 0)) return 9;
    if (check_slot("tex1", 1)) return 10;
    if (check_slot("tex2", 2)) return 11;
    if (check_slot("tex3", 3)) return 12;
    if (check_slot("tex4", 4)) return 13;
    if (reject_slot("tex5")) return 14;
    if (reject_slot("tex9")) return 15;
    if (reject_slot("tex")) return 16;
    if (reject_slot("tex00")) return 17;
    if (reject_slot("tex3 ")) return 18;
    if (reject_slot("diffuse")) return 19;
    if (reject_slot("")) return 20;
    if (vp_gxm_fragment_sampler_slot("tex0", NULL) == 0) return 21;

    /* The vertex stage keeps tex0 only; fragment accepts the fifth slot. */
    if (check_stage_slot("tex0", 1, 0)) return 22;
    if (reject_stage_slot("tex1", 1)) return 23;
    if (reject_stage_slot("tex3", 1)) return 24;
    if (reject_stage_slot("tex4", 1)) return 25;
    if (reject_stage_slot("color", 0)) return 26;
    if (check_stage_slot("tex3", 0, 3)) return 27;
    if (check_stage_slot("tex4", 0, 4)) return 28;
    if (vp_gxm_sampler_name_slot("tex0", 1, NULL) == 0) return 29;

    if (VP_GXM_FRAGMENT_SAMPLER_LIMIT != 5u) return 30;
    puts("PASS GXM sampler slots: tex0..tex4 offsets 32/40/48/56/64, "
         "five-slot bound and unknown-name refusal");
    return 0;
}

#include "nvmap_table.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CAP VP_GUEST_NVMAP_HANDLE_LIMIT

/* Exercises the table operations used by the nvmdp/nvdrv dispatcher:
 * capacity is a bound on simultaneously live handles, Free retires a record
 * and recycles its slot, ids stay monotonic across the whole session, and a
 * retired id is never resolvable again. */

static VPGuestNvMapHandle table[CAP];

static uint32_t create(uint32_t *count, uint32_t *next, uint32_t size) {
    uint32_t id = 0;
    assert(vp_nvmap_table_create(table, CAP, count, next, size, &id)
           == VP_NVMAP_OK);
    assert(id != 0);
    return id;
}

static void allocate(uint32_t count, uint32_t id, uint32_t address,
                     uint32_t flags) {
    VPGuestNvMapHandle *h = vp_nvmap_table_find(table, count, id);
    assert(h != NULL);
    h->address = address;
    h->flags = flags;
    h->allocated = 1;
}

static void test_full_live_table(void) {
    uint32_t count = 0, next = 0, ids[CAP], overflow = 0xdeadbeefu;

    memset(table, 0, sizeof(table));
    for (uint32_t i = 0; i < CAP; ++i) {
        ids[i] = create(&count, &next, 0x1000u + i);
        VPGuestNvMapHandle *h = vp_nvmap_table_find(table, count, ids[i]);
        assert(h != NULL);
        /* Created but not allocated is still live and owns its slot. */
        assert(!h->allocated && h->id == ids[i] && h->size == 0x1000u + i);
    }
    assert(count == CAP);
    assert(vp_nvmap_table_create(table, CAP, &count, &next, 0x1000u, &overflow)
           == VP_NVMAP_FULL);
    /* Rejection changes neither the table nor the caller's output. */
    assert(count == CAP && overflow == 0xdeadbeefu);
    for (uint32_t i = 0; i < CAP; ++i) {
        assert(vp_nvmap_table_find(table, count, ids[i]) != NULL);
        for (uint32_t j = 0; j < i; ++j) assert(ids[i] != ids[j]);
    }
    /* Every entry keeps its own creation size: no created-not-allocated
     * record was overwritten while the table filled. */
    for (uint32_t i = 0; i < CAP; ++i)
        assert(vp_nvmap_table_find(table, count, ids[i])->size == 0x1000u + i);
    /* Free of a live-but-never-allocated handle retires it (the kernel
     * destroys the object) and recycles the slot with a fresh id. */
    VPGuestNvMapHandle retired;
    assert(vp_nvmap_table_retire(table, &count, ids[0], &retired)
           == VP_NVMAP_OK);
    assert(retired.allocated == 0 && retired.address == 0);
    assert(vp_nvmap_table_find(table, count, ids[0]) == NULL);
    uint32_t recycled = create(&count, &next, 0x7777u);
    assert(recycled != ids[0] && count == CAP);
    puts("PASS 128 live created-not-allocated, 129th rejected");
}

static void test_allocated_full_and_retire_identity(void) {
    uint32_t count = 0, next = 0, ids[CAP], probe = 0;

    memset(table, 0, sizeof(table));
    for (uint32_t i = 0; i < CAP; ++i) {
        ids[i] = create(&count, &next, 0x2000u);
        allocate(count, ids[i], 0x80000000u + i * 0x2000u, i & 1u);
    }
    assert(vp_nvmap_table_create(table, CAP, &count, &next, 0x2000u, &probe)
           == VP_NVMAP_FULL);
    assert(count == CAP);

    /* Retire a middle allocated entry; the Free reply must carry exactly the
     * registered guest CPU address, aligned size and uncached flag. */
    VPGuestNvMapHandle retired;
    memset(&retired, 0, sizeof(retired));
    assert(vp_nvmap_table_retire(table, &count, ids[CAP / 2], &retired)
           == VP_NVMAP_OK);
    assert(retired.id == ids[CAP / 2]);
    assert(retired.address == 0x80000000u + (CAP / 2) * 0x2000u);
    assert(retired.size == 0x2000u && retired.allocated == 1);
    assert(retired.flags == ((CAP / 2) & 1u));
    assert(count == CAP - 1);
    /* Old handle is gone: no lookup and no second Free. */
    assert(vp_nvmap_table_find(table, count, retired.id) == NULL);
    assert(vp_nvmap_table_retire(table, &count, retired.id, NULL)
           == VP_NVMAP_NOT_FOUND);
    /* All other live objects survive with their own id/address. */
    for (uint32_t i = 0; i < CAP; ++i) {
        if (i == CAP / 2) continue;
        VPGuestNvMapHandle *h = vp_nvmap_table_find(table, count, ids[i]);
        assert(h != NULL && h->allocated);
        assert(h->address == 0x80000000u + i * 0x2000u);
    }
    /* A new Create reuses the slot but never the id. */
    uint32_t replacement = create(&count, &next, 0x3000u);
    assert(replacement != retired.id);
    for (uint32_t i = 0; i < CAP; ++i) assert(replacement != ids[i]);
    assert(count == CAP);
    puts("PASS allocated table bound, Free reply identity, no id reuse");
}

static void test_more_than_capacity_lifetime(void) {
    uint32_t count = 0, next = 0, previous = 0;

    memset(table, 0, sizeof(table));
    /* A session may create far more than 128 buffers as long as no more than
     * 128 are live at once. This is the language-switch/restart pattern. */
    for (uint32_t i = 0; i < 4u * CAP; ++i) {
        uint32_t id = create(&count, &next, 0x4000u + (i & 0x3fu));
        /* Distinct ids across every Create, including slot reuse. */
        assert(id > previous);
        previous = id;
        allocate(count, id, 0x90000000u + (i & 0xffu) * 0x4000u, 0);
        VPGuestNvMapHandle retired;
        assert(vp_nvmap_table_retire(table, &count, id, &retired)
               == VP_NVMAP_OK);
        assert(retired.address == 0x90000000u + (i & 0xffu) * 0x4000u);
        assert(count == 0);
        assert(vp_nvmap_table_find(table, count, id) == NULL);
    }
    assert(previous == 4u * CAP);
    assert(next == 4u * CAP);
    puts("PASS 512 create/alloc/free cycles, monotonic never-reused ids");
}

static void test_slot_reuse_does_not_alias(void) {
    uint32_t count = 0, next = 0;
    VPGuestNvMapHandle retired;

    memset(table, 0, sizeof(table));
    uint32_t a = create(&count, &next, 0x100u);
    uint32_t b = create(&count, &next, 0x200u);
    uint32_t c = create(&count, &next, 0x300u);
    allocate(count, a, 0x10000000u, 0);
    allocate(count, b, 0x20000000u, 1);
    allocate(count, c, 0x30000000u, 0);
    /* Retire the first slot (compaction moves c into it). */
    assert(vp_nvmap_table_retire(table, &count, a, &retired) == VP_NVMAP_OK);
    uint32_t d = create(&count, &next, 0x400u);
    allocate(count, d, 0x40000000u, 0);
    assert(d != a && d != b && d != c);
    assert(vp_nvmap_table_find(table, count, a) == NULL);
    assert(vp_nvmap_table_find(table, count, b)->address == 0x20000000u);
    assert(vp_nvmap_table_find(table, count, c)->address == 0x30000000u);
    assert(vp_nvmap_table_find(table, count, d)->address == 0x40000000u);
    assert(count == 3);
    puts("PASS slot compaction keeps other live records intact, no ABA");
}

static void test_mapping_holds_address_not_table_pointer(void) {
    uint32_t count = 0, next = 0;
    VPGuestNvMapHandle retired;

    memset(table, 0, sizeof(table));
    uint32_t id = create(&count, &next, 0x1000u);
    allocate(count, id, 0x12345000u, 0);
    /* MapBufferEx records the resolved CPU address (device_address) and the
     * handle id, never a pointer into the table. Simulate that record. */
    struct { uint32_t handle; uint32_t device_address; } mapping = {
        id, vp_nvmap_table_find(table, count, id)->address
    };
    assert(mapping.device_address == 0x12345000u);
    assert(vp_nvmap_table_retire(table, &count, id, &retired) == VP_NVMAP_OK);
    /* The mapping record still names the guest memory it was created from. */
    assert(mapping.device_address == retired.address);
    /* But the nvmap handle is no longer resolvable, so a later MapBufferEx /
     * media pin / binder-slot resolution rejects instead of using stale
     * metadata. A replacement allocation must not answer for the old id. */
    uint32_t replacement = create(&count, &next, 0x1000u);
    allocate(count, replacement, 0x54321000u, 0);
    assert(vp_nvmap_table_find(table, count, mapping.handle) == NULL);
    assert(vp_nvmap_table_find(table, count, replacement)->id == replacement);
    puts("PASS mapping keeps device address, freed handle stays unresolvable");
}

static void test_id_exhaustion_never_wraps(void) {
    uint32_t count = 0, next = UINT32_MAX - 2u, id = 0;

    memset(table, 0, sizeof(table));
    id = create(&count, &next, 0x100u);
    assert(id == UINT32_MAX - 1u);
    id = create(&count, &next, 0x100u);
    assert(id == UINT32_MAX);
    uint32_t before = count;
    uint32_t out = 0xabcdef01u;
    assert(vp_nvmap_table_create(table, CAP, &count, &next, 0x100u, &out)
           == VP_NVMAP_ID_EXHAUSTED);
    assert(count == before && out == 0xabcdef01u && next == UINT32_MAX);
    /* Freeing does not resurrect the id space. */
    assert(vp_nvmap_table_retire(table, &count, UINT32_MAX, NULL)
           == VP_NVMAP_OK);
    assert(vp_nvmap_table_create(table, CAP, &count, &next, 0x100u, &out)
           == VP_NVMAP_ID_EXHAUSTED);
    puts("PASS id space exhaustion rejected explicitly, no wrap to 0");
}

static void test_invalid_and_unknown_rejected(void) {
    uint32_t count = 0, next = 0, id = 0;
    VPGuestNvMapHandle retired;

    memset(table, 0, sizeof(table));
    assert(vp_nvmap_table_create(NULL, CAP, &count, &next, 0x100u, &id)
           == VP_NVMAP_INVALID_ARGUMENT);
    assert(vp_nvmap_table_create(table, CAP, NULL, &next, 0x100u, &id)
           == VP_NVMAP_INVALID_ARGUMENT);
    assert(vp_nvmap_table_create(table, CAP, &count, NULL, 0x100u, &id)
           == VP_NVMAP_INVALID_ARGUMENT);
    assert(vp_nvmap_table_create(table, CAP, &count, &next, 0x100u, NULL)
           == VP_NVMAP_INVALID_ARGUMENT);
    assert(vp_nvmap_table_find(NULL, 0, 1) == NULL);
    assert(vp_nvmap_table_retire(NULL, &count, 1, NULL)
           == VP_NVMAP_INVALID_ARGUMENT);
    assert(vp_nvmap_table_retire(table, NULL, 1, NULL)
           == VP_NVMAP_INVALID_ARGUMENT);
    id = create(&count, &next, 0x100u);
    /* Unknown ids, including 0 (never issued) and a never-created value. */
    assert(vp_nvmap_table_find(table, count, 0) == NULL);
    assert(vp_nvmap_table_find(table, count, id + 1u) == NULL);
    assert(vp_nvmap_table_retire(table, &count, 0, &retired)
           == VP_NVMAP_NOT_FOUND);
    assert(vp_nvmap_table_retire(table, &count, id + 1u, &retired)
           == VP_NVMAP_NOT_FOUND);
    assert(count == 1);
    puts("PASS invalid arguments, id 0 and unknown handles rejected");
}

int main(void) {
    test_full_live_table();
    test_allocated_full_and_retire_identity();
    test_more_than_capacity_lifetime();
    test_slot_reuse_does_not_alias();
    test_mapping_holds_address_not_table_pointer();
    test_id_exhaustion_never_wraps();
    test_invalid_and_unknown_rejected();
    puts("PASS nvmap table lifecycle (7 groups)");
    return 0;
}

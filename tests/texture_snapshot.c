#include "texture_snapshot.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    VPTextureSnapshots cache = {0};
    VPTextureSnapshotKey key = {0x508000000ull, 2, 2, 257, 0, 1};
    unsigned char source[32], owned[32], changed[32];
    for (unsigned i = 0; i < sizeof(source); ++i) source[i] = (unsigned char)i;
    memcpy(owned, source, sizeof(source));
    assert(!vp_texture_snapshot_find(&cache, &key, source, sizeof(source)));
    vp_texture_snapshot_remember(&cache, &key, owned, sizeof(owned));
    assert(vp_texture_snapshot_find(&cache, &key, source, sizeof(source)) == owned);
    /* A mutation in the last byte at the same VA must allocate a new snapshot;
     * queued draws must continue reading the original batch-owned bytes. */
    source[31] ^= 128;
    assert(!vp_texture_snapshot_find(&cache, &key, source, sizeof(source)));
    assert(owned[31] == 31);
    unsigned rollback = cache.count;
    memcpy(changed, source, sizeof(source));
    vp_texture_snapshot_remember(&cache, &key, changed, sizeof(changed));
    assert(vp_texture_snapshot_find(&cache, &key, source, sizeof(source)) == changed);
    cache.count = rollback;
    assert(!vp_texture_snapshot_find(&cache, &key, source, sizeof(source)));
    source[31] ^= 128;
    assert(vp_texture_snapshot_find(&cache, &key, source, sizeof(source)) == owned);
    /* Equal bytes do not make different views interchangeable. */
    VPTextureSnapshotKey view = key;
    view.width = 4; view.height = 1;
    assert(!vp_texture_snapshot_find(&cache, &view, source, sizeof(source)));
    view = key; view.cube = 1; view.mips = 2;
    assert(!vp_texture_snapshot_find(&cache, &view, source, sizeof(source)));
    view = key; view.format++;
    assert(!vp_texture_snapshot_find(&cache, &view, source, sizeof(source)));
    assert(!vp_texture_snapshot_find(&cache, &key, source, sizeof(source)-1));
    cache.count = 0; /* Completed flush: old pointers cannot be reused. */
    assert(!vp_texture_snapshot_find(&cache, &key, source, sizeof(source)));
    for (unsigned i = 0; i < VP_TEXTURE_SNAPSHOT_LIMIT + 1; ++i) {
        view = key; view.va += i;
        vp_texture_snapshot_remember(&cache, &view, owned, sizeof(owned));
    }
    assert(cache.count == VP_TEXTURE_SNAPSHOT_LIMIT);
    assert(!vp_texture_snapshot_find(&cache, &view, source, sizeof(source)));

    /* B105 owned decode-source associations. decoded is the decoder-owned
     * allocation; decoded_copy is the payload that copied its bytes. */
    VPTextureSnapshots owned_cache = {0};
    unsigned char decoded[32], decoded_copy[32];
    for (unsigned i = 0; i < sizeof(decoded); ++i) decoded[i] = (unsigned char)(0xa0u + i);
    memcpy(decoded_copy, decoded, sizeof(decoded));
    /* A marked pointer alone is not proof: no association exists yet. */
    assert(!vp_texture_snapshot_find_owned(&owned_cache, &key, decoded, sizeof(decoded)));
    /* The caller records it only after copying/compare-proving the bytes. */
    vp_texture_snapshot_remember_owned(&owned_cache, &key, decoded_copy,
                                       sizeof(decoded_copy), decoded);
    assert(vp_texture_snapshot_find_owned(&owned_cache, &key, decoded, sizeof(decoded)) == decoded_copy);
    /* The upload-cache bookkeeping shares this single-entry matcher. */
    assert(vp_texture_snapshot_owned_match(&owned_cache.entries[0], &key, decoded, sizeof(decoded)));
    /* Unmarked exact-byte lookup is unchanged. */
    assert(vp_texture_snapshot_find(&owned_cache, &key, decoded, sizeof(decoded)) == decoded_copy);
    /* Same pointer with a different layout, mips or size never matches. */
    view = key; view.width++;
    assert(!vp_texture_snapshot_find_owned(&owned_cache, &view, decoded, sizeof(decoded)));
    assert(!vp_texture_snapshot_owned_match(&owned_cache.entries[0], &view, decoded, sizeof(decoded)));
    view = key; view.height++;
    assert(!vp_texture_snapshot_find_owned(&owned_cache, &view, decoded, sizeof(decoded)));
    view = key; view.format++;
    assert(!vp_texture_snapshot_find_owned(&owned_cache, &view, decoded, sizeof(decoded)));
    view = key; view.cube = 1;
    assert(!vp_texture_snapshot_find_owned(&owned_cache, &view, decoded, sizeof(decoded)));
    view = key; view.mips = 2;
    assert(!vp_texture_snapshot_find_owned(&owned_cache, &view, decoded, sizeof(decoded)));
    view = key; view.va++;
    assert(!vp_texture_snapshot_find_owned(&owned_cache, &view, decoded, sizeof(decoded)));
    assert(!vp_texture_snapshot_find_owned(&owned_cache, &key, decoded, sizeof(decoded) - 1));
    /* Retirement precedes a rewrite: the SAME address mutates and misses. */
    assert(vp_texture_snapshot_retire_owned(&owned_cache, decoded) == 1);
    assert(!vp_texture_snapshot_find_owned(&owned_cache, &key, decoded, sizeof(decoded)));
    decoded[0] ^= 0xff;
    assert(!vp_texture_snapshot_find_owned(&owned_cache, &key, decoded, sizeof(decoded)));
    assert(!vp_texture_snapshot_find(&owned_cache, &key, decoded, sizeof(decoded)));
    /* Free/reuse of the SAME address cannot inherit the old proof, even when
     * the reused bytes happen to equal the previous payload. */
    memcpy(decoded, decoded_copy, sizeof(decoded));
    assert(!vp_texture_snapshot_find_owned(&owned_cache, &key, decoded, sizeof(decoded)));
    assert(vp_texture_snapshot_find(&owned_cache, &key, decoded, sizeof(decoded)) == decoded_copy);
    /* Only an explicit associate-after-proof path may re-establish it. */
    assert(vp_texture_snapshot_associate_owned(&owned_cache, &key, decoded_copy,
                                               sizeof(decoded_copy), decoded));
    assert(vp_texture_snapshot_find_owned(&owned_cache, &key, decoded, sizeof(decoded)) == decoded_copy);
    view = key; view.va++;
    assert(!vp_texture_snapshot_associate_owned(&owned_cache, &view, decoded_copy,
                                                sizeof(decoded_copy), decoded));
    assert(!vp_texture_snapshot_associate_owned(&owned_cache, &key, decoded_copy,
                                                sizeof(decoded_copy), NULL));
    assert(vp_texture_snapshot_retire_owned(&owned_cache, decoded) == 1);
    assert(vp_texture_snapshot_retire_owned(&owned_cache, decoded) == 0);
    /* An unmarked payload has no association to inherit. */
    VPTextureSnapshots unmarked = {0};
    vp_texture_snapshot_remember(&unmarked, &key, decoded_copy, sizeof(decoded_copy));
    assert(!vp_texture_snapshot_find_owned(&unmarked, &key, decoded_copy, sizeof(decoded_copy)));
    assert(!vp_texture_snapshot_find_owned(&unmarked, &key, decoded, sizeof(decoded)));
    puts("TEXTURE_SNAPSHOT PASS exact-content mutation rollback view flush bound "
         "owned-proof retire-reuse layout");
    return 0;
}

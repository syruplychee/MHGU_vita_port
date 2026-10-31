/* Batch-owned immutable payloads. Descriptors/samplers remain per draw.
 * An unmarked lookup always requires exact current bytes; no hashes, VA-only
 * reuse or content versions are accepted.
 *
 * Owned-source association: decoded RGBA returned by the VPTextureCache
 * decoder is private memory that only the decoder writes or frees. A payload
 * may record that allocation as owned_decode_source only after it copied those
 * exact bytes or compared them equal to the current bytes. The decoder calls
 * the GXM retirement function before it rewrites or frees the allocation,
 * which clears every association to that pointer. A marked lookup may then
 * trust pointer + full layout key + byte size without re-reading the bytes.
 * The association never extends GPU backing lifetime: entries and upload
 * pages stay allocated until their normal GPU-complete cleanup. */
#ifndef VP_TEXTURE_SNAPSHOT_H
#define VP_TEXTURE_SNAPSHOT_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(__vita__)
#include <psp2/kernel/clib.h>
#endif

static inline int vp_texture_bytes_equal(const void *left, const void *right,
                                         size_t bytes) {
#if defined(__vita__)
    return sceClibMemcmp(left, right, bytes) == 0;
#else
    return memcmp(left, right, bytes) == 0;
#endif
}

#define VP_TEXTURE_SNAPSHOT_LIMIT 80u
typedef struct {
    uint64_t va;
    unsigned width, height, format, cube, mips;
} VPTextureSnapshotKey;
typedef struct {
    VPTextureSnapshotKey key;
    const void *data;
    size_t bytes;
    /* Non-NULL only for a decoder-owned allocation whose exact bytes were
     * copied into data or proved equal to it. Cleared by the decoder's
     * retirement event before any rewrite/free of that allocation. */
    const void *owned_decode_source;
} VPTextureSnapshot;
typedef struct {
    VPTextureSnapshot entries[VP_TEXTURE_SNAPSHOT_LIMIT];
    unsigned count;
} VPTextureSnapshots;

static inline int vp_texture_snapshot_key_equal(
    const VPTextureSnapshotKey *left, const VPTextureSnapshotKey *right) {
    return left->va == right->va && left->width == right->width &&
           left->height == right->height && left->format == right->format &&
           left->cube == right->cube && left->mips == right->mips;
}

/* Unmarked path: exact current bytes are the only identity. */
static inline const void *vp_texture_snapshot_find(
    const VPTextureSnapshots *cache, const VPTextureSnapshotKey *key,
    const void *current, size_t bytes) {
    if (!current || !bytes) return NULL;
    for (unsigned i = 0; i < cache->count; ++i) {
        const VPTextureSnapshot *e = &cache->entries[i];
        if (e->bytes == bytes && vp_texture_snapshot_key_equal(&e->key, key) &&
            vp_texture_bytes_equal(e->data, current, bytes))
            return e->data;
    }
    return NULL;
}

/* Marked path: the association was established by a prior copy/full compare
 * and is invalidated by the decoder before the source allocation changes. */
static inline int vp_texture_snapshot_owned_match(
    const VPTextureSnapshot *entry, const VPTextureSnapshotKey *key,
    const void *owned_source, size_t bytes) {
    return entry->data != NULL && owned_source != NULL &&
           entry->owned_decode_source == owned_source && entry->bytes == bytes &&
           vp_texture_snapshot_key_equal(&entry->key, key);
}

static inline const void *vp_texture_snapshot_find_owned(
    const VPTextureSnapshots *cache, const VPTextureSnapshotKey *key,
    const void *owned_source, size_t bytes) {
    if (!owned_source || !bytes) return NULL;
    for (unsigned i = 0; i < cache->count; ++i) {
        const VPTextureSnapshot *e = &cache->entries[i];
        if (vp_texture_snapshot_owned_match(e, key, owned_source, bytes))
            return e->data;
    }
    return NULL;
}

/* Associate an existing entry after the caller has compared the current bytes
 * equal. Only an owned decoder allocation may be named as the source. */
static inline int vp_texture_snapshot_associate_owned(
    VPTextureSnapshots *cache, const VPTextureSnapshotKey *key,
    const void *data, size_t bytes, const void *owned_source) {
    if (!data || !owned_source || !bytes) return 0;
    for (unsigned i = 0; i < cache->count; ++i) {
        VPTextureSnapshot *e = &cache->entries[i];
        if (e->data == data && e->bytes == bytes &&
            vp_texture_snapshot_key_equal(&e->key, key)) {
            e->owned_decode_source = owned_source;
            return 1;
        }
    }
    return 0;
}

static inline void vp_texture_snapshot_remember_owned(VPTextureSnapshots *cache,
    const VPTextureSnapshotKey *key, const void *owned, size_t bytes,
    const void *owned_source) {
    if (!owned || !bytes || cache->count == VP_TEXTURE_SNAPSHOT_LIMIT) return;
    VPTextureSnapshot *e = &cache->entries[cache->count++];
    e->key = *key;
    e->data = owned;
    e->bytes = bytes;
    e->owned_decode_source = owned_source;
}

static inline void vp_texture_snapshot_remember(VPTextureSnapshots *cache,
    const VPTextureSnapshotKey *key, const void *owned, size_t bytes) {
    vp_texture_snapshot_remember_owned(cache, key, owned, bytes, NULL);
}

/* Decoder update event: drop associations before a rewrite/free. Entry data,
 * upload pages and their GPU lifetime are not touched. */
static inline unsigned vp_texture_snapshot_retire_owned(
    VPTextureSnapshots *cache, const void *owned_source) {
    unsigned cleared = 0;
    if (!owned_source) return 0;
    for (unsigned i = 0; i < cache->count; ++i) {
        if (cache->entries[i].owned_decode_source == owned_source) {
            cache->entries[i].owned_decode_source = NULL;
            cleared++;
        }
    }
    return cleared;
}
#endif

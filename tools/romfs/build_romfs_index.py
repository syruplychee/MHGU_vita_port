#!/usr/bin/env python3
"""Build the RomFS image prefix and data map for the extracted 1.4.0 tree.

The locked game never sees a packed RomFS image in this port.  Instead this
tool emits the exact metadata a RomFS image would contain (header, name-hash
tables and directory/file metadata) plus a map from image data offsets to the
files of the extracted tree.  The loader serves IStorage reads from those two
artefacts, which keeps the original RomFS format and the original game logic
without rewriting 12 GiB of content.

Format facts below were verified against the locked SDK module itself:

  nn::fs::RomFsFileSystem::Initialize             sdk+0x51f34
  nn::fs::RomFsFileSystem::GetRequiredWorkingMemorySize sdk+0x51e54
  nn::fs::HierarchicalRomFileTable::Initialize    sdk+0x4d4e8
  ...::GetDirectoryEntry / GetFileEntry           sdk+0x4e120 / 0x4e334
  ...::FindInternal (directory/file)              sdk+0x502d8 / 0x50984
  ...::FindNextDirectory / FindNextFile           sdk+0x4ebc0 / 0x4ed54
  ...::CreateRootDirectory                        sdk+0x4d698

  * header is 0x50 bytes of ten little-endian u64 values
  * directory entry header 0x18 bytes, file entry header 0x20 bytes
  * name hash = (parent_offset ^ 0x75BCD15), then for each name byte
    c: hash = c ^ ror32(hash, 5); bucket = hash % (hash_table_size / 4)
  * entry name_size is the byte length without a NUL terminator
  * buckets are s32 metadata offsets, -1 terminates a chain

The output is generated game data and must stay outside Git.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

HEADER_SIZE = 0x50
DIR_ENTRY_HEADER = 0x18
FILE_ENTRY_HEADER = 0x20
NAME_HASH_SEED = 0x75BCD15
NONE = 0xFFFFFFFF
MAX_NAME = 767  # KeyValueRomStorageTemplate<..., 768>
MAP_MAGIC = 0x4D525056  # "VPRM"
MAP_VERSION = 1
# magic, version, file count, path bytes, reserved, tree bytes, data offset,
# image size.  The loader cross-checks the last three against the RomFS header.
MAP_HEADER = struct.Struct("<4sIIIIQQQ")
MAP_RECORD = struct.Struct("<QQII")


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & ~(boundary - 1)


def ror32(value: int, bits: int) -> int:
    return ((value >> bits) | (value << (32 - bits))) & 0xFFFFFFFF


def name_hash(parent: int, name: bytes) -> int:
    h = (parent ^ NAME_HASH_SEED) & 0xFFFFFFFF
    for byte in name:
        h = (byte ^ ror32(h, 5)) & 0xFFFFFFFF
    return h


@dataclass
class Dir:
    name: bytes
    parent: int
    parent_offset: int = 0
    offset: int = 0
    sibling: int = NONE
    hash: int = 0
    next_in_bucket: int = NONE
    child_dirs: list[int] = field(default_factory=list)
    child_files: list[int] = field(default_factory=list)
    host_path: bytes = b""


@dataclass
class File:
    name: bytes
    parent: int
    rel_path: bytes
    size: int
    offset: int = 0
    data_offset: int = 0
    hash: int = 0
    next_in_bucket: int = NONE
    sibling: int = NONE


def walk_tree(root: Path) -> tuple[list[Dir], list[File]]:
    dirs = [Dir(name=b"", parent=0, host_path=b"")]
    files: list[File] = []

    def scan(host: Path, dir_index: int, rel: bytes) -> None:
        try:
            with os.scandir(host) as iterator:
                entries = sorted(iterator, key=lambda e: os.fsencode(e.name))
        except OSError as error:
            raise SystemExit(f"cannot scan {host}: {error}") from error
        for entry in entries:
            name = os.fsencode(entry.name)
            if not name or b"/" in name or b"\0" in name:
                raise SystemExit(f"unsupported name under {host}: {name!r}")
            if len(name) > MAX_NAME:
                raise SystemExit(f"name too long for RomFS ({len(name)} bytes): {host / entry.name}")
            child_rel = rel + b"/" + name if rel else name
            try:
                # Do not follow links: the shipped tree must not hide content
                # that the RomFS image would not contain.
                info = entry.stat(follow_symlinks=False)
            except OSError as error:
                raise SystemExit(f"cannot stat {entry.path}: {error}") from error
            if entry.is_symlink():
                raise SystemExit(f"symlink is not RomFS content: {entry.path}")
            if entry.is_dir(follow_symlinks=False):
                index = len(dirs)
                dirs.append(Dir(name=name, parent=dir_index, host_path=child_rel))
                dirs[dir_index].child_dirs.append(index)
                scan(Path(entry.path), index, child_rel)
            elif entry.is_file(follow_symlinks=False):
                index = len(files)
                files.append(File(name=name, parent=dir_index, rel_path=child_rel, size=info.st_size))
                dirs[dir_index].child_files.append(index)
            else:
                raise SystemExit(f"unsupported directory entry: {entry.path}")
        dirs[dir_index].child_dirs.sort(key=lambda i: dirs[i].name)
        dirs[dir_index].child_files.sort(key=lambda i: files[i].name)

    scan(root, 0, b"")
    return dirs, files


def build(root: Path, output: Path, buckets: int) -> dict:
    dirs, files = walk_tree(root)

    # Metadata offsets, in a deterministic order.  The root is entry 0, which
    # is what FindPathRecursive and CreateRootDirectory assume.
    dir_offset = 0
    for directory in dirs:
        directory.offset = dir_offset
        directory.parent_offset = dirs[directory.parent].offset if directory.parent != 0 else 0
        dir_offset += align(DIR_ENTRY_HEADER + len(directory.name), 4)
    dir_meta_size = dir_offset

    file_offset = 0
    for entry in files:
        entry.offset = file_offset
        file_offset += align(FILE_ENTRY_HEADER + len(entry.name), 4)
    file_meta_size = file_offset

    # Canonical table layout: header, directory hash, directory metadata,
    # file hash, file metadata, data.  Every section is 0x10 aligned.
    dir_hash_offset = align(HEADER_SIZE, 0x10)
    dir_hash_size = buckets * 4
    dir_meta_offset = align(dir_hash_offset + dir_hash_size, 0x10)
    file_hash_offset = align(dir_meta_offset + dir_meta_size, 0x10)
    file_hash_size = buckets * 4
    file_meta_offset = align(file_hash_offset + file_hash_size, 0x10)
    data_offset = align(file_meta_offset + file_meta_size, 0x10)

    # Sibling links and first-child pointers.
    for directory in dirs:
        for position, child in enumerate(directory.child_dirs):
            dirs[child].sibling = (
                dirs[directory.child_dirs[position + 1]].offset
                if position + 1 < len(directory.child_dirs) else NONE
            )
        for position, child in enumerate(directory.child_files):
            files[child].sibling = (
                files[directory.child_files[position + 1]].offset
                if position + 1 < len(directory.child_files) else NONE
            )

    # Data placement and the image-offset -> tree-file map.
    tree_bytes = 0
    records = []
    for entry in files:
        tree_bytes = align(tree_bytes, 0x10)
        entry.data_offset = tree_bytes
        tree_bytes += entry.size
        records.append((data_offset + entry.data_offset, entry.size, entry))
    tree_bytes = align(tree_bytes, 0x10)
    image_size = data_offset + tree_bytes
    # File data offsets and IStorage offsets are u64 in the locked SDK; only
    # guest buffer addresses and sizes are 32-bit.

    # Bucket chains.  Insertion order does not matter; use creation order.
    dir_buckets = [NONE] * buckets
    for directory in dirs:
        directory.hash = name_hash(directory.parent_offset, directory.name)
        bucket = directory.hash % buckets
        directory.next_in_bucket = dir_buckets[bucket]
        dir_buckets[bucket] = directory.offset
    file_buckets = [NONE] * buckets
    for entry in files:
        entry.hash = name_hash(dirs[entry.parent].offset, entry.name)
        bucket = entry.hash % buckets
        entry.next_in_bucket = file_buckets[bucket]
        file_buckets[bucket] = entry.offset

    meta = bytearray(image_size)
    header = struct.pack(
        "<10Q", HEADER_SIZE,
        dir_hash_offset, dir_hash_size,
        dir_meta_offset, dir_meta_size,
        file_hash_offset, file_hash_size,
        file_meta_offset, file_meta_size,
        data_offset,
    )
    meta[0:HEADER_SIZE] = header

    for bucket, head in enumerate(dir_buckets):
        struct.pack_into("<I", meta, dir_hash_offset + bucket * 4, head)
    for bucket, head in enumerate(file_buckets):
        struct.pack_into("<I", meta, file_hash_offset + bucket * 4, head)

    for directory in dirs:
        base = dir_meta_offset + directory.offset
        struct.pack_into(
            "<IIIIII", meta, base,
            directory.parent_offset,
            directory.sibling,
            dirs[directory.child_dirs[0]].offset if directory.child_dirs else NONE,
            files[directory.child_files[0]].offset if directory.child_files else NONE,
            directory.next_in_bucket,
            len(directory.name),
        )
        meta[base + DIR_ENTRY_HEADER:base + DIR_ENTRY_HEADER + len(directory.name)] = directory.name

    for entry in files:
        base = file_meta_offset + entry.offset
        struct.pack_into(
            "<IIQQII", meta, base,
            dirs[entry.parent].offset,
            entry.sibling,
            entry.data_offset,
            entry.size,
            entry.next_in_bucket,
            len(entry.name),
        )
        meta[base + FILE_ENTRY_HEADER:base + FILE_ENTRY_HEADER + len(entry.name)] = entry.name

    prefix = bytes(meta[:data_offset])

    # Map: records sorted by image offset, then a NUL-terminated path blob.
    path_blob = bytearray()
    record_structs = []
    for image_offset, size, entry in records:
        path_offset = len(path_blob)
        path_blob += entry.rel_path + b"\0"
        record_structs.append(MAP_RECORD.pack(image_offset, size, path_offset, 0))

    map_data = bytearray()
    map_data += MAP_HEADER.pack(
        b"VPRM", MAP_VERSION, len(files), len(path_blob), 0,
        tree_bytes, data_offset, image_size,
    )
    for record in record_structs:
        map_data += record
    map_data += path_blob
    if len(map_data) >= 1 << 32:
        raise SystemExit("data map exceeds the 32-bit loader model")

    output.mkdir(parents=True, exist_ok=True)
    index_path = output / "romfs.index"
    map_path = output / "romfs.map"
    index_path.write_bytes(prefix)
    map_path.write_bytes(map_data)

    # The map header carries the image fields the loader cross-checks.
    header_fields = struct.unpack("<10Q", prefix[:HEADER_SIZE])
    if header_fields[0] != HEADER_SIZE or header_fields[9] != data_offset:
        raise SystemExit("internal error: header round-trip mismatch")

    manifest = {
        "tree": str(root),
        "tree_directories": len(dirs),
        "tree_files": len(files),
        "tree_bytes": tree_bytes,
        "data_offset": data_offset,
        "image_size": image_size,
        "bucket_count": buckets,
        "dir_hash_offset": dir_hash_offset,
        "dir_hash_size": dir_hash_size,
        "dir_meta_offset": dir_meta_offset,
        "dir_meta_size": dir_meta_size,
        "file_hash_offset": file_hash_offset,
        "file_hash_size": file_hash_size,
        "file_meta_offset": file_meta_offset,
        "file_meta_size": file_meta_size,
        "index_bytes": len(prefix),
        "index_sha256": hashlib.sha256(prefix).hexdigest(),
        "map_bytes": len(map_data),
        "map_sha256": hashlib.sha256(map_data).hexdigest(),
    }
    (output / "romfs.json").write_text(json.dumps(manifest, indent=2) + "\n")
    verify(prefix, map_data, dirs, files, root, buckets)
    return manifest


def verify(prefix: bytes, map_data: bytes, dirs: list[Dir], files: list[File],
           root: Path, buckets: int) -> None:
    header = struct.unpack("<10Q", prefix[:HEADER_SIZE])
    (header_size, dir_hash_offset, dir_hash_size, dir_meta_offset, dir_meta_size,
     file_hash_offset, file_hash_size, file_meta_offset, file_meta_size, data_offset) = header
    if header_size != HEADER_SIZE:
        raise SystemExit("verify: bad header size")
    if dir_hash_size != buckets * 4 or file_hash_size != buckets * 4:
        raise SystemExit("verify: bad hash table size")
    if dir_meta_offset + dir_meta_size > len(prefix) or file_meta_offset + file_meta_size > len(prefix):
        raise SystemExit("verify: metadata beyond prefix")

    magic, version, count, path_bytes, reserved, tree_bytes, map_data_offset, image_size = \
        MAP_HEADER.unpack_from(map_data, 0)
    if magic != b"VPRM" or version != MAP_VERSION or reserved != 0:
        raise SystemExit("verify: bad map header")
    if count != len(files) or map_data_offset != data_offset:
        raise SystemExit("verify: map count/offset mismatch")
    if image_size != data_offset + tree_bytes:
        raise SystemExit("verify: map image size mismatch")

    def dir_entry(offset: int) -> tuple[int, int, int, int, int, int, bytes]:
        if offset + DIR_ENTRY_HEADER > dir_meta_size:
            raise SystemExit(f"verify: directory offset 0x{offset:x} out of range")
        base = dir_meta_offset + offset
        parent, sibling, child, first_file, nxt, name_size = struct.unpack_from("<IIIIII", prefix, base)
        name = prefix[base + DIR_ENTRY_HEADER:base + DIR_ENTRY_HEADER + name_size]
        return parent, sibling, child, first_file, nxt, name_size, name

    def file_entry(offset: int) -> tuple[int, int, int, int, int, int, bytes]:
        if offset + FILE_ENTRY_HEADER > file_meta_size:
            raise SystemExit(f"verify: file offset 0x{offset:x} out of range")
        base = file_meta_offset + offset
        parent, sibling, data, size, nxt, name_size = struct.unpack_from("<IIQQII", prefix, base)
        name = prefix[base + FILE_ENTRY_HEADER:base + FILE_ENTRY_HEADER + name_size]
        return parent, sibling, data, size, nxt, name_size, name

    root_fields = dir_entry(0)
    if root_fields[0] != 0:
        raise SystemExit("verify: root parent is not 0")

    seen_dirs: set[int] = set()
    seen_files: set[int] = set()
    queue = [0]
    while queue:
        offset = queue.pop()
        if offset in seen_dirs:
            raise SystemExit(f"verify: directory 0x{offset:x} appears twice")
        seen_dirs.add(offset)
        parent, sibling, child, first_file, nxt, name_size, name = dir_entry(offset)
        if offset != 0:
            expected = name_hash(parent, name)
            bucket = expected % buckets
            chain = struct.unpack_from("<I", prefix, dir_hash_offset + bucket * 4)[0]
            while chain != NONE and chain != offset:
                chain = dir_entry(chain)[4]
            if chain != offset:
                raise SystemExit(f"verify: directory {name!r} not found through its hash bucket")
        next_dir = child
        while next_dir != NONE:
            queue.append(next_dir)
            next_dir = dir_entry(next_dir)[1]
        next_file = first_file
        while next_file != NONE:
            if next_file in seen_files:
                raise SystemExit(f"verify: file offset 0x{next_file:x} appears twice")
            seen_files.add(next_file)
            fparent, fsibling, data, size, fnxt, fname_size, fname = file_entry(next_file)
            if fparent != offset:
                raise SystemExit("verify: file parent mismatch")
            expected = name_hash(offset, fname)
            bucket = expected % buckets
            chain = struct.unpack_from("<I", prefix, file_hash_offset + bucket * 4)[0]
            while chain != NONE and chain != next_file:
                chain = file_entry(chain)[4]
            if chain != next_file:
                raise SystemExit(f"verify: file {fname!r} not found through its hash bucket")
            if data + size > tree_bytes:
                raise SystemExit(f"verify: file {fname!r} data out of range")
            next_file = fsibling

    if len(seen_dirs) != len(dirs) or len(seen_files) != len(files):
        raise SystemExit(f"verify: reachable dirs/files {len(seen_dirs)}/{len(seen_files)} "
                         f"!= built {len(dirs)}/{len(files)}")

    # Cross-check every map record against a reachable file entry and the tree.
    position = MAP_HEADER.size
    paths_start = MAP_HEADER.size + count * MAP_RECORD.size
    if paths_start + path_bytes != len(map_data):
        raise SystemExit("verify: map path blob size mismatch")
    last_end = 0
    total = 0
    for _ in range(count):
        image_off, size, path_off, _reserved = MAP_RECORD.unpack_from(map_data, position)
        position += MAP_RECORD.size
        if image_off < last_end or image_off + size > image_size:
            raise SystemExit("verify: map records overlap or exceed the image")
        last_end = image_off + size
        total += size
        end = map_data.index(b"\0", paths_start + path_off)
        rel = bytes(map_data[paths_start + path_off:end])
        host = root / os.fsdecode(rel)
        if not host.is_file():
            raise SystemExit(f"verify: mapped file missing from tree: {host}")
        if host.stat().st_size != size:
            raise SystemExit(f"verify: mapped size differs from tree: {host}")
    if total != sum(entry.size for entry in files):
        raise SystemExit("verify: map byte total mismatch")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", required=True, type=Path, help="extracted RomFS tree root")
    parser.add_argument("--output", required=True, type=Path, help="output directory for romfs.index/romfs.map")
    parser.add_argument("--buckets", type=int, default=0x10000, help="hash buckets per table (default 65536)")
    args = parser.parse_args()
    if args.buckets <= 0 or args.buckets & 1:
        parser.error("--buckets must be a positive even number")
    if not args.tree.is_dir():
        parser.error(f"--tree is not a directory: {args.tree}")
    manifest = build(args.tree, args.output, args.buckets)
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())

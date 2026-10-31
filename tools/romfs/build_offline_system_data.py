"""Self-authored offline NgWord2 archive; no Nintendo firmware or word list.

The AC payload is a gzip stream of 4096 zero bytes: an empty dictionary.
The title still opens, decompresses and executes its own matching code.
This intentionally provides no profanity coverage and is for local play only.
Format cross-check: mhgu-runtime/core/file_sys/system_archive/ng_word.cpp;
no source or compressed bytes are copied from that implementation.
"""
import argparse
import gzip
from pathlib import Path
import struct
import tempfile
from build_romfs_index import build, MAP_HEADER, MAP_RECORD


def create(output):
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)/'tree'
        root.mkdir()
        dictionary = gzip.compress(bytes(4096), mtime=0)
        for language in [str(i) for i in range(16)]+['common']:
            for suffix in ('b1', 'b2', 'not_b'):
                (root/f'ac_{language}_{suffix}_nx').write_bytes(dictionary)
        (root/'version.dat').write_bytes(bytes((0, 0, 0, 0x1a)))
        meta = Path(temp)/'index'
        manifest = build(root, meta, 64)
        image = bytearray(manifest['image_size'])
        prefix = (meta/'romfs.index').read_bytes()
        image[:len(prefix)] = prefix
        mapping = (meta/'romfs.map').read_bytes()
        count = MAP_HEADER.unpack_from(mapping)[2]
        paths = MAP_HEADER.size+count*MAP_RECORD.size
        for i in range(count):
            offset, size, name, _ = MAP_RECORD.unpack_from(mapping, MAP_HEADER.size+i*MAP_RECORD.size)
            name = mapping[paths+name:].split(b'\0', 1)[0].decode()
            data = (root/name).read_bytes()
            assert len(data) == size
            image[offset:offset+size] = data
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(image)
        assert count == 52
        assert gzip.decompress(dictionary) == bytes(4096)
        print(f'offline NgWord2: {count} files, {len(image)} bytes; dictionary entries=0')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    create(parser.parse_args().output)

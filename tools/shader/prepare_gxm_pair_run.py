"""Bind a P4 probe build and resume log to exact source and bundle identities."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def seal(bundle, source_paths=None):
    source_paths = source_paths or ('src/probes/gxm_s2_pair_probe.c',
                    'src/platform/static_shader_index.c',
                    'src/platform/static_shader_index.h',
                    'CMakeLists.txt', 'tools/shader/prepare_gxm_pair_run.py')
    sources = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
               for name in source_paths}
    raw = (bundle / 'shader-index.bin').read_bytes()
    index_sha = hashlib.sha256(raw).hexdigest()
    manifest = json.loads((bundle / 'pair-manifest.json').read_text())
    if manifest['index_sha256'] != index_sha:
        raise ValueError('manifest/index SHA mismatch')
    source_sha = hashlib.sha256(json.dumps(sources, sort_keys=True).encode()).hexdigest()
    identity = hashlib.sha256((source_sha + index_sha).encode()).hexdigest()
    fnv = 0xcbf29ce484222325
    for byte in raw:
        fnv = ((fnv ^ byte) * 0x100000001b3) & ((1 << 64) - 1)
    header = ('#ifndef VP_PAIR_RUN_CONTRACT_H\n#define VP_PAIR_RUN_CONTRACT_H\n'
              f'#define VP_PAIR_RUN_ID "{identity}"\n'
              f'#define VP_PAIR_INDEX_SHA256 "{index_sha}"\n'
              f'#define VP_PAIR_SOURCE_SHA256 "{source_sha}"\n'
              f'#define VP_PAIR_INDEX_FNV 0x{fnv:016x}ull\n#endif\n')
    (bundle / 'pair_run_contract.h').write_text(header, encoding='ascii', newline='\n')
    result = dict(schema=1, identity=identity, index_sha256=index_sha,
                  source_sha256=source_sha, sources=sources,
                  manifest_sha256=hashlib.sha256((bundle / 'pair-manifest.json').read_bytes()).hexdigest())
    (bundle / 'run-contract.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--source', action='append', help='explicit source path relative to project root')
    args = parser.parse_args()
    print(json.dumps(seal(args.bundle, args.source), indent=2))

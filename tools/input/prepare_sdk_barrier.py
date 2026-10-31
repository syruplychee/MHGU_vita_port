"""Bind every legacy ARMv6 data-synchronization barrier in the locked SDK module.

The SDK was built for ARMv7 but emits the pre-ARMv7 `mcr p15,0,Rt,c7,c10,5`
encoding for DSB around its ldrex/strex atomics.  Dynarmic recognizes only the
architectural `dsb` encodings (0xF57FF04x); the legacy encoding reaches its
generic coprocessor path and Vita3K's CP15 model has no handler for it, which
trips a translation assertion.  Each reviewed site is rewritten in place to
`dsb sy`, preserving the full-system barrier with no register or memory side
effect.  The contract binds module identity, module-relative offset, ARM state
and the expected original word.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

import lz4.block

from nso_reloc_model import decode, require

ROOT = Path(__file__).resolve().parents[2]
# cond(1110) 1110 0000 0111 Rt 1111 101 1 1010 (MCR p15,0,Rt,c7,c10,5)
BARRIER_MASK = 0xFFFF0FFF
BARRIER_WORD = 0xEE070FBA
# cond(1111) 0101 0111 1111 1111 0000 0100 1111 (DSB sy)
DSB_WORD = 0xF57FF04F


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exefs', type=Path, default=ROOT.parent / 'unpacked/update-mhgu/game/0')
    p.add_argument('--plan', type=Path, default=ROOT / 'config/patch-plans/vp022-sdk-barrier.json')
    p.add_argument('--header', type=Path, default=ROOT / 'src/generated/sdk_barrier_contract.h')
    p.add_argument('--verify', action='store_true')
    a = p.parse_args()

    lock = json.loads((ROOT / 'config/target-1.4.0.json').read_text())
    ident = next(m for m in lock['modules'] if m['name'] == 'sdk')
    module = decode(a.exefs / 'sdk', ident, lz4.block.decompress)
    text = module.segments[0]
    sites = []
    for off in range(0, text.size, 4):
        word = struct.unpack_from('<I', module.image, off)[0]
        if word & BARRIER_MASK != BARRIER_WORD:
            continue
        sites.append({
            'offset': off,
            'expected_word': word,
            'state': 'ARM',
            'bytes': 4,
            'replacement_word': DSB_WORD,
            'abi': ('MCR p15,0,Rt,c7,c10,5 is the legacy ARMv6 drain-write-buffer '
                    'encoding of DSB; rewrite in place to architectural DSB sy; '
                    'no register, memory or control-flow side effect'),
        })
    # The locked SDK has exactly 12 such sites, all around the ldrex/strex
    # atomics in libc.  A different count means the input or scan changed.
    require(len(sites) == 12, 'reviewed legacy barrier count changed')
    plan = {
        'schema': 1,
        'date': '2026-10-17',
        'kind': 'sdk-barrier-sites',
        'purpose': 'Rewrite legacy ARMv6 DSB encodings to architectural DSB sy; never execute MCR p15,c7,c10,5',
        'module': 'sdk',
        'source_sha256': ident['sha256'],
        'build_id': ident['build_id'],
        'text_bytes': text.size,
        'site_count': len(sites),
        'replacement_word': DSB_WORD,
        'apply_ready': True,
        'sites': sites,
    }
    body = json.dumps(plan, indent=1) + '\n'
    lines = [
        '#ifndef VP_SDK_BARRIER_CONTRACT_H',
        '#define VP_SDK_BARRIER_CONTRACT_H',
        '#define VP_SDK_BARRIER_WORD 0x%08xu' % DSB_WORD,
        'typedef struct { unsigned offset, word; } VPSDKBarrierSite;',
        'static const VPSDKBarrierSite vp_sdk_barrier_sites[] = {',
    ]
    for s in sites:
        lines.append('{0x%xu,0x%xu},' % (s['offset'], s['expected_word']))
    lines += ['};', '#endif', '']
    header = '\n'.join(lines)
    if a.verify:
        require(a.plan.read_text() == body, 'stale SDK barrier plan; regenerate with prepare_sdk_barrier.py')
        require(a.header.read_text() == header, 'stale SDK barrier header; regenerate with prepare_sdk_barrier.py')
    else:
        a.plan.write_text(body, encoding='utf-8', newline='\n')
        a.header.write_text(header, encoding='utf-8', newline='\n')
    print('SDK_BARRIER sites=%d replacement=0x%08x verified=%d' % (len(sites), DSB_WORD, a.verify))


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print('SDK_BARRIER_ERROR %s' % exc, file=sys.stderr)
        raise

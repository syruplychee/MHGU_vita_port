"""Bind every ARM generic-timer read in the locked SDK module (read-only).

The SDK reads the AArch32 physical timer with `mrrc p15,0,Rt,Rt2,c14`.  User
mode cannot issue that on the Vita, and Vita3K's CP15 model does not implement
it, so each reviewed site is replaced by a branch to a loader-owned island that
asks the host for a 64-bit tick count and resumes at the instruction after the
MRRC.  The contract binds module identity, module-relative offset, ARM state and
the expected original word; the destination register pair is part of the ABI.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

import lz4.block

from nso_reloc_model import decode, require

ROOT = Path(__file__).resolve().parents[2]
# cond(1110) 1100 0101 Rt Rt2 1111 0000 1110 (MRRC p15,0,Rt,Rt2,c14)
MRRC_MASK = 0xFFF00F0F
MRRC_WORD = 0xEC500F0E


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exefs', type=Path, default=ROOT.parent / 'unpacked/update-mhgu/game/0')
    p.add_argument('--plan', type=Path, default=ROOT / 'config/patch-plans/vp022-sdk-timer.json')
    p.add_argument('--header', type=Path, default=ROOT / 'src/generated/sdk_timer_contract.h')
    p.add_argument('--verify', action='store_true')
    a = p.parse_args()

    lock = json.loads((ROOT / 'config/target-1.4.0.json').read_text())
    ident = next(m for m in lock['modules'] if m['name'] == 'sdk')
    module = decode(a.exefs / 'sdk', ident, lz4.block.decompress)
    text = module.segments[0]
    sites = []
    for off in range(0, text.size, 4):
        word = struct.unpack_from('<I', module.image, off)[0]
        if word & MRRC_MASK != MRRC_WORD:
            continue
        rt = (word >> 12) & 0xF
        rt2 = (word >> 16) & 0xF
        require(rt % 2 == 0 and rt2 == rt + 1 and rt <= 10,
                'MRRC destination pair is not a bridgeable even/odd pair at 0x%x' % off)
        sites.append({
            'offset': off,
            'expected_word': word,
            'state': 'ARM',
            'bytes': 4,
            'rt': rt,
            'rt2': rt2,
            'abi': ('MRRC p15,0,Rt,Rt2,c14 reads the 64-bit generic timer; '
                    'host writes the 64-bit monotonic tick to Rt:Rt2 and resumes at site+4; '
                    'no guest memory is accessed'),
        })
    # The locked SDK has exactly 32 such reads; every one belongs to nn::os
    # timer/timeout code.  A different count means the input or scan changed.
    require(len(sites) == 32, 'reviewed generic-timer read count changed')
    plan = {
        'schema': 1,
        'date': '2026-10-17',
        'kind': 'sdk-timer-sites',
        'purpose': 'Replace AArch32 generic-timer reads with a bound host monotonic tick service; never execute MRRC p15,c14',
        'module': 'sdk',
        'source_sha256': ident['sha256'],
        'build_id': ident['build_id'],
        'text_bytes': text.size,
        'site_count': len(sites),
        'tick_frequency_hz': 19200000,
        'apply_ready': True,
        'sites': sites,
    }
    body = json.dumps(plan, indent=1) + '\n'
    lines = [
        '#ifndef VP_SDK_TIMER_CONTRACT_H',
        '#define VP_SDK_TIMER_CONTRACT_H',
        'typedef struct { unsigned offset, word, rt; } VPSDKTimerSite;',
        'static const VPSDKTimerSite vp_sdk_timer_sites[] = {',
    ]
    for s in sites:
        lines.append('{0x%xu,0x%xu,%du},' % (s['offset'], s['expected_word'], s['rt']))
    lines += ['};', '#endif', '']
    header = '\n'.join(lines)
    if a.verify:
        require(a.plan.read_text() == body, 'stale SDK timer plan; regenerate with prepare_sdk_timer.py')
        require(a.header.read_text() == header, 'stale SDK timer header; regenerate with prepare_sdk_timer.py')
    else:
        a.plan.write_text(body, encoding='utf-8', newline='\n')
        a.header.write_text(header, encoding='utf-8', newline='\n')
    print('SDK_TIMER sites=%d frequency=%d verified=%d' % (len(sites), plan['tick_frequency_hz'], a.verify))


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print('SDK_TIMER_ERROR %s' % exc, file=sys.stderr)
        raise

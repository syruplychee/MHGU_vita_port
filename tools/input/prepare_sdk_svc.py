"""Bind every ARM SVC site in the locked SDK module (read-only; no game payload).

The SDK keeps its kernel interface in a veneer table.  Each site is replaced by
a branch to a loader-owned island that forwards the *original* registers to a
host service and resumes at the instruction after the SVC.  The contract binds
module identity, module-relative offset and the expected original word.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

import lz4.block

from nso_reloc_model import decode, require

ROOT = Path(__file__).resolve().parents[2]
SVC_MASK = 0xFF000000
SVC_WORD = 0xEF000000


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exefs', type=Path, default=ROOT.parent / 'unpacked/update-mhgu/game/0')
    p.add_argument('--plan', type=Path, default=ROOT / 'config/patch-plans/vp022-sdk-svc.json')
    p.add_argument('--header', type=Path, default=ROOT / 'src/generated/sdk_svc_contract.h')
    p.add_argument('--verify', action='store_true')
    a = p.parse_args()

    lock = json.loads((ROOT / 'config/target-1.4.0.json').read_text())
    ident = next(m for m in lock['modules'] if m['name'] == 'sdk')
    module = decode(a.exefs / 'sdk', ident, lz4.block.decompress)
    text = module.segments[0]
    # The locked SDK keeps every real syscall in one contiguous veneer table.
    # A full-text word scan also matches literal data (0x41cfc0) and Thumb
    # mis-decodes, so the scan is bounded to the reviewed table and required
    # to find the 56 ARM sites (the older audit missed SVC 0x1c and 0x1f).
    begin, end = 0xC0600, 0xC0A40
    require(end <= text.size, 'SVC veneer table outside text')
    sites = []
    for off in range(begin, end, 4):
        word = struct.unpack_from('<I', module.image, off)[0]
        if word & SVC_MASK != SVC_WORD:
            continue
        svc = word & 0xFFFFFF
        require(svc <= 0x7F, 'unexpected SVC immediate')
        sites.append({
            'offset': off,
            'expected_word': word,
            'svc': svc,
            'state': 'ARM',
            'bytes': 4,
            'abi': 'Original registers forwarded; kernel return in r0 and r1:r2; continuation is site+4',
        })
    require(len(sites) == 56, 'reviewed SVC veneer count changed')
    plan = {
        'schema': 1,
        'date': '2026-10-17',
        'kind': 'sdk-svc-sites',
        'purpose': 'Route every original AArch32 SVC site through a bound native service; never execute a Vita kernel call',
        'module': 'sdk',
        'source_sha256': ident['sha256'],
        'build_id': ident['build_id'],
        'text_bytes': text.size,
        'site_count': len(sites),
        'svc_ids': sorted({s['svc'] for s in sites}),
        'apply_ready': True,
        'sites': sites,
    }
    body = json.dumps(plan, indent=1) + '\n'
    lines = [
        '#ifndef VP_SDK_SVC_CONTRACT_H',
        '#define VP_SDK_SVC_CONTRACT_H',
        'typedef struct { unsigned offset, word, svc; } VPSDKSvcSite;',
        'static const VPSDKSvcSite vp_sdk_svc_sites[] = {',
    ]
    for s in sites:
        lines.append('{0x%xu,0x%xu,%du},' % (s['offset'], s['expected_word'], s['svc']))
    lines += ['};', '#endif', '']
    header = '\n'.join(lines)
    if a.verify:
        require(a.plan.read_text() == body, 'stale SDK SVC plan; regenerate with prepare_sdk_svc.py')
        require(a.header.read_text() == header, 'stale SDK SVC header; regenerate with prepare_sdk_svc.py')
    else:
        a.plan.write_text(body, encoding='utf-8', newline='\n')
        a.header.write_text(header, encoding='utf-8', newline='\n')
    print('SDK_SVC sites=%d ids=%s verified=%d' % (len(sites), plan['svc_ids'], a.verify))


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print('SDK_SVC_ERROR %s' % exc, file=sys.stderr)
        raise

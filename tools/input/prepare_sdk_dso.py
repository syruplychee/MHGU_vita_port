"""Generate or verify the reviewed SDK experiment binding header (no game payload)."""
import argparse
import json
from pathlib import Path


def main():
    root=Path(__file__).resolve().parents[2]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,default=root/'src/generated/sdk_dso_contract.h')
    p.add_argument('--verify',action='store_true');a=p.parse_args()
    plan=json.loads((root/'config/patch-plans/vp022-sdk-dso.json').read_text())
    locked=json.loads((root/'config/target-1.4.0.json').read_text())['modules'][3]
    if plan['source_sha256']!=locked['sha256'] or plan['build_id']!=locked['build_id']: raise ValueError('wrong SDK identity')
    seen=set();lines=['#ifndef VP_SDK_DSO_CONTRACT_H','#define VP_SDK_DSO_CONTRACT_H',
        'typedef struct { unsigned offset,word,bytes; unsigned char sha[32]; } VPSDKDSOPatch;',
        'static const VPSDKDSOPatch vp_sdk_dso_patches[] = {']
    for patch in plan['patches']:
        off=patch['offset'];size=patch['function_bytes'];h=patch['sha256']
        if off in seen or off%4 or size<4 or len(bytes.fromhex(h))!=32 or patch['state']!='ARM': raise ValueError('invalid patch binding')
        seen.add(off)
        lines.append('{0x%xu,0x%xu,%du,{%s}},'%(off,int.from_bytes(bytes.fromhex(patch['expected_original_bytes']),'little'),size,
            ','.join('0x'+h[i:i+2] for i in range(0,64,2))))
    lines+=['};','#endif',''];text='\n'.join(lines)
    if a.verify:
        if a.output.read_text()!=text: raise ValueError('stale SDK DSO header; regenerate with prepare_sdk_dso.py')
    else:a.output.write_text(text,encoding='utf-8',newline='\n')
    print('SDK_DSO_CONTRACT patches=%d verified=%d'%(len(seen),a.verify))


if __name__=='__main__':main()

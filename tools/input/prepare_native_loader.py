"""Generate identity-only C declarations for the locked loader. No game bytes."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    lock = json.loads((root/'config/target-1.4.0.json').read_text())
    def array(value):
        return '{'+','.join('0x'+value[i:i+2] for i in range(0, len(value), 2))+'}'
    lines = ['/* Generated from config/target-1.4.0.json; identities only. */',
             '#ifndef VP_TARGET_IDENTITY_H', '#define VP_TARGET_IDENTITY_H',
             'typedef struct { const char *name; unsigned bytes; unsigned char sha[32], build[32]; } VPIdentity;',
             'static const VPIdentity vp_identities[4] = {']
    for m in lock['modules']:
        lines.append('    {"%s",%du,%s,%s},' % (m['name'], m['size'], array(m['sha256']), array(m['build_id'])))
    lines += ['};', '#endif', '']
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('\n'.join(lines), encoding='utf-8', newline='\n')
    archive=json.loads((root/'config/patch-plans/vp020-1.4.0-archive.json').read_text())
    lines=['/* Generated bound TLS sites; no unapproved UDIV sites. */',
           '#ifndef VP_TARGET_TLS_H','#define VP_TARGET_TLS_H',
           'typedef struct { unsigned offset, original, kind; } VPTLSSite;',
           'static const VPTLSSite vp_tls_sites[] = {']
    kinds={'read_ro':0,'read_ro_r0':1,'read_ro_r2':2,'write_rw':3,'write_rw_r0':4}
    islands={x['id']:x for x in archive['islands']}
    for site in archive['sites']:
        if site['module']!='sdk' or not site['apply_ready']:
            continue
        symbol=islands[site['island_id']]['template_symbol']
        kind=symbol.removeprefix('vp_tls_island_').removesuffix('_begin')
        lines.append('    {0x%xu,0x%xu,%du},' % (site['module_offset'],int.from_bytes(bytes.fromhex(site['expected_original_bytes']),'little'),kinds[kind]))
    lines+=['};','#endif','']
    args.output.with_name('target_tls.h').write_text('\n'.join(lines),encoding='utf-8',newline='\n')
    boundary=json.loads((root/'config/patch-plans/vp022-sdk-init-boundary.json').read_text())
    sdk=next(m for m in lock['modules'] if m['name']=='sdk')
    if boundary['source_sha256']!=sdk['sha256'] or boundary['build_id']!=sdk['build_id']:
        raise ValueError('startup boundary does not match locked SDK')
    lines=['/* Generated from bound VP02.2 diagnostic plan; not a successful service. */',
           '#ifndef VP_TARGET_STARTUP_H','#define VP_TARGET_STARTUP_H',
           f'#define VP_DSO_OFFSET {boundary["module_offset"]}u',
           f'#define VP_DSO_WORD 0x{int.from_bytes(bytes.fromhex(boundary["expected_original_bytes"]),"little"):08x}u',
           f'#define VP_DSO_BYTES {boundary["function_bytes"]}u',
           'static const unsigned char vp_dso_sha256[32]='+array(boundary['function_sha256'])+';',
           '#endif','']
    args.output.with_name('target_startup.h').write_text('\n'.join(lines),encoding='utf-8',newline='\n')


if __name__ == '__main__':
    main()

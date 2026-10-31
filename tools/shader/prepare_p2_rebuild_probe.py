"""Rebuild the P2 regression shaders from the actual current emitter helpers.

Keep the locked probe's cases and raw compiler controls. Replace the original
three helper programs and both fix slots with current emitted definitions.
No game shader is used here. The baseline VPK is checked against run7.
"""
import argparse
import json
from pathlib import Path
import subprocess
import zipfile

from audit_p2_rebuild import helper_parts
from compile_static_candidates import digest, valid_gxp
from convert_cache import compile_serial
from shader_ir import emit_cg, lower

ROOT = Path(__file__).resolve().parents[2]


def emitted_helpers():
    head = 'layout(location=0)in vec4 uv;layout(location=0)out vec4 c;'
    expressions = {
        'vp_float_bits_to_uint_numeric': 'uint a=floatBitsToUint(uv.x);c=vec4(float(a));',
        'vp_float_bits_to_int_numeric': 'uint a=floatBitsToUint(uv.x);c=vec4(float(int(a)));',
        'vp_uint_to_float': 'uint a=1u;c=vec4(float(a));',
    }
    return {name: helper_parts(emit_cg(lower(head + 'void main(){' + body + '}',
                                           'fragment'))[0])[0][name]
            for name, body in expressions.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-vpk', type=Path, default=ROOT / 'build/gxm-s2u/vita_port_gxm_s2_uint.vpk')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--optimization', choices=('0', '1'), default='0')
    args = parser.parse_args()
    historical = json.loads((ROOT / 'config/validation/gxm-s2-p2-20261023.json').read_text(encoding='utf-8'))
    if digest(args.baseline_vpk) != historical['vpk_sha256']:
        raise ValueError('baseline VPK differs from run7')
    if args.output.exists():
        raise ValueError('use a fresh output directory')
    args.output.mkdir(parents=True)
    with zipfile.ZipFile(args.baseline_vpk) as archive:
        for name in archive.namelist():
            if name.startswith('s2u_') and name.endswith('.gxp') and '/' not in name:
                (args.output / name).write_bytes(archive.read(name))
    helpers = emitted_helpers()
    slots = {'s2u_decode_f': 'vp_float_bits_to_uint_numeric',
             's2u_decode_int_f': 'vp_float_bits_to_int_numeric',
             's2u_uint_to_float_f': 'vp_uint_to_float',
             's2u_decode_fix_f': 'vp_float_bits_to_uint_numeric',
             's2u_uint_to_float_fix_f': 'vp_uint_to_float'}
    config = ROOT / 'local/shader-tools/shacc.conf'
    executable = ROOT / 'local/shader-tools/shacc-bin/Vita3K-Shacc-windows-x86_64/vita3k-shacc.exe'
    values = dict(line.split('=', 1) for line in config.read_text(encoding='utf-8').splitlines() if line)
    compiler = Path(values['root']) / 'fs/ur0/data/external/libshacccg.suprx'
    lock = json.loads((ROOT / 'config/graphics/shader-toolchain.json').read_text(encoding='utf-8'))
    if (digest(executable) != lock['shacc']['executable_sha256'] or
            digest(Path(values['service-vpk'])) != lock['shacc']['service_vpk_sha256'] or
            digest(compiler) != lock['libshacccg']['sha256']):
        raise ValueError('compiler identity mismatch')
    records = []
    for slot, helper in slots.items():
        source, target = [args.output / (slot + ext) for ext in ('.cg', '.gxp')]
        arg = 'int(c17[0].x)' if helper == 'vp_uint_to_float' else 'c17[0].x'
        source.write_text(helpers[helper] + '\nvoid main(out float4 frag : COLOR0, uniform float4 c17[1]) {\n'
                          '  frag = float4(%s(%s), 0.0, 0.0, 1.0);\n}\n' % (helper, arg),
                          encoding='utf-8', newline='\n')
        target.unlink(missing_ok=True)
        result = compile_serial([str(executable), '--config', str(config), '-profile', 'sce_fp_psp2',
                                 '-O' + args.optimization, '-o', str(target.resolve()), str(source.resolve()),
                                 '--timeout', '40'], config.parent)
        (args.output / (slot + '.compile.txt')).write_bytes(result.stdout + result.stderr)
        ok, _ = valid_gxp(target)
        if result.returncode or not ok:
            raise RuntimeError('probe shader failed: ' + slot)
        records.append({'program': slot, 'helper': helper, 'source_sha256': digest(source),
                        'gxp_sha256': digest(target)})
        print(slot, 'COMPILED', flush=True)
    manifest = {'schema': 1, 'baseline_vpk_sha256': digest(args.baseline_vpk),
                'shader_ir_sha256': digest(ROOT / 'tools/shader/shader_ir.py'),
                'tool_sha256': digest(Path(__file__)), 'compiler_sha256': digest(compiler),
                'shacc_sha256': digest(executable), 'config_sha256': digest(config),
                'probe_source_sha256': digest(ROOT / 'src/probes/gxm_s2_uint_probe.c'),
                'flags': ['-O' + args.optimization], 'recompiled': records,
                'gxp_sha256': {p.name: digest(p) for p in sorted(args.output.glob('*.gxp'))}}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n',
                                            encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()

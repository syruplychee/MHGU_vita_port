"""Add logical fragment coordinates to the locked bank's screen-space shaders."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from convert_cache import compile_serial


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('base', 'cg', 'shadow_cg', 'compiler_records', 'shacc', 'shacc_config', 'output'):
        parser.add_argument('--' + name.replace('_', '-'), type=Path, required=True)
    args = parser.parse_args()
    base = args.base.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    index = json.loads((base / 'shader-index.json').read_text())
    spec = {'schema': 1, 'base_index_sha256': digest(base / 'shader-index.bin'),
            'base_shader_index_json_sha256': digest(base / 'shader-index.json'),
            'replace': [], 'add': []}
    sources = {f.stem: f for f in args.cg.glob('*.cg')}
    sources.update({f.parent.name: f for f in args.shadow_cg.glob('*/shader.frag.cg')})
    for variant in index['variants']:
        stage = index['stages'][variant['stage_index']]
        if stage['stage'] != 'frag':
            continue
        source = sources[stage['shader_id']]
        cg = source.read_text()
        if not re.search(r'vp_fragcoord\.[xyzw]*[xy]', cg):
            continue
        record = json.loads((args.compiler_records / (stage['shader_id'] + '.json')).read_text())
        if record['cg_sha256'] != digest(source) or record['gxp_sha256'] != variant['gxp_sha256']:
            raise ValueError('source/compiler receipt does not match the base variant: ' + stage['shader_id'])
        # Preserve z/w, including shaders that explicitly write depth.
        cg = cg.replace('float4 vp_fragcoord : WPOS,',
                        'float4 vp_fragcoord : WPOS,\n'
                        '          uniform float4 vp_fragcoord_scale,', 1)
        # WPOS is an input register. Scale reads rather than writing it back.
        cg = re.sub(r'\bvp_fragcoord\.([xyzw]+)\b',
                    lambda match: ('(' + match[0] + ' * vp_fragcoord_scale.' +
                                   match[1] + ')')
                    if 'x' in match[1] or 'y' in match[1] else match[0], cg)
        name = stage['shader_id']
        cg_path = output / (name + '.cg')
        gxp_path = output / (name + '.gxp')
        cg_path.write_text(cg, encoding='utf-8', newline='\n')
        flags = record['flags']
        command = [str(args.shacc.resolve()), '--config', str(args.shacc_config.resolve()),
                   '-profile', 'sce_fp_psp2', *flags, '-o', str(gxp_path),
                   str(cg_path), '--timeout', '120']
        result = compile_serial(command, args.shacc_config.resolve().parent, timeout=135)
        (output / (name + '.log')).write_bytes(result.stdout + result.stderr)
        if result.returncode or not gxp_path.is_file() or gxp_path.read_bytes()[:4] != b'GXP\0':
            raise RuntimeError('render-scale compile failed: ' + name)
        spec['replace'].append({'stage_index': stage['index'], 'old_key': variant['key'],
                                'old_gxp_sha256': variant['gxp_sha256'],
                                'viewport_mask': variant['viewport_mask']})
        spec['add'].append({
            'stage_index': stage['index'], 'shader_id': name,
            'stage_sha256': stage['stage_sha256'],
            'viewport_mask': variant['viewport_mask'],
            'input_signature': variant['input_signature'],
            'source_kind': 'logical-fragment-coordinates',
            'gxp_source': str(gxp_path), 'gxp_bytes': gxp_path.stat().st_size,
            'gxp_sha256': digest(gxp_path), 'cg_sha256': digest(cg_path),
            'environment_sha256': record['environment_sha256'], 'flags': flags,
            'provenance': ['resolution rescaling; source:' + str(source.resolve())]})
        print('compiled', name, flush=True)
    spec_path = output / 'candidate-spec.json'
    spec_path.write_text(json.dumps(spec, indent=2) + '\n')
    subprocess.run([sys.executable, str(Path(__file__).with_name('augment_static_shader_package.py')),
                    '--base', str(base), '--base-abi', '2', '--spec', str(spec_path),
                    '--output', str(output / 'package')], check=True)
    resources = json.loads((base / 'shader-resources.manifest.json').read_text())
    subprocess.run([
        sys.executable, str(Path(__file__).with_name('build_resource_layout.py')),
        '--catalogue', resources['catalogue']['path'],
        '--index', str(output / 'package' / 'shader-index.bin'),
        '--glsl-root', resources['glsl']['root'],
        '--shadow-spec', resources['shadow_usage_audit']['spec']['path'],
        '--output', str(output / 'package' / 'shader-resources.bin'),
        '--manifest', str(output / 'package' / 'shader-resources.manifest.json')], check=True)


if __name__ == '__main__':
    main()

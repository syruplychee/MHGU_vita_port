"""Compile lossless vertex samplers from exact, previously verified Cg sources.

The private source map names old variant keys/GXP hashes, source Cg paths/hashes,
environment hashes and original compiler flags. No source discovery, installation
or fallback is performed here. Generated records bind the helper and toolchain.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

from convert_cache import compile_serial
from compile_static_candidates import valid_gxp
from vertex_rg32f import lower_cg


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--shader-tools', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    base, output = args.base.resolve(), args.output.resolve()
    config = args.shader_tools / 'shacc.conf'
    compiler = args.shader_tools / 'shacc-bin/Vita3K-Shacc-windows-x86_64/vita3k-shacc.exe'
    values = dict(line.split('=', 1) for line in config.read_text(encoding='utf-8').splitlines() if line)
    lock = json.loads((Path(__file__).resolve().parents[2] /
                       'config/graphics/shader-toolchain.json').read_text(encoding='utf-8'))
    identity = {'shacc_sha256': digest(compiler),
                'service_vpk_sha256': digest(Path(values['service-vpk'])),
                'libshacccg_sha256': digest(Path(values['root']) / 'fs/ur0/data/external/libshacccg.suprx'),
                'config_sha256': digest(config),
                'helper_sha256': digest(Path(__file__).with_name('vertex_rg32f.py'))}
    if (identity['shacc_sha256'] != lock['shacc']['executable_sha256'] or
            identity['service_vpk_sha256'] != lock['shacc']['service_vpk_sha256'] or
            identity['libshacccg_sha256'] != lock['libshacccg']['sha256']):
        raise ValueError('shader compiler identity mismatch')
    index = json.loads((base / 'shader-index.json').read_text(encoding='utf-8'))
    source_map = json.loads(args.sources.read_text(encoding='utf-8'))
    if source_map['base_index_sha256'] != digest(base / 'shader-index.bin'):
        raise ValueError('source map belongs to another shader index')
    output.mkdir(parents=True, exist_ok=True)
    # A failed rebuild must not leave a previous successful spec looking new.
    (output / 'candidate-spec.json').unlink(missing_ok=True)
    replace, additions, seen, failures = [], [], set(), []
    rows = source_map['sources']
    if not rows:
        raise ValueError('empty source map')
    for row in rows:
        key = row['old_key']
        if key in seen:
            raise ValueError('duplicate source key')
        seen.add(key)
        variants = [v for v in index['variants'] if v['key'] == key]
        if len(variants) != 1:
            raise ValueError('source variant missing or ambiguous')
        variant = variants[0]
        stage = index['stages'][variant['stage_index']]
        if stage['stage'] != 'vert' or variant['gxp_sha256'] != row['old_gxp_sha256']:
            raise ValueError('source is not the exact vertex variant')
        if digest(base / variant['gxp']) != row['old_gxp_sha256']:
            raise ValueError('old GXP identity mismatch')
        source = Path(row['cg_path'])
        if digest(source) != row['cg_sha256'] or row['flags'] not in (['-O0'], ['-O1']):
            raise ValueError('source Cg/flags identity mismatch')
        cg, samplers = lower_cg(source.read_text(encoding='utf-8'))
        if samplers != ['tex0']:
            raise ValueError('only the runtime vertex tex0 binding is supported')
        content = {**identity, **row, 'cg_output_sha256': hashlib.sha256(cg.encode()).hexdigest()}
        content_key = hashlib.sha256(canonical(content)).hexdigest()
        cg_path = output / (key + '.cg')
        gxp_path = output / (key + '.gxp')
        receipt_path = output / (key + '.json')
        cg_path.write_text(cg, encoding='utf-8', newline='\n')
        reused = False
        if receipt_path.exists():
            receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
            reused = (receipt['content_key'] == content_key and gxp_path.exists() and
                      receipt['gxp_sha256'] == digest(gxp_path) and valid_gxp(gxp_path)[0])
        if not reused:
            command = [str(compiler), '--config', str(config), '-profile', 'sce_vp_psp2',
                       *row['flags'], '-o', str(gxp_path), str(cg_path), '--timeout', '40']
            result = compile_serial(command, args.shader_tools, timeout=50)
            (output / (key + '.log')).write_bytes(result.stdout + result.stderr)
            if result.returncode or not valid_gxp(gxp_path)[0]:
                failures.append(key)
                print(f'FAILED stage={variant["stage_index"]} key={key}', flush=True)
                continue
            receipt_path.write_text(json.dumps({'content_key': content_key,
                'gxp_sha256': digest(gxp_path), 'source': content}, indent=2), encoding='utf-8')
        replace.append({'stage_index': variant['stage_index'], 'old_key': key,
            'old_gxp_sha256': row['old_gxp_sha256'], 'viewport_mask': variant['viewport_mask']})
        additions.append({'stage_index': variant['stage_index'], 'shader_id': stage['shader_id'],
            'stage_sha256': stage['stage_sha256'], 'viewport_mask': variant['viewport_mask'],
            'input_signature': variant['input_signature'], 'source_kind': 'vertex-rgba32f-rg32f',
            'gxp_source': str(gxp_path), 'gxp_bytes': gxp_path.stat().st_size,
            'gxp_sha256': digest(gxp_path), 'cg_path': str(cg_path), 'cg_sha256': digest(cg_path),
            'environment_sha256': row['environment_sha256'], 'flags': row['flags'],
            'provenance': ['exact old vertex Cg + lossless RG32F helper',
                           'compiler-and-helper-content-sha256:' + content_key]})
        print(f'{len(additions)}/{len(rows)} stage={variant["stage_index"]} key={key} reused={reused}', flush=True)
    if failures:
        (output / 'failures.json').write_text(json.dumps(failures, indent=2), encoding='utf-8')
        raise RuntimeError(f'{len(failures)} vertex RG32F compilations failed; no package spec written')
    spec = {'schema': 1, 'base_index_sha256': digest(base / 'shader-index.bin'),
            'base_shader_index_json_sha256': digest(base / 'shader-index.json'),
            'replace': replace, 'add': additions}
    (output / 'candidate-spec.json').write_text(json.dumps(spec, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()

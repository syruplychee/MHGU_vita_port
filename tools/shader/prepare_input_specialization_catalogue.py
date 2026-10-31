"""Derive an offline Cg catalogue for captured vertex-input specializations.

The base catalogue declares every generic Maxwell input as vec4 float. A draw
whose captured descriptor is an integer attribute needs an integer GXP input;
`shader_ir.specialize_unsigned_attributes` performs that transformation. This
tool records the derivation as a first-class catalogue so
`compile_static_candidates.py` can bind (source GLSL, IR revision, environment,
profile, flags and input signature) to the resulting GXP. It does not compile,
link, install or claim pixel correctness.

Each request is `shader_id:location[,location...]`. The derived entry keeps the
original Maxwell content SHA and environment SHA, changes only the Cg input
declaration, and records the unsigned locations explicitly.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=False).encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalogue', type=Path, required=True,
                        help='base audit catalogue with shader-catalogue.jsonl')
    parser.add_argument('--glsl-root', type=Path, required=True,
                        help='directory holding <shader_id>/shader.glsl')
    parser.add_argument('--output', type=Path, required=True,
                        help='new catalogue directory (must not exist)')
    parser.add_argument('--request', action='append', default=[],
                        metavar='SHADER_ID:LOCATIONS',
                        help='include one base stage and specialize these input locations')
    args = parser.parse_args()
    if not args.request:
        raise ValueError('at least one --request is required')
    catalogue = args.catalogue.resolve()
    glsl_root = args.glsl_root.resolve()
    output = args.output.resolve()
    if output.exists():
        raise ValueError('output must not exist')
    base = [json.loads(line) for line in
            (catalogue / 'shader-catalogue.jsonl').read_text(encoding='utf-8').splitlines()
            if line.strip()]
    by_id = {entry['shader_id']: entry for entry in base}
    if len(by_id) != len(base):
        raise ValueError('duplicate shader id in base catalogue')
    summary_path = catalogue / 'summary.json'
    summary = json.loads(summary_path.read_text(encoding='utf-8'))
    if summary['outputs']['shader-catalogue.jsonl'] != digest(catalogue / 'shader-catalogue.jsonl'):
        raise ValueError('base catalogue hash mismatch')

    ir = Path(__file__).with_name('shader_ir.py')
    rows, requests = [], []
    cg_dir = output / 'cg-candidates'
    cg_dir.mkdir(parents=True)
    for request in args.request:
        shader_id, separator, locations_text = request.partition(':')
        if not separator or not shader_id:
            raise ValueError('request must be SHADER_ID:LOCATIONS')
        locations = sorted({int(item) for item in locations_text.split(',') if item.strip()})
        if not locations or any(location < 0 or location > 15 for location in locations):
            raise ValueError('input locations must be within 0..15')
        base_entry = by_id.get(shader_id)
        if base_entry is None:
            raise ValueError('unknown base shader ' + shader_id)
        if base_entry['stage'] != 'vert' or base_entry.get('cg_lowering') != 'accepted':
            raise ValueError('base shader is not an accepted vertex stage: ' + shader_id)
        glsl = glsl_root / shader_id / 'shader.glsl'
        if not glsl.is_file() or digest(glsl) != base_entry['glsl_sha256']:
            raise ValueError('GLSL hash/path mismatch for ' + shader_id)
        suffix = '_uattr' + '_'.join(str(location) for location in locations)
        derived_id = shader_id + suffix
        if derived_id in by_id:
            raise ValueError('derived id collides with base catalogue: ' + derived_id)
        cg = cg_dir / (derived_id + '.cg')
        result = subprocess.run(
            [sys.executable, str(ir), '--glsl', str(glsl), '--stage', 'vertex',
             '--unsigned-attributes', ','.join(str(x) for x in locations),
             '--output', str(cg)],
            capture_output=True, text=True, timeout=180)
        if result.returncode != 0 or not cg.is_file():
            raise ValueError('shader_ir rejected %s: %s' %
                             (shader_id, (result.stderr or result.stdout).strip()[:400]))
        metadata = json.loads(result.stdout)
        text = cg.read_text(encoding='utf-8')
        for location in locations:
            if 'in_attr%d_uint' % location not in text:
                raise ValueError('specialization did not produce in_attr%d_uint' % location)
        entry = dict(base_entry)
        entry.update({
            'shader_id': derived_id,
            'stage': 'vert',
            'maxwell_sha256': base_entry['maxwell_sha256'],
            'environment_sha256': base_entry['environment_sha256'],
            'glsl_sha256': base_entry['glsl_sha256'],
            'conversion_sha256': base_entry['conversion_sha256'],
            'resources_sha256': base_entry.get('resources_sha256'),
            'origin': 'derived-uattr',
            'derived_from_shader_id': shader_id,
            'input_signature': {'unsigned_attributes': locations},
            'cg_candidate': 'cg-candidates/' + cg.name,
            'cg_sha256': digest(cg),
            'cg_lowering': 'accepted',
            'lowering_notes': metadata['lowering_notes'],
            # Input specialization is applied here. Any other requirement the
            # base entry declared (resource specialization, missing runtime
            # data, semantic verification) is preserved untouched.
            'specialization_required': base_entry.get('specialization_required', False),
            'input_specialization_applied': True,
        })
        rows.append(entry)
        requests.append({'shader_id': shader_id, 'derived_shader_id': derived_id,
                         'unsigned_attributes': locations, 'cg_sha256': entry['cg_sha256']})
    rows.sort(key=lambda row: row['shader_id'])
    (output / 'shader-catalogue.jsonl').write_text(
        ''.join(json.dumps(row, sort_keys=True) + '\n' for row in rows),
        encoding='utf-8', newline='\n')
    out_summary = {
        'schema': 1,
        'base_catalogue_summary_sha256': digest(summary_path),
        'base_catalogue_shader_catalogue_sha256': digest(catalogue / 'shader-catalogue.jsonl'),
        'shader_ir_sha256': digest(ir),
        'outputs': {'shader-catalogue.jsonl': digest(output / 'shader-catalogue.jsonl')},
        'requests': requests,
    }
    (output / 'summary.json').write_text(
        json.dumps(out_summary, indent=2, sort_keys=True) + '\n',
        encoding='utf-8', newline='\n')
    print(json.dumps({'output': str(output), 'derived': len(rows),
                      'requests': requests}, indent=2))


if __name__ == '__main__':
    main()

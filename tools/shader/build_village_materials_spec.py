"""Build the M6-B14 village-material augmentation spec.

The four fragment stages below are the observed village MATERIAL gaps whose Cg
was already accepted and compiled into the P5 vp1 package. This tool re-audits
the exact stage identity, catalogue texture-resource pattern and P5 GXP
bytes/environment/flags before emitting an augmentation spec. It never compiles
shaders, never edits a bank and never writes a manifest field that claims a
runtime or pixel result.

The base bank for this round is the ABI 2 M6-B3 candidate bank. Use
augment_static_shader_package.py with --base-abi 2.
"""
import argparse
import hashlib
import json
from pathlib import Path

VIEWPORT_ONE = 2
EXPECTED_BASE_INDEX_SHA256 = \
    '7cf058953ccd28695ba7f99e3d7e7f33d0fa8e68c734888a2ee0a2b8ff454f74'
EXPECTED_BASE_COUNTS = {'stages': 2402, 'variants': 278, 'programs': 6311, 'abi': 2}
EXPECTED_STAGES = (
    ('frag_33c64dc29495047907e215fe', 742, 2),
    ('frag_7e6796521c087b9d134dd9cf', 1329, 2),
    ('frag_d9de0bf10d66ed92137358d6', 2085, 4),
    ('frag_e4bccab8600cc5821a9c6bba', 2169, 4),
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_jsonl(path):
    return [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()
            if line.strip()]


def check_resource_pattern(shader_id, row, sampler_count):
    """Require the exact bounded fragment texture ABI used by this backend.

    Every texture descriptor must be bank 0, count 1, shift 0, size_shift 3,
    with no secondary/depth binding. tex0..texN use handle offsets 32+8*N; only
    the last sampler (the colour cube) is catalogue type 5. The catalogue type
    domain is not the runtime TIC type domain: type 5 here means ColorCube,
    while the guest TIC reports cube as texture_type 3.
    """
    resources = row.get('resources') or {}
    textures = resources.get('textures') or []
    if len(textures) != sampler_count:
        raise ValueError('%s: catalogue texture count %d != %d' %
                         (shader_id, len(textures), sampler_count))
    for index, texture in enumerate(textures):
        expected = {
            'bank': 0, 'offset': 32 + 8 * index, 'count': 1,
            'shift': 0, 'size_shift': 3, 'secondary': 0, 'depth': 0,
            'type': 5 if index == sampler_count - 1 else 2,
        }
        for key, value in expected.items():
            if texture.get(key) != value:
                raise ValueError('%s: texture%d %s=%r, expected %r' %
                                 (shader_id, index, key, texture.get(key), value))
    if resources.get('storage_buffers') != 0 or resources.get('images') != 0:
        raise ValueError('%s: unexpected storage buffer or image resource' % shader_id)
    metadata = row.get('cg_metadata') or []
    samplers = [entry for entry in metadata if entry.get('storage') == 'sampler']
    expected_names = ['tex%d' % index for index in range(sampler_count)]
    if [entry.get('name') for entry in samplers] != expected_names:
        raise ValueError('%s: Cg sampler names differ from %s' %
                         (shader_id, expected_names))
    for index, entry in enumerate(samplers):
        if entry.get('binding') != index:
            raise ValueError('%s: Cg sampler %s binding %r != %d' %
                             (shader_id, entry.get('name'),
                              entry.get('binding'), index))
        expected_type = 'samplerCube' if index == sampler_count - 1 else 'sampler2D'
        if entry.get('type') != expected_type:
            raise ValueError('%s: Cg sampler %s type %r != %s' %
                             (shader_id, entry.get('name'), entry.get('type'),
                              expected_type))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalogue', type=Path, required=True,
                        help='directory containing shader-catalogue.jsonl')
    parser.add_argument('--base-index', type=Path, required=True,
                        help='B3 candidate-bank shader-index.json (read only)')
    parser.add_argument('--base-bank', type=Path, required=True,
                        help='B3 candidate-bank directory (read only)')
    parser.add_argument('--p5-manifest', type=Path, required=True)
    parser.add_argument('--run15-log', type=Path, required=True)
    parser.add_argument('--run17-log', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]

    base_bin = args.base_bank / 'shader-index.bin'
    base_index_sha = digest(base_bin)
    if base_index_sha != EXPECTED_BASE_INDEX_SHA256:
        raise ValueError('B3 base index SHA drift: ' + base_index_sha)
    base = json.loads(args.base_index.read_text(encoding='utf-8'))
    if base.get('abi_version') != EXPECTED_BASE_COUNTS['abi']:
        raise ValueError('base bank is not ABI 2')
    counts = {'stages': len(base['stages']), 'variants': len(base['variants']),
              'programs': len(base['programs'])}
    for key, value in counts.items():
        if value != EXPECTED_BASE_COUNTS[key]:
            raise ValueError('base %s count %d != %d' %
                             (key, value, EXPECTED_BASE_COUNTS[key]))
    stages = {row['index']: row for row in base['stages']}
    variants_by_stage = {}
    for variant in base['variants']:
        variants_by_stage.setdefault(variant['stage_index'], []).append(variant)

    catalogue_path = args.catalogue.resolve()
    catalogue_file = catalogue_path / 'shader-catalogue.jsonl'
    catalogue = {row['shader_id']: row for row in read_jsonl(catalogue_file)}

    p5_path = args.p5_manifest.resolve()
    p5 = json.loads(p5_path.read_text(encoding='utf-8'))
    if p5.get('counts') != {'compiled': 3, 'imported_unchanged': 2328}:
        raise ValueError('P5 manifest is not the 7203 growth package')
    p5_by_id = {row['shader_id']: row for row in p5['programs']}
    p5_root = p5_path.parent

    add = []
    for shader_id, stage_index, sampler_count in EXPECTED_STAGES:
        stage = stages.get(stage_index)
        if stage is None or stage['shader_id'] != shader_id:
            raise ValueError('stage %d identity mismatch: %s' %
                             (stage_index, shader_id))
        if stage['stage'] != 'frag':
            raise ValueError('stage %d is not a fragment stage' % stage_index)
        existing = [variant for variant in variants_by_stage.get(stage_index, [])
                    if variant['viewport_mask'] & VIEWPORT_ONE]
        if existing:
            raise ValueError('unexpected superset: stage %d already has a '
                             'viewport-1 variant' % stage_index)
        row = catalogue.get(shader_id)
        if row is None:
            raise ValueError('catalogue row missing: ' + shader_id)
        if row.get('stage') != 'frag':
            raise ValueError('catalogue stage mismatch: ' + shader_id)
        if row.get('maxwell_sha256') != stage['stage_sha256']:
            raise ValueError('catalogue/SPK stage SHA mismatch: ' + shader_id)
        check_resource_pattern(shader_id, row, sampler_count)
        program = p5_by_id.get(shader_id)
        if program is None:
            raise ValueError('P5 program missing: ' + shader_id)
        if program.get('status') != 'compiled' or not program.get('gxp_verified'):
            raise ValueError('P5 program is not compiled/verified: ' + shader_id)
        if program.get('maxwell_sha256') != stage['stage_sha256']:
            raise ValueError('P5 stage identity mismatch: ' + shader_id)
        if program.get('cg_sha256') != row.get('cg_sha256'):
            raise ValueError('P5 Cg SHA differs from catalogue: ' + shader_id)
        if program.get('environment_sha256') != row.get('environment_sha256'):
            raise ValueError('P5 environment differs from catalogue: ' + shader_id)
        gxp = p5_root / program['gxp']
        if not gxp.is_file():
            raise ValueError('P5 GXP missing: ' + str(gxp))
        data = gxp.read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        if sha != program['gxp_sha256'] or len(data) != program['gxp_bytes']:
            raise ValueError('P5 GXP bytes drifted: ' + shader_id)
        add.append({
            'stage_index': stage_index,
            'stage': 'frag',
            'shader_id': shader_id,
            'stage_sha256': stage['stage_sha256'],
            'viewport_mask': VIEWPORT_ONE,
            'input_signature': 0,
            'source_kind': 'p5-verified-reuse',
            'gxp_source': str(gxp.relative_to(repo)).replace('\\', '/'),
            'gxp_sha256': sha,
            'gxp_bytes': len(data),
            'flags': program['flags'],
            'environment_sha256': program['environment_sha256'],
            'content_key': program['content_key'],
            'cg_sha256': program['cg_sha256'],
            'catalogue_texture_slots': sampler_count,
            'provenance': [
                'p5-7203-20261025',
                'p5-manifest-sha256:' + digest(p5_path),
                'catalogue-summary-sha256:' +
                p5['build_identity']['catalogue_summary_sha256'],
                'm6b14-observed-village-material',
            ],
        })

    if len(add) != 4:
        raise ValueError('expected exactly four additions')

    logs = []
    for path in (args.run15_log, args.run17_log):
        resolved = path.resolve()
        logs.append({'path': str(resolved.relative_to(repo)).replace('\\', '/'),
                     'sha256': digest(resolved)})
    spec = {
        'schema': 1,
        'mode': 'augment-static-index',
        'description': 'M6-B14 four observed village MATERIAL fragment stages; '
                       'P5 vp1 GXP reuse only',
        'base_index_sha256': base_index_sha,
        'base_shader_index_json_sha256': digest(args.base_index),
        'base_counts': counts,
        'sources': {
            'p5_manifest': {
                'path': str(p5_path.relative_to(repo)).replace('\\', '/'),
                'sha256': digest(p5_path)},
            'catalogue': {
                'path': str(catalogue_path.relative_to(repo)).replace('\\', '/'),
                'shader_catalogue_sha256': digest(catalogue_file)},
            'observed_runs': logs,
        },
        'already_present': [],
        'add': sorted(add, key=lambda row: row['stage_index']),
        'replace': [],
        'blocked': [],
        'notes': [
            'All four additions are generic fragment viewport-1 variants '
            '(viewport mask 2, input signature 0).',
            'The candidate keeps every one of the 278 base variant keys, GXP '
            'bytes, flags and signatures.',
            'The base stage and program tables (2402 stages, 6311 programs) are '
            'preserved without substitutes.',
            'The observed-run logs are evidence of the runs; the four stage '
            'identities come from the audited village missing-variant set.',
            'GXP compilation does not imply runtime linking, pixels or hardware.',
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(spec, indent=2, sort_keys=True) + '\n',
                           encoding='utf-8', newline='\n')
    print(json.dumps({'output': str(args.output), 'add': len(add),
                      'base_index_sha256': base_index_sha,
                      'stages': [row['stage_index'] for row in add]}, indent=2))


if __name__ == '__main__':
    main()

"""Build the M6-B3 village augmentation spec from the run05 runtime audit.

Only stages actually named by the audit are considered. A missing
(stage, viewport) pair is covered by reusing the already verified P5 GXP for
the same stage identity; a stage whose Cg lowering was rejected is recorded as
blocked with its exact catalogue error. Nothing is compiled or installed here.

The three UINT-input vertex stages observed in the attribute-refusal section get
an additional ABI 2 specialization (signature bit = attribute location). Their
generic viewport-1 GXP stays in place, so non-UINT draws keep the old bytes.
"""
import argparse
import hashlib
import json
from pathlib import Path

VIEWPORT_ONE = 2
ATTR3_UINT = 1 << 3


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_jsonl(path):
    return [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()
            if line.strip()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--audit', type=Path, required=True,
                        help='codex-runtime-audit.json')
    parser.add_argument('--run', default='run05-storage-diagnostic')
    parser.add_argument('--base-index', type=Path, required=True,
                        help='installed shader-index.json (read only)')
    parser.add_argument('--base-bank', type=Path, required=True,
                        help='installed static-shaders directory (read only)')
    parser.add_argument('--p5-manifest', type=Path, required=True)
    parser.add_argument('--catalogue', type=Path, required=True,
                        help='audit-vp1 directory with shader-catalogue.jsonl')
    parser.add_argument('--uattr-manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    audit_path = args.audit.resolve()
    audit = json.loads(audit_path.read_text(encoding='utf-8'))[args.run]
    if audit.get('unique_missing_stage_pairs') != len(audit['missing_variants']):
        raise ValueError('audit pair count mismatch')
    base = json.loads(args.base_index.read_text(encoding='utf-8'))
    if base['abi_version'] != 1:
        raise ValueError('base bank is not ABI 1')
    stages = {row['index']: row for row in base['stages']}
    variants_by_stage = {}
    for variant in base['variants']:
        variants_by_stage.setdefault(variant['stage_index'], []).append(variant)
    p5_path = args.p5_manifest.resolve()
    p5 = json.loads(p5_path.read_text(encoding='utf-8'))
    p5_by_id = {row['shader_id']: row for row in p5['programs']}
    p5_root = p5_path.parent
    catalogue_path = args.catalogue.resolve()
    catalogue = {row['shader_id']: row for row in
                 read_jsonl(catalogue_path / 'shader-catalogue.jsonl')}
    uattr_path = args.uattr_manifest.resolve()
    uattr = json.loads(uattr_path.read_text(encoding='utf-8'))
    uattr_by_id = {row['shader_id'].replace('_uattr3', ''): row
                   for row in uattr['programs']}
    uattr_root = uattr_path.parent

    needed = {}
    for pair in audit['missing_variants']:
        if pair['viewport'] != 1:
            raise ValueError('unexpected viewport in audit')
        for side in ('vs', 'fs'):
            needed[pair[side]['index']] = pair[side]

    add, blocked, already_present = [], [], []
    for index, observed in sorted(needed.items()):
        stage = stages.get(index)
        if stage is None or stage['shader_id'] != observed['shader_id']:
            raise ValueError('audit stage missing or drifted: %d' % index)
        existing = [variant for variant in variants_by_stage.get(index, [])
                    if variant['viewport_mask'] & VIEWPORT_ONE]
        if existing:
            already_present.append({'stage_index': index,
                                    'shader_id': stage['shader_id'],
                                    'viewport_mask': VIEWPORT_ONE,
                                    'keys': [variant['key'] for variant in existing]})
            continue
        program = p5_by_id.get(stage['shader_id'])
        if program is None:
            row = catalogue.get(stage['shader_id'])
            if row is None or row.get('cg_lowering') != 'unsupported':
                raise ValueError('needed stage has no verified GXP and no explicit '
                                 'Cg rejection: %d %s' % (index, stage['shader_id']))
            blocked.append({
                'stage_index': index,
                'stage': stage['stage'],
                'shader_id': stage['shader_id'],
                'stage_sha256': stage['stage_sha256'],
                'viewport_mask': VIEWPORT_ONE,
                'reason': row.get('cg_error') or 'Cg lowering unsupported',
                'cg_lowering': row.get('cg_lowering'),
                'cg_candidate': row.get('cg_candidate'),
                'specialization_required': row.get('specialization_required'),
                'library_specialization_required':
                    row.get('library_specialization_required'),
                'events': sum(pair['events'] for pair in audit['missing_variants']
                              if pair['fs']['index'] == index),
            })
            continue
        gxp = p5_root / program['gxp']
        if not gxp.is_file():
            raise ValueError('P5 GXP is missing: ' + str(gxp))
        data = gxp.read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        if sha != program['gxp_sha256'] or len(data) != program['gxp_bytes']:
            raise ValueError('P5 GXP bytes drifted: ' + str(gxp))
        if program.get('status') != 'compiled' or not program.get('gxp_verified'):
            raise ValueError('P5 GXP is not marked verified: ' + str(gxp))
        if program.get('maxwell_sha256') != stage['stage_sha256']:
            raise ValueError('P5 stage identity mismatch: ' + stage['shader_id'])
        add.append({
            'stage_index': index,
            'stage': stage['stage'],
            'shader_id': stage['shader_id'],
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
            'provenance': ['p5-7203-20261025',
                           'p5-manifest-sha256:' + digest(p5_path),
                           'catalogue-summary-sha256:' +
                           p5['build_identity']['catalogue_summary_sha256']],
        })

    uattr_add = []
    for shader_id in ('vert_d0fc5c566131cf0c541f0533',
                      'vert_3e829bd65d61ebb62c4b518f',
                      'vert_8fad4f35a746da97496a1620'):
        stage = next((row for row in base['stages']
                      if row['shader_id'] == shader_id), None)
        if stage is None:
            raise ValueError('UINT specialization stage missing: ' + shader_id)
        program = uattr_by_id.get(shader_id)
        if program is None:
            raise ValueError('UINT specialization GXP missing: ' + shader_id)
        gxp = uattr_root / program['gxp']
        data = gxp.read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        if sha != program['gxp_sha256'] or len(data) != program['gxp_bytes']:
            raise ValueError('UINT GXP bytes drifted: ' + str(gxp))
        if program.get('status') != 'compiled' or not program.get('gxp_verified'):
            raise ValueError('UINT GXP is not marked verified: ' + str(gxp))
        if program.get('maxwell_sha256') != stage['stage_sha256']:
            raise ValueError('UINT stage identity mismatch: ' + shader_id)
        if not any(variant['viewport_mask'] & VIEWPORT_ONE
                   for variant in variants_by_stage.get(stage['index'], [])):
            raise ValueError('UINT stage lacks a generic viewport-1 variant')
        uattr_add.append({
            'stage_index': stage['index'],
            'stage': 'vert',
            'shader_id': stage['shader_id'],
            'stage_sha256': stage['stage_sha256'],
            'viewport_mask': VIEWPORT_ONE,
            'input_signature': ATTR3_UINT,
            'source_kind': 'uattr3',
            'gxp_source': str(gxp.relative_to(repo)).replace('\\', '/'),
            'gxp_sha256': sha,
            'gxp_bytes': len(data),
            'flags': program['flags'],
            'environment_sha256': program['environment_sha256'],
            'content_key': program['content_key'],
            'cg_sha256': program['cg_sha256'],
            'provenance': ['m6b3-uattr3', 'uattr3-manifest-sha256:' + digest(uattr_path),
                           'catalogue-summary-sha256:' +
                           uattr['build_identity']['catalogue_summary_sha256'],
                           'unsigned-attributes:3'],
        })

    spec = {
        'schema': 1,
        'mode': 'augment-static-index',
        'description': 'M6-B3 village gaps observed in run05, plus exact '
                       'attribute-refusal specializations',
        'base_index_sha256': digest(args.base_bank / 'shader-index.bin'),
        'base_shader_index_json_sha256': digest(args.base_index),
        'audit': {'path': str(audit_path.relative_to(repo)).replace('\\', '/'),
                  'sha256': digest(audit_path), 'run': args.run,
                  'missing_variant_events': audit['missing_variant_events'],
                  'unique_missing_stage_pairs': audit['unique_missing_stage_pairs'],
                  'attribute_refusal_events': audit['attribute_refusal_events']},
        'sources': {
            'p5_manifest': {'path': str(p5_path.relative_to(repo)).replace('\\', '/'),
                            'sha256': digest(p5_path)},
            'catalogue': {'path': str(catalogue_path.relative_to(repo)).replace('\\', '/'),
                          'shader_catalogue_sha256':
                              digest(catalogue_path / 'shader-catalogue.jsonl')},
            'uattr_manifest': {'path': str(uattr_path.relative_to(repo)).replace('\\', '/'),
                               'sha256': digest(uattr_path)},
        },
        'already_present': already_present,
        'add': sorted(add, key=lambda row: row['stage_index']) +
               sorted(uattr_add, key=lambda row: row['stage_index']),
        'replace': [],
        'blocked': sorted(blocked, key=lambda row: row['stage_index']),
        'notes': [
            'All added generic records use viewport mask 2 (runtime viewport 1).',
            'The candidate keeps every base stage, variant, program and GXP.',
            'Blocked stages stay explicit; no substitute shader is invented.',
            'GXP linking, target pixels and hardware remain unverified.',
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(spec, indent=2, sort_keys=True) + '\n',
                           encoding='utf-8', newline='\n')
    print(json.dumps({'output': str(args.output), 'add_generic': len(add),
                      'add_uattr': len(uattr_add), 'blocked': len(blocked),
                      'already_present': len(already_present),
                      'blocked_stages': blocked}, indent=2))


if __name__ == '__main__':
    main()

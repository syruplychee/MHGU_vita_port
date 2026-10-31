"""Build the M6-B24 shadow candidate augmentation spec.

The spec is derived only from immutable offline evidence:

* the exact ABI 2 base bank (282 variants, 2402 stages, 6311 programs) and its
  manifest identity;
* the M6-B24 checkpoint/receipts for exactly nine O0 GXPs compiled under the
  proven 300 s service bound;
* the locked catalogue/resolved GLSL identity and exact texture descriptors;
* the shared shadow policy/emitter identity.

Every added record binds a *content identity* (stage, Cg, GXP, environment,
flags, profile, strategy, policy, emitter, helper and compiler identity) into
the ABI 2 variant key and provenance.  Retained base keys are never recomputed
and never changed.  This tool compiles nothing, installs nothing and makes no
runtime, pixel, atlas, game, batching, performance or hardware claim.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_shadow_candidates as csc
from augment_static_shader_package import ABI_CANDIDATE, canonical, variant_key
from shadow_helpers import (STRATEGY, emitter_source_sha256,
                            helper_source_sha256, policy_sha256)

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = 1
TASK = 'M6-B24'
COMPILE_TASK = 'M6-B24'
PROFILE = 'sce_fp_psp2'
VIEWPORT_ONE = 2
INPUT_SIGNATURE = 0
EXPECTED_BASE_INDEX_SHA256 = (
    '0c97e433aecbcc4d8915b6efcc54cb9c8f5715ba7277bc6ec61bc926e16ba6cd')
EXPECTED_BASE_INDEX_JSON_SHA256 = (
    '73e53f9d7286c103774cc4f9a8501225e8c6e34138e2bf46da8c3f8b14017c90')
EXPECTED_BASE_MANIFEST_SHA256 = (
    'b27d08f9f4dd27a7b0a34be72124c88f374f791c3e9a531e32e86b79b2db6a3c')
EXPECTED_COUNTS = {'stages': 2402, 'variants': 282, 'programs': 6311}
EXPECTED_SERVICE_TIMEOUT = 300
MIN_GXP_BYTES = 32


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_catalogue(path):
    entries = []
    for line in path.read_text(encoding='utf-8').splitlines():
        if line.strip():
            entries.append(json.loads(line))
    return {entry['shader_id']: entry for entry in entries}


def valid_gxp(data):
    if len(data) < MIN_GXP_BYTES or data[:4] != b'GXP\0':
        raise ValueError('not a GXP')
    declared = struct.unpack_from('<I', data, 8)[0]
    if declared < MIN_GXP_BYTES or declared > len(data):
        raise ValueError('GXP declared size invalid')
    return data


def content_identity_for(entry, task=TASK):
    return {
        'schema': 1,
        'task': task,
        'shader_id': entry['shader_id'],
        'stage_sha256': entry['stage_sha256'],
        'environment_sha256': entry['environment_sha256'],
        'resources_sha256': entry['resources_sha256'],
        'cg_sha256': entry['cg_sha256'],
        'cg_bytes': entry['cg_bytes'],
        'gxp_sha256': entry['gxp_sha256'],
        'gxp_bytes': entry['gxp_bytes'],
        'profile': PROFILE,
        'flags': list(entry['flags']),
        'strategy': STRATEGY,
        'policy_sha256': policy_sha256(),
        'emitter_sha256': emitter_source_sha256(),
        'helper_sha256': helper_source_sha256(),
        'toolchain': entry['toolchain'],
        'service_timeout_seconds': entry['service_timeout_seconds'],
        'outer_timeout_seconds': entry['outer_timeout_seconds'],
    }


def validate_base_binding(spec, index_sha256, index_json_sha256,
                          manifest_sha256):
    checks = {
        'base_index_sha256': index_sha256,
        'base_shader_index_json_sha256': index_json_sha256,
        'base_manifest_sha256': manifest_sha256,
    }
    for field, expected in checks.items():
        if spec.get(field) != expected:
            raise ValueError('%s differs from the immutable base identity' % field)
    if spec.get('base_abi_version') != ABI_CANDIDATE:
        raise ValueError('base ABI is not the ABI 2 candidate')
    return True


def validate_spec_shape(spec, expected_ids, expected_flags=('-O0',)):
    """Independent shape/identity guard used by tests and before publishing."""
    if spec.get('schema') != SCHEMA:
        raise ValueError('spec schema unsupported')
    add = spec.get('add') or []
    expected = list(expected_ids)
    if len(add) != len(expected):
        raise ValueError('expected exactly %d added variants, got %d'
                         % (len(expected), len(add)))
    ids = [entry.get('shader_id') for entry in add]
    if sorted(ids) != sorted(expected) or len(set(ids)) != len(ids):
        raise ValueError('added variant stage ids differ from the nine observed')
    if spec.get('replace'):
        raise ValueError('this spec must not replace a retained variant')
    if spec.get('blocked'):
        raise ValueError('this spec must not carry blocked substitutions')
    if len({entry.get('stage_index') for entry in add}) != len(add):
        raise ValueError('duplicate added stage index')
    keys = set()
    for entry in add:
        identity = entry.get('content_identity')
        if not isinstance(identity, dict):
            raise ValueError('added entry lacks content_identity: %s'
                             % entry.get('shader_id'))
        computed = hashlib.sha256(canonical(identity)).hexdigest()
        if entry.get('content_identity_sha256') != computed:
            raise ValueError('content identity SHA mismatch: %s'
                             % entry.get('shader_id'))
        for field, value in (
                ('stage_sha256', entry.get('stage_sha256')),
                ('gxp_sha256', entry.get('gxp_sha256')),
                ('cg_sha256', entry.get('cg_sha256')),
                ('environment_sha256', entry.get('environment_sha256')),
                ('flags', entry.get('flags')),
                ('profile', entry.get('profile')),
                ('strategy', entry.get('strategy')),
                ('policy_sha256', entry.get('policy_sha256')),
                ('emitter_sha256', entry.get('emitter_sha256')),
                ('helper_sha256', entry.get('helper_sha256'))):
            if identity.get(field) != value:
                raise ValueError('content identity %s differs from entry: %s'
                                 % (field, entry.get('shader_id')))
        if entry.get('viewport_mask') != VIEWPORT_ONE:
            raise ValueError('shadow variants must target viewport 1')
        if entry.get('input_signature') != INPUT_SIGNATURE:
            raise ValueError('shadow variants must use generic input signature')
        if entry.get('flags') != list(expected_flags):
            raise ValueError('shadow variants have unexpected compiler flags')
        if entry.get('profile') != PROFILE:
            raise ValueError('shadow variants must use the fragment profile')
        key = variant_key(bytes.fromhex(entry['stage_sha256']),
                          bytes.fromhex(entry['gxp_sha256']),
                          ABI_CANDIDATE, entry['input_signature'],
                          entry.get('environment_sha256') or '',
                          entry.get('flags') or [], computed)
        if key.hex() != entry.get('expected_variant_key'):
            raise ValueError('expected variant key differs from the ABI 2 '
                             'content-identity formula: %s' % entry['shader_id'])
        if key in keys:
            raise ValueError('duplicate expected variant key')
        keys.add(key)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-bank', type=Path,
                        default=ROOT / 'artifacts/m6b14-village-materials-20261026/candidate-bank')
    parser.add_argument('--base-manifest', type=Path, default=None)
    parser.add_argument('--compile-root', type=Path,
                        default=ROOT / 'artifacts/m6b24-shadow-bank-20261027/actual9')
    parser.add_argument('--compile-checkpoint', type=Path, default=None)
    parser.add_argument('--compile-manifest', type=Path, default=None)
    parser.add_argument('--catalogue', type=Path,
                        default=ROOT / 'artifacts/p2-rebuild-20261025/audit-vp1/shader-catalogue.jsonl')
    parser.add_argument('--resolved-root', type=Path,
                        default=ROOT / 'artifacts/a2-20261022/resolved')
    parser.add_argument('--shacc', type=Path,
                        default=ROOT / 'local/shader-tools/shacc-bin/Vita3K-Shacc-windows-x86_64/vita3k-shacc.exe')
    parser.add_argument('--shacc-config', type=Path,
                        default=ROOT / 'local/shader-tools/shacc.conf')
    parser.add_argument('--expected-base-index-sha256',
                        default=EXPECTED_BASE_INDEX_SHA256)
    parser.add_argument('--expected-base-index-json-sha256',
                        default=EXPECTED_BASE_INDEX_JSON_SHA256)
    parser.add_argument('--expected-base-manifest-sha256',
                        default=EXPECTED_BASE_MANIFEST_SHA256)
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'artifacts/m6b24-shadow-bank-20261027/shadow-candidate-spec.json')
    parser.add_argument('--optimization', choices=('O0', 'O1'), default='O0',
                        help='compiler optimization whose nine receipts are '
                             'admitted; default O0 preserves the historical spec')
    parser.add_argument('--task', default=TASK,
                        help='task label for the new spec and content identity; '
                             'compile evidence still requires M6-B24 receipts')
    args = parser.parse_args()
    args.compile_root = args.compile_root.resolve()
    args.base_bank = args.base_bank.resolve()
    args.output = args.output.resolve()
    flag = '-' + args.optimization
    result_key = flag.replace('-', '').lower()

    base_manifest_path = args.base_manifest or (args.base_bank / 'manifest.json')
    checkpoint_path = args.compile_checkpoint or (args.compile_root / 'checkpoint.json')
    compile_manifest_path = args.compile_manifest or (args.compile_root / 'manifest.json')

    base_index_path = args.base_bank / 'shader-index.bin'
    base_json_path = args.base_bank / 'shader-index.json'
    if digest(base_index_path) != args.expected_base_index_sha256:
        raise ValueError('base index SHA differs from the immutable expectation')
    if digest(base_json_path) != args.expected_base_index_json_sha256:
        raise ValueError('base index JSON SHA differs from the immutable expectation')
    if digest(base_manifest_path) != args.expected_base_manifest_sha256:
        raise ValueError('base manifest SHA differs from the immutable expectation')
    base = json.loads(base_json_path.read_text(encoding='utf-8'))
    base_manifest = json.loads(base_manifest_path.read_text(encoding='utf-8'))
    counts = {
        'stages': len(base['stages']),
        'variants': len(base['variants']),
        'programs': len(base['programs']),
    }
    if base.get('abi_version') != ABI_CANDIDATE or counts != EXPECTED_COUNTS:
        raise ValueError('base bank ABI/counts differ from the ABI 2 expectation')
    if base_manifest.get('index_sha256') != args.expected_base_index_sha256:
        raise ValueError('base manifest index SHA mismatch')
    if base_manifest.get('index_json_sha256') != args.expected_base_index_json_sha256:
        raise ValueError('base manifest index JSON SHA mismatch')
    if base_manifest.get('candidate_counts') != EXPECTED_COUNTS:
        raise ValueError('base manifest candidate counts mismatch')
    base_counts = base_manifest.get('base_counts') or {}
    if (base_manifest.get('added_variants') != 4
            or base_counts.get('stages') != EXPECTED_COUNTS['stages']
            or base_counts.get('programs') != EXPECTED_COUNTS['programs']
            or base_counts.get('variants') + 4 != EXPECTED_COUNTS['variants']):
        raise ValueError('base manifest is not the four-added M6-B14 bank')

    checkpoint = json.loads(checkpoint_path.read_text(encoding='utf-8'))
    compile_manifest = json.loads(compile_manifest_path.read_text(encoding='utf-8'))
    if checkpoint.get('task') != COMPILE_TASK or \
            compile_manifest.get('task') != COMPILE_TASK:
        raise ValueError('compile evidence task mismatch')
    if checkpoint.get('optimizations') != [flag]:
        raise ValueError('compile batch optimization differs from the request')
    if checkpoint.get('service_timeout_seconds', 0) < EXPECTED_SERVICE_TIMEOUT:
        raise ValueError('compile service timeout is below the proven 300 s bound')
    if checkpoint.get('outer_timeout_seconds', 0) <= checkpoint['service_timeout_seconds']:
        raise ValueError('compile outer timeout is not above the service timeout')
    if checkpoint.get('strategy') != STRATEGY:
        raise ValueError('compile strategy mismatch')
    if checkpoint.get('policy_sha256') != policy_sha256():
        raise ValueError('compile policy mismatch')
    if checkpoint.get('emitter_sha256') != emitter_source_sha256():
        raise ValueError('compile emitter mismatch')
    if checkpoint.get('helper_sha256') != helper_source_sha256():
        raise ValueError('compile helper mismatch')
    if checkpoint.get('complete') is not True:
        raise ValueError('compile checkpoint is not complete')
    if sorted(checkpoint.get('compiled_stages', [])) != sorted(csc.OBSERVED):
        raise ValueError('compile checkpoint does not contain exactly nine '
                         'compiled stages')
    _, toolchain = csc.load_shacc_identity(args.shacc, args.shacc_config)
    if checkpoint.get('toolchain') != csc.stage_toolchain_identity(toolchain):
        raise ValueError('compile checkpoint toolchain differs from the locked one')
    if compile_manifest.get('optimizations') != [flag]:
        raise ValueError('compile manifest optimization differs from the request')
    if not (compile_manifest.get('claims') or {}).get(
            'observed_stages_compiled_' + result_key):
        raise ValueError('compile manifest does not claim nine %s stages'
                         % args.optimization)
    if args.optimization == 'O0' and not (compile_manifest.get('claims') or {}).get(
            'o1_not_attempted_in_this_batch'):
        raise ValueError('O0 compile manifest must record that O1 was not attempted')
    if sorted(checkpoint.get('stages', {})) != sorted(csc.OBSERVED):
        raise ValueError('compile checkpoint stage set differs from the nine')

    catalogue = load_catalogue(args.catalogue)
    stage_by_id = {row['shader_id']: row for row in base['stages']}
    variants_by_stage = {}
    for variant in base['variants']:
        variants_by_stage.setdefault(variant['stage_index'], []).append(variant)

    add = []
    for shader_id in csc.OBSERVED:
        stage = stage_by_id.get(shader_id)
        record = catalogue.get(shader_id)
        if stage is None or record is None:
            raise ValueError('stage missing from base bank or catalogue: ' + shader_id)
        if stage.get('stage') != 'frag' or record.get('stage') != 'frag':
            raise ValueError('shadow stage is not a fragment stage: ' + shader_id)
        if stage['stage_sha256'] != record['maxwell_sha256']:
            raise ValueError('base stage SHA differs from catalogue: ' + shader_id)
        if stage.get('source_flags') != 1:
            raise ValueError('base stage source flags are not fragment: ' + shader_id)
        existing = [variant for variant in variants_by_stage.get(stage['index'], [])
                    if variant['viewport_mask'] & VIEWPORT_ONE
                    and variant.get('input_signature', 0) == INPUT_SIGNATURE]
        if existing:
            raise ValueError('stage already has a generic viewport-1 variant: '
                             + shader_id)
        resolution_path = args.resolved_root / shader_id / 'resolution.json'
        glsl_path = args.resolved_root / shader_id / 'shader.glsl'
        resources_path = args.resolved_root / shader_id / 'shader.glsl.resources.json'
        resolution = json.loads(resolution_path.read_text(encoding='utf-8'))
        resolved = resolution['resolved']
        checks = {
            'maxwell': resolution.get('maxwell_sha256') == record['maxwell_sha256'],
            'environment': resolved.get('environment_sha256') ==
                           record['environment_sha256'],
            'resources': resolved.get('resources_sha256') ==
                         record['resources_sha256'],
            'glsl': resolved.get('glsl_sha256') == digest(glsl_path),
            'resources_file': resolved.get('resources_sha256') == digest(resources_path),
        }
        if not all(checks.values()):
            raise ValueError('resolved identity mismatch for %s: %r'
                             % (shader_id, checks))
        reflection = json.loads(resources_path.read_text(encoding='utf-8'))
        catalogue_textures = (record.get('resources') or {}).get('textures')
        if reflection.get('textures') != catalogue_textures:
            raise ValueError('catalogue and resolved texture descriptors differ: '
                             + shader_id)
        depth_slots = [index for index, texture in enumerate(catalogue_textures)
                       if texture.get('depth') == 1]
        if not depth_slots:
            raise ValueError('shadow stage has no depth descriptor: ' + shader_id)
        if any(texture.get('depth') not in (0, 1)
               for texture in catalogue_textures):
            raise ValueError('unknown depth flag in descriptors: ' + shader_id)

        stage_dir = args.compile_root / 'stages' / shader_id
        cg_path = stage_dir / 'shader.frag.cg'
        cg_sha = digest(cg_path)
        cg_bytes = cg_path.stat().st_size
        receipt_path = stage_dir / ('receipt%s.json' % result_key)
        stage_checkpoint_path = stage_dir / ('checkpoint%s.json' % result_key)
        receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
        stage_checkpoint = json.loads(
            stage_checkpoint_path.read_text(encoding='utf-8'))
        for row, label in ((receipt, 'receipt'), (stage_checkpoint, 'checkpoint')):
            if row.get('shader_id') != shader_id:
                raise ValueError('%s shader id mismatch: %s' % (label, shader_id))
            if row.get('status', row.get('classification')) != 'compiled':
                raise ValueError('%s is not compiled: %s' % (label, shader_id))
            if row.get('gxp_sha256') is None:
                raise ValueError('%s lacks a GXP SHA: %s' % (label, shader_id))
        if receipt.get('classification') != 'compiled':
            raise ValueError('receipt classification is not compiled: ' + shader_id)
        if receipt.get('optimization') != flag or receipt.get('flags') != [flag]:
            raise ValueError('receipt optimization differs from the request: '
                             + shader_id)
        if receipt.get('profile') != PROFILE:
            raise ValueError('receipt profile mismatch: ' + shader_id)
        if receipt.get('source_sha256') != cg_sha or receipt.get('source_bytes') != cg_bytes:
            raise ValueError('receipt source Cg identity mismatch: ' + shader_id)
        if receipt.get('environment_sha256') != record['environment_sha256']:
            raise ValueError('receipt environment mismatch: ' + shader_id)
        if receipt.get('stage_sha256') != record['maxwell_sha256']:
            raise ValueError('receipt stage SHA mismatch: ' + shader_id)
        if receipt.get('strategy') != STRATEGY or \
                receipt.get('policy_sha256') != policy_sha256() or \
                receipt.get('emitter_sha256') != emitter_source_sha256() or \
                receipt.get('helper_sha256') != helper_source_sha256():
            raise ValueError('receipt shadow contract mismatch: ' + shader_id)
        if receipt.get('toolchain') != csc.stage_toolchain_identity(toolchain):
            raise ValueError('receipt toolchain mismatch: ' + shader_id)
        if receipt.get('service_timeout_seconds', 0) < EXPECTED_SERVICE_TIMEOUT:
            raise ValueError('receipt service timeout below the proven bound: '
                             + shader_id)
        stage_entry = {
            'shader_id': shader_id,
            'catalogue_maxwell_sha256': record['maxwell_sha256'],
            'environment_sha256': record['environment_sha256'],
            'cg_sha256': cg_sha,
        }
        expected_identity = csc.stage_identity(stage_entry, flag, toolchain)
        if receipt.get('identity_sha256') != expected_identity:
            raise ValueError('receipt identity differs from the current evidence: '
                             + shader_id)
        if stage_checkpoint.get('identity_sha256') != expected_identity:
            raise ValueError('stage checkpoint identity mismatch: ' + shader_id)

        gxp_path = stage_dir / ('shader.frag%s.gxp' % result_key.upper())
        gxp = valid_gxp(gxp_path.read_bytes())
        gxp_sha = hashlib.sha256(gxp).hexdigest()
        if receipt.get('gxp_sha256') != gxp_sha or receipt.get('gxp_bytes') != len(gxp):
            raise ValueError('receipt GXP identity mismatch: ' + shader_id)
        if stage_checkpoint.get('gxp_sha256') != gxp_sha or \
                stage_checkpoint.get('gxp_bytes') != len(gxp):
            raise ValueError('stage checkpoint GXP identity mismatch: ' + shader_id)
        if compile_manifest['counts']['compiled_' + result_key] != len(csc.OBSERVED):
            raise ValueError('compile manifest compiled_%s count is not nine'
                             % result_key)
        manifest_stage = next(
            (row for row in compile_manifest['stages']
             if row['shader_id'] == shader_id), None)
        if manifest_stage is None or \
                manifest_stage.get('cg_sha256') != cg_sha or \
                (manifest_stage.get(result_key) or {}).get('gxp_sha256') != gxp_sha:
            raise ValueError('compile manifest stage identity mismatch: ' + shader_id)
        shadow_samplers = manifest_stage['shadow_contract']['samplers']
        for sampler in shadow_samplers:
            slot = int(sampler[3:])
            if slot not in depth_slots:
                raise ValueError('shadow sampler %s is not a depth descriptor'
                                 % sampler)
        if len({int(sampler[3:]) for sampler in shadow_samplers}) != len(depth_slots):
            raise ValueError('shadow sampler/depth descriptor count differs: '
                             + shader_id)

        base_entry = {
            'shader_id': shader_id,
            'stage': 'frag',
            'stage_sha256': stage['stage_sha256'],
            'stage_index': stage['index'],
            'environment_sha256': record['environment_sha256'],
            'resources_sha256': record['resources_sha256'],
            'cg_sha256': cg_sha,
            'cg_bytes': cg_bytes,
            'gxp_sha256': gxp_sha,
            'gxp_bytes': len(gxp),
            'flags': [flag],
            'toolchain': csc.stage_toolchain_identity(toolchain),
            'service_timeout_seconds': receipt['service_timeout_seconds'],
            'outer_timeout_seconds': receipt['outer_timeout_seconds'],
        }
        identity = content_identity_for(base_entry, args.task)
        identity_sha = hashlib.sha256(canonical(identity)).hexdigest()
        entry = {
            'stage_index': stage['index'],
            'stage': 'frag',
            'shader_id': shader_id,
            'stage_sha256': stage['stage_sha256'],
            'viewport_mask': VIEWPORT_ONE,
            'input_signature': INPUT_SIGNATURE,
            'source_kind': 'shadow-emitter-' + result_key,
            'gxp_source': str(gxp_path.relative_to(ROOT)).replace('\\', '/'),
            'gxp_sha256': gxp_sha,
            'gxp_bytes': len(gxp),
            'flags': [flag],
            'environment_sha256': record['environment_sha256'],
            'profile': PROFILE,
            'strategy': STRATEGY,
            'policy_sha256': policy_sha256(),
            'emitter_sha256': emitter_source_sha256(),
            'helper_sha256': helper_source_sha256(),
            'cg_path': str(cg_path.relative_to(ROOT)).replace('\\', '/'),
            'cg_sha256': cg_sha,
            'cg_bytes': cg_bytes,
            'reflection_path': str(resources_path.relative_to(ROOT)).replace('\\', '/'),
            'resources_sha256': record['resources_sha256'],
            'receipt': str(receipt_path.relative_to(ROOT)).replace('\\', '/'),
            'receipt_sha256': digest(receipt_path),
            'receipt_imported': bool(receipt.get('imported')),
            'import_provenance': receipt.get('import_provenance'),
            'content_identity': identity,
            'content_identity_sha256': identity_sha,
            'expected_variant_key': variant_key(
                bytes.fromhex(stage['stage_sha256']), bytes.fromhex(gxp_sha),
                ABI_CANDIDATE, INPUT_SIGNATURE, record['environment_sha256'],
                [flag], identity_sha).hex(),
            'provenance': [
                'm6b27-shadow-' + result_key + '-300',
                'compile-checkpoint-sha256:' + digest(checkpoint_path),
                'compile-receipt-sha256:' + digest(receipt_path),
                'content-identity-sha256:' + identity_sha,
            ],
        }
        add.append(entry)

    spec = {
        'schema': SCHEMA,
        'mode': 'augment-static-index',
        'task': args.task,
        'description': 'Exactly nine observed sampler2DShadow stages lowered '
                       'by the shared statement-inline emitter and compiled '
                       '%s under the proven 300 s Shacc service bound.'
                       % args.optimization,
        'date': '2026-10-27',
        'base_index_sha256': args.expected_base_index_sha256,
        'base_shader_index_json_sha256': args.expected_base_index_json_sha256,
        'base_manifest_sha256': args.expected_base_manifest_sha256,
        'base_abi_version': ABI_CANDIDATE,
        'base_counts': counts,
        'compile_evidence': {
            'root': str(args.compile_root),
            'checkpoint': {'path': str(checkpoint_path),
                           'sha256': digest(checkpoint_path)},
            'manifest': {'path': str(compile_manifest_path),
                         'sha256': digest(compile_manifest_path)},
            'optimizations': [flag],
            'service_timeout_seconds': checkpoint['service_timeout_seconds'],
            'outer_timeout_seconds': checkpoint['outer_timeout_seconds'],
            'toolchain': csc.stage_toolchain_identity(toolchain),
        },
        'identity_contract': {
            'key_formula': 'sha256(canonical({schema:2, stage_sha256, '
                           'gxp_sha256, abi_version, input_signature, '
                           'environment_sha256, flags, '
                           'content_identity_sha256}))[:16]',
            'content_identity_sha256': 'sha256(canonical(content_identity))',
            'content_identity_fields': [
                'shader_id', 'stage_sha256', 'environment_sha256',
                'resources_sha256', 'cg_sha256', 'cg_bytes', 'gxp_sha256',
                'gxp_bytes', 'profile', 'flags', 'strategy', 'policy_sha256',
                'emitter_sha256', 'helper_sha256', 'toolchain',
                'service_timeout_seconds', 'outer_timeout_seconds'],
            'retained_rule': 'the 282 base variant keys, bytes, flags and '
                             'signatures are copied unchanged; the extra key '
                             'field applies only to these nine additions',
            'limit': 'Offline identity only; no runtime link, draw, pixel, '
                     'atlas, game, batching, performance or hardware claim.',
        },
        'add': add,
        'replace': [],
        'blocked': [],
        'audit': {
            'stage_ids': [entry['shader_id'] for entry in add],
            'stage_indices': [entry['stage_index'] for entry in add],
            'depth_descriptor_total': sum(
                sum(1 for texture in (catalogue[entry['shader_id']]['resources']
                                      .get('textures') or [])
                    if texture.get('depth') == 1)
                for entry in add),
            'viewport_mask': VIEWPORT_ONE,
            'input_signature': INPUT_SIGNATURE,
            'flags': [flag],
            'all_receipts_compiled': True,
            'o0_attempted': args.optimization == 'O0',
            'o1_attempted': args.optimization == 'O1',
        },
        'notes': [
            'Only the nine stages already present in the 2402-stage base table '
            'gain a variant; no stage, program or GXP is replaced or dropped.',
            'The first two GXPs are imported from the M6-B23 300 s trial and '
            'the M6-B25 O1 receipt only after source, toolchain, flags, policy, '
            'emitter and GXP identity matched.',
            'The remaining seven GXPs are fresh %s compilations in M6-B27.'
            % args.optimization,
            'The content identity is offline metadata binding; it is not a '
            'runtime or pixel claim.',
        ],
    }
    validate_base_binding(spec, args.expected_base_index_sha256,
                          args.expected_base_index_json_sha256,
                          args.expected_base_manifest_sha256)
    validate_spec_shape(spec, csc.OBSERVED, (flag,))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(spec, indent=2, sort_keys=True) + '\n',
                           encoding='utf-8', newline='\n')
    print(json.dumps({
        'output': str(args.output),
        'spec_sha256': digest(args.output),
        'added': len(add),
        'stage_indices': [entry['stage_index'] for entry in add],
        'keys': [entry['expected_variant_key'] for entry in add],
        'imported': sum(1 for entry in add if entry['receipt_imported']),
        'base_index_sha256': args.expected_base_index_sha256,
    }, indent=2))


if __name__ == '__main__':
    main()

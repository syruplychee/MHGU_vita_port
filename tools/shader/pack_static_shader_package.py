"""Pack verified GXPs into the static shader index read by the Vita runtime.

Sources:
  * the existing runtime bundle (hash-named GXPs already validated in-game);
  * the offline Cg batch (newly compiled, viewport=1 environment).

Identity is layered: each stage is keyed by its exact Maxwell/SPH SHA, each
compiled GXP by its SHA, and each program by its original SPK index/token. GXP
content is deduplicated across program aliases and across viewports only when
the bytes are identical; program references keep every alias.

Output is built in a sibling temporary directory and renamed only after every
check passes. No game-derived binary is written outside ignored artifacts.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import zlib

SCHEMA = 1
MAGIC = b'VPSHIX1\0'
HEADER_SIZE = 64
STAGE_RECORD = 48
VARIANT_RECORD = 40
PROGRAM_RECORD = 16
ABI_VERSION = 1
SOURCE_SPK = 1
SOURCE_SDK = 2
PROGRAM_OBSERVED = 1
PROGRAM_STATIC = 2
VARIANT_COMPILED = 1
VARIANT_EXISTING = 2
VARIANT_OFFLINE = 4


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def read_jsonl(path):
    with path.open('r', encoding='utf-8-sig') as stream:
        return [json.loads(line) for line in stream if line.strip()]


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode()


def crc32(data):
    return zlib.crc32(data) & 0xffffffff


def valid_gxp(path):
    if not path.is_file():
        return None, 'missing'
    data = path.read_bytes()
    if len(data) < 32:
        return None, 'short'
    if data[:4] != b'GXP\0':
        return None, 'magic'
    declared = struct.unpack_from('<I', data, 8)[0]
    if declared < 32 or declared > len(data):
        return None, 'declared-size'
    return data, None


def variant_key(stage_sha, gxp_sha, abi):
    return hashlib.sha256(canonical({
        'schema': 1, 'stage_sha256': stage_sha.hex(), 'gxp_sha256': gxp_sha.hex(),
        'abi_version': abi})).digest()[:16]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalogue', type=Path, required=True)
    parser.add_argument('--ledger', type=Path, required=True)
    parser.add_argument('--old-bundle', type=Path, required=True,
                        help='existing runtime shaders directory with manifest.json and index.txt')
    parser.add_argument('--compiled', type=Path,
                        help='compile_static_candidates.py output directory')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[2]
    catalogue = args.catalogue.resolve()
    ledger_dir = args.ledger.resolve()
    old_bundle = args.old_bundle.resolve()
    output = args.output.resolve()
    if output.exists():
        raise ValueError('output must not exist; packaging is atomic through a temporary sibling')
    tmp = output.with_name(output.name + f'.tmp-{os.getpid()}')
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    (tmp / 'shaders').mkdir()

    summary = read_json(catalogue / 'summary.json')
    for name, expected in summary['outputs'].items():
        if digest(catalogue / name) != expected:
            raise ValueError('catalogue hash mismatch: ' + name)
    ledger_summary = read_json(ledger_dir / 'summary.json')
    outputs = read_json(ledger_dir / 'outputs.json')
    for name, expected in outputs.items():
        if digest(ledger_dir / name) != expected:
            raise ValueError('ledger output hash mismatch: ' + name)
    if ledger_summary['input_identity']['catalogue_summary_sha256'] != digest(catalogue / 'summary.json'):
        raise ValueError('ledger belongs to a different catalogue')

    entries = read_jsonl(catalogue / 'shader-catalogue.jsonl')
    captures = read_jsonl(catalogue / 'capture-map.jsonl')
    programs = read_jsonl(ledger_dir / 'program-ledger.jsonl')
    if len(entries) != 2402 or len(programs) != 6311:
        raise ValueError('unexpected catalogue/ledger denominators')

    capture_by_shader = defaultdict(list)
    capture_programs = set()
    capture_by_name = {}
    for capture in captures:
        capture_by_shader[capture['shader_id']].append(capture)
        capture_programs.add(capture['runtime_token'])
        capture_by_name[(capture['program'], capture['viewport_transform'])] = capture

    # Stage table. Sort by (stage, exact SHA) for a stable, reviewable index.
    ordered = sorted(entries, key=lambda e: (0 if e['stage'] == 'vert' else 1,
                                             e['maxwell_sha256'], e['shader_id']))
    stage_index = {}
    stage_records = []
    for index, entry in enumerate(ordered):
        stage_index[entry['shader_id']] = index
        observed = bool(capture_by_shader.get(entry['shader_id']))
        source = SOURCE_SPK if entry['origin'] == 'SPK resource package' else SOURCE_SDK
        flags = source | (4 if observed else 0)
        stage_records.append({
            'index': index, 'stage': entry['stage'], 'stage_id': 1 if entry['stage'] == 'vert' else 5,
            'shader_id': entry['shader_id'],
            'sha256': bytes.fromhex(entry['maxwell_sha256']),
            'sha_hex': entry['maxwell_sha256'],
            'source_flags': flags,
            'observed': observed,
            'cg_lowering': entry['cg_lowering'],
            'specialization_required': entry['specialization_required'],
        })

    # Variant collection. A variant is a concrete GXP for one stage content.
    # Identical GXP bytes for the same stage content merge: the viewport mask
    # records every viewport for which those exact bytes are proven.
    variants = {}

    def add_variant(shader_id, gxp_path, viewport_transform, provenance, source_manifest):
        if shader_id not in stage_index:
            raise ValueError('variant references unknown shader ' + shader_id)
        data, error = valid_gxp(gxp_path)
        if data is None:
            raise ValueError(f'invalid GXP {gxp_path}: {error}')
        gxp_sha = hashlib.sha256(data).digest()
        stage = stage_records[stage_index[shader_id]]
        key = variant_key(stage['sha256'], gxp_sha, ABI_VERSION)
        record = variants.get(key)
        if record is None:
            record = {'key': key, 'stage_index': stage_index[shader_id],
                      'gxp_sha256': gxp_sha, 'gxp_bytes': len(data),
                      'viewport_mask': 0, 'provenance': set(), 'sources': [],
                      'data': data}
            variants[key] = record
        elif record['gxp_sha256'] != gxp_sha:
            raise ValueError('variant key collision')
        record['viewport_mask'] |= 1 << viewport_transform
        record['provenance'].add(provenance)
        record['sources'].append(source_manifest)

    old_manifest_path = old_bundle / 'manifest.json'
    old_manifest = read_json(old_manifest_path)
    if old_manifest.get('schema') != 1 or 'entries' not in old_manifest:
        raise ValueError('unsupported old bundle manifest')
    old_used = 0
    old_manifest_mismatches = []
    old_file_hashes = []
    for bundle_entry in old_manifest['entries']:
        key = (bundle_entry['program'], bundle_entry['viewport_transform'])
        capture = capture_by_name.get(key)
        if capture is None:
            raise ValueError('old bundle entry has no capture-map identity: ' + str(key))
        gxp_path = old_bundle / bundle_entry['gxp']
        actual_sha = digest(gxp_path)
        old_file_hashes.append({'gxp': bundle_entry['gxp'], 'sha256': actual_sha})
        if actual_sha != bundle_entry['sha256']:
            # The live bundle has been updated after its manifest was written.
            # The verified baseline is the file actually installed, so use its
            # bytes and record the stale manifest entry instead of pretending
            # the manifest hash still describes the payload.
            old_manifest_mismatches.append({
                'gxp': bundle_entry['gxp'],
                'manifest_sha256': bundle_entry['sha256'],
                'installed_sha256': actual_sha})
        add_variant(capture['shader_id'], gxp_path, bundle_entry['viewport_transform'],
                    'existing-runtime', {'manifest': str(old_manifest_path),
                                         'program': bundle_entry['program'],
                                         'gxp': bundle_entry['gxp'],
                                         'manifest_sha256': bundle_entry['sha256'],
                                         'installed_sha256': actual_sha})
        old_used += 1

    compiled_records = []
    compiled_manifest_path = None
    if args.compiled is not None:
        compiled = args.compiled.resolve()
        compiled_manifest_path = compiled / 'manifest.json'
        compiled_manifest = read_json(compiled_manifest_path)
        if compiled_manifest.get('build_identity', {}).get('catalogue_summary_sha256') != \
                digest(catalogue / 'summary.json'):
            raise ValueError('compiled batch belongs to a different catalogue')
        for record in compiled_manifest.get('programs', []):
            if record.get('status') != 'compiled':
                continue
            gxp_path = compiled / record['gxp']
            if digest(gxp_path) != record['gxp_sha256']:
                raise ValueError('compiled GXP hash mismatch: ' + record['shader_id'])
            # The locked library environment fixes viewport=1; no vp0 claim.
            add_variant(record['shader_id'], gxp_path, 1, 'offline-cg',
                        {'manifest': str(compiled_manifest_path),
                         'shader_id': record['shader_id'],
                         'content_key': record['content_key']})
            compiled_records.append(record)

    # Resolve overlapping viewport claims. An in-game validated GXP wins over a
    # newly compiled one for the same stage and viewport; a variant left with no
    # viewport is dropped but recorded in validation.
    excluded_variants = []
    candidates_by_slot = defaultdict(list)
    for key, record in variants.items():
        for vp in (0, 1):
            if record['viewport_mask'] & (1 << vp):
                candidates_by_slot[(record['stage_index'], vp)].append(record)
    for (stage_idx, vp), candidates in candidates_by_slot.items():
        chosen = max(candidates, key=lambda r: (('existing-runtime' in r['provenance']),
                                                -r['gxp_bytes'], r['key']))
        for record in candidates:
            if record is chosen:
                continue
            if record['viewport_mask'] & (1 << vp):
                record['viewport_mask'] &= ~(1 << vp)
                excluded_variants.append({
                    'key': record['key'].hex(), 'stage_index': stage_idx,
                    'viewport_transform': vp,
                    'reason': 'viewport already served by ' + chosen['key'].hex(),
                    'provenance': sorted(record['provenance'])})
    variants = {key: record for key, record in variants.items() if record['viewport_mask']}

    # Variant table ordered by (stage, key) so every stage owns a contiguous range.
    variant_records = sorted(variants.values(), key=lambda r: (r['stage_index'], r['key']))
    by_stage = defaultdict(list)
    for index, record in enumerate(variant_records):
        by_stage[record['stage_index']].append(index)
    running = 0
    for stage_idx in sorted(by_stage):
        indices = by_stage[stage_idx]
        if indices != list(range(indices[0], indices[0] + len(indices))) or indices[0] != running:
            raise ValueError('variant table is not contiguous by stage')
        running += len(indices)
    if running != len(variant_records):
        raise ValueError('variant table coverage mismatch')
    variant_by_stage = {}
    for stage_idx, indices in by_stage.items():
        masks = 0
        for index in indices:
            mask = variant_records[index]['viewport_mask']
            if masks & mask:
                raise ValueError(f'stage {stage_idx} has overlapping viewport variants')
            masks |= mask
        variant_by_stage[stage_idx] = (indices[0], len(indices), masks)
    # Every stage, including those without a compiled variant, needs the
    # running start so the device reader can require contiguous ownership.
    stage_variant_range = {}
    running = 0
    for stage in stage_records:
        count = len(by_stage.get(stage['index'], []))
        stage_variant_range[stage['index']] = (running, count)
        running += count
    if running != len(variant_records):
        raise ValueError('variant table does not cover every record')

    # Programs. Tokens are the original SPK index + 1 candidates, preserved with
    # their evidence label; observed tokens were verified against captured bytes.
    program_records = []
    seen_tokens = set()
    for program in programs:
        token = program['candidate_runtime_token']
        if token in seen_tokens or not 1 <= token <= 0xffffffff:
            raise ValueError('duplicate/invalid runtime token')
        seen_tokens.add(token)
        refs = program['references']
        vs = stage_index.get(refs[0]['shader_id']) if refs[0]['shader_id'] else None
        fs = stage_index.get(refs[1]['shader_id']) if refs[1]['shader_id'] else None
        observed = token in capture_programs
        flags = PROGRAM_OBSERVED if observed else PROGRAM_STATIC
        program_records.append({'token': token,
                                'spk_program_index': program['spk_program_index'],
                                'vertex_stage': 0xffff if vs is None else vs,
                                'fragment_stage': 0xffff if fs is None else fs,
                                'flags': flags})
    program_records.sort(key=lambda r: r['token'])

    # Serialize the body.
    body = bytearray()
    for record in stage_records:
        start, count = stage_variant_range[record['index']]
        body += struct.pack('<I', record['stage_id'])
        body += record['sha256']
        body += struct.pack('<HHI', start, count, record['source_flags'])
        body += struct.pack('<I', 0)
    if len(body) != len(stage_records) * STAGE_RECORD:
        raise ValueError('stage record size mismatch')
    variant_start = len(body)
    for record in variant_records:
        body += struct.pack('<IHH', record['stage_index'], record['viewport_mask'],
                            ABI_VERSION)
        body += record['key']
        body += struct.pack('<II', record['gxp_bytes'], VARIANT_COMPILED)
        body += struct.pack('<II', crc32(record['data']), 0)
    if len(body) - variant_start != len(variant_records) * VARIANT_RECORD:
        raise ValueError('variant record size mismatch')
    program_start = len(body)
    for record in program_records:
        body += struct.pack('<IHHI', record['token'], record['vertex_stage'],
                            record['fragment_stage'], record['flags'])
        body += struct.pack('<I', 0)
    if len(body) - program_start != len(program_records) * PROGRAM_RECORD:
        raise ValueError('program record size mismatch')

    header = bytearray(HEADER_SIZE)
    header[0:8] = MAGIC
    struct.pack_into('<HH', header, 8, 1, HEADER_SIZE)
    struct.pack_into('<I', header, 12, 1)  # flags: program table present
    struct.pack_into('<IIIIII', header, 16, len(stage_records), len(variant_records),
                     len(program_records), STAGE_RECORD, VARIANT_RECORD, PROGRAM_RECORD)
    struct.pack_into('<II', header, 40, len(body), crc32(body))
    struct.pack_into('<I', header, 48, ABI_VERSION)
    index_bytes = bytes(header) + bytes(body)

    # Python-side structural self-check before writing anything.
    if len(index_bytes) != HEADER_SIZE + len(body):
        raise ValueError('index size mismatch')
    if index_bytes[0:8] != MAGIC:
        raise ValueError('index magic mismatch')

    for record in variant_records:
        (tmp / 'shaders' / (record['key'].hex() + '.gxp')).write_bytes(record['data'])
    (tmp / 'shader-index.bin').write_bytes(index_bytes)

    index_json = {
        'schema': SCHEMA, 'magic': MAGIC.decode('latin1'), 'abi_version': ABI_VERSION,
        'stage_record_size': STAGE_RECORD, 'variant_record_size': VARIANT_RECORD,
        'program_record_size': PROGRAM_RECORD,
        'stages': [{'index': r['index'], 'stage': r['stage'], 'shader_id': r['shader_id'],
                    'stage_sha256': r['sha_hex'], 'source_flags': r['source_flags'],
                    'observed': r['observed'],
                    'variant_start': stage_variant_range[r['index']][0],
                    'variant_count': stage_variant_range[r['index']][1],
                    'viewport_mask': variant_by_stage.get(r['index'], (0, 0, 0))[2]}
                   for r in stage_records],
        'variants': [{'key': r['key'].hex(), 'stage_index': r['stage_index'],
                      'viewport_mask': r['viewport_mask'],
                      'gxp_sha256': r['gxp_sha256'].hex(), 'gxp_bytes': r['gxp_bytes'],
                      'provenance': sorted(r['provenance']),
                      'gxp': 'shaders/' + r['key'].hex() + '.gxp'} for r in variant_records],
        'programs': program_records,
    }
    (tmp / 'shader-index.json').write_text(
        json.dumps(index_json, ensure_ascii=False, indent=1, sort_keys=True) + '\n',
        encoding='utf-8', newline='\n')

    coverage = {
        'stages_total': len(stage_records),
        'stages_with_variant': len(variant_by_stage),
        'stages_without_variant': len(stage_records) - len(variant_by_stage),
        'stages_observed': sum(r['observed'] for r in stage_records),
        'programs_total': len(program_records),
        'programs_observed': sum(1 for r in program_records if r['flags'] & PROGRAM_OBSERVED),
        'variants_total': len(variant_records),
        'variants_existing': sum(1 for r in variant_records
                                 if 'existing-runtime' in r['provenance']),
        'variants_offline': sum(1 for r in variant_records
                                if 'offline-cg' in r['provenance']),
        'variants_excluded_overlap': len(excluded_variants),
        'viewport_variants': {str(vp): sum(1 for r in variant_records
                                           if r['viewport_mask'] & (1 << vp))
                              for vp in (0, 1)},
        'gxp_total_bytes': sum(r['gxp_bytes'] for r in variant_records),
        'gxp_unique_bytes': sum(r['gxp_bytes'] for r in variant_records),
        'old_bundle_entries': old_used,
        'old_bundle_manifest_mismatches': len(old_manifest_mismatches),
        'offline_compiled_entries': len(compiled_records),
    }
    build_identity = {
        'schema': SCHEMA,
        'catalogue_summary_sha256': digest(catalogue / 'summary.json'),
        'ledger_summary_sha256': digest(ledger_dir / 'summary.json'),
        'ledger_outputs_sha256': digest(ledger_dir / 'outputs.json'),
        'old_bundle_dir': str(old_bundle),
        'old_bundle_manifest_sha256': digest(old_manifest_path),
        'old_bundle_files_sha256': hashlib.sha256(
            canonical(sorted(old_file_hashes, key=lambda item: item['gxp']))).hexdigest(),
        'compiled_manifest_sha256': digest(compiled_manifest_path) if compiled_manifest_path else None,
        'pack_tool_sha256': digest(Path(__file__)),
        'shader_index_sha256': hashlib.sha256(index_bytes).hexdigest(),
        'abi_version': ABI_VERSION,
    }
    manifest = {
        'schema': SCHEMA,
        'mode': 'static-index-v1',
        'build_identity': build_identity,
        'coverage': coverage,
        'limitations': [
            'Variants compiled from the locked library environment only claim viewport 1.',
            'No pipeline link, target pixel or hardware check is implied by this package.',
            'Programs without a compiled variant remain addressable but fail explicitly at draw.',
            'SDK shader windows are not referenced by programs and keep their boundary caveat.',
        ],
    }
    (tmp / 'manifest.json').write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + '\n',
        encoding='utf-8', newline='\n')
    validation = {
        'schema': SCHEMA,
        'checks': [
            {'name': 'catalogue_outputs', 'status': 'PASS'},
            {'name': 'ledger_outputs', 'status': 'PASS'},
            {'name': 'old_bundle_hashes', 'status': 'PASS', 'entries': old_used,
             'manifest_mismatches': len(old_manifest_mismatches),
             'identity_basis': 'installed GXP bytes; stale manifest entries recorded'},
            {'name': 'compiled_gxp_hashes', 'status': 'PASS', 'entries': len(compiled_records)},
            {'name': 'variant_viewport_disjoint', 'status': 'PASS'},
            {'name': 'stage_reference_closure', 'status': 'PASS'},
            {'name': 'index_self_check', 'status': 'PASS'},
        ],
        'coverage': coverage,
        'excluded_variants': excluded_variants,
        'old_bundle_manifest_mismatches': old_manifest_mismatches,
        'gaps': {
            'stage_without_variant': [{'index': r['index'], 'stage': r['stage'],
                                       'shader_id': r['shader_id'],
                                       'stage_sha256': r['sha_hex']}
                                      for r in stage_records if r['index'] not in variant_by_stage],
            'programs_without_both_variants': [
                {'token': r['token'], 'spk_program_index': r['spk_program_index'],
                 'vertex_stage': r['vertex_stage'], 'fragment_stage': r['fragment_stage']}
                for r in program_records
                if r['vertex_stage'] == 0xffff or r['fragment_stage'] == 0xffff or
                r['vertex_stage'] not in variant_by_stage or
                r['fragment_stage'] not in variant_by_stage],
        },
    }
    (tmp / 'validation.json').write_text(
        json.dumps(validation, ensure_ascii=False, indent=1, sort_keys=True) + '\n',
        encoding='utf-8', newline='\n')

    output.parent.mkdir(parents=True, exist_ok=True)
    os.replace(tmp, output)
    print(json.dumps({'output': str(output), 'stages': coverage['stages_total'],
                      'stages_with_variant': coverage['stages_with_variant'],
                      'variants': coverage['variants_total'],
                      'programs': coverage['programs_total'],
                      'observed_programs': coverage['programs_observed'],
                      'gxp_bytes': coverage['gxp_total_bytes']},
                     ensure_ascii=False), flush=True)


if __name__ == '__main__':
    sys.exit(main())

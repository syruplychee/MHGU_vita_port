"""Build the versioned Stage A ledger from the verified offline catalogue.

Consumes the output of audit_static_library.py (never the source library) and
adds the per-stage program references, dependency graph and failure groups that
the catalogue does not record directly. Every one of the 2,402 stage sources,
6,311 programs and 12,614 original stage references is accounted for; a missing
reference is an error, not an empty row.

Optional --gxp accepts a compile_static_candidates.py output directory and folds
its per-item GXP status into the ledger. --previous compares two ledgers and
reports added/removed stages, programs and status changes.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import sys

SCHEMA = 1


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def read_jsonl(path):
    with path.open('r', encoding='utf-8-sig') as stream:
        return [json.loads(line) for line in stream if line.strip()]


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + '\n',
                    encoding='utf-8', newline='\n')


def write_jsonl(path, values):
    with path.open('w', encoding='utf-8', newline='\n') as stream:
        for value in values:
            stream.write(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                    separators=(',', ':')) + '\n')


def content_key(stage, sha):
    return f'{stage}:{sha}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalogue', type=Path, required=True,
                        help='output directory of audit_static_library.py')
    parser.add_argument('--library', type=Path, required=True,
                        help='read-only extracted shader library used by the catalogue')
    parser.add_argument('--gxp', type=Path,
                        help='optional compile_static_candidates.py output directory')
    parser.add_argument('--previous', type=Path,
                        help='optional previous ledger output directory')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    catalogue_dir = args.catalogue.resolve()
    library = args.library.resolve()
    output = args.output.resolve()
    if output == catalogue_dir or output.is_relative_to(catalogue_dir):
        raise ValueError('output must not be the catalogue directory')
    if output.exists() and any(output.iterdir()):
        raise ValueError('use a new or empty output directory')
    output.mkdir(parents=True, exist_ok=True)

    summary = read_json(catalogue_dir / 'summary.json')
    for name, expected in summary['outputs'].items():
        if digest(catalogue_dir / name) != expected:
            raise ValueError('catalogue output hash mismatch: ' + name)
    if summary['shader_count'] != 2402 or summary['program_count'] != 6311:
        raise ValueError('unexpected catalogue denominators')

    entries = read_jsonl(catalogue_dir / 'shader-catalogue.jsonl')
    programs = read_jsonl(catalogue_dir / 'program-map.jsonl')
    captures = read_jsonl(catalogue_dir / 'capture-map.jsonl')
    stage_entries = read_jsonl(library / 'stage_mapping.jsonl')
    stage_entry_by_key = {}
    for entry in stage_entries:
        key = (entry['stage'], entry['index'])
        if key in stage_entry_by_key:
            raise ValueError(f'duplicate stage table entry {key}')
        stage_entry_by_key[key] = entry
    library_programs = read_jsonl(library / 'programs_mapping.jsonl')
    requirements = read_json(library / 'runtime_requirements.json')
    extraction = read_json(library / 'extraction.json')

    if len(entries) != summary['shader_count']:
        raise ValueError('catalogue shader count mismatch')
    if len(programs) != summary['program_count']:
        raise ValueError('catalogue program count mismatch')

    by_id = {}
    by_content = defaultdict(list)
    for entry in entries:
        shader_id = entry['shader_id']
        if shader_id in by_id:
            raise ValueError('duplicate shader id ' + shader_id)
        by_id[shader_id] = entry
        by_content[content_key(entry['stage'], entry['maxwell_sha256'])].append(shader_id)

    gxp_records = {}
    gxp_identity = None
    if args.gxp is not None:
        gxp_dir = args.gxp.resolve()
        manifest_path = gxp_dir / 'manifest.json'
        manifest = read_json(manifest_path)
        identity = manifest.get('build_identity', manifest)
        gxp_identity = {
            'manifest_sha256': digest(manifest_path),
            'schema': manifest.get('schema'),
            'compiler_sha256': identity.get('libshacccg_sha256',
                                            identity.get('compiler_sha256')),
            'shacc_sha256': identity.get('shacc_sha256'),
            'config_sha256': identity.get('config_sha256'),
            'catalogue_summary_sha256': identity.get('catalogue_summary_sha256'),
            'compiled': manifest.get('compiled'),
            'rejected': manifest.get('rejected'),
        }
        if identity.get('catalogue_summary_sha256') != digest(catalogue_dir / 'summary.json'):
            raise ValueError('GXP manifest belongs to a different catalogue')
        for record in manifest.get('programs', []):
            gxp_records[record['shader_id']] = record

    # Runtime requirements is a query -> shader_ids map. Invert it so each stage
    # carries every missing key that caused a defaulted value.
    deps = defaultdict(list)
    for key, shader_ids in requirements['requests'].items():
        for shader_id in shader_ids:
            deps[shader_id].append(key)
    for shader_id in deps:
        if shader_id not in by_id:
            raise ValueError('runtime requirement references unknown shader ' + shader_id)

    capture_by_shader = defaultdict(list)
    capture_programs = set()
    for capture in captures:
        capture_by_shader[capture['shader_id']].append(capture)
        capture_programs.add(capture['program'])

    # Program references from the original mapping file, cross-checked with the
    # catalogue copy. Every reference must resolve; empty references stay explicit.
    stage_users = defaultdict(list)
    stage_entry_users = Counter()
    program_rows = []
    refs_total = 0
    for source, prepared in zip(library_programs, programs):
        if source['index'] != prepared['spk_program_index']:
            raise ValueError('program mapping order mismatch')
        if source['vertex_shader'] != prepared['vertex_shader'] or \
                source['fragment_shader'] != prepared['fragment_shader']:
            raise ValueError('catalogue and library program references differ')
        refs = []
        program_entries = []
        for role, field, entry_field in (('vert', 'vertex_shader', 'vertex_entry'),
                                         ('frag', 'fragment_shader', 'fragment_entry')):
            shader_id = prepared.get(field)
            entry_index = source.get(entry_field)
            table_entry = stage_entry_by_key.get((role, entry_index))
            if table_entry is None:
                raise ValueError(f'program {prepared["spk_program_index"]} has no {role} '
                                 f'stage entry {entry_index}')
            if table_entry['shader_id'] != shader_id:
                raise ValueError(f'program {prepared["spk_program_index"]} {role} entry '
                                 f'resolves to {table_entry["shader_id"]}, expected {shader_id}')
            stage_entry_users[(role, entry_index)] += 1
            program_entries.append({'stage': role, 'stage_entry_index': entry_index,
                                    'shader_id': shader_id})
            if shader_id is None:
                refs.append({'stage': role, 'shader_id': None, 'content_key': None,
                             'status': 'empty'})
                continue
            if shader_id not in by_id:
                raise ValueError(f'program {prepared["spk_program_index"]} has unknown {field}')
            entry = by_id[shader_id]
            if entry['stage'] != role:
                raise ValueError(f'program {prepared["spk_program_index"]} {field} stage mismatch')
            key = content_key(role, entry['maxwell_sha256'])
            stage_users[key].append(prepared['spk_program_index'])
            refs.append({'stage': role, 'shader_id': shader_id, 'content_key': key,
                         'status': 'resolved'})
            refs_total += 1
        program_rows.append({
            'spk_program_index': prepared['spk_program_index'],
            'candidate_runtime_token': prepared['candidate_runtime_token'],
            'spk_offset': prepared['spk_offset'],
            'raw_words': prepared['raw_words'],
            'mapping_status': prepared['mapping_status'],
            'stage_entries': program_entries,
            'references': refs,
            'observed': prepared['candidate_runtime_token'] in {
                int(c['program'].split('_0x')[1], 16) for c in captures},
        })
    if len(stage_entries) != summary['stage_entry_count']:
        raise ValueError('original stage table count mismatch')
    if set(stage_entry_users) != set(stage_entry_by_key):
        raise ValueError('not every original stage entry is referenced by a program')
    if sum(stage_entry_users.values()) != refs_total:
        raise ValueError('stage-entry user count differs from program references')

    stage_rows = []
    stage_entry_indices = defaultdict(list)
    for program in program_rows:
        for stage_entry in program['stage_entries']:
            stage_entry_indices[stage_entry['shader_id']].append(stage_entry['stage_entry_index'])
    for entry in entries:
        shader_id = entry['shader_id']
        key = content_key(entry['stage'], entry['maxwell_sha256'])
        users = sorted(stage_users.get(key, []))
        source = (library / entry['maxwell'])
        if not source.is_file():
            raise ValueError('missing maxwell source ' + entry['maxwell'])
        exact_bytes = source.stat().st_size
        gxp = gxp_records.get(shader_id)
        if gxp is None:
            gxp_status = 'not_selected'
            gxp_sha = None
        elif gxp.get('gxp_verified'):
            gxp_status = 'compiled'
            gxp_sha = gxp.get('gxp_sha256')
        elif gxp.get('failure_kind'):
            gxp_status = gxp['failure_kind']
            gxp_sha = None
        elif gxp.get('compiler_returncode') not in (None, 0):
            gxp_status = 'compiler_rejected'
            gxp_sha = None
        else:
            gxp_status = 'invalid_output' if not gxp.get('gxp_verified') else 'compiled'
            gxp_sha = None
        observed = [{'program': c['program'], 'runtime_token': c['runtime_token'],
                     'viewport_transform': c['viewport_transform'],
                     'capture_sha256': c['capture_sha256'],
                     'canonical_bytes': c['canonical_bytes'],
                     'capture_bytes': c['capture_bytes']}
                    for c in capture_by_shader.get(shader_id, [])]
        aliases = [other for other in by_content.get(key, []) if other != shader_id]
        stage_rows.append({
            'shader_id': shader_id,
            'stage': entry['stage'],
            'content_key': key,
            'maxwell_sha256': entry['maxwell_sha256'],
            'exact_bytes': exact_bytes,
            'origin': entry['origin'],
            'specialization_required': entry['specialization_required'],
            'missing_environment_keys': sorted(set(entry['missing_environment_keys'])),
            'missing_runtime_data': entry['missing_runtime_data'],
            'cg_lowering': entry['cg_lowering'],
            'cg_error': entry.get('cg_error'),
            'cg_sha256': entry.get('cg_sha256'),
            'cg_candidate': entry.get('cg_candidate'),
            'environment_sha256': entry['environment_sha256'],
            'conversion_sha256': entry['conversion_sha256'],
            'resource_sha256': entry['resources_sha256'],
            'aliases': aliases,
            'stage_entry_indices': sorted(stage_entry_indices.get(shader_id, [])),
            'program_references': len(users),
            'program_indices': users,
            'observed_captures': observed,
            'gxp_status': gxp_status,
            'gxp_sha256': gxp_sha,
            'verification': 'source+content+mapping' if entry['cg_lowering'] == 'accepted'
                            else 'source+content+mapping; Cg rejected',
            'pipeline_linked': False,
            'pixel_verified': False,
        })

    # Failure groups: first failure only, as the catalogue records it. Following
    # failures are not guessed; A3 reruns expose them one class at a time.
    by_reason = defaultdict(lambda: {'count': 0, 'stages': Counter(), 'shader_ids': [],
                                     'missing_keys': Counter(), 'sample_error': None})
    for row in stage_rows:
        if row['cg_error'] is None:
            continue
        group = by_reason[row['cg_error']]
        group['count'] += 1
        group['stages'][row['stage']] += 1
        group['shader_ids'].append(row['shader_id'])
        for key in row['missing_environment_keys']:
            group['missing_keys'][key] += 1
        if group['sample_error'] is None:
            group['sample_error'] = row['cg_error']
    failure_groups = []
    for reason, group in sorted(by_reason.items(), key=lambda item: (-item[1]['count'], item[0])):
        failure_groups.append({'reason': reason, 'count': group['count'],
                               'stages': dict(group['stages']),
                               'missing_environment_keys': dict(group['missing_keys']),
                               'shader_ids': group['shader_ids']})
    env_keys = Counter()
    env_stages = defaultdict(list)
    for row in stage_rows:
        for key in row['missing_environment_keys']:
            env_keys[key] += 1
            env_stages[key].append(row['shader_id'])
    environment_gaps = {
        'key_count': len(env_keys),
        'keys': [{'key': key, 'stage_count': count, 'shader_ids': env_stages[key]}
                 for key, count in sorted(env_keys.items())],
    }

    dependency_graph = {
        'schema': SCHEMA,
        'program_count': len(program_rows),
        'stage_reference_count': refs_total,
        'stage_table_entries': len(stage_entries),
        'shared_stage_entries': refs_total - len(stage_entries),
        'unique_stage_contents': len(by_content),
        'unique_program_pairs': len({(p['references'][0]['content_key'],
                                      p['references'][1]['content_key']) for p in program_rows}),
        'stage_users': {key: sorted(users) for key, users in sorted(stage_users.items())},
        'programs': [{'index': p['spk_program_index'],
                      'token': p['candidate_runtime_token'],
                      'vertex': p['references'][0]['content_key'],
                      'fragment': p['references'][1]['content_key']}
                     for p in program_rows],
    }

    compiled = sum(row['gxp_status'] == 'compiled' for row in stage_rows)
    coverage = {
        'sources': {'SPK': sum(row['origin'] == 'SPK resource package' for row in stage_rows),
                    'SDK_window': sum(row['origin'] != 'SPK resource package' for row in stage_rows),
                    'total': len(stage_rows)},
        'programs': len(program_rows),
        'stage_references': refs_total,
        'stage_table_entries': len(stage_entries),
        'shared_stage_entries': refs_total - len(stage_entries),
        'unique_stage_contents': len(by_content),
        'unique_program_pairs': dependency_graph['unique_program_pairs'],
        'captured_programs': len({c['runtime_token'] for c in captures}),
        'captured_stage_records': len(captures),
        'captured_unique_content': len({c['canonical_sha256'] for c in captures}),
        'cg_accepted': sum(row['cg_lowering'] == 'accepted' for row in stage_rows),
        'cg_rejected': sum(row['cg_lowering'] == 'unsupported' for row in stage_rows),
        'missing_environment': sum(bool(row['missing_environment_keys']) for row in stage_rows),
        'specialization_required': sum(row['specialization_required'] for row in stage_rows),
        'specialization_without_missing_key': sum(
            row['specialization_required'] and not row['missing_environment_keys']
            for row in stage_rows),
        'no_missing_environment': sum(not row['missing_environment_keys'] for row in stage_rows),
        'gxp_compiled': compiled,
        'gxp_status': dict(Counter(row['gxp_status'] for row in stage_rows)),
    }

    changes = None
    if args.previous is not None:
        previous = args.previous.resolve()
        prev_stages = {row['shader_id']: row for row in read_jsonl(previous / 'stage-ledger.jsonl')}
        prev_programs = {row['spk_program_index']: row for row in read_jsonl(previous / 'program-ledger.jsonl')}
        current_stages = {row['shader_id']: row for row in stage_rows}
        changes = {
            'previous': str(previous),
            'stages_added': sorted(set(by_id) - set(prev_stages)),
            'stages_removed': sorted(set(prev_stages) - set(by_id)),
            'programs_added': sorted(set(range(len(program_rows))) - set(prev_programs)),
            'programs_removed': sorted(set(prev_programs) - set(range(len(program_rows)))),
            'gxp_status_changes': [
                {'shader_id': shader_id, 'from': prev_stages[shader_id]['gxp_status'],
                 'to': current_stages[shader_id]['gxp_status']}
                for shader_id in sorted(set(prev_stages) & set(current_stages))
                if prev_stages[shader_id]['gxp_status'] != current_stages[shader_id]['gxp_status']
            ],
            'cg_status_changes': [
                {'shader_id': shader_id, 'from': prev_stages[shader_id]['cg_lowering'],
                 'to': current_stages[shader_id]['cg_lowering']}
                for shader_id in sorted(set(prev_stages) & set(current_stages))
                if prev_stages[shader_id]['cg_lowering'] != current_stages[shader_id]['cg_lowering']
            ],
        }

    ledger = {
        'schema': SCHEMA,
        'denominators': {'stage_sources': 2402, 'programs': 6311,
                         'stage_references': refs_total,
                         'stage_table_entries': len(stage_entries),
                         'unique_pairs': 5589},
        'coverage': coverage,
        'input_identity': {
            'catalogue_summary_sha256': digest(catalogue_dir / 'summary.json'),
            'source_spk_sha256': summary['source_spk_sha256'],
            'source_arc_sha256': summary['source_arc_sha256'],
            'target_main_sha256': summary['target_main']['sha256'],
            'target_sdk_sha256': summary['target_sdk']['sha256'],
            'library_source_versions': [s.get('version') for s in extraction.get('sources', [])],
            'extraction_sha256': digest(library / 'extraction.json'),
            'program_mapping_sha256': digest(library / 'programs_mapping.jsonl'),
            'stage_mapping_sha256': digest(library / 'stage_mapping.jsonl'),
            'runtime_requirements_sha256': digest(library / 'runtime_requirements.json'),
        },
        'gxp_build': gxp_identity,
        'environment_gaps': environment_gaps,
        'failure_groups': failure_groups,
        'changes': changes,
        'limitations': [
            'GXP compiled means Shacc returned a valid header/length; no pipeline or pixel check.',
            'Cg acceptance is the current IR gate, not semantic or numeric equivalence.',
            'Every entry records only the FIRST Cg failure; later failures must be re-probed.',
            'SDK shader windows retain their recorded boundaries and are not exact SPK entries.',
            'Only observed runtime tokens are verified; all other index+1 tokens remain candidates.',
        ],
    }

    write_jsonl(output / 'stage-ledger.jsonl', stage_rows)
    write_jsonl(output / 'program-ledger.jsonl', program_rows)
    write_json(output / 'dependency-graph.json', dependency_graph)
    write_json(output / 'failure-groups.json',
               {'schema': SCHEMA, 'failure_groups': failure_groups,
                'environment_gaps': environment_gaps})
    write_json(output / 'summary.json', ledger)
    write_json(output / 'outputs.json', {
        name: digest(output / name) for name in
        ('stage-ledger.jsonl', 'program-ledger.jsonl', 'dependency-graph.json',
         'failure-groups.json', 'summary.json')})

    print(json.dumps({
        'stages': len(stage_rows), 'programs': len(program_rows),
        'references': refs_total, 'cg_accepted': coverage['cg_accepted'],
        'cg_rejected': coverage['cg_rejected'],
        'missing_environment': coverage['missing_environment'],
        'gxp_compiled': coverage['gxp_compiled'],
        'failure_groups': len(failure_groups),
    }, ensure_ascii=False), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())

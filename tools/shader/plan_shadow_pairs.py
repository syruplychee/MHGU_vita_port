"""Plan deterministic observed VS/FS pairs for the nine shadow stages.

This is a static index join only:

* every base program token whose fragment stage is one of the nine is listed;
* each partner vertex stage is checked to exist in the same immutable index;
* a deterministic nine-pair plan is selected to cover nine distinct fragment
  stages and as many distinct real vertex stages as possible.

No API, GXM link, draw, pixel or runtime claim is made.  A future backend gate
must use real GL compile/link plus a draw correlated to the GXP internal SHA;
``PAIR ok`` alone is not evidence.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_shadow_candidates as csc


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_stage_indices(spec_path):
    spec = json.loads(spec_path.read_text(encoding='utf-8'))
    entries = spec.get('add') or []
    ids = [entry['shader_id'] for entry in entries]
    if sorted(ids) != sorted(csc.OBSERVED) or len(ids) != len(csc.OBSERVED):
        raise ValueError('spec does not contain exactly the nine observed stages')
    return {entry['shader_id']: entry['stage_index'] for entry in entries}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=Path,
                        default=ROOT / 'artifacts/m6b14-village-materials-20261026/candidate-bank')
    parser.add_argument('--spec', type=Path,
                        default=ROOT / 'artifacts/m6b24-shadow-bank-20261027/shadow-candidate-spec.json')
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'config/validation/m6b24-shadow-pairs-20261027.json')
    args = parser.parse_args()

    index_json_path = args.bank / 'shader-index.json'
    index_bin_path = args.bank / 'shader-index.bin'
    bank = json.loads(index_json_path.read_text(encoding='utf-8'))
    stage_by_index = {row['index']: row for row in bank['stages']}
    by_id = {row['shader_id']: row for row in bank['stages']}
    stage_indices = load_stage_indices(args.spec)
    for shader_id, stage_index in stage_indices.items():
        stage = stage_by_index.get(stage_index)
        if stage is None or stage['shader_id'] != shader_id or stage['stage'] != 'frag':
            raise ValueError('spec stage index does not match the bank: ' + shader_id)

    observed = []
    by_fs = collections.defaultdict(list)
    for program in bank['programs']:
        fs = program['fragment_stage']
        if fs not in stage_indices.values():
            continue
        vs = program['vertex_stage']
        vertex = stage_by_index.get(vs) if vs != 0xffff else None
        row = {
            'token': program['token'],
            'token_hex': '0x%x' % program['token'],
            'program_flags': program['flags'],
            'fragment_stage_index': fs,
            'fragment_shader_id': stage_by_index[fs]['shader_id'],
            'fragment_stage_sha256': stage_by_index[fs]['stage_sha256'],
            'vertex_stage_index': vs if vs != 0xffff else None,
            'vertex_shader_id': vertex['shader_id'] if vertex else None,
            'vertex_stage_sha256': vertex['stage_sha256'] if vertex else None,
            'vertex_stage_available': vertex is not None,
        }
        observed.append(row)
        by_fs[fs].append(row)
    observed.sort(key=lambda row: (row['fragment_stage_index'], row['token']))

    missing_vs = [row for row in observed if not row['vertex_stage_available']]
    per_fs = {}
    for stage_index, rows in sorted(by_fs.items()):
        distinct = sorted({row['vertex_stage_index'] for row in rows
                           if row['vertex_stage_available']})
        per_fs[stage_by_index[stage_index]['shader_id']] = {
            'fragment_stage_index': stage_index,
            'programs': len(rows),
            'distinct_real_vertex_stages': len(distinct),
            'vertex_stage_indices': distinct,
            'missing_vertex_stage_programs': sum(
                1 for row in rows if not row['vertex_stage_available']),
        }

    # Deterministic covering plan: walk fragment stages by index and choose the
    # lowest (vertex index, token) whose vertex stage is not already used.
    used_vs = set()
    selected = []
    for shader_id, stage_index in sorted(stage_indices.items(),
                                         key=lambda item: item[1]):
        candidates = sorted(
            (row for row in by_fs[stage_index]
             if row['vertex_stage_available']),
            key=lambda row: (row['vertex_stage_index'], row['token']))
        choice = next((row for row in candidates
                       if row['vertex_stage_index'] not in used_vs), None)
        if choice is None and candidates:
            choice = candidates[0]
        if choice is None:
            raise ValueError('no real vertex partner for ' + shader_id)
        selected.append(dict(choice))
        used_vs.add(choice['vertex_stage_index'])

    if len({row['fragment_stage_index'] for row in selected}) != len(stage_indices):
        raise ValueError('plan does not cover nine distinct fragment stages')
    distinct_vs = len({row['vertex_stage_index'] for row in selected})
    all_vs = {row['vertex_stage_index'] for row in observed
              if row['vertex_stage_available']}
    result = {
        'schema': 1,
        'task': 'M6-B24',
        'date': '2026-10-27',
        'bank': {
            'path': str(args.bank),
            'index_sha256': digest(index_bin_path),
            'index_json_sha256': digest(index_json_path),
            'stages': len(bank['stages']),
            'variants': len(bank['variants']),
            'programs': len(bank['programs']),
        },
        'spec': {'path': str(args.spec), 'sha256': digest(args.spec)},
        'summary': {
            'shadow_fragment_stages': len(stage_indices),
            'programs_touching_shadow_fs': len(observed),
            'programs_with_missing_vertex_stage': len(missing_vs),
            'distinct_real_vertex_stages_observed': len(all_vs),
            'per_fragment_stage': per_fs,
            'plan_pairs': len(selected),
            'plan_distinct_fragment_stages': len(
                {row['fragment_stage_index'] for row in selected}),
            'plan_distinct_vertex_stages': distinct_vs,
            'plan_covers_all_nine': len(selected) == len(csc.OBSERVED),
            'plan_every_partner_available': all(
                row['vertex_stage_available'] for row in selected),
        },
        'selected_plan': selected,
        'observed_pairs': observed,
        'claims': {
            'static_index_join_only': True,
            'api_compile_or_link_verified': False,
            'draw_or_pixel_verified': False,
            'runtime_gpu_ready': False,
        },
        'next_gate': {
            'required': [
                'real GL compile/link of every selected VS/FS pair',
                'a draw correlated to the GXP internal SHA, not a PAIR ok flag',
                'fixture/C planner supplying real depth and the three shadow '
                'uniform vectors',
                'explicit rejection when a synthetic ABI lacks shadow uniform '
                'initialization',
            ],
            'notes': [
                'The existing P4 pair probe may consume this index, but its '
                'synthetic ABI can lack shadow uniform initialization.',
                'PAIR ok alone must not be treated as program success.',
            ],
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=False) + '\n',
                           encoding='utf-8', newline='\n')
    print(json.dumps(result['summary'], indent=2))
    print('selected pairs:')
    for row in selected:
        print('  fs=%s vs=%s token=%s' % (
            row['fragment_shader_id'], row['vertex_shader_id'], row['token_hex']))


if __name__ == '__main__':
    main()

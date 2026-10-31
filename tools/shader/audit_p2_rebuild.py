"""Prove the P2 rebuild scope, compiler provenance and downstream identity update.

Detailed shader identities belong in ignored artifacts. This tool never edits
baseline inputs or promotes compilation into ABI/pixel/package acceptance.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

from compile_static_candidates import digest, item_key, valid_gxp

HELPERS = ('vp_uint_to_float', 'vp_float_bits_to_uint_numeric',
           'vp_float_bits_to_int_numeric')


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def read_rows(path):
    rows = [json.loads(line) for line in Path(path).read_text(encoding='utf-8').splitlines()]
    result = {r['shader_id']: r for r in rows}
    if len(result) != len(rows):
        raise ValueError('duplicate shader id: ' + str(path))
    return result


def helper_parts(cg):
    definitions = {}
    rest = cg
    for name in HELPERS:
        match = re.search(r'float ' + name + r'\([^)]*\)\s*\{', rest)
        if not match:
            continue
        begin = match.start()
        cursor = match.end()
        depth = 1
        while depth and cursor < len(rest):
            depth += (rest[cursor] == '{') - (rest[cursor] == '}')
            cursor += 1
        if depth:
            raise ValueError('unterminated helper: ' + name)
        definitions[name] = rest[begin:cursor]
        rest = rest[:begin] + rest[cursor:]
    # Helper preamble comments are not executable source.
    body = '\n'.join(line for line in rest.splitlines() if line.strip() and not line.lstrip().startswith('//'))
    calls = Counter({name: len(re.findall(r'\b' + name + r'\s*\(', body)) for name in HELPERS})
    return definitions, body, calls


def audit_viewport(old_cat, cat, old_build, build):
    old_summary, summary = [read_json(p / 'summary.json') for p in (old_cat, cat)]
    for folder, report in ((old_cat, old_summary), (cat, summary)):
        if report['outputs']['shader-catalogue.jsonl'] != digest(folder / 'shader-catalogue.jsonl'):
            raise ValueError('catalogue hash mismatch')
    for name in ('program-map.jsonl', 'capture-map.jsonl'):
        if digest(old_cat / name) != digest(cat / name):
            raise ValueError('program/capture identity mapping changed: ' + name)
    for field in ('source_spk_sha256', 'source_arc_sha256', 'target_main', 'target_sdk'):
        if old_summary[field] != summary[field]:
            raise ValueError('locked input identity changed: ' + field)
    old, new = [read_rows(p / 'shader-catalogue.jsonl') for p in (old_cat, cat)]
    if set(old) != set(new):
        raise ValueError('shader denominator changed')
    baseline_manifest = read_json(old_build / 'manifest.json')
    baseline_records = {r['shader_id']: r for r in baseline_manifest['programs']}
    manifest = read_json(build / 'manifest.json') if (build / 'manifest.json').exists() else None
    records = {r['shader_id']: r for r in manifest['programs']} if manifest else {}
    if manifest and set(records) != set(baseline_records):
        raise ValueError('compiled selection changed')
    if manifest and manifest['build_identity']['catalogue_summary_sha256'] != digest(cat / 'summary.json'):
        raise ValueError('new manifest catalogue mismatch')
    affected = []
    reused = 0
    counters = Counter()
    helper_counts = Counter()
    gxp_changes = Counter()
    compiler_flags = Counter()
    for sid in sorted(new):
        a, b = old[sid], new[sid]
        for field in ('stage', 'maxwell_sha256', 'environment_sha256', 'glsl_sha256',
                      'resolution_sha256', 'resources_sha256', 'cg_lowering', 'cg_error',
                      'specialization_required'):
            if a.get(field) != b.get(field):
                raise ValueError('non-P2 input changed: %s %s' % (sid, field))
        changed = a.get('cg_sha256') != b.get('cg_sha256')
        if a.get('cg_candidate'):
            acg, bcg = [(p / r['cg_candidate']).read_text(encoding='utf-8') for p, r in ((old_cat, a), (cat, b))]
            if hashlib.sha256(acg.encode()).hexdigest() != a['cg_sha256'] or hashlib.sha256(bcg.encode()).hexdigest() != b['cg_sha256']:
                raise ValueError('Cg bytes do not match catalogue')
            ad, abody, acalls = helper_parts(acg)
            bd, bbody, bcalls = helper_parts(bcg)
            if abody != bbody or acalls != bcalls or set(ad) != set(bd):
                raise ValueError('change outside P2 helper definitions: ' + sid)
            changed_helpers = [name for name in ad if ad[name] != bd[name]]
            if changed != bool(changed_helpers):
                raise ValueError('Cg difference is not explained by P2 helpers: ' + sid)
            if changed:
                for name in changed_helpers:
                    helper_counts[name] += 1
                    if not bcalls[name]:
                        raise ValueError('changed helper has no call: ' + sid)
                affected.append({'shader_id': sid, 'stage': b['stage'],
                                 'helpers': changed_helpers, 'calls': dict(bcalls),
                                 'old_cg_sha256': a['cg_sha256'], 'cg_sha256': b['cg_sha256'],
                                 'previously_selected': sid in baseline_records})
        if not manifest or sid not in records:
            continue
        record = records[sid]
        prior = baseline_records[sid]
        if prior['status'] == 'compiled' and record['status'] != 'compiled':
            raise ValueError('previously compiled stage regressed: ' + sid)
        for field in ('stage', 'maxwell_sha256', 'environment_sha256', 'cg_sha256'):
            if record[field] != b[field]:
                raise ValueError('new compiler record mismatch: ' + sid)
        flags = record.get('flags', manifest['build_identity']['flags'])
        compiler_flags[' '.join(flags)] += 1
        if record['content_key'] != item_key(b, record['profile'], manifest['build_identity'], flags):
            raise ValueError('new compiler key mismatch: ' + sid)
        if changed and record.get('compilation_provenance'):
            provenance = record['compilation_provenance']
            if provenance['content_key'] != item_key(b, record['profile'], provenance['build_identity'], flags):
                raise ValueError('changed shader reused a pre-correction compilation: ' + sid)
        if not changed:
            provenance = record.get('compilation_provenance', {})
            while provenance and provenance.get('manifest_sha256') != digest(old_build / 'manifest.json'):
                provenance = provenance.get('ancestor') or {}
            if (provenance.get('manifest_sha256') != digest(old_build / 'manifest.json') or
                    provenance.get('content_key') != prior['content_key'] or
                    record.get('gxp_sha256') != prior.get('gxp_sha256') or
                    record['status'] != prior['status']):
                raise ValueError('unchanged compilation provenance mismatch: ' + sid)
            reused += 1
        counters[record['status']] += 1
        if record['status'] == 'compiled':
            ok, data = valid_gxp(build / record['gxp'], record['gxp_bytes'])
            if not ok or hashlib.sha256(data).hexdigest() != record['gxp_sha256']:
                raise ValueError('new GXP hash/header mismatch: ' + sid)
            gxp_changes['changed' if record['gxp_sha256'] != prior.get('gxp_sha256') else 'identical'] += 1
    return {'stages': len(new), 'affected': len(affected),
            'affected_selected': sum(r['previously_selected'] for r in affected),
            'helper_stage_counts': dict(helper_counts), 'affected_stages': affected,
            'compiler_status': dict(counters), 'unchanged_records_reused': reused,
            'gxp_changes': dict(gxp_changes),
            'compiler_flags': dict(compiler_flags), 'build_root': str(build),
            'catalogue_sha256': digest(cat / 'summary.json'),
            'old_catalogue_sha256': digest(old_cat / 'summary.json'),
            'manifest_sha256': digest(build / 'manifest.json') if manifest else None,
            'baseline_manifest_sha256': digest(old_build / 'manifest.json')}


def compare_pairs(old_path, new_path):
    old_path, new_path = Path(old_path), Path(new_path)
    old, new = read_json(old_path), read_json(new_path)
    if old['programs'] != new['programs'] or old['counts']['stages'] != new['counts']['stages']:
        raise ValueError('observed pair set or stage ordering changed')
    stages = {r['index']: r for r in new['stages']}
    previous = {r['index']: r for r in old['stages']}
    changed = set()
    for index, row in stages.items():
        before = previous[index]
        if (row['shader_id'], row['stage_sha256']) != (before['shader_id'], before['stage_sha256']):
            raise ValueError('pair stage source identity changed')
        if row['gxp_sha256'] != before['gxp_sha256']:
            if row['variant_key'] == before['variant_key']:
                raise ValueError('changed GXP retained its old pair variant identity')
            changed.add(index)
        elif row['variant_key'] != before['variant_key']:
            raise ValueError('unchanged GXP unexpectedly changed its variant key')
    affected = [p for p in new['programs'] if
                p['vertex_stage'] in changed or p['fragment_stage'] in changed]
    return {'old_manifest_sha256': digest(old_path), 'manifest_sha256': digest(new_path),
            'old_index_sha256': old['index_sha256'], 'index_sha256': new['index_sha256'],
            'counts': new['counts'], 'changed_stage_variants': len(changed),
            'affected_programs': len(affected),
            'affected_by_phase': dict(Counter(p['phase'] for p in affected)),
            'target_pair_results_carried': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--rebuilt', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--old-pairs', type=Path)
    parser.add_argument('--pairs', type=Path)
    parser.add_argument('--vp0-build', type=Path,
                        help='Final vp0 assembly if a separately pinned compiler variant was required')
    args = parser.parse_args()
    result = {'schema': 1, 'scope': 'P2 helper rebuild identity; no ABI/pixel/install acceptance',
              'viewports': {}}
    for vp in ('vp0', 'vp1'):
        result['viewports'][vp] = audit_viewport(
            args.baseline / ('audit-' + vp + 'c'), args.rebuilt / ('audit-' + vp),
            args.baseline / ('gxp-' + vp),
            args.vp0_build if vp == 'vp0' and args.vp0_build else args.rebuilt / ('gxp-' + vp))
    result['identity_verified'] = all(r['manifest_sha256'] for r in result['viewports'].values())
    if result['identity_verified']:
        # The sparse vp0 baseline omits stages whose Cg is identical to vp1.
        # Bind every affected omission to the new vp1 bytes explicitly.
        c0 = read_rows(args.rebuilt / 'audit-vp0/shader-catalogue.jsonl')
        c1 = read_rows(args.rebuilt / 'audit-vp1/shader-catalogue.jsonl')
        m1 = read_json(args.rebuilt / 'gxp-vp1/manifest.json')
        r1 = {r['shader_id']: r for r in m1['programs']}
        shared = []
        for entry in result['viewports']['vp0']['affected_stages']:
            if entry['previously_selected']:
                continue
            sid = entry['shader_id']
            source = r1[sid]
            if c0[sid]['cg_sha256'] != c1[sid]['cg_sha256'] or source['status'] != 'compiled':
                raise ValueError('affected vp0 omission has no identical rebuilt vp1 Cg: ' + sid)
            shared.append({'shader_id': sid, 'cg_sha256': c0[sid]['cg_sha256'],
                           'vp0_environment_sha256': c0[sid]['environment_sha256'],
                           'vp1_environment_sha256': c1[sid]['environment_sha256'],
                           'source_content_key': source['content_key'],
                           'source_manifest_sha256': result['viewports']['vp1']['manifest_sha256'],
                           'gxp_sha256': source['gxp_sha256'],
                           'basis': 'identical emitted Cg; no ABI/pixel/target acceptance'})
        result['vp0_shared_affected_variants'] = shared
    if args.pairs:
        if not args.old_pairs:
            raise ValueError('--pairs requires --old-pairs')
        result['pairs'] = compare_pairs(args.old_pairs, args.pairs)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps({vp: {k: v for k, v in r.items() if k != 'affected_stages'}
                      for vp, r in result['viewports'].items()}, indent=2))


if __name__ == '__main__':
    main()

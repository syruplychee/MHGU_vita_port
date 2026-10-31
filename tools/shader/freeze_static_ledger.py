"""S2/P0: freeze the A3 static ledger with every acceptance dimension separated.

The A3 stage ledger records Cg lowering and GXP build status for both viewport
variants.  It does not record resource-ABI establishment, pair linking, pixel
verification, package installation or runtime hits.  This tool copies the locked
ledger into an immutable, hash-bound view and adds those dimensions explicitly as
``not_established`` / ``not_linked`` / ``not_verified`` / ``not_installed``.

The 71 Cg rejections and the 3 Shacc register-exhaustion failures stay visible as
gaps.  They are not folded into a "compiled" total and the historical ledger is
not modified.  No GXP is rebuilt and no target code runs.

Usage (from the vita-port root):
  python tools/shader/freeze_static_ledger.py \
    --artifact artifacts/a3-20261022 \
    --output artifacts/a3-20261022/frozen-ledger-20261023 \
    --validation config/validation/static-ledger-freeze-20261023.json
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

SCHEMA = 1


class FreezeError(Exception):
    pass


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def read_jsonl(path):
    return [json.loads(line) for line in
            Path(path).read_text(encoding='utf-8-sig').splitlines() if line.strip()]


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n',
                    encoding='utf-8', newline='\n')


def write_jsonl(path, values):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('w', encoding='utf-8', newline='\n') as stream:
        for value in values:
            stream.write(json.dumps(value, ensure_ascii=False,
                                    separators=(',', ':')) + '\n')


def index(rows, label):
    result = {}
    for row in rows:
        sid = row.get('shader_id')
        if not sid:
            raise FreezeError(f'{label}: row without shader_id')
        if sid in result:
            raise FreezeError(f'{label}: duplicate shader_id {sid}')
        result[sid] = row
    return result


def require(condition, message):
    if not condition:
        raise FreezeError(message)


def freeze(artifact, output, vp0_build=None):
    artifact = Path(artifact)
    vp1_ledger_path = artifact / 'ledger-vp1-gxp/stage-ledger.jsonl'
    vp0_ledger_path = artifact / 'ledger-vp0-gxp/stage-ledger.jsonl'
    vp1_summary_path = artifact / 'ledger-vp1-gxp/summary.json'
    vp0_summary_path = artifact / 'ledger-vp0-gxp/summary.json'
    vp1_manifest_path = artifact / 'gxp-vp1/manifest.json'
    vp0_manifest_path = (Path(vp0_build) if vp0_build else artifact / 'gxp-vp0') / 'manifest.json'
    for path in (vp1_ledger_path, vp0_ledger_path, vp1_summary_path,
                 vp0_summary_path, vp1_manifest_path, vp0_manifest_path):
        if not path.is_file():
            raise FreezeError('missing input ' + str(path))

    vp1_rows = index(read_jsonl(vp1_ledger_path), 'vp1 ledger')
    vp0_rows = index(read_jsonl(vp0_ledger_path), 'vp0 ledger')
    require(vp1_rows.keys() == vp0_rows.keys(),
            'viewport ledgers do not cover the same 2402 stages')

    vp1_summary = read_json(vp1_summary_path)
    vp0_summary = read_json(vp0_summary_path)
    vp1_manifest = read_json(vp1_manifest_path)
    vp0_manifest = read_json(vp0_manifest_path)

    counts = {
        'stages': len(vp1_rows),
        'cg_accepted': sum(1 for r in vp1_rows.values()
                           if r['cg_lowering'] == 'accepted'),
        'cg_rejected': sum(1 for r in vp1_rows.values()
                           if r['cg_lowering'] != 'accepted'),
        'vp1_gxp_compiled': sum(1 for r in vp1_rows.values()
                                if r['gxp_status'] == 'compiled'),
        'vp1_gxp_compiler_rejected': sum(1 for r in vp1_rows.values()
                                         if r['gxp_status'] == 'compiler_rejected'),
        'vp0_gxp_compiled': sum(1 for r in vp0_rows.values()
                                if r['gxp_status'] == 'compiled'),
        'vp0_gxp_compiler_rejected': sum(1 for r in vp0_rows.values()
                                         if r['gxp_status'] == 'compiler_rejected'),
        'gxp_compiled_any_viewport': sum(
            1 for sid in vp1_rows
            if 'compiled' in (vp1_rows[sid]['gxp_status'], vp0_rows[sid]['gxp_status'])),
        'abi_established': 0,
        'pair_linked': 0,
        'pixel_verified': 0,
        'installed': 0,
        'runtime_verified': 0,
    }
    expected = {
        'cg_accepted': vp1_summary['coverage']['cg_accepted'],
        'cg_rejected': vp1_summary['coverage']['cg_rejected'],
        'vp1_gxp_compiled': vp1_summary['coverage']['gxp_status'].get('compiled', 0),
        'vp1_gxp_compiler_rejected':
            vp1_summary['coverage']['gxp_status'].get('compiler_rejected', 0),
        'vp0_gxp_compiled': vp0_summary['coverage']['gxp_status'].get('compiled', 0),
        'vp0_gxp_compiler_rejected':
            vp0_summary['coverage']['gxp_status'].get('compiler_rejected', 0),
    }
    for key, value in expected.items():
        require(counts[key] == value,
                f'{key}: ledger {counts[key]} != summary {value}')
    require(vp1_manifest['compiled'] == counts['vp1_gxp_compiled'],
            'vp1 manifest compiled count differs from ledger')
    require(vp0_manifest['compiled'] == counts['vp0_gxp_compiled'],
            'vp0 manifest compiled count differs from ledger')
    manifest_failures = {f['shader_id'] for f in vp1_manifest['failures']}
    ledger_failures = {sid for sid, row in vp1_rows.items()
                       if row['gxp_status'] == 'compiler_rejected'}
    require(manifest_failures == ledger_failures,
            'vp1 manifest failure ids differ from ledger')
    require(vp1_manifest.get('installed') is False,
            'historical vp1 manifest unexpectedly installed')
    require(vp1_manifest.get('pipeline_linked') is False,
            'historical vp1 manifest unexpectedly linked')
    require(vp1_manifest.get('pixel_verified') is False,
            'historical vp1 manifest unexpectedly pixel-verified')
    require(counts['gxp_compiled_any_viewport'] == counts['cg_accepted'] -
            len(ledger_failures),
            'compiled-any-viewport does not equal accepted minus compiler failures')

    stage_rows = []
    gaps = []
    for sid in sorted(vp1_rows):
        one = vp1_rows[sid]
        zero = vp0_rows[sid]
        statuses = {'vp0': zero['gxp_status'], 'vp1': one['gxp_status']}
        if 'compiled' in statuses.values():
            gxp_union = 'compiled'
        elif set(statuses.values()) == {'compiler_rejected'}:
            gxp_union = 'compiler_rejected'
        else:
            gxp_union = 'not_built'
        cg_ok = one['cg_lowering'] == 'accepted'
        row = {
            'schema': SCHEMA,
            'shader_id': sid,
            'stage': one['stage'],
            'content_key': one.get('content_key'),
            'maxwell_sha256': one.get('maxwell_sha256'),
            'cg_status': one['cg_lowering'],
            'cg_error': one.get('cg_error'),
            'cg_sha256': one.get('cg_sha256'),
            'gxp_status_by_viewport': statuses,
            'gxp_status_union': gxp_union,
            'gxp_sha256_by_viewport': {
                'vp0': zero.get('gxp_sha256') if zero['gxp_status'] == 'compiled' else None,
                'vp1': one.get('gxp_sha256') if one['gxp_status'] == 'compiled' else None,
            },
            'abi_status': 'not_established',
            'pair_status': 'not_linked',
            'pixel_status': 'not_verified',
            'package_status': 'not_installed',
            'runtime_status': 'not_verified',
            'acceptance': {
                'cg_accepted': cg_ok,
                'gxp_compiled_any_viewport': gxp_union == 'compiled',
                'abi_established': False,
                'pair_linked': False,
                'pixel_verified': False,
                'installed': False,
                'runtime_verified': False,
            },
            'gap': None,
        }
        if not cg_ok:
            row['gap'] = {'kind': 'cg_lowering', 'reason': one.get('cg_error')}
            gaps.append({'shader_id': sid, 'stage': one['stage'],
                         'kind': 'cg_lowering', 'reason': one.get('cg_error')})
        elif gxp_union == 'compiler_rejected':
            row['gap'] = {'kind': 'gxp_compiler', 'reason': 'shacc compiler_rejected'}
            gaps.append({'shader_id': sid, 'stage': one['stage'],
                         'kind': 'gxp_compiler', 'reason': 'shacc compiler_rejected'})
        stage_rows.append(row)

    require(len(gaps) == counts['cg_rejected'] + counts['vp1_gxp_compiler_rejected'],
            'gap rows do not equal Cg rejections plus compiler failures')

    summary = {
        'schema': SCHEMA,
        'scope': ('S2/P0 frozen A3 ledger; Cg/GXP/ABI/pair/pixel/package/runtime '
                  'reported separately; no rebuild, target run or pixel check'),
        'counts': counts,
        'expected_from_summaries': expected,
        'gaps': {
            'cg_rejected': counts['cg_rejected'],
            'gxp_compiler_rejected': counts['vp1_gxp_compiler_rejected'],
            'total': len(gaps),
            'by_kind': dict(sorted(Counter(g['kind'] for g in gaps).items())),
            'shader_ids': [g['shader_id'] for g in gaps],
        },
        'inputs': {
            'vp1_ledger_sha256': digest(vp1_ledger_path),
            'vp0_ledger_sha256': digest(vp0_ledger_path),
            'vp1_summary_sha256': digest(vp1_summary_path),
            'vp0_summary_sha256': digest(vp0_summary_path),
            'vp1_manifest_sha256': digest(vp1_manifest_path),
            'vp0_manifest_sha256': digest(vp0_manifest_path),
            'vp1_manifest_sha256_recorded': vp1_manifest.get('build_identity', {}),
            'vp1_gxp_build': vp1_summary.get('gxp_build'),
            'tools': {
                'shader_ir_sha256': vp1_manifest['build_identity'].get('shader_ir_sha256'),
                'compile_tool_sha256': vp1_manifest['build_identity'].get('tool_sha256'),
                'shacc_sha256': vp1_manifest['build_identity'].get('shacc_sha256'),
                'libshacccg_sha256':
                    vp1_manifest['build_identity'].get('libshacccg_sha256'),
                'config_sha256': vp1_manifest['build_identity'].get('config_sha256'),
            },
            'freeze_tool_sha256': digest(Path(__file__)),
        },
        'denominators': {
            'stages': len(vp1_rows),
            'cg_accepted': counts['cg_accepted'],
            'gxp_compiled_any_viewport': counts['gxp_compiled_any_viewport'],
            'paired_stage_sets': 0,
            'pixel_verified': 0,
            'installed': 0,
        },
        'limitations': [
            'ABI, pair, pixel, package and runtime columns are explicit not-established values; they are not failures of the ledger.',
            'GXP compiled means Shacc returned a valid GXP; it is not a linked GXM pair.',
            'Gap shader IDs are preserved; a later fix must produce a new ledger rather than editing this one.',
            'Only the observed runtime token subset is verified; all other index+1 tokens remain candidates.',
            'No GXP was rebuilt, no package was installed and no target was run in this freeze.',
        ],
        'runtime_verified': False,
        'pixel_verified': False,
        'hardware_tested': False,
    }

    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    write_jsonl(output / 'stage-status.jsonl', stage_rows)
    write_jsonl(output / 'gaps.jsonl', gaps)
    summary['outputs'] = {
        'stage_status_sha256': digest(output / 'stage-status.jsonl'),
        'gaps_sha256': digest(output / 'gaps.jsonl'),
    }
    write_json(output / 'summary.json', summary)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--validation', type=Path)
    parser.add_argument('--vp0-build', type=Path,
                        help='Explicit final vp0 build, when it differs from artifact/gxp-vp0')
    args = parser.parse_args()
    summary = freeze(args.artifact, args.output, args.vp0_build)
    if args.validation:
        write_json(args.validation, summary)
    print(json.dumps({'counts': summary['counts'], 'gaps': summary['gaps']},
                     ensure_ascii=False))


if __name__ == '__main__':
    main()

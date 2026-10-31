#!/usr/bin/env python3
"""Strict full-nine shadow backend verifier.

This verifies the *evidence chain*, not shader semantics:

* header/manifest/candidate-bank/sidecar/run-id identities;
* exactly nine unique tokens and their terminal PAIR rows;
* the exact O1 receipt, Cg and GXP SHA for every fragment stage;
* raw dump offset/size/count/FNV closure per row;
* INFO backend PROGRAM_READY/CACHED and GLDRAW linked/error state matched by
  the internal GXP SHA, not by token order or GL program number alone;
* exception timing relative to the last target draw.

Zero coverage is reported as undetermined content, not as a verifier failure.
A timeout/fail row, a missing terminal row, an identity mismatch or a draw with
a GL error is a failure. A post-draw cleanup exception is reported separately;
it is never silently treated as a clean shutdown.
"""
import argparse
import hashlib
import json
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/shader'))

from verify_gxm_s2_execution import parse_emulator_log  # noqa: E402
import compile_shadow_candidates as csc  # noqa: E402
import prepare_gxm_shadow_pair_bundle as pairbundle  # noqa: E402
import augment_static_shader_package as aug  # noqa: E402

EXPECTED_TOKENS = (19, 653, 882, 1156, 1374, 1821, 3572, 4862, 4956)
EXPECTED_FLAGS = ['-O1']
TITLE_ID = 'MHGUSP001'
FNV_INIT = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3


def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha_file(path):
    return sha_bytes(Path(path).read_bytes())


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def read_text(path):
    return Path(path).read_text(encoding='utf-8-sig')


def fnv_bytes(data):
    value = FNV_INIT
    for byte in data:
        value ^= byte
        value = (value * FNV_PRIME) & 0xffffffffffffffff
    return value


def fnv_u32(values):
    value = FNV_INIT
    for item in values:
        for shift in (0, 8, 16, 24):
            value ^= (item >> shift) & 0xff
            value = (value * FNV_PRIME) & 0xffffffffffffffff
    return value


def gxp_internal_sha(data):
    """Hash only the GXP-declared body, matching the emulator program hash."""
    if len(data) < 32 or data[:4] != b'GXP\0':
        raise ValueError('not a GXP')
    declared = struct.unpack_from('<I', data, 8)[0]
    if declared < 32 or declared > len(data):
        raise ValueError('GXP declared size invalid')
    return sha_bytes(data[:declared])


def parse_header(path):
    text = read_text(path)
    defines = {}
    for key, value in re.findall(r'^#define\s+VP_SHADOW_PAIR_([A-Z0-9_]+)[ \t]+(.+)$',
                                 text, re.MULTILINE):
        defines[key] = value.strip().strip('"')
    rows = []
    pattern = re.compile(
        r'\{\s*(\d+)u,\s*(\d+)u,\s*(\d+)u,\s*(\d+)u,\s*0x([0-9a-fA-F]+)u,\s*'
        r'"([0-9a-fA-F]{64})",\s*"([0-9a-fA-F]{64})",\s*'
        r'"([0-9a-fA-F]{64})",\s*"([0-9a-fA-F]{32})"\s*\}')
    for match in pattern.finditer(text):
        rows.append({
            'token': int(match.group(1)),
            'vertex_stage': int(match.group(2)),
            'fragment_stage': int(match.group(3)),
            'shadow_slot': int(match.group(4)),
            'color_mask': int(match.group(5), 16),
            'vertex_stage_sha256': match.group(6).lower(),
            'fragment_stage_sha256': match.group(7).lower(),
            'fragment_gxp_sha256': match.group(8).lower(),
            'fragment_variant_key': match.group(9).lower(),
        })
    return defines, rows


def parse_device_log(path):
    blocks = []
    current = None
    for number, raw in enumerate(read_text(path).splitlines(), 1):
        line = raw.rstrip()
        if not line:
            continue
        kind = line.split(None, 1)[0]
        fields = {}
        for token in line.split()[1:]:
            if '=' in token:
                key, value = token.split('=', 1)
                fields[key] = value
        if kind == 'RUN-ID':
            current = {'line': number, 'run_id': fields.get('identity'),
                       'terminals': [], 'dumps': [], 'timeouts': [],
                       'control_pixel': None, 'result': None, 'run': None,
                       'raw_lines': []}
            blocks.append(current)
        if current is None:
            current = {'line': number, 'run_id': None, 'terminals': [],
                       'dumps': [], 'timeouts': [], 'control_pixel': None,
                       'result': None, 'run': None, 'raw_lines': []}
            blocks.append(current)
        current['raw_lines'].append(line)
        if kind == 'CONTROL' and 'pixel' in fields:
            current['control_pixel'] = int(fields['pixel'], 16)
        elif kind == 'PAIR':
            current['terminals'].append({'kind': 'PAIR', 'line': number,
                                         'fields': fields})
        elif kind == 'TIMEOUT':
            current['timeouts'].append({'kind': 'TIMEOUT', 'line': number,
                                        'fields': fields})
        elif kind == 'DUMP':
            current['dumps'].append({'kind': 'DUMP', 'line': number,
                                     'fields': fields})
        elif kind == 'RESULT':
            current['result'] = fields.get('value') if 'value' in fields else \
                (line.split(None, 1)[1] if len(line.split(None, 1)) > 1 else '')
        elif kind == 'RUN':
            current['run'] = fields
    return blocks


def parse_terminal(row):
    fields = row['fields']
    if row['kind'] == 'TIMEOUT':
        return {
            'kind': 'TIMEOUT', 'line': row['line'],
            'token': int(fields['token']), 'status': 'timeout',
            'stage': 'execute', 'detail': 'detail=' + fields.get('detail', ''),
            'api_ok': False,
        }
    result = {'kind': 'PAIR', 'line': row['line']}
    for key in ('token', 'ci', 'cov', 'nonzero', 'attempt'):
        if key in fields:
            try:
                result[key] = int(fields[key])
            except ValueError:
                pass
    for key in ('status', 'stage', 'detail', 'hash', 'gxp_sha', 'key',
                'vs_stage', 'fs_stage', 'shadow_slot', 'cull', 'face',
                'gxp_crc'):
        if key in fields:
            result[key] = fields[key]
    for key in ('create', 'register', 'reflect', 'vp', 'fp', 'bind', 'draw',
                'notify', 'finish', 'readback', 'stable', 'shadow_contract',
                'descriptor_depth', 'api_ok'):
        if key in fields:
            result[key] = fields[key] == '1'
    result['status'] = fields.get('status', 'unknown')
    result['api_ok'] = fields.get('api_ok') == '1'
    return result


def first_failure(checks, label, failures):
    if not checks:
        failures.append(label)
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir', type=Path, required=True)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--candidate-bank', type=Path, required=True)
    parser.add_argument('--spec', type=Path, required=True)
    parser.add_argument('--compile-root', type=Path, required=True)
    parser.add_argument('--probe-source', type=Path,
                        default=ROOT / 'src/probes/gxm_shadow_pair_probe.c')
    parser.add_argument('--expected-run-prefix',
                        default='MHGUSP001-B27-20261027')
    parser.add_argument('--summary', type=Path, required=True)
    parser.add_argument('--require-clean-exit', action='store_true',
                        help='also fail when the backend reports a cleanup exception')
    args = parser.parse_args()

    failures = []
    warnings = []
    checks = {}

    header_path = args.bundle / 'shadow_pair_contract.h'
    manifest_path = args.bundle / 'shadow-pair-manifest.json'
    index_path = args.bundle / 'shader-index.bin'
    header, header_rows = parse_header(header_path)
    manifest = read_json(manifest_path)
    index_data = index_path.read_bytes()
    parsed_index = pairbundle.parse_index(index_data)

    checks['header_parse'] = len(header_rows) == 9
    checks['header_program_count'] = header.get('PROGRAM_COUNT') == '9u'
    checks['header_stage_count'] = header.get('STAGE_COUNT') == '18u'
    checks['header_variant_count'] = header.get('VARIANT_COUNT') == '18u'
    checks['header_title'] = header.get('TITLE') == TITLE_ID
    checks['manifest_tokens'] = sorted(manifest.get('bound_tokens', [])) == \
        list(EXPECTED_TOKENS)
    checks['header_tokens'] = sorted(row['token'] for row in header_rows) == \
        list(EXPECTED_TOKENS)
    checks['manifest_pairs'] = len(manifest.get('pairs', [])) == 9
    checks['manifest_run_id'] = manifest.get('run_id') == header.get('RUN_ID')
    checks['header_run_prefix'] = str(header.get('RUN_ID', '')).startswith(
        args.expected_run_prefix)
    checks['index_sha'] = header.get('INDEX_SHA256') == sha_bytes(index_data)
    checks['manifest_index_sha'] = manifest.get('index', {}).get('sha256') == \
        sha_bytes(index_data)
    checks['bundle_counts'] = (
        len(parsed_index['stages']), len(parsed_index['variants']),
        len(parsed_index['programs'])) == (18, 18, 9)
    for key, ok in checks.items():
        first_failure(ok, 'check failed: ' + key, failures)

    probe_source = args.probe_source
    checks['probe_source_sha'] = header.get('SOURCE_SHA256') == \
        sha_file(probe_source)
    identity = pairbundle.identity_inputs(probe_source, index_data)
    checks['header_identity'] = header.get('IDENTITY_SHA256') == \
        identity['sha256']
    expected_run = args.expected_run_prefix + '-' + identity['sha256'][:16]
    checks['run_id_derived'] = header.get('RUN_ID') == expected_run
    checks['manifest_probe_identity'] = manifest.get('probe_identity', {}).get(
        'sha256') == identity['sha256']
    for key, ok in list(checks.items())[-4:]:
        first_failure(ok, 'check failed: ' + key, failures)

    pairs = {row['token']: row for row in manifest.get('pairs', [])}
    header_by_token = {row['token']: row for row in header_rows}
    reuse = {row.get('shader_id'): row for row in
             manifest.get('source_variant_reuse', [])}
    checks['pair_tokens_unique'] = len(pairs) == 9 and len(header_by_token) == 9
    first_failure(checks['pair_tokens_unique'],
                  'check failed: pair/header token uniqueness', failures)
    for token in EXPECTED_TOKENS:
        pair = pairs.get(token)
        row = header_by_token.get(token)
        if pair is None or row is None:
            failures.append('missing pair/header token %d' % token)
            continue
        checks['pair_%d_header' % token] = (
            pair.get('fragment_stage') == row['fragment_stage'] and
            pair.get('vertex_stage') == row['vertex_stage'] and
            pair.get('shadow_slot') == row['shadow_slot'] and
            pair.get('gxp_sha256') == row['fragment_gxp_sha256'] and
            pair.get('variant_key') == row['fragment_variant_key'])
        first_failure(checks['pair_%d_header' % token],
                      'check failed: pair/header token %d' % token, failures)
        stage = parsed_index['stages'][row['fragment_stage']]
        variant = parsed_index['variants'][stage['variant_start']]
        checks['bundle_variant_%d' % token] = (
            stage['sha256'] == row['fragment_stage_sha256'] and
            variant['key'] == row['fragment_variant_key'] and
            variant['gxp_bytes'] == pair.get('gxp_bytes') and
            variant['input_signature'] == 0 and variant['viewport_mask'] == 2)
        first_failure(checks['bundle_variant_%d' % token],
                      'check failed: bundle variant token %d' % token, failures)
        vs = pairs.get(token, {}).get('vertex_shader_id')
        checks['vs_reuse_%d' % token] = vs in reuse and \
            reuse[vs].get('shader_id') == vs
        first_failure(checks['vs_reuse_%d' % token],
                      'check failed: VS reuse token %d' % token, failures)
        vs_stage = parsed_index['stages'][row['vertex_stage']]
        checks['bundle_vs_stage_%d' % token] = \
            vs_stage['sha256'] == row['vertex_stage_sha256']
        first_failure(checks['bundle_vs_stage_%d' % token],
                      'check failed: bundle VS stage token %d' % token, failures)

    # Candidate bank, spec and O1 receipts.
    bank_index_json, bank_stages, bank_variants, bank_programs, bank_abi = \
        aug.read_base(args.candidate_bank, aug.ABI_CANDIDATE)
    checks['candidate_bank_abi'] = bank_abi == aug.ABI_CANDIDATE
    checks['candidate_bank_counts'] = (
        len(bank_stages), len(bank_variants), len(bank_programs)) == \
        (2402, 291, 6311)
    checks['candidate_bank_index'] = sha_file(args.candidate_bank /
                                              'shader-index.bin') == \
        manifest.get('inputs', {}).get('bank_index_sha256')
    sidecar_manifest_path = args.candidate_bank / 'shader-resources.manifest.json'
    checks['sidecar_manifest_present'] = sidecar_manifest_path.is_file()
    if sidecar_manifest_path.is_file():
        sidecar_manifest = read_json(sidecar_manifest_path)
        checks['sidecar_index_sha'] = sidecar_manifest.get('index', {}).get(
            'sha256') == sha_file(args.candidate_bank / 'shader-index.bin')
        checks['sidecar_index_crc'] = str(sidecar_manifest.get('index', {}).get(
            'crc32', '')).lower() == ('%08x' % (zlib.crc32(
                (args.candidate_bank / 'shader-index.bin').read_bytes()) &
                0xffffffff))
    else:
        checks['sidecar_index_sha'] = False
        checks['sidecar_index_crc'] = False
    for key in ('candidate_bank_abi', 'candidate_bank_counts',
                'candidate_bank_index', 'sidecar_manifest_present',
                'sidecar_index_sha', 'sidecar_index_crc'):
        first_failure(checks[key], 'check failed: ' + key, failures)

    spec = read_json(args.spec)
    spec_add = {row['shader_id']: row for row in spec.get('add', [])}
    checks['spec_nine'] = len(spec_add) == 9
    checks['spec_flags'] = all(row.get('flags') == EXPECTED_FLAGS
                               for row in spec_add.values())
    checks['spec_stage_set'] = set(spec_add) == {
        pair['fragment_shader_id'] for pair in pairs.values()}
    for key in ('spec_nine', 'spec_flags', 'spec_stage_set'):
        first_failure(checks[key], 'check failed: ' + key, failures)

    _, toolchain = csc.load_shacc_identity(
        ROOT / 'local/shader-tools/shacc-bin/Vita3K-Shacc-windows-x86_64/vita3k-shacc.exe',
        ROOT / 'local/shader-tools/shacc.conf')
    receipts = {}
    for token in EXPECTED_TOKENS:
        pair = pairs.get(token)
        if pair is None:
            continue
        shader_id = pair['fragment_shader_id']
        spec_row = spec_add.get(shader_id)
        receipt_path = args.compile_root / 'stages' / shader_id / 'receipto1.json'
        checks['receipt_present_%d' % token] = receipt_path.is_file()
        if not receipt_path.is_file():
            failures.append('missing O1 receipt for token %d' % token)
            continue
        receipt = read_json(receipt_path)
        checks['receipt_sha_%d' % token] = (
            receipt.get('shader_id') == shader_id and
            receipt.get('optimization') == '-O1' and
            receipt.get('flags') == EXPECTED_FLAGS and
            receipt.get('classification') == 'compiled' and
            receipt.get('valid_header') is True and
            receipt.get('gxp_sha256') == pair.get('gxp_sha256') and
            receipt.get('gxp_bytes') == pair.get('gxp_bytes') and
            receipt.get('stage_sha256') == \
            header_by_token[token].get('fragment_stage_sha256') and
            spec_row is not None and
            receipt.get('source_sha256') == spec_row.get('cg_sha256') and
            spec_row.get('gxp_sha256') == pair.get('gxp_sha256') and
            spec_row.get('expected_variant_key') ==
            pair.get('variant_key') and
            receipt.get('toolchain') == csc.stage_toolchain_identity(toolchain))
        first_failure(checks['receipt_sha_%d' % token],
                      'check failed: O1 receipt token %d' % token, failures)
        receipts[str(token)] = {
            'path': str(receipt_path),
            'sha256': sha_file(receipt_path),
            'gxp_sha256': receipt.get('gxp_sha256'),
            'gxp_bytes': receipt.get('gxp_bytes'),
            'source_sha256': receipt.get('source_sha256'),
            'identity_sha256': receipt.get('identity_sha256'),
        }

    # Device rows and raw dump.
    device_path = args.run_dir / 'gxm-shadow-pair.log'
    blocks = parse_device_log(device_path)
    checks['device_blocks'] = len(blocks) >= 1
    latest = blocks[-1] if blocks else None
    all_terminals = []
    for block in blocks:
        all_terminals.extend(parse_terminal(row) for row in block['terminals'])
        all_terminals.extend(parse_terminal(row) for row in block['timeouts'])
    final_by_token = {}
    for row in all_terminals:
        token = row.get('token')
        if token is not None:
            final_by_token[token] = row
    checks['terminal_nine'] = sorted(final_by_token) == list(EXPECTED_TOKENS)
    checks['terminal_unique'] = len(final_by_token) == 9
    checks['latest_result_complete'] = latest is not None and \
        latest.get('result') == 'COMPLETE'
    checks['latest_run_terminal'] = latest is not None and \
        latest.get('run', {}).get('terminal') == '9/9'
    for key in ('device_blocks', 'terminal_nine', 'terminal_unique',
                'latest_result_complete', 'latest_run_terminal'):
        first_failure(checks[key], 'check failed: ' + key, failures)

    device_run_id = latest.get('run_id') if latest else None
    checks['device_run_id'] = device_run_id == header.get('RUN_ID')
    first_failure(checks['device_run_id'], 'check failed: device RUN-ID',
                  failures)

    dump_path = args.run_dir / 'gxm-shadow-pair.rgba'
    raw = dump_path.read_bytes() if dump_path.is_file() else b''
    checks['raw_present'] = dump_path.is_file() and len(raw) > 0
    raw_sha = sha_bytes(raw) if raw else None
    dump_by_token = {}
    if latest is not None:
        for row in latest['dumps']:
            fields = row['fields']
            token = int(fields['token'])
            dump_by_token[token] = {
                'offset': int(fields['offset']), 'bytes': int(fields['bytes']),
                'fnv': int(fields['fnv'], 16), 'line': row['line']}
    background = latest.get('control_pixel') if latest else None
    token_results = []
    raw_offsets = []
    for token in EXPECTED_TOKENS:
        row = final_by_token.get(token)
        if row is None:
            token_results.append({'token': token, 'status': 'missing'})
            continue
        item = dict(row)
        item['gxp_matches_header'] = row.get('gxp_sha') == \
            header_by_token.get(token, {}).get('fragment_gxp_sha256')
        item['key_matches_header'] = row.get('key') == \
            header_by_token.get(token, {}).get('fragment_variant_key')
        item['steps_all'] = all(row.get(k) is True for k in
                                ('create', 'register', 'reflect', 'vp', 'fp',
                                 'bind', 'draw', 'notify', 'finish', 'readback'))
        if not item['gxp_matches_header'] or not item['key_matches_header']:
            failures.append('device GXP/key mismatch token %d' % token)
        if row.get('status') in ('fail', 'timeout'):
            failures.append('device terminal status %s token %d'
                            % (row.get('status'), token))
        if not item['steps_all'] and row.get('status') not in ('fail', 'timeout'):
            failures.append('device API step incomplete token %d' % token)
        dump = dump_by_token.get(token)
        if dump is None:
            item['dump'] = None
            if row.get('status') in ('ok', 'undetermined'):
                failures.append('missing DUMP row token %d' % token)
        else:
            start = dump['offset']
            end = start + dump['bytes']
            item['dump'] = dict(dump)
            checks['dump_range_%d' % token] = 0 <= start <= end <= len(raw) \
                and dump['bytes'] == 16384
            first_failure(checks['dump_range_%d' % token],
                          'check failed: raw range token %d' % token, failures)
            if checks['dump_range_%d' % token]:
                region = raw[start:end]
                values = list(struct.unpack('<%dI' % (len(region) // 4),
                                            region))
                computed_fnv = fnv_u32(values)
                computed_cov = sum(1 for value in values
                                    if value != background) \
                    if background is not None else None
                computed_nz = sum(1 for value in values if value & 0xffffff)
                item['dump']['fnv_match'] = computed_fnv == dump['fnv'] and \
                    computed_fnv == int(row.get('hash', '0'), 16)
                item['dump']['cov_match'] = computed_cov == row.get('cov')
                item['dump']['nonzero_match'] = computed_nz == row.get('nonzero')
                item['dump']['byte_sha256'] = sha_bytes(region)
                checks['dump_content_%d' % token] = all((
                    item['dump']['fnv_match'], item['dump']['cov_match'],
                    item['dump']['nonzero_match']))
                first_failure(checks['dump_content_%d' % token],
                              'check failed: raw content token %d' % token,
                              failures)
                raw_offsets.append((start, end, token))
        if row.get('status') == 'timeout' or row.get('status') == 'fail':
            item['content'] = 'failed'
        elif row.get('status') == 'ok':
            item['content'] = 'nonzero'
        else:
            item['content'] = 'undetermined'
            warnings.append('token %d zero coverage: content undetermined'
                            % token)
        token_results.append(item)
    if latest is not None and len(latest['dumps']) != 9:
        warnings.append('latest dump row count is %d, not 9'
                        % len(latest['dumps']))
    raw_offsets.sort()
    checks['raw_offsets_unique'] = all(
        raw_offsets[i][0] >= raw_offsets[i - 1][1]
        for i in range(1, len(raw_offsets)))
    first_failure(checks['raw_offsets_unique'], 'check failed: raw offset overlap',
                  failures)
    if raw:
        checks['raw_size'] = len(raw) % 16384 == 0
        first_failure(checks['raw_size'], 'check failed: raw size', failures)

    # INFO backend parse by exact GXP SHA.
    emu_path = args.run_dir / 'emulator.log'
    if not emu_path.is_file():
        emu_path = args.run_dir / 'backend-game.log'
    checks['emulator_log_present'] = emu_path.is_file()
    first_failure(checks['emulator_log_present'],
                  'check failed: emulator log missing', failures)
    backend = parse_emulator_log(emu_path) if emu_path.is_file() else {
        'program_ready': {}, 'draws': [], 'exceptions': [],
        'program_fail': {}, 'draw_errors': [], 'lines': 0}
    draws = backend.get('draws', [])
    last_draw_ts = max((row['ts'] for row in draws), default=None)
    target_programs = {}
    for token in EXPECTED_TOKENS:
        pair = pairs.get(token)
        if pair is None:
            continue
        vs_id = pair.get('vertex_shader_id')
        vs_row = reuse.get(vs_id) or {}
        frag_key = pair.get('variant_key')
        vs_key = vs_row.get('variant_key')
        frag_path = args.bundle / 'shaders' / (str(frag_key) + '.gxp')
        vs_path = args.bundle / 'shaders' / (str(vs_key) + '.gxp')
        checks['gxp_files_%d' % token] = frag_path.is_file() and vs_path.is_file()
        if not checks['gxp_files_%d' % token]:
            failures.append('missing bundle GXP files for token %d' % token)
            continue
        frag_data = frag_path.read_bytes()
        vs_data = vs_path.read_bytes()
        checks['gxp_sha_%d' % token] = (
            sha_bytes(frag_data) == pair.get('gxp_sha256') and
            sha_bytes(vs_data) == vs_row.get('gxp_sha256'))
        first_failure(checks['gxp_sha_%d' % token],
                      'check failed: bundle GXP SHA token %d' % token, failures)
        key = (gxp_internal_sha(frag_data), gxp_internal_sha(vs_data))
        ready = backend.get('program_ready', {}).get(key, [])
        target_programs[token] = {
            'key': {'fragment_gxp': key[0], 'vertex_gxp': key[1],
                    'fragment_file_sha256': sha_bytes(frag_data),
                    'vertex_file_sha256': sha_bytes(vs_data)},
            'ready_rows': ready,
            'program': ready[-1]['prog'] if ready else None,
            'cached': [row.get('cached') for row in ready],
        }
        checks['program_ready_%d' % token] = bool(ready) and \
            not backend.get('program_fail', {}).get(key)
        first_failure(checks['program_ready_%d' % token],
                      'check failed: program ready token %d' % token, failures)
        program = target_programs[token]['program']
        token_draws = [row for row in draws if row['prog'] == program] \
            if program is not None else []
        good = token_draws and all(
            row.get('linked') == 1 and row.get('state_err') == 0 and
            row.get('draw_err') == 0 for row in token_draws)
        target_programs[token]['draw_count'] = len(token_draws)
        target_programs[token]['linked'] = [row.get('linked') for row in token_draws]
        target_programs[token]['draw_errors'] = [row.get('draw_err') for row in token_draws]
        checks['draw_%d' % token] = bool(good)
        first_failure(checks['draw_%d' % token],
                      'check failed: linked draw token %d' % token, failures)

    exceptions = backend.get('exceptions', [])
    pre_draw_exception = []
    post_draw_exception = []
    for exception in exceptions:
        if last_draw_ts is not None and exception['ts'] < last_draw_ts:
            pre_draw_exception.append(exception)
        else:
            post_draw_exception.append(exception)
    checks['no_pre_draw_exception'] = not pre_draw_exception
    first_failure(checks['no_pre_draw_exception'],
                  'check failed: exception before last target draw', failures)
    if post_draw_exception:
        warnings.append('backend exception after last target draw; cleanup not '
                        'declared benign')
    if args.require_clean_exit and exceptions:
        failures.append('backend exception present (require-clean-exit)')

    process = read_json(args.run_dir / 'process.json') \
        if (args.run_dir / 'process.json').is_file() else {}
    checks['host_result_complete'] = process.get('result') == 'COMPLETE'
    first_failure(checks['host_result_complete'],
                  'check failed: host result COMPLETE', failures)
    checks['host_natural_exit'] = process.get('natural_exit') is True and \
        process.get('exit_code') == 0
    if not checks['host_natural_exit']:
        warnings.append('host process did not exit naturally with code 0; '
                        'cleanup is not declared benign')

    content_nonzero = any(row.get('content') == 'nonzero'
                          for row in token_results)
    evidence_complete = not failures
    summary = {
        'schema': 1,
        'task': 'M6-B27',
        'run_dir': str(args.run_dir),
        'bundle': {
            'path': str(args.bundle),
            'index_sha256': sha_bytes(index_data),
            'index_bytes': len(index_data),
            'header_sha256': sha_file(header_path),
            'manifest_sha256': sha_file(manifest_path),
            'run_id': header.get('RUN_ID'),
            'identity_sha256': header.get('IDENTITY_SHA256'),
            'source_sha256': header.get('SOURCE_SHA256'),
        },
        'candidate_bank': {
            'path': str(args.candidate_bank),
            'index_sha256': sha_file(args.candidate_bank / 'shader-index.bin'),
            'index_json_sha256': sha_file(args.candidate_bank / 'shader-index.json'),
            'manifest_sha256': sha_file(args.candidate_bank / 'manifest.json'),
            'sidecar_sha256': sha_file(args.candidate_bank / 'shader-resources.bin'),
            'sidecar_manifest_sha256': sha_file(
                args.candidate_bank / 'shader-resources.manifest.json'),
            'counts': {'stages': len(bank_stages),
                       'variants': len(bank_variants),
                       'programs': len(bank_programs)},
        },
        'spec': {'path': str(args.spec), 'sha256': sha_file(args.spec),
                 'add_count': len(spec_add)},
        'device': {
            'path': str(device_path), 'sha256': sha_file(device_path),
            'run_id': device_run_id, 'blocks': len(blocks),
            'latest_result': latest.get('result') if latest else None,
            'latest_run': latest.get('run') if latest else None,
            'control_pixel': ('%08x' % background) if background is not None
                             else None,
            'dumps_latest_block': len(latest['dumps']) if latest else 0,
        },
        'raw': {'path': str(dump_path), 'bytes': len(raw), 'sha256': raw_sha,
                'rows': len(raw_offsets)},
        'backend': {
            'path': str(emu_path), 'sha256': sha_file(emu_path)
            if emu_path.is_file() else None,
            'lines': backend.get('lines'),
            'target_draws_total': sum(row.get('draw_count', 0)
                                      for row in target_programs.values()),
            'last_target_draw_ts': last_draw_ts,
            'exceptions': exceptions,
            'pre_last_draw_exceptions': pre_draw_exception,
            'post_last_draw_exceptions': post_draw_exception,
            'program_fail': {str(key): value for key, value in
                             backend.get('program_fail', {}).items()},
            'compile_errors': backend.get('compile_errors', []),
            'draw_errors': backend.get('draw_errors', []),
        },
        'programs': target_programs,
        'receipts': receipts,
        'tokens': token_results,
        'host': process,
        'checks': checks,
        'failures': failures,
        'warnings': warnings,
        'evidence': {
            'nine_unique_terminals': checks.get('terminal_nine', False) and
                                     checks.get('terminal_unique', False),
            'api_complete': all(row.get('steps_all') for row in token_results
                                if row.get('status') in ('ok', 'undetermined')),
            'gl_program_ready': all(checks.get('program_ready_%d' % token, False)
                                    for token in EXPECTED_TOKENS),
            'gl_draw_linked_no_error': all(checks.get('draw_%d' % token, False)
                                           for token in EXPECTED_TOKENS),
            'raw_rows_closed': all(checks.get('dump_content_%d' % token, False)
                                   for token in EXPECTED_TOKENS
                                   if final_by_token.get(token, {}).get('status')
                                   in ('ok', 'undetermined')),
            'content_nonzero_all': content_nonzero and all(
                row.get('content') == 'nonzero' for row in token_results),
            'content_undetermined_all': all(
                row.get('content') == 'undetermined' for row in token_results),
            'clean_exit': checks.get('host_natural_exit', False) and
                          not exceptions,
            'evidence_complete': evidence_complete,
        },
        'claims': {
            'backend_gate_complete': evidence_complete,
            'actual9_content_observed': content_nonzero and all(
                row.get('content') == 'nonzero' for row in token_results),
            'actual9_content_undetermined': all(
                row.get('content') == 'undetermined' for row in token_results),
            'game_pixels_observed': False,
            'actual_atlas_observed': False,
            'batching_implemented': False,
            'performance_verified': False,
            'hardware_verified': False,
        },
    }
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    args.summary.write_text(json.dumps(summary, indent=2) + '\n',
                            encoding='utf-8', newline='\n')
    print(json.dumps({
        'summary': str(args.summary),
        'failures': failures,
        'warnings': warnings,
        'evidence': summary['evidence'],
        'claims': summary['claims'],
    }, indent=2))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())

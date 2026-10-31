"""Augment a packed static shader bank with verified input specializations.

This tool consumes the *already packed* index (JSON + binary) rather than a
catalogue/ledger pair. It preserves the base stage, variant and program tables
byte for byte: every retained GXP is copied and re-hashed, and only the variant
table gains records. Stages that already have a generic variant keep it; a
specialized record is added next to it with a non-zero input signature.

Versioned identity
------------------
The base bank must be static-index ABI 1. ABI 2 reuses the variant record's
reserved 32-bit word for a 16-bit UINT-input signature. Overlapping viewports
are legal only when the signatures differ; the reader rejects any ambiguous
duplicate. Old ABI 1 banks remain readable and mean "generic float inputs".

The spec is intentionally narrow: `add` records carry an exact stage SHA, GXP
file SHA/size, viewport mask, input signature and provenance; `replace` records
must name the exact old key/SHA and are rejected when that entry is absent.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zlib

MAGIC = b'VPSHIX1\0'
HEADER_SIZE = 64
STAGE_RECORD = 48
VARIANT_RECORD = 40
PROGRAM_RECORD = 16
ABI_BASE = 1
ABI_CANDIDATE = 2
VARIANT_COMPILED = 1
PROGRAM_OBSERVED = 1
PROGRAM_STATIC = 2


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=False).encode()


def variant_key(stage_sha, gxp_sha, abi, input_signature, environment_sha, flags,
                content_identity_sha256=None):
    """Content identity for an added variant.
    ABI 2 keys bind the exact stage, GXP bytes, environment, compiler flags and
    the input descriptor signature, so a specialization of the same GXP for a
    different input set cannot share a key.  New M6-B24 shadow entries
    additionally bind a caller-supplied content identity (Cg/policy/emitter/
    compiler metadata); the field is omitted for older entries so every
    retained key and every historical spec keeps its original formula.
    """
    identity = {
        'schema': 2, 'stage_sha256': stage_sha.hex(),
        'gxp_sha256': gxp_sha.hex(), 'abi_version': abi,
        'input_signature': input_signature,
        'environment_sha256': environment_sha, 'flags': flags}
    if content_identity_sha256 is not None:
        identity['content_identity_sha256'] = content_identity_sha256
    return hashlib.sha256(canonical(identity)).digest()[:16]


def valid_gxp(path):
    data = path.read_bytes()
    if len(data) < 32 or data[:4] != b'GXP\0':
        raise ValueError('not a GXP: ' + str(path))
    declared = struct.unpack_from('<I', data, 8)[0]
    if declared < 32 or declared > len(data):
        raise ValueError('GXP declared size invalid: ' + str(path))
    return data


def read_base(base, expected_abi=ABI_BASE):
    index_json = json.loads((base / 'shader-index.json').read_text(encoding='utf-8'))
    bin_data = (base / 'shader-index.bin').read_bytes()
    if len(bin_data) < HEADER_SIZE or bin_data[:8] != MAGIC:
        raise ValueError('base index magic/size invalid')
    version, header_size = struct.unpack_from('<HH', bin_data, 8)
    flags = struct.unpack_from('<I', bin_data, 12)[0]
    stages, variants, programs, stage_size, variant_size, program_size = \
        struct.unpack_from('<IIIIII', bin_data, 16)
    body_bytes, body_crc = struct.unpack_from('<II', bin_data, 40)
    abi = struct.unpack_from('<I', bin_data, 48)[0]
    if version != 1 or header_size != HEADER_SIZE or flags != 1:
        raise ValueError('base index header unsupported')
    if (stage_size, variant_size, program_size) != \
            (STAGE_RECORD, VARIANT_RECORD, PROGRAM_RECORD):
        raise ValueError('base index record sizes unsupported')
    if abi not in (ABI_BASE, ABI_CANDIDATE):
        raise ValueError('base bank ABI unsupported')
    if abi != expected_abi:
        raise ValueError('base bank ABI %d differs from --base-abi %d' %
                         (abi, expected_abi))
    expected = stages * STAGE_RECORD + variants * VARIANT_RECORD + programs * PROGRAM_RECORD
    if body_bytes != expected or len(bin_data) != HEADER_SIZE + body_bytes:
        raise ValueError('base index body size mismatch')
    body = bin_data[HEADER_SIZE:]
    if zlib.crc32(body) & 0xffffffff != body_crc:
        raise ValueError('base index body CRC mismatch')
    if index_json.get('schema') != 1 or index_json.get('abi_version') != abi:
        raise ValueError('base index JSON identity mismatch')
    if len(index_json['stages']) != stages or len(index_json['variants']) != variants \
            or len(index_json['programs']) != programs:
        raise ValueError('base index JSON table counts differ from binary')
    # Decode and cross-check every binary record against the JSON table.
    decoded_stages, decoded_variants, decoded_programs = [], [], []
    for i in range(stages):
        offset = i * STAGE_RECORD
        stage_id = struct.unpack_from('<I', body, offset)[0]
        sha = body[offset + 4:offset + 36]
        start, count, source_flags, reserved = struct.unpack_from('<HHII', body, offset + 36)
        row = index_json['stages'][i]
        if (row['index'] != i or row['stage'] != ('vert' if stage_id == 1 else 'frag') or
                bytes.fromhex(row['stage_sha256']) != sha or
                row['variant_start'] != start or row['variant_count'] != count or
                row['source_flags'] != source_flags or reserved != 0):
            raise ValueError('base stage record %d differs from JSON' % i)
        decoded_stages.append({
            'index': i, 'stage_id': stage_id, 'sha256': sha,
            'variant_start': start, 'variant_count': count,
            'source_flags': source_flags, 'json': row})
    for i in range(variants):
        offset = stages * STAGE_RECORD + i * VARIANT_RECORD
        stage_index, viewport_mask, variant_abi = struct.unpack_from('<IHH', body, offset)
        key = body[offset + 8:offset + 24]
        gxp_bytes, vflags, crc, signature = struct.unpack_from('<IIII', body, offset + 24)
        row = index_json['variants'][i]
        if (row['stage_index'] != stage_index or row['viewport_mask'] != viewport_mask or
                variant_abi != abi or bytes.fromhex(row['key']) != key or
                row['gxp_bytes'] != gxp_bytes or vflags != VARIANT_COMPILED or
                row['gxp_sha256'] == '' or row.get('input_signature', 0) != signature):
            raise ValueError('base variant record %d differs from JSON' % i)
        if abi == ABI_BASE and signature != 0:
            raise ValueError('base ABI 1 variant %d has a signature' % i)
        if abi == ABI_CANDIDATE and signature & ~0xffff:
            raise ValueError('base ABI 2 variant %d signature out of range' % i)
        decoded_variants.append({
            'stage_index': stage_index, 'viewport_mask': viewport_mask, 'key': key,
            'gxp_bytes': gxp_bytes, 'flags': vflags, 'crc32': crc,
            'input_signature': signature, 'json': row, 'base_order': i,
            'gxp': row['gxp'], 'gxp_sha256': bytes.fromhex(row['gxp_sha256']),
            'provenance': list(row.get('provenance', []))})
    for i in range(programs):
        offset = stages * STAGE_RECORD + variants * VARIANT_RECORD + i * PROGRAM_RECORD
        token, vs, fs, pflags, reserved = struct.unpack_from('<IHHII', body, offset)
        row = index_json['programs'][i]
        if (row['token'] != token or row['vertex_stage'] != vs or
                row['fragment_stage'] != fs or row['flags'] != pflags or reserved != 0):
            raise ValueError('base program record %d differs from JSON' % i)
        decoded_programs.append({'token': token, 'vertex_stage': vs,
                                 'fragment_stage': fs, 'flags': pflags})
    return index_json, decoded_stages, decoded_variants, decoded_programs, abi


def verify_base_gxps(base, variants):
    total = 0
    for variant in variants:
        path = base / variant['gxp']
        if not path.is_file():
            raise ValueError('base GXP deleted: ' + str(path))
        data = valid_gxp(path)
        if len(data) != variant['gxp_bytes']:
            raise ValueError('base GXP size mismatch: ' + str(path))
        if hashlib.sha256(data).digest() != variant['gxp_sha256']:
            raise ValueError('base GXP SHA mismatch: ' + str(path))
        if zlib.crc32(data) & 0xffffffff != variant['crc32']:
            raise ValueError('base GXP CRC mismatch: ' + str(path))
        total += len(data)
    return total


def load_spec(path, repo):
    spec = json.loads(path.read_text(encoding='utf-8'))
    if spec.get('schema') != 1:
        raise ValueError('spec schema unsupported')
    return spec


def resolve(repo, value):
    path = Path(value)
    return path if path.is_absolute() else (repo / path).resolve()


def checked_source(repo, entry):
    source = resolve(repo, entry['gxp_source'])
    data = valid_gxp(source)
    if len(data) != entry['gxp_bytes']:
        raise ValueError('candidate GXP size mismatch: ' + str(source))
    sha = hashlib.sha256(data).hexdigest()
    if sha != entry['gxp_sha256']:
        raise ValueError('candidate GXP SHA mismatch: ' + str(source))
    return data, sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True,
                        help='existing packed bank (must not be modified)')
    parser.add_argument('--base-abi', type=int, choices=(ABI_BASE, ABI_CANDIDATE),
                        default=ABI_BASE,
                        help='expected ABI of the base bank (default: 1)')
    parser.add_argument('--spec', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True,
                        help='new candidate bank directory (must not exist)')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    base = args.base.resolve()
    output = args.output.resolve()
    spec = load_spec(args.spec, repo)
    if output.exists():
        raise ValueError('output must not exist')
    if digest(base / 'shader-index.bin') != spec['base_index_sha256']:
        raise ValueError('base index SHA differs from spec')
    if spec.get('base_shader_index_json_sha256') and \
            digest(base / 'shader-index.json') != spec['base_shader_index_json_sha256']:
        raise ValueError('base index JSON SHA differs from spec')

    index_json, stages, variants, programs, base_abi = read_base(base, args.base_abi)
    base_gxp_bytes = verify_base_gxps(base, variants)
    stage_by_index = {row['index']: row for row in index_json['stages']}

    # Validate replacement requests before any mutation.
    replace_by_index = {}
    for request in spec.get('replace', []):
        stage_index = request['stage_index']
        old_key = request['old_key']
        old_sha = request['old_gxp_sha256']
        matches = [variant for variant in variants
                   if variant['stage_index'] == stage_index and
                   variant['key'].hex() == old_key and
                   variant['gxp_sha256'].hex() == old_sha and
                   variant['viewport_mask'] == request['viewport_mask']]
        if len(matches) != 1:
            raise ValueError('replace target missing/ambiguous: stage %d key %s' %
                             (stage_index, old_key))
        if stage_index in replace_by_index:
            raise ValueError('duplicate replace request')
        replace_by_index[stage_index] = matches[0]['key']
    retained = [variant for variant in variants
                if not (variant['stage_index'] in replace_by_index and
                        variant['key'] == replace_by_index[variant['stage_index']])]

    added = []
    for entry in spec.get('add', []):
        stage_index = entry['stage_index']
        if stage_index not in stage_by_index:
            raise ValueError('added variant stage out of range')
        stage_row = stage_by_index[stage_index]
        if entry['shader_id'] != stage_row['shader_id'] or \
                entry['stage_sha256'] != stage_row['stage_sha256']:
            raise ValueError('added variant stage identity mismatch')
        viewport_mask = entry['viewport_mask']
        signature = entry['input_signature']
        if viewport_mask == 0 or viewport_mask & ~3:
            raise ValueError('added variant viewport mask invalid')
        if signature & ~0xffff:
            raise ValueError('added variant input signature out of range')
        if signature and stage_row['stage'] != 'vert':
            raise ValueError('input signature only applies to vertex stages')
        if signature == 0 and entry.get('source_kind') == 'uattr3':
            raise ValueError('specialized source must carry a signature')
        data, sha = checked_source(repo, entry)
        content_identity = entry.get('content_identity')
        content_identity_sha = None
        if content_identity is not None:
            if not isinstance(content_identity, dict):
                raise ValueError('content_identity must be an object')
            computed = hashlib.sha256(canonical(content_identity)).hexdigest()
            if entry.get('content_identity_sha256') != computed:
                raise ValueError('content_identity SHA mismatch')
            expected = {
                'stage_sha256': stage_row['stage_sha256'],
                'gxp_sha256': sha,
                'cg_sha256': entry.get('cg_sha256'),
                'environment_sha256': entry.get('environment_sha256'),
                'flags': entry.get('flags'),
                'profile': entry.get('profile'),
                'strategy': entry.get('strategy'),
                'policy_sha256': entry.get('policy_sha256'),
                'emitter_sha256': entry.get('emitter_sha256'),
                'helper_sha256': entry.get('helper_sha256'),
            }
            for field, value in expected.items():
                if content_identity.get(field) != value:
                    raise ValueError('content_identity %s is not bound to the '
                                     'spec entry' % field)
            for field in ('cg_sha256', 'stage_sha256', 'gxp_sha256',
                          'environment_sha256', 'policy_sha256',
                          'emitter_sha256', 'helper_sha256', 'profile',
                          'strategy'):
                if not isinstance(content_identity.get(field), str) or \
                        not content_identity.get(field):
                    raise ValueError('content_identity %s is missing' % field)
            if not isinstance(content_identity.get('flags'), list) or \
                    not content_identity.get('flags'):
                raise ValueError('content_identity flags are missing')
            content_identity_sha = computed
        key = variant_key(bytes.fromhex(stage_row['stage_sha256']), bytes.fromhex(sha),
                          ABI_CANDIDATE, signature,
                          entry.get('environment_sha256') or '',
                          entry.get('flags') or [],
                          content_identity_sha)
        if sha != entry['gxp_sha256']:
            raise ValueError('candidate SHA drift')
        provenance = list(entry.get('provenance', [])) + ['candidate-add']
        if content_identity_sha:
            provenance.append('content-identity-sha256:' + content_identity_sha)
        added.append({
            'stage_index': stage_index, 'viewport_mask': viewport_mask,
            'key': key, 'gxp_bytes': len(data), 'flags': VARIANT_COMPILED,
            'crc32': zlib.crc32(data) & 0xffffffff,
            'input_signature': signature, 'base_order': len(variants),
            'gxp': 'shaders/' + key.hex() + '.gxp',
            'gxp_sha256': bytes.fromhex(sha), 'data': data,
            'provenance': provenance,
            'source_kind': entry.get('source_kind'),
            'source_path': entry['gxp_source'],
            'environment_sha256': entry.get('environment_sha256'),
            'compiler_flags': entry.get('flags'),
            'cg_sha256': entry.get('cg_sha256'),
            'content_identity': content_identity,
            'content_identity_sha256': content_identity_sha,
        })

    all_variants = retained + added
    keys = set()
    for variant in all_variants:
        if variant['key'] in keys:
            raise ValueError('duplicate variant key after augmentation')
        keys.add(variant['key'])
    # Reject ambiguous overlaps: same stage, intersecting viewport and the same
    # input signature. Different signatures are the intended ABI 2 extension.
    for i, left in enumerate(all_variants):
        for right in all_variants[:i]:
            if left['stage_index'] != right['stage_index']:
                continue
            if not (left['viewport_mask'] & right['viewport_mask']):
                continue
            if left['input_signature'] == right['input_signature']:
                raise ValueError('ambiguous overlapping specialization stage %d '
                                 'signature 0x%x' % (left['stage_index'],
                                                     left['input_signature']))
    # Retained records keep their base relative order; new records follow their
    # stage's retained records. Stage ranges are rebuilt below.
    all_variants.sort(key=lambda row: (row['stage_index'], row['base_order'],
                                       row['input_signature'], row['key']))
    ranges = {}
    for index, stage in enumerate(index_json['stages']):
        rows = [variant for variant in all_variants if variant['stage_index'] == index]
        ranges[index] = (rows, 0)
    running = 0
    stage_records = []
    for row in index_json['stages']:
        rows = [variant for variant in all_variants if variant['stage_index'] == row['index']]
        masks = 0
        for variant in rows:
            masks |= variant['viewport_mask']
        stage_records.append({
            'index': row['index'], 'stage_id': 1 if row['stage'] == 'vert' else 5,
            'sha256': bytes.fromhex(row['stage_sha256']),
            'variant_start': running, 'variant_count': len(rows),
            'source_flags': row['source_flags'], 'json': row})
        running += len(rows)
    if running != len(all_variants):
        raise ValueError('variant table ownership mismatch')

    body = bytearray()
    for record in stage_records:
        body += struct.pack('<I', record['stage_id'])
        body += record['sha256']
        body += struct.pack('<HHI', record['variant_start'], record['variant_count'],
                            record['source_flags'])
        body += struct.pack('<I', 0)
    for variant in all_variants:
        body += struct.pack('<IHH', variant['stage_index'], variant['viewport_mask'],
                            ABI_CANDIDATE)
        body += variant['key']
        body += struct.pack('<II', variant['gxp_bytes'], variant['flags'])
        body += struct.pack('<II', variant['crc32'], variant['input_signature'])
    for program in programs:
        body += struct.pack('<IHHI', program['token'], program['vertex_stage'],
                            program['fragment_stage'], program['flags'])
        body += struct.pack('<I', 0)
    header = bytearray(HEADER_SIZE)
    header[0:8] = MAGIC
    struct.pack_into('<HH', header, 8, 1, HEADER_SIZE)
    struct.pack_into('<I', header, 12, 1)
    struct.pack_into('<IIIIII', header, 16, len(stage_records), len(all_variants),
                     len(programs), STAGE_RECORD, VARIANT_RECORD, PROGRAM_RECORD)
    struct.pack_into('<II', header, 40, len(body), zlib.crc32(body) & 0xffffffff)
    struct.pack_into('<I', header, 48, ABI_CANDIDATE)
    index_bytes = bytes(header) + bytes(body)

    tmp = output.with_name(output.name + '.tmp')
    if tmp.exists():
        shutil.rmtree(tmp)
    (tmp / 'shaders').mkdir(parents=True)
    # Retained GXP bytes come from the immutable base bank and are re-verified.
    for variant in retained:
        source = base / variant['gxp']
        target = tmp / 'shaders' / (variant['key'].hex() + '.gxp')
        shutil.copyfile(source, target)
        if digest(target) != variant['gxp_sha256'].hex():
            raise ValueError('retained GXP changed during copy')
    for variant in added:
        (tmp / 'shaders' / (variant['key'].hex() + '.gxp')).write_bytes(variant['data'])
    (tmp / 'shader-index.bin').write_bytes(index_bytes)

    out_json = {
        'schema': 1, 'magic': MAGIC.decode('latin1'), 'abi_version': ABI_CANDIDATE,
        'stage_record_size': STAGE_RECORD, 'variant_record_size': VARIANT_RECORD,
        'program_record_size': PROGRAM_RECORD,
        'stages': [],
        'variants': [],
        'programs': programs,
    }
    by_stage = {}
    for variant in all_variants:
        by_stage.setdefault(variant['stage_index'], []).append(variant)
    for record in stage_records:
        rows = by_stage.get(record['index'], [])
        masks = 0
        for variant in rows:
            masks |= variant['viewport_mask']
        out_json['stages'].append({
            'index': record['index'],
            'stage': record['json']['stage'],
            'shader_id': record['json']['shader_id'],
            'stage_sha256': record['sha256'].hex(),
            'source_flags': record['source_flags'],
            'observed': record['json']['observed'],
            'variant_start': record['variant_start'],
            'variant_count': record['variant_count'],
            'viewport_mask': masks})
    for variant in all_variants:
        row = {
            'key': variant['key'].hex(),
            'stage_index': variant['stage_index'],
            'viewport_mask': variant['viewport_mask'],
            'input_signature': variant['input_signature'],
            'gxp_sha256': variant['gxp_sha256'].hex(),
            'gxp_bytes': variant['gxp_bytes'],
            'gxp_crc32': variant['crc32'],
            'provenance': sorted(set(variant['provenance'])),
            'gxp': 'shaders/' + variant['key'].hex() + '.gxp'}
        # Optional metadata for content-identity-bound additions.  Retained
        # variants and every historical ABI 2 key keep their original JSON
        # fields and bytes.
        if variant.get('content_identity_sha256'):
            row['content_identity_sha256'] = variant['content_identity_sha256']
            row['cg_sha256'] = variant.get('cg_sha256')
        out_json['variants'].append(row)
    (tmp / 'shader-index.json').write_text(
        json.dumps(out_json, ensure_ascii=False, indent=1, sort_keys=True) + '\n',
        encoding='utf-8', newline='\n')

    added_keys = {variant['key'] for variant in added}
    manifest = {
        'schema': 1,
        'mode': 'static-index-augment',
        'abi_version': ABI_CANDIDATE,
        'base_abi_version': base_abi,
        'base_bank': str(base),
        'base_index_sha256': digest(base / 'shader-index.bin'),
        'base_index_json_sha256': digest(base / 'shader-index.json'),
        'base_counts': {'stages': len(stages), 'variants': len(variants),
                        'programs': len(programs)},
        'candidate_counts': {'stages': len(stage_records),
                             'variants': len(all_variants),
                             'programs': len(programs)},
        'retained_variants': len(retained),
        'added_variants': len(added),
        'replaced_variants': len(replace_by_index),
        'retained_gxp_bytes': base_gxp_bytes,
        'added_gxp_bytes': sum(variant['gxp_bytes'] for variant in added),
        'index_sha256': hashlib.sha256(index_bytes).hexdigest(),
        'index_json_sha256': '',
        'stage_table_preserved': True,
        'program_table_preserved': True,
        'retained_gxp_bytes_identical': True,
        'added': [{'stage_index': variant['stage_index'],
                   'key': variant['key'].hex(),
                   'input_signature': variant['input_signature'],
                   'viewport_mask': variant['viewport_mask'],
                   'gxp_sha256': variant['gxp_sha256'].hex(),
                   'gxp_bytes': variant['gxp_bytes'],
                   'source_kind': variant['source_kind'],
                   'source_path': variant['source_path'],
                   'environment_sha256': variant['environment_sha256'],
                   'compiler_flags': variant['compiler_flags'],
                   'cg_sha256': variant.get('cg_sha256'),
                   'content_identity_sha256': variant.get('content_identity_sha256'),
                   'content_identity': variant.get('content_identity'),
                   'provenance': variant['provenance']} for variant in added],
        'replaced': [{'stage_index': variant['stage_index'],
                      'key': variant['key'].hex(),
                      'gxp_sha256': variant['gxp_sha256'].hex()}
                     for variant in variants
                     if variant['key'] in replace_by_index.values()],
        'blocked': spec.get('blocked', []),
        'notes': spec.get('notes', []),
        'input_selection': {
            'generic_signature': 0,
            'specialized_exact_subset': True,
            'generic_fallback_retained': True,
            'ambiguous_overlap_rejected': True},
        'content_identity_binding': {
            'applies_to': 'entries that supply content_identity',
            'fields': ['stage_sha256', 'gxp_sha256', 'cg_sha256',
                       'environment_sha256', 'flags', 'profile', 'strategy',
                       'policy_sha256', 'emitter_sha256', 'helper_sha256'],
            'key_rule': 'content_identity_sha256 is included in the ABI 2 key '
                        'only for entries that declare it; retained and older '
                        'entries keep their original key formula',
            'limits': 'This binds offline source/compiler metadata into the '
                      'candidate key. It is not a runtime or pixel claim.'},
        'limitations': [
            'No GXM linking, pixel, target-runtime or hardware check is implied.',
            'The candidate bank is not installed by this tool.',
            'UINT specialization is selected only for descriptors with type 4 '
            'at the signed locations; all other inputs use the generic variant.',
        ],
    }
    (tmp / 'manifest.json').write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + '\n',
        encoding='utf-8', newline='\n')
    shutil.copyfile(args.spec, tmp / 'candidate-spec.json')
    # Structural re-read of the written JSON before publishing.
    written = json.loads((tmp / 'shader-index.json').read_text(encoding='utf-8'))
    if len(written['variants']) != len(all_variants) or \
            sum(row['variant_count'] for row in written['stages']) != len(all_variants):
        raise ValueError('candidate index self-check failed')
    manifest['index_json_sha256'] = digest(tmp / 'shader-index.json')
    (tmp / 'manifest.json').write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + '\n',
        encoding='utf-8', newline='\n')
    tmp.rename(output)
    print(json.dumps({'output': str(output), 'retained': len(retained),
                      'added': len(added), 'index_sha256': manifest['index_sha256'],
                      'variants': len(all_variants)}, indent=2))


if __name__ == '__main__':
    main()

"""Independently verify an augmented static shader bank against its base bank.

This deliberately does not import the augmentation tool. It re-decodes both
packed indexes from their bytes and checks that:

* every base stage/program record and every base GXP key, SHA, CRC and payload
  is retained unchanged;
* each added record is exactly one declared spec entry (stage identity,
  viewport mask, input signature, GXP SHA/bytes and source bytes);
* the ABI 2 table has no duplicate keys or ambiguous same-signature overlaps;
* blocked stages are carried through without invented substitutes.

It reports structural and byte-identity evidence only. GXM linking, target
draws, pixels and hardware are not verified here.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

MAGIC = b'VPSHIX1\0'
ABI1 = 1
ABI2 = 2


def sha(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=False).encode()


def added_key(stage_sha, gxp_sha, signature, environment, flags, abi=ABI2,
              content_identity_sha256=None):
    identity = {
        'schema': 2, 'stage_sha256': stage_sha.hex(),
        'gxp_sha256': gxp_sha.hex(), 'abi_version': abi,
        'input_signature': signature,
        'environment_sha256': environment or '', 'flags': flags or []}
    if content_identity_sha256 is not None:
        identity['content_identity_sha256'] = content_identity_sha256
    return hashlib.sha256(canonical(identity)).digest()[:16]


def read_bank(root):
    binary = (root / 'shader-index.bin').read_bytes()
    sidecar = json.loads((root / 'shader-index.json').read_text(encoding='utf-8'))
    if binary[:8] != MAGIC:
        raise ValueError('bad magic in ' + str(root))
    version, header = struct.unpack_from('<HH', binary, 8)
    flags = struct.unpack_from('<I', binary, 12)[0]
    stages, variants, programs = struct.unpack_from('<III', binary, 16)
    sizes = struct.unpack_from('<III', binary, 28)
    body_bytes, crc = struct.unpack_from('<II', binary, 40)
    abi = struct.unpack_from('<I', binary, 48)[0]
    if (version, header, flags, sizes) != (1, 64, 1, (48, 40, 16)):
        raise ValueError('unsupported header in ' + str(root))
    expected = stages * 48 + variants * 40 + programs * 16
    if body_bytes != expected or len(binary) != 64 + expected:
        raise ValueError('body size mismatch in ' + str(root))
    body = binary[64:]
    if zlib.crc32(body) & 0xffffffff != crc:
        raise ValueError('body CRC mismatch in ' + str(root))
    if sidecar['abi_version'] != abi:
        raise ValueError('sidecar ABI mismatch in ' + str(root))
    if (len(sidecar['stages']), len(sidecar['variants']),
            len(sidecar['programs'])) != (stages, variants, programs):
        raise ValueError('sidecar counts mismatch in ' + str(root))
    decoded_stages = []
    for i in range(stages):
        offset = i * 48
        stage_id = struct.unpack_from('<I', body, offset)[0]
        stage_sha = body[offset + 4:offset + 36]
        start, count, source_flags = struct.unpack_from('<HHI', body, offset + 36)
        row = sidecar['stages'][i]
        if (row['index'] != i or row['stage_sha256'] != stage_sha.hex() or
                row['variant_start'] != start or row['variant_count'] != count or
                row['source_flags'] != source_flags):
            raise ValueError('stage %d differs from sidecar' % i)
        decoded_stages.append({'index': i, 'stage_id': stage_id,
                               'sha256': stage_sha, 'variant_start': start,
                               'variant_count': count,
                               'source_flags': source_flags,
                               'shader_id': row['shader_id']})
    decoded_variants = []
    for i in range(variants):
        offset = stages * 48 + i * 40
        stage_index, viewport_mask, variant_abi = struct.unpack_from('<IHH', body, offset)
        key = body[offset + 8:offset + 24]
        gxp_bytes, variant_flags = struct.unpack_from('<II', body, offset + 24)
        gxp_crc, signature = struct.unpack_from('<II', body, offset + 32)
        row = sidecar['variants'][i]
        if (row['key'] != key.hex() or row['stage_index'] != stage_index or
                row['viewport_mask'] != viewport_mask or
                row['gxp_bytes'] != gxp_bytes or
                row['gxp_sha256'] == ''):
            raise ValueError('variant %d differs from sidecar' % i)
        if abi == ABI1 and (signature != 0 or variant_abi != ABI1):
            raise ValueError('ABI 1 reserved/signature word not zero')
        if abi == ABI2 and (variant_abi != ABI2 or signature & ~0xffff):
            raise ValueError('ABI 2 variant ABI/signature invalid')
        path = root / row['gxp']
        data = path.read_bytes()
        if (len(data) != gxp_bytes or sha(data) != row['gxp_sha256'] or
                zlib.crc32(data) & 0xffffffff != gxp_crc):
            raise ValueError('GXP bytes invalid: ' + str(path))
        decoded_variants.append({
            'index': i, 'stage_index': stage_index, 'viewport_mask': viewport_mask,
            'key': key, 'gxp_bytes': gxp_bytes, 'flags': variant_flags,
            'crc32': gxp_crc, 'signature': signature, 'gxp_sha256': bytes.fromhex(
                row['gxp_sha256']), 'data': data, 'path': path,
            'gxp': row['gxp'], 'abi': variant_abi,
            'provenance': row.get('provenance', [])})
    decoded_programs = []
    for i in range(programs):
        offset = stages * 48 + variants * 40 + i * 16
        token, vs, fs, program_flags = struct.unpack_from('<IHHI', body, offset)
        reserved = struct.unpack_from('<I', body, offset + 12)[0]
        row = sidecar['programs'][i]
        if (row['token'] != token or row['vertex_stage'] != vs or
                row['fragment_stage'] != fs or row['flags'] != program_flags or
                reserved != 0):
            raise ValueError('program %d differs from sidecar' % i)
        decoded_programs.append({'token': token, 'vertex_stage': vs,
                                 'fragment_stage': fs, 'flags': program_flags})
    # Contiguous stage ownership and no same-signature overlap.
    running = 0
    for stage in decoded_stages:
        if stage['variant_start'] != running:
            raise ValueError('non-contiguous stage ranges')
        for j in range(stage['variant_count']):
            if decoded_variants[stage['variant_start'] + j]['stage_index'] != stage['index']:
                raise ValueError('variant owned by the wrong stage')
        rows = decoded_variants[stage['variant_start']:
                                stage['variant_start'] + stage['variant_count']]
        for a in range(len(rows)):
            for b in range(a):
                if (rows[a]['viewport_mask'] & rows[b]['viewport_mask'] and
                        rows[a]['signature'] == rows[b]['signature']):
                    raise ValueError('ambiguous overlapping specialization')
        running += stage['variant_count']
    if running != variants:
        raise ValueError('variant table not fully owned')
    if len({row['key'] for row in decoded_variants}) != variants:
        raise ValueError('duplicate variant key')
    return {'root': root, 'abi': abi, 'stages': decoded_stages,
            'variants': decoded_variants, 'programs': decoded_programs,
            'index_sha256': sha(binary),
            'index_json_sha256': sha((root / 'shader-index.json').read_bytes())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--base-abi', type=int, choices=(ABI1, ABI2), default=ABI1,
                        help='expected ABI of the base bank (default: 1)')
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--spec', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    base = read_bank(args.base.resolve())
    candidate = read_bank(args.candidate.resolve())
    spec = json.loads(args.spec.read_text(encoding='utf-8'))

    if base['abi'] != args.base_abi or candidate['abi'] != ABI2:
        raise ValueError('expected ABI %d base and ABI 2 candidate' % args.base_abi)
    if len(candidate['stages']) != len(base['stages']):
        raise ValueError('stage table size changed')
    for old, new in zip(base['stages'], candidate['stages']):
        if (old['stage_id'], old['sha256'], old['source_flags'], old['shader_id']) != \
                (new['stage_id'], new['sha256'], new['source_flags'], new['shader_id']):
            raise ValueError('base stage identity changed')
    if candidate['programs'] != base['programs']:
        raise ValueError('program table changed')

    old_by_key = {row['key']: row for row in base['variants']}
    new_by_key = {row['key']: row for row in candidate['variants']}
    missing = set(old_by_key) - set(new_by_key)
    if missing:
        raise ValueError('base variant keys deleted: %s' %
                         ', '.join(key.hex() for key in sorted(missing)))
    retained_identical = 0
    for key, old in old_by_key.items():
        new = new_by_key[key]
        if (old['stage_index'], old['viewport_mask'], old['gxp_bytes'],
                old['crc32'], old['gxp_sha256'], old['data']) != \
                (new['stage_index'], new['viewport_mask'], new['gxp_bytes'],
                 new['crc32'], new['gxp_sha256'], new['data']):
            raise ValueError('retained variant bytes/identity changed: ' + key.hex())
        if new['signature'] != old['signature']:
            raise ValueError('retained variant signature changed: ' + key.hex())
        retained_identical += 1

    stage_by_index = {row['index']: row for row in candidate['stages']}
    expected = {}
    for entry in spec['add']:
        content_identity = entry.get('content_identity')
        content_identity_sha = None
        if content_identity is not None:
            computed = sha(canonical(content_identity))
            if entry.get('content_identity_sha256') != computed:
                raise ValueError('spec content_identity SHA mismatch')
            for field, value in (
                    ('stage_sha256', entry['stage_sha256']),
                    ('gxp_sha256', entry['gxp_sha256']),
                    ('cg_sha256', entry.get('cg_sha256')),
                    ('environment_sha256', entry.get('environment_sha256')),
                    ('flags', entry.get('flags')),
                    ('profile', entry.get('profile')),
                    ('strategy', entry.get('strategy')),
                    ('policy_sha256', entry.get('policy_sha256')),
                    ('emitter_sha256', entry.get('emitter_sha256')),
                    ('helper_sha256', entry.get('helper_sha256'))):
                if content_identity.get(field) != value:
                    raise ValueError('content_identity %s not bound to spec' % field)
            content_identity_sha = computed
        key = added_key(bytes.fromhex(entry['stage_sha256']),
                        bytes.fromhex(entry['gxp_sha256']),
                        entry['input_signature'], entry.get('environment_sha256'),
                        entry.get('flags'), content_identity_sha256=content_identity_sha)
        if key.hex() in expected:
            raise ValueError('duplicate spec add')
        expected[key.hex()] = entry
    added_keys = {key.hex() for key in (set(new_by_key) - set(old_by_key))}
    if added_keys != set(expected):
        raise ValueError('added variant key set differs from spec')
    additions = []
    for key_hex in sorted(added_keys):
        entry = expected[key_hex]
        row = new_by_key[bytes.fromhex(key_hex)]
        stage = stage_by_index[entry['stage_index']]
        if (stage['shader_id'] != entry['shader_id'] or
                stage['sha256'].hex() != entry['stage_sha256'] or
                row['stage_index'] != entry['stage_index'] or
                row['viewport_mask'] != entry['viewport_mask'] or
                row['signature'] != entry['input_signature'] or
                row['gxp_sha256'].hex() != entry['gxp_sha256'] or
                row['gxp_bytes'] != entry['gxp_bytes']):
            raise ValueError('added record identity differs from spec: ' + key_hex)
        source = Path(entry['gxp_source'])
        if not source.is_absolute():
            source = args.spec.resolve().parents[2] / source
        if row['data'] != source.read_bytes():
            raise ValueError('added GXP bytes differ from source: ' + key_hex)
        if entry.get('content_identity_sha256') and \
                ('content-identity-sha256:' +
                 entry['content_identity_sha256']) not in row['provenance']:
            raise ValueError('added provenance does not record content identity: '
                             + key_hex)
        if entry['input_signature'] and entry['stage'] != 'vert':
            raise ValueError('input signature on a non-vertex stage')
        additions.append({
            'key': key_hex, 'stage_index': row['stage_index'],
            'shader_id': stage['shader_id'],
            'stage_sha256': stage['sha256'].hex(),
            'viewport_mask': row['viewport_mask'],
            'input_signature': row['signature'],
            'gxp_sha256': row['gxp_sha256'].hex(),
            'gxp_bytes': row['gxp_bytes'],
            'source_kind': entry.get('source_kind'),
            'environment_sha256': entry.get('environment_sha256'),
            'flags': entry.get('flags'),
            'cg_sha256': entry.get('cg_sha256'),
            'content_identity_sha256': entry.get('content_identity_sha256'),
            'crc32': row['crc32']})
    if sorted(spec.get('blocked', []), key=lambda row: row['stage_index']) != \
            sorted(json.loads((args.candidate / 'manifest.json').read_text(
                encoding='utf-8'))['blocked'], key=lambda row: row['stage_index']):
        raise ValueError('blocked list differs from spec')

    manifest = {
        'schema': 1,
        'verdict': 'structural-identity-pass',
        'base': {'path': str(args.base.resolve()),
                 'abi_version': base['abi'],
                 'expected_abi_version': args.base_abi,
                 'index_sha256': base['index_sha256'],
                 'counts': {'stages': len(base['stages']),
                            'variants': len(base['variants']),
                            'programs': len(base['programs'])}},
        'candidate': {'path': str(args.candidate.resolve()),
                      'abi_version': candidate['abi'],
                      'index_sha256': candidate['index_sha256'],
                      'index_json_sha256': candidate['index_json_sha256'],
                      'counts': {'stages': len(candidate['stages']),
                                 'variants': len(candidate['variants']),
                                 'programs': len(candidate['programs'])}},
        'retained': {'variants': retained_identical,
                     'gxp_bytes_identical': True,
                     'stage_identity_preserved': True,
                     'program_table_preserved': True},
        'added': {'variants': len(additions),
                  'generic': sum(1 for row in additions if row['input_signature'] == 0),
                  'input_specialized': sum(1 for row in additions
                                           if row['input_signature'] != 0),
                  'gxp_bytes': sum(row['gxp_bytes'] for row in additions),
                  'records': additions},
        'blocked': {'count': len(spec.get('blocked', [])),
                    'stages': spec.get('blocked', [])},
        'checks': {
            'base_keys_all_retained': True,
            'base_gxp_bytes_unchanged': True,
            'stage_sha_and_source_flags_unchanged': True,
            'program_tokens_stages_flags_unchanged': True,
            'abi2_signatures_bounded': True,
            'no_ambiguous_same_signature_overlap': True,
            'added_keys_bind_stage_gxp_environment_flags_signature': True,
            'added_keys_bind_content_identity_when_declared':
                all(row['content_identity_sha256'] is None or
                    row['key'] == added_key(
                        bytes.fromhex(row['stage_sha256']),
                        bytes.fromhex(row['gxp_sha256']),
                        row['input_signature'], row['environment_sha256'],
                        row['flags'],
                        content_identity_sha256=row['content_identity_sha256']
                    ).hex() for row in additions),
            'blocked_stages_not_substituted': True},
        'not_verified': ['GXM link', 'target draw', 'pixels', 'formal equivalence',
                         'hardware'],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n',
                           encoding='utf-8', newline='\n')
    print(json.dumps({'verdict': manifest['verdict'],
                      'retained': retained_identical,
                      'added': len(additions),
                      'blocked': len(spec.get('blocked', [])),
                      'index_sha256': candidate['index_sha256']}, indent=2))


if __name__ == '__main__':
    main()

"""Build the M6-B25 actual-nine shadow VS/FS GPU fixture bundle.

The bundle is a compact VPSHIX1 ABI 2 index containing only the nine pairs
selected by ``plan_shadow_pairs.py``, the exact candidate-bank variant bytes and
keys for their 18 stages, plus a contract header naming the new probe identity.
Nothing is copied from the historical P4 bundle: the index, stage indices,
program tokens, title and run identity are all rebuilt and independently
checked here.  The tool never writes game-derived bytes outside the ignored
bundle directory.

Every input identity is pinned.  A tampered pair plan, spec, candidate index,
sidecar audit, selected GXP or missing/extra token is rejected before an output
file is created.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

SCHEMA = 1
TITLE_ID = 'MHGUSP001'
RUN_ID_PREFIX = 'MHGUSP001-B26-20261027'
MAGIC = b'VPSHIX1\0'
HEADER_SIZE = 64
STAGE_RECORD = 48
VARIANT_RECORD = 40
PROGRAM_RECORD = 16
ABI_VERSION = 2
VIEWPORT_TRANSFORM = 1
VIEWPORT_MASK = 1 << VIEWPORT_TRANSFORM
STAGE_VERTEX = 1
STAGE_FRAGMENT = 5
STAGE_NONE = 0xffff
VARIANT_COMPILED = 1
PROGRAM_STATIC = 2

BANK_INDEX_SHA = '3b806e618bb900a5da119eaab4acb904420e89ddcc64b48ebc40531093ab3238'
BANK_INDEX_JSON_SHA = 'a095cf5fc2d2ec4196257ba8ceb80ba2c91af1e54e0d81bdef732f6e0f9d5638'
BANK_MANIFEST_SHA = 'cb7be4714777510a768bd2de360238574d6687ad0bf3d761dc03532edd90f73e'
BANK_COUNTS = {'stages': 2402, 'variants': 291, 'programs': 6311}
PAIR_PLAN_SHA = '83d62a85a3db20269d28edb5a8805cde4c356eda68675883787e0f5475511975'
SHADOW_SPEC_SHA = 'f4b40a96c0c12a36b54cd82bad66f8fe7dea9f7df1f607c1dc9eb59bf2e38a47'
SIDECAR_SHA = '113fb88c8c6f38743bc395c4f1f5b348344c9efa151709427b5357f3c3b7d2ca'
SIDECAR_MANIFEST_SHA = '43b4f181b184d62d986c8ea26468cef399019604155f6a9a382b7f6bd4596d13'
EXPECTED_TOKENS = (19, 653, 882, 1156, 1374, 1821, 3572, 4862, 4956)
EXPECTED_FRAGMENT_IDS = (
    'frag_03af1ac5efa36f74732f1263',
    'frag_0d45655f63c9a5922d06ea44',
    'frag_14905bcbbb89613d76a00208',
    'frag_64b44f9e813bdb8eb067eed5',
    'frag_aa4caac3ec191706ca19e7c0',
    'frag_b1d649c2889d6d8497bc7b50',
    'frag_cd5a92fb2987e1ab57fcb495',
    'frag_d5dd8db5ece6d7921a3d8efe',
    'frag_e71b9c5e3c7fad339fa1e1b1',
)


def sha256_bytes(data):
    return hashlib.sha256(data).digest()


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=False).encode()


def crc32(data):
    return zlib.crc32(data) & 0xffffffff


def fnv1a64(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001b3) & 0xffffffffffffffff
    return value


def valid_gxp(data):
    if len(data) < 32:
        return 'short'
    if data[:4] != b'GXP\0':
        return 'magic'
    declared = struct.unpack_from('<I', data, 8)[0]
    if declared < 32 or declared > len(data):
        return 'declared-size'
    return None


def expect(condition, message):
    if not condition:
        raise ValueError(message)


def check_sha(path, expected, label):
    actual = sha256_file(path)
    expect(actual == expected, '%s SHA mismatch: %s != %s'
           % (label, actual, expected))
    return actual


def load_inputs(args):
    """Read and identity-check every pinned offline input."""
    bank = Path(args.bank)
    index_path = bank / 'shader-index.bin'
    index_json_path = bank / 'shader-index.json'
    manifest_path = bank / 'manifest.json'
    sidecar_path = bank / 'shader-resources.bin'
    sidecar_manifest_path = bank / 'shader-resources.manifest.json'
    for path in (index_path, index_json_path, manifest_path, sidecar_path,
                 sidecar_manifest_path):
        expect(path.is_file(), 'missing input: ' + str(path))
    expected = {
        'bank_index_sha256': args.expected_bank_index_sha256,
        'bank_index_json_sha256': args.expected_bank_index_json_sha256,
        'bank_manifest_sha256': args.expected_bank_manifest_sha256,
        'pair_plan_sha256': args.expected_pair_plan_sha256,
        'shadow_spec_sha256': args.expected_shadow_spec_sha256,
        'sidecar_sha256': args.expected_sidecar_sha256,
        'sidecar_manifest_sha256': args.expected_sidecar_manifest_sha256,
    }
    check_sha(index_path, expected['bank_index_sha256'], 'candidate index')
    check_sha(index_json_path, expected['bank_index_json_sha256'],
              'candidate index JSON')
    check_sha(manifest_path, expected['bank_manifest_sha256'],
              'candidate manifest')
    check_sha(sidecar_path, expected['sidecar_sha256'], 'sidecar')
    check_sha(sidecar_manifest_path, expected['sidecar_manifest_sha256'],
              'sidecar manifest')
    check_sha(args.pairs, expected['pair_plan_sha256'], 'pair plan')
    check_sha(args.shadow_spec, expected['shadow_spec_sha256'], 'shadow spec')
    return {
        'bank': bank,
        'index': read_json(index_json_path),
        'sidecar_manifest': read_json(sidecar_manifest_path),
        'pairs': read_json(args.pairs),
        'spec': read_json(args.shadow_spec),
        'expected': expected,
        'run_id_prefix': args.run_id_prefix,
    }


def build_contract(inputs):
    index = inputs['index']
    pairs_doc = inputs['pairs']
    spec = inputs['spec']
    sidecar = inputs['sidecar_manifest']

    expect(index.get('abi_version') == ABI_VERSION, 'candidate index ABI')
    counts = {
        'stages': len(index['stages']),
        'variants': len(index['variants']),
        'programs': len(index['programs']),
    }
    expect(counts == BANK_COUNTS, 'candidate counts %r' % (counts,))
    selected = pairs_doc.get('selected_plan')
    expect(isinstance(selected, list) and len(selected) == 9,
           'pair plan must select exactly nine pairs')
    tokens = [row['token'] for row in selected]
    expect(len(set(tokens)) == 9, 'duplicate token in pair plan')
    expect(sorted(tokens) == list(EXPECTED_TOKENS),
           'unexpected selected token set')
    expect({row['fragment_shader_id'] for row in selected}
           == set(EXPECTED_FRAGMENT_IDS), 'selected fragment set differs')
    expect(len({row['vertex_stage_index'] for row in selected}) == 9,
           'selected vertex stages are not distinct')
    for row in selected:
        expect(row.get('vertex_stage_available') is True,
               'pair lacks its real vertex stage')
        expect(row['program_flags'] == PROGRAM_STATIC,
               'pair program is not static')
        expect(row['fragment_stage_index']
               == index['stages'][row['fragment_stage_index']]['index'],
               'fragment stage index mismatch')
        expect(row['vertex_stage_index']
               == index['stages'][row['vertex_stage_index']]['index'],
               'vertex stage index mismatch')
        frag = index['stages'][row['fragment_stage_index']]
        vert = index['stages'][row['vertex_stage_index']]
        expect(frag['shader_id'] == row['fragment_shader_id'],
               'fragment shader id mismatch')
        expect(vert['shader_id'] == row['vertex_shader_id'],
               'vertex shader id mismatch')
        expect(frag['stage_sha256'] == row['fragment_stage_sha256'],
               'fragment stage SHA mismatch')
        expect(vert['stage_sha256'] == row['vertex_stage_sha256'],
               'vertex stage SHA mismatch')
        expect(frag['stage'] == 'frag' and vert['stage'] == 'vert',
               'pair stage kind mismatch')

    spec_add = spec.get('add')
    expect(isinstance(spec_add, list) and len(spec_add) == 9,
           'shadow spec must add exactly nine stages')
    spec_by_id = {row['shader_id']: row for row in spec_add}
    expect(len(spec_by_id) == 9, 'duplicate shader id in shadow spec')
    expect(set(spec_by_id) == set(EXPECTED_FRAGMENT_IDS),
           'shadow spec fragment set differs')
    for row in selected:
        add = spec_by_id[row['fragment_shader_id']]
        expect(add['stage_index'] == row['fragment_stage_index'],
               'shadow spec stage index mismatch')
        expect(add['stage_sha256'] == row['fragment_stage_sha256'],
               'shadow spec stage SHA mismatch')

    sidecar_stages = {row['shader_id']: row
                      for row in sidecar['shadow_usage_audit']['stages']}
    expect(len(sidecar_stages) == 9, 'sidecar shadow audit count')
    expect(set(sidecar_stages) == set(EXPECTED_FRAGMENT_IDS),
           'sidecar shadow audit set differs')

    plan = []
    for row in sorted(selected, key=lambda item: item['token']):
        frag = index['stages'][row['fragment_stage_index']]
        vert = index['stages'][row['vertex_stage_index']]
        audit = sidecar_stages[row['fragment_shader_id']]
        expect(audit['stage_sha256'] == frag['stage_sha256'],
               'sidecar stage SHA mismatch')
        expect(audit['stage_index'] == frag['index'],
               'sidecar stage index mismatch')
        shadows = [entry for entry in audit['textures']
                   if entry.get('depth') == 1]
        expect(len(shadows) == 1,
               'exactly one descriptor depth=1 slot is required')
        shadow = shadows[0]
        expect(shadow.get('usage') == 1 and shadow.get('usage_analyzed') is True,
               'shadow slot must be an analyzed exact .r read')
        expect('tex%u' % shadow['slot'] in audit['samplers'],
               'shadow slot absent from reflected sampler declarations')
        for entry in audit['textures']:
            if entry is shadow:
                continue
            expect(entry.get('depth') == 0,
                   'ordinary sampler marked as depth')
        variant = next((item for item in index['variants']
                        if item['stage_index'] == frag['index']), None)
        expect(variant is not None, 'fragment stage has no variant')
        expect(variant['key'] == spec_by_id[frag['shader_id']]
               ['expected_variant_key'],
               'fragment variant key differs from the shadow spec')
        expect(variant['viewport_mask'] == VIEWPORT_MASK,
               'fragment viewport mask')
        expect(variant.get('input_signature', 0) == 0,
               'fragment input signature must be generic')
        plan.append({
            'token': row['token'],
            'vertex_stage_index': vert['index'],
            'fragment_stage_index': frag['index'],
            'vertex_shader_id': vert['shader_id'],
            'fragment_shader_id': frag['shader_id'],
            'vertex_stage_sha256': vert['stage_sha256'],
            'fragment_stage_sha256': frag['stage_sha256'],
            'shadow_slot': shadow['slot'],
            'descriptor_depth': 1,
            'shadow_usage': shadow['usage'],
            'color_slots': sorted(entry['slot'] for entry in audit['textures']
                                  if entry.get('depth') == 0),
            'variant_key': variant['key'],
            'gxp_bytes': variant['gxp_bytes'],
            'gxp_crc32': variant['gxp_crc32'],
            'gxp_sha256': variant['gxp_sha256'],
        })

    expect(len({row['gxp_sha256'] for row in plan}) > 0, 'empty GXP identity')
    source_stage_indices = sorted(
        {row['vertex_stage_index'] for row in plan}
        | {row['fragment_stage_index'] for row in plan})
    expect(len(source_stage_indices) == 18, 'fixture must reference 18 stages')
    return plan, source_stage_indices, index, spec


def build_index(plan, source_stage_indices, index, overrides=None):
    overrides = overrides or {}
    remap = {source: position
             for position, source in enumerate(source_stage_indices)}
    variants_by_stage = {item['stage_index']: item for item in index['variants']}
    stages = []
    variants = []
    payloads = {}
    expected_reuse = {}
    for source in source_stage_indices:
        stage = index['stages'][source]
        variant = variants_by_stage.get(source)
        expect(variant is not None, 'stage has no variant: %s'
               % stage['shader_id'])
        expect(variant['viewport_mask'] == VIEWPORT_MASK,
               'source variant viewport mask')
        expect(variant.get('input_signature', 0) == 0,
               'source variant input signature')
        override = overrides.get(stage['shader_id'])
        if override is not None:
            key = override['key']
            gxp_bytes = override['gxp_bytes']
            gxp_crc = override['gxp_crc32']
            gxp_sha = override['gxp_sha256']
            gxp_path = Path(override['gxp_path'])
        else:
            key = variant['key']
            gxp_bytes = variant['gxp_bytes']
            gxp_crc = variant['gxp_crc32']
            gxp_sha = variant['gxp_sha256']
            gxp_path = None
        stages.append({
            'stage': STAGE_VERTEX if stage['stage'] == 'vert'
                     else STAGE_FRAGMENT,
            'sha256': stage['stage_sha256'],
            'variant_start': len(variants),
            'variant_count': 1,
            'source_flags': stage['source_flags'],
        })
        variants.append({
            'stage_index': remap[source],
            'viewport_mask': variant['viewport_mask'],
            'key': key,
            'gxp_bytes': gxp_bytes,
            'flags': VARIANT_COMPILED,
            'gxp_crc32': gxp_crc,
            'input_signature': variant.get('input_signature', 0),
        })
        expected_reuse[key] = {
            'source_stage_index': source,
            'shader_id': stage['shader_id'],
            'gxp_bytes': gxp_bytes,
            'gxp_crc32': gxp_crc,
            'gxp_sha256': gxp_sha,
            'gxp_path': gxp_path,
        }
    programs = []
    for row in plan:
        programs.append({
            'token': row['token'],
            'vertex_stage': remap[row['vertex_stage_index']],
            'fragment_stage': remap[row['fragment_stage_index']],
            'flags': PROGRAM_STATIC,
        })
    return stages, variants, programs, remap, payloads, expected_reuse


def serialize_index(stages, variants, programs):
    body = bytearray()
    for stage in stages:
        body += struct.pack('<I', stage['stage'])
        body += bytes.fromhex(stage['sha256'])
        body += struct.pack('<HHI', stage['variant_start'],
                            stage['variant_count'], stage['source_flags'])
        body += struct.pack('<I', 0)
    for variant in variants:
        body += struct.pack('<IHH', variant['stage_index'],
                            variant['viewport_mask'], ABI_VERSION)
        body += bytes.fromhex(variant['key'])
        body += struct.pack('<II', variant['gxp_bytes'],
                            VARIANT_COMPILED)
        body += struct.pack('<II', variant['gxp_crc32'],
                            variant['input_signature'])
    for program in programs:
        body += struct.pack('<IHHI', program['token'],
                            program['vertex_stage'], program['fragment_stage'],
                            program['flags'])
        body += struct.pack('<I', 0)
    header = bytearray(HEADER_SIZE)
    header[0:8] = MAGIC
    struct.pack_into('<HH', header, 8, 1, HEADER_SIZE)
    struct.pack_into('<I', header, 12, 1)
    struct.pack_into('<IIIIII', header, 16, len(stages), len(variants),
                     len(programs), STAGE_RECORD, VARIANT_RECORD,
                     PROGRAM_RECORD)
    struct.pack_into('<II', header, 40, len(body), crc32(body))
    struct.pack_into('<I', header, 48, ABI_VERSION)
    return bytes(header) + bytes(body)


def parse_index(data):
    """Strict ABI 2 reader for the compact fixture; mirrors the C reader."""
    expect(len(data) >= HEADER_SIZE, 'index shorter than header')
    header = data[:HEADER_SIZE]
    expect(header[:8] == MAGIC, 'index magic mismatch')
    version, header_size = struct.unpack_from('<HH', header, 8)
    flags, = struct.unpack_from('<I', header, 12)
    stage_count, variant_count, program_count, srec, vrec, prec = \
        struct.unpack_from('<IIIIII', header, 16)
    body_size, body_crc = struct.unpack_from('<II', header, 40)
    abi, = struct.unpack_from('<I', header, 48)
    expect(version == 1 and header_size == HEADER_SIZE and flags == 1,
           'unsupported index header')
    expect((srec, vrec, prec) == (STAGE_RECORD, VARIANT_RECORD,
                                  PROGRAM_RECORD), 'record sizes')
    expect(abi == ABI_VERSION, 'fixture ABI')
    body = data[HEADER_SIZE:]
    expect(len(body) == body_size and crc32(body) == body_crc,
           'index body length/CRC')
    expect(len(body) >= stage_count * srec + variant_count * vrec
           + program_count * prec, 'index body too short')
    stages = []
    running = 0
    seen_sha = set()
    for position in range(stage_count):
        off = position * srec
        stage_id, = struct.unpack_from('<I', body, off)
        stage_sha = body[off + 4:off + 36]
        start, count, sources = struct.unpack_from('<HHI', body, off + 36)
        expect(stage_id in (STAGE_VERTEX, STAGE_FRAGMENT), 'stage kind')
        expect(sources & ~7 == 0, 'stage sources')
        expect(start == running and start + count <= variant_count,
               'stage variant range')
        expect(stage_sha not in seen_sha, 'duplicate stage content')
        seen_sha.add(stage_sha)
        stages.append({'stage': stage_id, 'sha256': stage_sha.hex(),
                       'variant_start': start, 'variant_count': count,
                       'source_flags': sources})
        running += count
    expect(running == variant_count, 'variant table not fully owned')
    variants = []
    keys = set()
    vbase = stage_count * srec
    for position in range(variant_count):
        off = vbase + position * vrec
        stage_index, mask, vabi = struct.unpack_from('<IHH', body, off)
        key = body[off + 8:off + 24]
        gxp_bytes, vflags, gxp_crc, signature = struct.unpack_from(
            '<IIII', body, off + 24)
        expect(stage_index < stage_count, 'variant stage range')
        expect(vabi == ABI_VERSION, 'variant ABI')
        expect(vflags == VARIANT_COMPILED, 'variant flags')
        expect(mask == VIEWPORT_MASK, 'variant viewport')
        expect(signature == 0, 'fixture variant input signature')
        expect(32 <= gxp_bytes <= 16 * 1024 * 1024, 'variant GXP size')
        expect(key not in keys, 'duplicate variant key')
        keys.add(key)
        variants.append({'stage_index': stage_index, 'viewport_mask': mask,
                         'abi_version': vabi, 'key': key.hex(),
                         'gxp_bytes': gxp_bytes, 'flags': vflags,
                         'gxp_crc32': gxp_crc,
                         'input_signature': signature})
    for position, stage in enumerate(stages):
        masks = 0
        for index in range(stage['variant_start'],
                           stage['variant_start'] + stage['variant_count']):
            variant = variants[index]
            expect(variant['stage_index'] == position, 'variant stage range')
            expect(masks & variant['viewport_mask'] == 0,
                   'overlapping viewport variants')
            masks |= variant['viewport_mask']
    programs = []
    pbase = vbase + variant_count * vrec
    previous = 0
    for position in range(program_count):
        off = pbase + position * prec
        token, vs, fs, pflags = struct.unpack_from('<IHHI', body, off)
        expect(position == 0 or token > previous, 'program token order')
        previous = token
        expect(pflags == PROGRAM_STATIC, 'program flags')
        expect(vs != STAGE_NONE and fs != STAGE_NONE, 'program lacks a stage')
        expect(vs < stage_count and stages[vs]['stage'] == STAGE_VERTEX,
               'program vertex stage')
        expect(fs < stage_count and stages[fs]['stage'] == STAGE_FRAGMENT,
               'program fragment stage')
        programs.append({'token': token, 'vertex_stage': vs,
                         'fragment_stage': fs, 'flags': pflags})
    return {'stages': stages, 'variants': variants, 'programs': programs,
            'abi_version': abi}


def identity_inputs(probe_source, index_data):
    """Bind every executable input that can change probe behaviour.

    The historical P4 run id was index-only; a changed reader or probe source
    could then resume a log written by a different fixture.  The B25 identity
    includes the probe, reader/planner sources, CMake target definition and
    generator as well as the exact index bytes.
    """
    files = {
        'probe_source': Path(probe_source),
        'static_shader_index': ROOT / 'src/platform/static_shader_index.c',
        'gxm_depth_view': ROOT / 'src/platform/gxm_depth_view.c',
        'gxm_depth_view_native': ROOT / 'src/platform/gxm_depth_view_native.c',
        'gxm_shadow_sampler': ROOT / 'src/platform/gxm_shadow_sampler.c',
        'cmake': ROOT / 'CMakeLists.txt',
        'generator': Path(__file__),
    }
    identity = {'index_sha256': hashlib.sha256(index_data).hexdigest(),
                'files': {}}
    for name, path in files.items():
        expect(path.is_file(), 'missing identity file: ' + str(path))
        identity['files'][name] = sha256_file(path)
    identity['sha256'] = hashlib.sha256(canonical(identity)).hexdigest()
    return identity


def contract_header(plan, remap, index_data, probe_source, identity,
                    run_id_prefix):
    fnv = fnv1a64(index_data)
    index_sha = hashlib.sha256(index_data).hexdigest()
    run_id = '%s-%s' % (run_id_prefix, identity['sha256'][:16])
    rows = []
    for row in plan:
        color_mask = 0
        for slot in row['color_slots']:
            color_mask |= 1 << slot
        rows.append(
            '    {%uu, %uu, %uu, %uu, 0x%04xu, "%s", "%s", "%s", "%s"},'
            % (row['token'], remap[row['vertex_stage_index']],
               remap[row['fragment_stage_index']], row['shadow_slot'],
               color_mask, row['vertex_stage_sha256'],
               row['fragment_stage_sha256'],
               row['gxp_sha256'], row['variant_key']))
    lines = [
        '#ifndef VP_SHADOW_PAIR_CONTRACT_H',
        '#define VP_SHADOW_PAIR_CONTRACT_H',
        '',
        '#define VP_SHADOW_PAIR_TITLE "%s"' % TITLE_ID,
        '#define VP_SHADOW_PAIR_RUN_ID "%s"' % run_id,
        '#define VP_SHADOW_PAIR_IDENTITY_SHA256 "%s"' % identity['sha256'],
        '#define VP_SHADOW_PAIR_INDEX_SHA256 "%s"' % index_sha,
        '#define VP_SHADOW_PAIR_INDEX_FNV 0x%016xull' % fnv,
        '#define VP_SHADOW_PAIR_INDEX_BYTES %uu' % len(index_data),
        '#define VP_SHADOW_PAIR_PROGRAM_COUNT %uu' % len(plan),
        '#define VP_SHADOW_PAIR_STAGE_COUNT %uu' % (len(plan) * 2),
        '#define VP_SHADOW_PAIR_VARIANT_COUNT %uu' % (len(plan) * 2),
        '#define VP_SHADOW_PAIR_ABI_VERSION 2u',
        '#define VP_SHADOW_PAIR_VIEWPORT 1u',
        '#define VP_SHADOW_PAIR_SOURCE_SHA256 "%s"' % sha256_file(probe_source),
        '',
        'typedef struct {',
        '    unsigned token;',
        '    unsigned vertex_stage;',
        '    unsigned fragment_stage;',
        '    unsigned shadow_slot;',
        '    unsigned color_mask;',
        '    const char *vertex_stage_sha256;',
        '    const char *fragment_stage_sha256;',
        '    const char *fragment_gxp_sha256;',
        '    const char *fragment_variant_key;',
        '} VPShadowPairContract;',
        '',
        'static const VPShadowPairContract vp_shadow_pairs['
        'VP_SHADOW_PAIR_PROGRAM_COUNT] = {',
    ]
    lines.extend(rows)
    lines.extend(['};', '', '#endif', ''])
    return '\n'.join(lines), run_id, index_sha, fnv


def testable_variant_key(stage_sha, gxp_sha, environment_sha, flags,
                         content_identity_sha256):
    return hashlib.sha256(canonical({
        'schema': 2,
        'stage_sha256': stage_sha,
        'gxp_sha256': gxp_sha,
        'abi_version': ABI_VERSION,
        'input_signature': 0,
        'environment_sha256': environment_sha,
        'flags': flags,
        'content_identity_sha256': content_identity_sha256,
    })).digest()[:16].hex()


def diagnostic_selection(plan, spec_add, args):
    """Return one plan row with a receipt-bound O1 fragment override."""
    if not args.diagnostic_token:
        return plan, None
    chosen = [row for row in plan if row['token'] == args.diagnostic_token]
    expect(len(chosen) == 1, 'diagnostic token is not in the selected plan')
    row = dict(chosen[0])
    if not args.diagnostic_gxp and not args.diagnostic_receipt:
        return [row], {'kind': 'bank-o0', 'token': row['token']}
    expect(args.diagnostic_gxp and args.diagnostic_receipt,
           'diagnostic selection needs --diagnostic-gxp and --diagnostic-receipt')
    spec_entry = next(item for item in spec_add
                      if item['shader_id'] == row['fragment_shader_id'])
    gxp_path = Path(args.diagnostic_gxp)
    receipt_path = Path(args.diagnostic_receipt)
    expect(gxp_path.is_file(), 'diagnostic GXP missing')
    expect(receipt_path.is_file(), 'diagnostic receipt missing')
    gxp = gxp_path.read_bytes()
    expect(valid_gxp(gxp) is None, 'diagnostic GXP is malformed')
    receipt = read_json(receipt_path)
    expected_toolchain = {
        'shacc_sha256': '8f13b52af31dfc0bb2d2658044376fccf5ee1df6b438008905a4441ffe232b5b',
        'service_vpk_sha256': '568284169d694b6a9c44dc51be0ec905c372e0ccf51a2e0e10b478b2e1d7fe52',
        'libshacccg_sha256': '188adb8c34c3983d63244c32d981fd7b518a8dcfd200f5886233fface6b5fa40',
        'config_sha256': '796bcd58fdedca33c1af213fa54d0fd70c5166a546ce89c116660829ad528457',
    }
    expect(receipt.get('shader_id') == row['fragment_shader_id'],
           'receipt shader id differs from the selected pair')
    expect(receipt.get('stage_sha256') == row['fragment_stage_sha256'],
           'receipt stage SHA differs')
    expect(receipt.get('source_sha256') == spec_entry['cg_sha256'],
           'receipt Cg SHA differs from the B24 emitted source')
    expect(receipt.get('optimization') == '-O1'
           and receipt.get('flags') == ['-O1'],
           'diagnostic receipt is not an -O1 result')
    expect(receipt.get('profile') == 'sce_fp_psp2', 'receipt profile')
    expect(receipt.get('strategy') == spec_entry['strategy'], 'receipt strategy')
    expect(receipt.get('policy_sha256') == spec_entry['policy_sha256'],
           'receipt policy')
    expect(receipt.get('emitter_sha256') == spec_entry['emitter_sha256'],
           'receipt emitter')
    expect(receipt.get('helper_sha256') == spec_entry['helper_sha256'],
           'receipt helper')
    expect(receipt.get('toolchain') == expected_toolchain, 'receipt toolchain')
    expect(receipt.get('service_timeout_seconds') == 300
           and receipt.get('outer_timeout_seconds') == 330,
           'receipt timeout budget')
    expect(receipt.get('classification') == 'compiled'
           and receipt.get('compiled') is True, 'receipt did not compile')
    expect(receipt.get('gxp_bytes') == len(gxp)
           and receipt.get('gxp_sha256') == hashlib.sha256(gxp).hexdigest(),
           'receipt GXP identity differs from the supplied file')
    content_identity = dict(spec_entry['content_identity'])
    content_identity.update({
        'cg_bytes': spec_entry['cg_bytes'],
        'cg_sha256': spec_entry['cg_sha256'],
        'flags': ['-O1'],
        'gxp_bytes': len(gxp),
        'gxp_sha256': hashlib.sha256(gxp).hexdigest(),
        'service_timeout_seconds': 300,
        'outer_timeout_seconds': 330,
        'toolchain': receipt['toolchain'],
    })
    content_identity_sha = hashlib.sha256(canonical(content_identity)).hexdigest()
    key = testable_variant_key(row['fragment_stage_sha256'],
                               content_identity['gxp_sha256'],
                               spec_entry['environment_sha256'], ['-O1'],
                               content_identity_sha)
    row.update({
        'key_override': key,
        'variant_key': key,
        'gxp_override_path': gxp_path,
        'gxp_bytes': len(gxp),
        'gxp_crc32': crc32(gxp),
        'gxp_sha256': content_identity['gxp_sha256'],
        'flags_override': ['-O1'],
        'diagnostic_receipt': {
            'path': str(receipt_path),
            'sha256': sha256_file(receipt_path),
            'classification': receipt['classification'],
            'wall_seconds': receipt.get('wall_seconds'),
            'content_identity': content_identity,
            'content_identity_sha256': content_identity_sha,
            'variant_key': key,
        },
    })
    return [row], row['diagnostic_receipt']


def build(args):
    inputs = load_inputs(args)
    plan, source_stage_indices, index, spec = build_contract(inputs)
    plan = sorted(plan, key=lambda item: item['token'])
    plan, diagnostic = diagnostic_selection(plan, spec['add'], args)
    source_stage_indices = sorted(
        {row['vertex_stage_index'] for row in plan}
        | {row['fragment_stage_index'] for row in plan})
    overrides = {}
    if diagnostic is not None and 'key_override' in plan[0]:
        row = plan[0]
        overrides[row['fragment_shader_id']] = {
            'key': row['key_override'],
            'gxp_path': row['gxp_override_path'],
            'gxp_bytes': row['gxp_bytes'],
            'gxp_crc32': row['gxp_crc32'],
            'gxp_sha256': row['gxp_sha256'],
        }
    stages, variants, programs, remap, _payloads, reuse = build_index(
        plan, source_stage_indices, index, overrides)
    expected_stages = 2 if diagnostic is not None else 18
    expected_programs = 1 if diagnostic is not None else 9
    index_data = serialize_index(stages, variants, programs)
    parsed = parse_index(index_data)
    expect((len(parsed['stages']), len(parsed['variants']),
            len(parsed['programs'])) == (expected_stages, expected_stages,
                                         expected_programs),
           'fixture counts')
    expect([row['token'] for row in parsed['programs']]
           == [row['token'] for row in plan], 'fixture token order')
    for row in plan:
        fixture_index = remap[row['fragment_stage_index']]
        fixture_variant = parsed['variants'][fixture_index]
        expect(fixture_variant['key'] == row['variant_key'],
               'contract variant key differs from the serialized index')
        expect(fixture_variant['gxp_bytes'] == row['gxp_bytes']
               and fixture_variant['gxp_crc32'] == row['gxp_crc32'],
               'contract GXP bytes/CRC differs from the serialized index')

    bank = Path(args.bank)
    gxp_payloads = {}
    for key, expected in reuse.items():
        path = expected.get('gxp_path') or (bank / 'shaders' / (key + '.gxp'))
        expect(path.is_file(), 'missing GXP: ' + str(path))
        data = path.read_bytes()
        error = valid_gxp(data)
        expect(error is None, 'invalid GXP %s: %s' % (path, error))
        expect(len(data) == expected['gxp_bytes'], 'GXP byte count mismatch')
        expect(crc32(data) == expected['gxp_crc32'], 'GXP CRC mismatch')
        expect(hashlib.sha256(data).hexdigest() == expected['gxp_sha256'],
               'GXP SHA mismatch')
        gxp_payloads[key] = data
    expect(len(gxp_payloads) == expected_stages,
           'fixture must reference %u GXP payloads' % expected_stages)

    probe_source = Path(args.probe_source)
    expect(probe_source.is_file(), 'missing probe source: ' + str(probe_source))
    identity = identity_inputs(probe_source, index_data)
    header, run_id, index_sha, fnv = contract_header(
        plan, remap, index_data, probe_source, identity,
        inputs['run_id_prefix'])
    output = Path(args.output)
    expect(not output.exists(), 'output must not exist')
    tmp = output.with_name(output.name + '.tmp-%d' % os.getpid())
    if tmp.exists():
        shutil.rmtree(tmp)
    (tmp / 'shaders').mkdir(parents=True)
    (tmp / 'shader-index.bin').write_bytes(index_data)
    for key, data in gxp_payloads.items():
        (tmp / 'shaders' / (key + '.gxp')).write_bytes(data)
    control = {}
    for kind, path in (('v', Path(args.control_v)), ('f', Path(args.control_f))):
        data = path.read_bytes()
        error = valid_gxp(data)
        expect(error is None, 'invalid control GXP %s: %s' % (path, error))
        (tmp / ('control_%s.gxp' % kind)).write_bytes(data)
        control[kind] = {'sha256': hashlib.sha256(data).hexdigest(),
                         'bytes': len(data)}
    (tmp / 'shadow_pair_contract.h').write_text(header, encoding='utf-8',
                                                newline='\n')

    manifest = {
        'schema': SCHEMA,
        'scope': ('one-pair diagnostic fixture; not the nine-pair matrix and '
                  'not an installed package') if diagnostic else
                 ('actual-nine shadow VS/FS target fixture; not an installed '
                  'package'),
        'diagnostic': diagnostic,
        'title_id': TITLE_ID,
        'run_id': run_id,
        'probe_identity': identity,
        'probe_source': {'path': str(probe_source),
                         'sha256': sha256_file(probe_source)},
        'tool_sha256': sha256_file(Path(__file__)),
        'index': {'sha256': index_sha, 'bytes': len(index_data),
                  'fnv1a64': '%016x' % fnv, 'abi_version': ABI_VERSION,
                  'counts': {'stages': len(stages), 'variants': len(variants),
                             'programs': len(programs)}},
        'bound_tokens': [row['token'] for row in plan],
        'source_variant_reuse': [
            {'fixture_stage_index': remap[key['source_stage_index']],
             'source_stage_index': key['source_stage_index'],
             'shader_id': key['shader_id'],
             'variant_key': variant_key,
             'gxp_bytes': key['gxp_bytes'],
             'gxp_crc32': key['gxp_crc32'],
             'gxp_sha256': key['gxp_sha256']}
            for variant_key, key in sorted(reuse.items())
        ],
        'pairs': [{
            'token': row['token'],
            'vertex_stage': remap[row['vertex_stage_index']],
            'fragment_stage': remap[row['fragment_stage_index']],
            'source_vertex_stage_index': row['vertex_stage_index'],
            'source_fragment_stage_index': row['fragment_stage_index'],
            'vertex_shader_id': row['vertex_shader_id'],
            'fragment_shader_id': row['fragment_shader_id'],
            'shadow_slot': row['shadow_slot'],
            'color_slots': row['color_slots'],
            'descriptor_depth': 1,
            'shadow_usage': row['shadow_usage'],
            'variant_key': row['variant_key'],
            'gxp_bytes': row['gxp_bytes'],
            'gxp_crc32': row['gxp_crc32'],
            'gxp_sha256': row['gxp_sha256'],
        } for row in plan],
        'control': control,
        'inputs': dict(inputs['expected']),
    }
    (tmp / 'shadow-pair-manifest.json').write_text(
        json.dumps(manifest, indent=1, sort_keys=True) + '\n',
        encoding='utf-8', newline='\n')
    tmp.rename(output)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=Path,
                        default=ROOT / 'artifacts/m6b24-shadow-bank-20261027/candidate-package')
    parser.add_argument('--pairs', type=Path,
                        default=ROOT / 'config/validation/m6b24-shadow-pairs-20261027.json')
    parser.add_argument('--shadow-spec', type=Path,
                        default=ROOT / 'artifacts/m6b24-shadow-bank-20261027/shadow-candidate-spec.json')
    parser.add_argument('--expected-bank-index-sha256', default=BANK_INDEX_SHA)
    parser.add_argument('--expected-bank-index-json-sha256',
                        default=BANK_INDEX_JSON_SHA)
    parser.add_argument('--expected-bank-manifest-sha256',
                        default=BANK_MANIFEST_SHA)
    parser.add_argument('--expected-pair-plan-sha256', default=PAIR_PLAN_SHA)
    parser.add_argument('--expected-shadow-spec-sha256',
                        default=SHADOW_SPEC_SHA)
    parser.add_argument('--expected-sidecar-sha256', default=SIDECAR_SHA)
    parser.add_argument('--expected-sidecar-manifest-sha256',
                        default=SIDECAR_MANIFEST_SHA)
    parser.add_argument('--run-id-prefix', default=RUN_ID_PREFIX)
    parser.add_argument('--probe-source', type=Path,
                        default=ROOT / 'src/probes/gxm_shadow_pair_probe.c')
    parser.add_argument('--control-v', type=Path,
                        default=ROOT / 'artifacts/p2-rebuild-20261025/pairs/control_v.gxp')
    parser.add_argument('--control-f', type=Path,
                        default=ROOT / 'artifacts/p2-rebuild-20261025/pairs/control_f.gxp')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--diagnostic-token', type=int, default=None,
                        help='build a receipt-bound one-pair diagnostic fixture')
    parser.add_argument('--diagnostic-gxp', type=Path, default=None,
                        help='exact O1 GXP for the diagnostic fragment stage')
    parser.add_argument('--diagnostic-receipt', type=Path, default=None,
                        help='receipt that binds the diagnostic GXP')
    parser.add_argument('--summary', type=Path,
                        help='compact machine summary (no shader bytes)')
    args = parser.parse_args()
    manifest = build(args)
    summary = {
        'schema': 1,
        'title_id': manifest['title_id'],
        'run_id': manifest['run_id'],
        'index_sha256': manifest['index']['sha256'],
        'index_bytes': manifest['index']['bytes'],
        'index_fnv1a64': manifest['index']['fnv1a64'],
        'counts': manifest['index']['counts'],
        'bound_tokens': manifest['bound_tokens'],
        'pairs': manifest['pairs'],
        'tool_sha256': manifest['tool_sha256'],
        'probe_source_sha256': manifest['probe_source']['sha256'],
    }
    print(json.dumps(summary['counts'], indent=1))
    print('index_sha256=' + summary['index_sha256'])
    print('run_id=' + summary['run_id'])
    if args.summary:
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(
            json.dumps(summary, indent=1, sort_keys=True) + '\n',
            encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()

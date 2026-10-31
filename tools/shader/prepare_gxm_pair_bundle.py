"""Build the S2/P4 observed-pair GXM test bundle.

The bundle is a small VPSHIX1 index in the same format the Vita runtime reads
plus one GXP per referenced stage.  Programs are the 21 older observed pairs
the B runtime already exercised first, followed by every PMF-observed pair
whose both stages have an A3 GXP.  The index is validated here before any
target run; the tool never writes game-derived bytes outside the ignored
bundle directory.

Only identities and counts are copied to the caller's summary.  The pair
manifest stays in the ignored bundle directory because it names observed
shaders but contains no code bytes.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
import zlib
from pathlib import Path

SCHEMA = 1
MAGIC = b'VPSHIX1\0'
HEADER_SIZE = 64
STAGE_RECORD = 48
VARIANT_RECORD = 40
PROGRAM_RECORD = 16
ABI_VERSION = 1
STAGE_VERTEX = 1
STAGE_FRAGMENT = 5
STAGE_NONE = 0xffff
SOURCE_SPK = 1
SOURCE_OBSERVED = 4
PROGRAM_OBSERVED = 1
VARIANT_COMPILED = 1
VIEWPORT_TRANSFORM = 1
VIEWPORT_MASK = 1 << VIEWPORT_TRANSFORM

ROOT = Path(__file__).resolve().parents[2]


def sha256_bytes(data):
    return hashlib.sha256(data).digest()


def sha256_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def read_jsonl(path):
    with path.open('r', encoding='utf-8-sig') as stream:
        return [json.loads(line) for line in stream if line.strip()]


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=False).encode()


def variant_key(stage_sha, gxp_sha, abi=ABI_VERSION):
    """Same derivation as pack_static_shader_package.py."""
    return hashlib.sha256(canonical({
        'schema': 1, 'stage_sha256': stage_sha.hex(),
        'gxp_sha256': gxp_sha.hex(), 'abi_version': abi})).digest()[:16]


def crc32(data):
    return zlib.crc32(data) & 0xffffffff


def valid_gxp(data):
    if len(data) < 32:
        return 'short'
    if data[:4] != b'GXP\0':
        return 'magic'
    declared = struct.unpack_from('<I', data, 8)[0]
    if declared < 32 or declared > len(data):
        return 'declared-size'
    return None


def align4(value):
    return (value + 3) & ~3


def parse_index(data):
    """Mirror the checks in src/platform/static_shader_index.c.

    Returns a dict with the parsed tables.  Raises ValueError on any structural
    mismatch; used as the generator's own gate before writing a bundle.
    """
    if len(data) < HEADER_SIZE:
        raise ValueError('index shorter than header')
    header = data[:HEADER_SIZE]
    if header[:8] != MAGIC:
        raise ValueError('index magic mismatch')
    version, header_size = struct.unpack_from('<HH', header, 8)
    flags, = struct.unpack_from('<I', header, 12)
    stage_count, variant_count, program_count, srec, vrec, prec = \
        struct.unpack_from('<IIIIII', header, 16)
    body_size, body_crc = struct.unpack_from('<II', header, 40)
    abi, = struct.unpack_from('<I', header, 48)
    if version != 1 or header_size != HEADER_SIZE or flags != 1:
        raise ValueError('unsupported index header')
    if (srec, vrec, prec) != (STAGE_RECORD, VARIANT_RECORD, PROGRAM_RECORD):
        raise ValueError('unexpected record sizes')
    if abi != ABI_VERSION:
        raise ValueError('unexpected ABI version')
    body = data[HEADER_SIZE:]
    if len(body) != body_size or crc32(body) != body_crc:
        raise ValueError('index body length/CRC mismatch')
    if len(body) < stage_count * srec + variant_count * vrec + program_count * prec:
        raise ValueError('index body too short')
    stages = []
    running = 0
    seen_sha = set()
    for i in range(stage_count):
        off = i * srec
        stage_id, = struct.unpack_from('<I', body, off)
        stage_sha = body[off + 4:off + 36]
        start, count, sources = struct.unpack_from('<HHI', body, off + 36)
        if stage_id not in (STAGE_VERTEX, STAGE_FRAGMENT):
            raise ValueError('invalid stage type')
        if sources & ~7:
            raise ValueError('invalid stage source flags')
        if start != running or start + count > variant_count:
            raise ValueError('stage variant range invalid')
        if stage_sha in seen_sha:
            raise ValueError('duplicate stage content')
        seen_sha.add(stage_sha)
        stages.append({'stage': stage_id, 'sha256': stage_sha.hex(),
                       'variant_start': start, 'variant_count': count,
                       'source_flags': sources})
        running += count
    if running != variant_count:
        raise ValueError('variant table not fully owned')
    variants = []
    keys = set()
    vbase = stage_count * srec
    for i in range(variant_count):
        off = vbase + i * vrec
        stage_index, mask, vabi = struct.unpack_from('<IHH', body, off)
        key = body[off + 8:off + 24]
        gxp_bytes, vflags, gxp_crc = struct.unpack_from('<III', body, off + 24)
        if stage_index >= stage_count:
            raise ValueError('variant stage index out of range')
        if vabi != ABI_VERSION or not (vflags & VARIANT_COMPILED) or vflags & ~7:
            raise ValueError('invalid variant ABI/flags')
        if mask == 0 or mask & ~3:
            raise ValueError('invalid viewport mask')
        if not 32 <= gxp_bytes <= 16 * 1024 * 1024:
            raise ValueError('variant GXP size out of range')
        if key in keys:
            raise ValueError('duplicate variant key')
        keys.add(key)
        variants.append({'stage_index': stage_index, 'viewport_mask': mask,
                         'abi_version': vabi, 'key': key.hex(),
                         'gxp_bytes': gxp_bytes, 'flags': vflags,
                         'gxp_crc32': gxp_crc})
    for i, stage in enumerate(stages):
        masks = 0
        for j in range(stage['variant_start'],
                       stage['variant_start'] + stage['variant_count']):
            variant = variants[j]
            if variant['stage_index'] != i:
                raise ValueError('variant in wrong stage range')
            if masks & variant['viewport_mask']:
                raise ValueError('overlapping viewport variants')
            masks |= variant['viewport_mask']
    programs = []
    pbase = vbase + variant_count * vrec
    previous = 0
    for i in range(program_count):
        off = pbase + i * prec
        token, vs, fs, pflags = struct.unpack_from('<IHHI', body, off)
        if i and token <= previous:
            raise ValueError('program tokens not strictly increasing')
        previous = token
        if pflags & ~3:
            raise ValueError('invalid program flags')
        if vs == STAGE_NONE and fs == STAGE_NONE:
            raise ValueError('program has no stage')
        if vs != STAGE_NONE and (vs >= stage_count or
                                 stages[vs]['stage'] != STAGE_VERTEX):
            raise ValueError('program vertex stage invalid')
        if fs != STAGE_NONE and (fs >= stage_count or
                                 stages[fs]['stage'] != STAGE_FRAGMENT):
            raise ValueError('program fragment stage invalid')
        programs.append({'token': token, 'vertex_stage': vs,
                         'fragment_stage': fs, 'flags': pflags})
    return {'stages': stages, 'variants': variants, 'programs': programs}


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
        body += struct.pack('<II', variant['gxp_bytes'], VARIANT_COMPILED)
        body += struct.pack('<II', variant['gxp_crc32'], 0)
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
                     len(programs), STAGE_RECORD, VARIANT_RECORD, PROGRAM_RECORD)
    struct.pack_into('<II', header, 40, len(body), crc32(body))
    struct.pack_into('<I', header, 48, ABI_VERSION)
    return bytes(header) + bytes(body)


def build_bundle(observed_path, ledger_path, old_package_path, gxp_dir,
                 validation_path, output, control_v_path, control_f_path,
                 expansion_limit=None, strict=True):
    observed = read_json(observed_path)
    ledger = {record['shader_id']: record
              for record in read_jsonl(ledger_path)}
    old_package = read_json(old_package_path)
    validation = read_json(validation_path) if validation_path else None

    if observed.get('schema') != 1 or not isinstance(observed.get('pairs'), list):
        raise ValueError('unsupported observed-pair manifest')
    observed_pairs = [tuple(pair) for pair in observed['pairs']]
    if any(len(pair) != 2 for pair in observed_pairs):
        raise ValueError('observed pair is not a (vertex, fragment) tuple')
    observed_set = set(observed_pairs)
    if len(observed_set) != len(observed_pairs):
        raise ValueError('duplicate observed pair')
    if strict:
        if len(observed_pairs) != 658 or len(observed.get('stages', [])) != 429:
            raise ValueError('unexpected observed-pair denominators')
        if validation is not None:
            if observed.get('manifest_sha256') != validation.get('manifest_sha256'):
                raise ValueError('observed manifest belongs to another PMF')
            if validation.get('pairs_with_both_gxp') != 570:
                raise ValueError('validation does not record 570 pairs with both GXP')
        if len(ledger) != 2402:
            raise ValueError('unexpected A3 ledger denominator')
        if len(old_package.get('programs', [])) != 6311:
            raise ValueError('unexpected old package denominator')

    # Older observed programs actually exercised by the B runtime.
    old_stages = old_package['stages']
    old_programs = sorted((p for p in old_package['programs'] if p['flags'] & 1),
                          key=lambda p: p['token'])
    old_pairs = []
    for program in old_programs:
        if program['vertex_stage'] == STAGE_NONE or \
                program['fragment_stage'] == STAGE_NONE:
            raise ValueError('old observed program lacks a stage')
        vertex = old_stages[program['vertex_stage']]['shader_id']
        fragment = old_stages[program['fragment_stage']]['shader_id']
        if strict and (vertex, fragment) not in observed_set:
            raise ValueError('old observed pair is not in the PMF set')
        old_pairs.append({'spk_token': program['token'],
                          'vertex_id': vertex, 'fragment_id': fragment})
    if strict and len(old_pairs) != 21:
        raise ValueError('unexpected old observed-pair count')

    # A pair is testable only when both stages have a compiled A3 GXP.
    def testable(vertex, fragment):
        for shader_id in (vertex, fragment):
            record = ledger.get(shader_id)
            if record is None or record.get('gxp_status') != 'compiled':
                return False
            if not (gxp_dir / (shader_id + '.gxp')).is_file():
                return False
        return True

    expanded = [pair for pair in observed_pairs if testable(*pair)]
    expanded.sort()
    if strict and len(expanded) != 570:
        raise ValueError('expected 570 observed pairs with both GXPs, got %d'
                         % len(expanded))
    if strict and not {(p['vertex_id'], p['fragment_id']) for p in old_pairs} <= set(expanded):
        raise ValueError('an old observed pair is not in the expanded set')
    if expansion_limit is not None:
        expanded = expanded[:expansion_limit]
    if len(set(expanded)) != len(expanded):
        raise ValueError('duplicate expansion pair')

    # Stage table, ordered by (kind, exact content SHA, id) like the package.
    old_stage_ids = set()
    for pair in old_pairs:
        old_stage_ids.add(pair['vertex_id'])
        old_stage_ids.add(pair['fragment_id'])
    required = set(old_stage_ids)
    for vertex, fragment in expanded:
        required.add(vertex)
        required.add(fragment)
    missing = sorted(shader_id for shader_id in required if shader_id not in ledger)
    if missing:
        raise ValueError('selected stage absent from the A3 ledger: ' + missing[0])
    # The 570 testable pairs do not reference every one of the 408 compiled
    # observed stages: a stage used only by a pair whose counterpart is one of
    # the 21 rejects never enters this bundle.  The exact count comes from the
    # selected pairs and is recorded in the manifest.
    if strict and len(required) > 408:
        raise ValueError('selected stages exceed the compiled observed count')
    ordered_ids = sorted(
        required,
        key=lambda shader_id: (
            0 if ledger[shader_id]['stage'] == 'vert' else 1,
            ledger[shader_id]['maxwell_sha256'], shader_id))
    stage_index = {}
    stages = []
    variants = []
    gxp_payloads = {}
    for shader_id in ordered_ids:
        record = ledger[shader_id]
        if record.get('gxp_status') != 'compiled':
            raise ValueError('stage has no compiled GXP: ' + shader_id)
        path = gxp_dir / (shader_id + '.gxp')
        data = path.read_bytes()
        error = valid_gxp(data)
        if error:
            raise ValueError('invalid GXP %s: %s' % (path, error))
        gxp_sha = sha256_bytes(data)
        if gxp_sha.hex() != record.get('gxp_sha256'):
            raise ValueError('GXP SHA mismatch for ' + shader_id)
        stage_sha = bytes.fromhex(record['maxwell_sha256'])
        key = variant_key(stage_sha, gxp_sha, ABI_VERSION)
        index = len(stages)
        stage_index[shader_id] = index
        stages.append({
            'index': index, 'shader_id': shader_id,
            'stage': STAGE_VERTEX if record['stage'] == 'vert' else STAGE_FRAGMENT,
            'sha256': stage_sha.hex(),
            'variant_start': len(variants), 'variant_count': 1,
            'source_flags': SOURCE_SPK | SOURCE_OBSERVED,
        })
        variants.append({
            'stage_index': index, 'viewport_mask': VIEWPORT_MASK,
            'key': key.hex(), 'gxp_bytes': len(data),
            'gxp_crc32': crc32(data), 'gxp_sha256': gxp_sha.hex(),
        })
        gxp_payloads[key.hex()] = data

    programs = []
    token = 1
    for pair in old_pairs:
        programs.append({'token': token, 'phase': 'old',
                         'spk_token': pair['spk_token'],
                         'vertex_id': pair['vertex_id'],
                         'fragment_id': pair['fragment_id'],
                         'vertex_stage': stage_index[pair['vertex_id']],
                         'fragment_stage': stage_index[pair['fragment_id']],
                         'flags': PROGRAM_OBSERVED})
        token += 1
    old_first, old_last = 1, len(old_pairs)
    expanded_first = token
    for vertex, fragment in expanded:
        programs.append({'token': token, 'phase': 'expanded', 'spk_token': None,
                         'vertex_id': vertex, 'fragment_id': fragment,
                         'vertex_stage': stage_index[vertex],
                         'fragment_stage': stage_index[fragment],
                         'flags': PROGRAM_OBSERVED})
        token += 1

    index_data = serialize_index(stages, variants, programs)
    parsed = parse_index(index_data)
    if (len(parsed['stages']), len(parsed['variants']),
            len(parsed['programs'])) != (len(stages), len(variants), len(programs)):
        raise ValueError('index round-trip count mismatch')

    output = Path(output)
    if output.exists():
        raise ValueError('output must not exist; build is atomic via a temporary sibling')
    tmp = output.with_name(output.name + '.tmp-%d' % os.getpid())
    if tmp.exists():
        shutil.rmtree(tmp)
    (tmp / 'shaders').mkdir(parents=True)
    (tmp / 'shader-index.bin').write_bytes(index_data)
    for key, data in gxp_payloads.items():
        (tmp / 'shaders' / (key + '.gxp')).write_bytes(data)

    # A self-authored pair paints the target before the game pair draws.  It
    # proves the scene really reaches the framebuffer and gives the pixel test a
    # known background that survives Vita3K's scene-begin clear.
    control = {}
    for kind, path in (('v', control_v_path), ('f', control_f_path)):
        data = Path(path).read_bytes()
        error = valid_gxp(data)
        if error:
            raise ValueError('invalid control GXP %s: %s' % (path, error))
        (tmp / ('control_%s.gxp' % kind)).write_bytes(data)
        control[kind] = {'sha256': hashlib.sha256(data).hexdigest(),
                         'bytes': len(data)}

    manifest = {
        'schema': SCHEMA,
        'scope': 'S2/P4 observed pair/ABI target test bundle; not an installed package',
        'viewport_transform': VIEWPORT_TRANSFORM,
        'abi_version': ABI_VERSION,
        'index_sha256': hashlib.sha256(index_data).hexdigest(),
        'index_bytes': len(index_data),
        'counts': {
            'stages': len(stages), 'variants': len(variants),
            'programs': len(programs), 'old': len(old_pairs),
            'expanded': len(expanded), 'gxp_files': len(gxp_payloads),
            'gxp_bytes': sum(len(data) for data in gxp_payloads.values()),
        },
        'control': control,
        'phases': {
            'old': {'first': old_first, 'last': old_last},
            'expanded': {'first': expanded_first, 'last': token - 1},
        },
        'stages': [{
            'index': stage['index'], 'shader_id': stage['shader_id'],
            'stage': 'vert' if stage['stage'] == STAGE_VERTEX else 'frag',
            'stage_sha256': stage['sha256'],
            'variant_key': variants[stage['variant_start']]['key'],
            'gxp_sha256': variants[stage['variant_start']]['gxp_sha256'],
            'gxp_bytes': variants[stage['variant_start']]['gxp_bytes'],
            'gxp_crc32': variants[stage['variant_start']]['gxp_crc32'],
        } for stage in stages],
        'programs': programs,
        'inputs': {
            'tool_sha256': sha256_file(Path(__file__)),
            'observed_sha256': sha256_file(observed_path),
            'ledger_sha256': sha256_file(ledger_path),
            'old_package_sha256': sha256_file(old_package_path),
            'validation_sha256': sha256_file(validation_path) if validation_path else None,
        },
    }
    (tmp / 'pair-manifest.json').write_text(
        json.dumps(manifest, indent=1, sort_keys=True) + '\n',
        encoding='utf-8', newline='\n')
    contract = [
        '#ifndef VP_PAIR_BUNDLE_CONTRACT_H',
        '#define VP_PAIR_BUNDLE_CONTRACT_H',
        '#define VP_PAIR_ABI_VERSION 1u',
        '#define VP_PAIR_VIEWPORT %du' % VIEWPORT_TRANSFORM,
        '#define VP_PAIR_OLD_FIRST %du' % old_first,
        '#define VP_PAIR_OLD_COUNT %du' % len(old_pairs),
        '#define VP_PAIR_EXPANDED_FIRST %du' % expanded_first,
        '#define VP_PAIR_EXPANDED_COUNT %du' % len(expanded),
        '#define VP_PAIR_PROGRAM_COUNT %du' % len(programs),
        '#define VP_PAIR_STAGE_COUNT %du' % len(stages),
        '#define VP_PAIR_VARIANT_COUNT %du' % len(variants),
        '#endif',
        '',
    ]
    (tmp / 'pair_bundle_contract.h').write_text('\n'.join(contract),
                                                encoding='utf-8', newline='\n')
    tmp.rename(output)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--observed', type=Path, default=ROOT / 'artifacts/suyu-reuse-20261023/observed-pairs.json')
    # P2 refreshed baseline. Historical A3 helpers must not enter new P4 runs.
    parser.add_argument('--ledger', type=Path, default=ROOT / 'artifacts/p2-rebuild-20261025/ledger-vp1-gxp/stage-ledger.jsonl')
    parser.add_argument('--old-package', type=Path, default=ROOT / 'artifacts/deepseek-static-20261022-171155/static-package-a3/shader-index.json')
    parser.add_argument('--gxp-dir', type=Path, default=ROOT / 'artifacts/p2-rebuild-20261025/gxp-vp1/shaders')
    parser.add_argument('--validation', type=Path, default=ROOT / 'config/validation/suyu-gurecomp-20261023.json')
    parser.add_argument('--control-v', type=Path,
                        default=ROOT / 'artifacts/s2-vary-20261023/shaders/s2v_v.gxp')
    parser.add_argument('--control-f', type=Path,
                        default=ROOT / 'artifacts/s2-vary-20261023/shaders/s2v_color0_f.gxp')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--summary', type=Path,
                        help='compact machine summary (no shader bytes or ids)')
    parser.add_argument('--limit-expansion', type=int, default=None,
                        help='pilot only: keep the first N expanded pairs')
    args = parser.parse_args()
    manifest = build_bundle(args.observed, args.ledger, args.old_package,
                            args.gxp_dir, args.validation, args.output,
                            args.control_v, args.control_f,
                            expansion_limit=args.limit_expansion, strict=True)
    print(json.dumps(manifest['counts'], indent=1))
    print('index_sha256=' + manifest['index_sha256'])
    if args.summary:
        summary = {
            'schema': 1,
            'scope': manifest['scope'],
            'index_sha256': manifest['index_sha256'],
            'index_bytes': manifest['index_bytes'],
            'counts': manifest['counts'],
            'phases': manifest['phases'],
            'control': manifest['control'],
            'inputs': manifest['inputs'],
            'pilot': args.limit_expansion is not None,
        }
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(json.dumps(summary, indent=1, sort_keys=True) + '\n',
                                encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()

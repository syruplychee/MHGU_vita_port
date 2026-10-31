"""S1/A2-R: cross-check observed reference environments against the port's bound=0 environment.

The reference pipeline manifest (SPMF v1, cache 18) is an observation of what the
reference translator actually asked its environment at compile time.  This tool
imports those raw records without treating session addresses as identities, then
compiles every observed bank-1 stage three ways with this repository's pinned
frontend:

  A  bound=2, the observed texture-type/format/cbuf tables (the reference form)
  P  bound=2, the same tables but every bank-1 cbuf value removed (folded as 0,
     and recorded by the frontend as missing)
  B  bound=0, the existing A2 resolved environment (the port form, bank 1 stays
     a runtime uniform)

A and P share code extents and descriptors by construction; a difference between
them is therefore evidence that a bank-1 value reaches the emitted program.  For
each bank-1 word a further run zeroes only that word, which separates values that
change the program structure (handles/configuration) from values used as data.
B is then compared structurally with A; bank-1 reads are the only expected
difference, and the runtime contract is the exact observed bit pattern.

This is host static evidence.  It does not execute a shader, create a GXM pair,
compare pixels, or claim hardware behavior.  The tool never writes into the
supplied library, the PMF, or the historical A2 resolved directories.

Scope choices that follow the supplement:
  * identity is the full canonical SPK stage bytes and SHA; the PMF program
    extent is recorded separately and never replaces that identity;
  * words[2..4] (start_address, cached_lowest, cached_highest) are session GPU
    addresses and are excluded from the environment variant key;
  * every observed texture type is checked against the reference's integer
    pixel-format predicate; S8_UINT_D24_UNORM is *not* an integer format in that
    predicate, which matters for interpreting the format table;
  * values observed once for one stage are not frozen for every frame.  The
    output records a per-stage runtime binding contract, not a compile-time
    constant substitution.
"""
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
import difflib
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

import audit_suyu_manifest as manifest_audit

TERMINATOR = bytes.fromhex('0f0087ffff0f40e23412769801000000')
SESSION_WORD_INDICES = (2, 3, 4)
ENVIRONMENT_FIELDS = 9

# VideoCore::Surface::PixelFormat order in the locked reference tree.  Only the
# members reachable from the observed tables need names; unknown values stay
# numeric and are reported as unresolved rather than guessed.
PIXEL_FORMAT_NAMES = {
    0: 'A8B8G8R8_UNORM', 1: 'A8B8G8R8_SNORM', 2: 'A8B8G8R8_SINT',
    3: 'A8B8G8R8_UINT', 4: 'R5G6B5_UNORM', 5: 'B5G6R5_UNORM',
    6: 'A1R5G5B5_UNORM', 7: 'A2B10G10R10_UNORM', 8: 'A2B10G10R10_UINT',
    9: 'A2R10G10B10_UNORM', 10: 'A1B5G5R5_UNORM', 11: 'A5B5G5R1_UNORM',
    12: 'R8_UNORM', 13: 'R8_SNORM', 14: 'R8_SINT', 15: 'R8_UINT',
    16: 'R16G16B16A16_FLOAT', 17: 'R16G16B16A16_UNORM',
    18: 'R16G16B16A16_SNORM', 19: 'R16G16B16A16_SINT',
    20: 'R16G16B16A16_UINT', 21: 'B10G11R11_FLOAT', 22: 'R32G32B32A32_UINT',
    23: 'BC1_RGBA_UNORM', 24: 'BC2_UNORM', 25: 'BC3_UNORM', 26: 'BC4_UNORM',
    27: 'BC4_SNORM', 28: 'BC5_UNORM', 29: 'BC5_SNORM', 30: 'BC7_UNORM',
    31: 'BC6H_UFLOAT', 32: 'BC6H_SFLOAT', 33: 'ASTC_2D_4X4_UNORM',
    34: 'B8G8R8A8_UNORM', 35: 'R32G32B32A32_FLOAT', 36: 'R32G32B32A32_SINT',
    37: 'R32G32_FLOAT', 38: 'R32G32_SINT', 39: 'R32_FLOAT', 40: 'R16_FLOAT',
    41: 'R16_UNORM', 42: 'R16_SNORM', 43: 'R16_UINT', 44: 'R16_SINT',
    45: 'R16G16_UNORM', 46: 'R16G16_FLOAT', 47: 'R16G16_UINT',
    48: 'R16G16_SINT', 49: 'R16G16_SNORM', 50: 'R32G32B32_FLOAT',
    51: 'A8B8G8R8_SRGB', 52: 'R8G8_UNORM', 53: 'R8G8_SNORM',
    54: 'R8G8_SINT', 55: 'R8G8_UINT', 56: 'R32G32_UINT',
    57: 'R16G16B16X16_FLOAT', 58: 'R32_UINT', 59: 'R32_SINT',
    105: 'D32_FLOAT', 106: 'D16_UNORM', 107: 'X8_D24_UNORM', 108: 'S8_UINT',
    109: 'D24_UNORM_S8_UINT', 110: 'S8_UINT_D24_UNORM', 111: 'D32_FLOAT_S8_UINT',
}

# VideoCore::Surface::IsPixelFormatInteger in the locked reference.  Depth and
# stencil formats are deliberately absent; S8_UINT_D24_UNORM is not integer.
INTEGER_PIXEL_FORMATS = {
    2, 3, 8, 14, 15, 19, 20, 22, 36, 38, 43, 44, 47, 48, 54, 55, 56, 58, 59,
}
DEPTH_PIXEL_FORMATS = {105, 106, 107, 108, 109, 110, 111}

# Values the reference translator saw folded at offsets 0/4 across most stages.
# Matching one is a pattern observation, never provenance by itself.
EPSILON_PATTERN = {0: 0x358637bd, 4: 0x3727c5ac}

FLOAT_LITERAL_RE = re.compile(
    r'(?<![\w.])(-?(?:\d+\.\d*|\.\d+|\d+[eE][-+]?\d+)(?:[eE][-+]?\d+)?f?)(?![\w.])')
UINT_LITERAL_RE = re.compile(r'(?<![\w.])0x[0-9a-fA-F]+u\b|(?<![\w.])\d+u\b')
INT_LITERAL_RE = re.compile(r'(?<![\w.])(-?\d+)(?![\w.])')


class CrosscheckError(Exception):
    pass


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sha(data):
    return hashlib.sha256(data).hexdigest()


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


def f32_from_bits(bits):
    return struct.unpack('<f', struct.pack('<I', bits & 0xffffffff))[0]


def is_finite(bits):
    return (bits & 0x7f800000) != 0x7f800000


def is_nan_or_inf(bits):
    return not is_finite(bits)


def table_digest(rows):
    return sha(json.dumps(rows, separators=(',', ':')).encode())


def environment_variant_key(words, tables):
    """Identity excluding session GPU addresses; tables are kept verbatim."""
    identity_words = [words[i] for i in range(len(words)) if i not in SESSION_WORD_INDICES]
    return sha(json.dumps({'words': identity_words,
                           'tables': [list(map(list, table)) for table in tables]},
                          separators=(',', ':')).encode())


def load_library(library):
    library = Path(library)
    spk = (library / 'source/root.spkg').read_bytes()
    extraction = read_json(library / 'extraction.json')
    if sha(spk) != extraction.get('spk_sha256'):
        raise CrosscheckError('library SPK SHA256 mismatch')
    entries = {e['spk_sph_offset']: e for e in read_jsonl(library / 'stage_entries.jsonl')}
    shaders = {e['id']: e for e in read_jsonl(library / 'shaders.jsonl')}
    return spk, entries, shaders, extraction


def collect_observations(manifest, spk, entries, shaders):
    """Read PMF occurrences and group them by exact stage content identity.

    Returns (header, stages) where each stage carries every distinct raw
    environment seen for that stage.  Full canonical bytes remain the identity;
    the PMF code extent is only recorded.
    """
    data = Path(manifest).read_bytes()
    header, pipes = manifest_audit.parse(data, spk)
    stages = {}
    occurrences = []
    for pipe_index, (pipe_stages, key) in enumerate(pipes):
        for stage in pipe_stages:
            entry = entries.get(stage['offset'])
            if entry is None:
                raise CrosscheckError('PMF offset is not a canonical SPH boundary')
            sid = entry['shader_id']
            canonical = spk[stage['offset']:stage['offset'] + entry['maxwell_bytes']]
            if sha(canonical) != shaders[sid]['sha256']:
                raise CrosscheckError('canonical stage SHA mismatch for ' + sid)
            if (entry['stage'], stage['words'][6]) not in (('vert', 0), ('frag', 4)):
                raise CrosscheckError('stage kind mismatch for ' + sid)
            if stage['program_size'] > entry['maxwell_bytes']:
                raise CrosscheckError('PMF extent exceeds canonical stage for ' + sid)
            if stage['code_size'] not in (stage['program_size'],
                                          stage['program_size'] + len(TERMINATOR)):
                raise CrosscheckError('unexpected terminator extent for ' + sid)
            sph_sha = sha(canonical[:80])
            if stage['sph_sha256'] != sph_sha:
                raise CrosscheckError('PMF program header differs from canonical stage for ' + sid)
            tables = [list(map(tuple, table)) for table in stage['tables']]
            key_hash = environment_variant_key(stage['words'], tables)
            record = stages.setdefault(sid, {
                'schema': 1,
                'shader_id': sid,
                'stage': entry['stage'],
                'origin': entry.get('origin'),
                'maxwell_bytes': len(canonical),
                'maxwell_sha256': sha(canonical),
                'sph_sha256': sph_sha,
                'occurrences': 0,
                'pipeline_indices': set(),
                'variants': {},
                'raw_environment_records': set(),
            })
            record['occurrences'] += 1
            record['pipeline_indices'].add(pipe_index)
            variant = record['variants'].setdefault(key_hash, {
                'environment_key': key_hash,
                'count': 0,
                'pipe_indices': set(),
                'words': list(stage['words']),
                'tables': tables,
                'pmf_offsets': set(),
                'program_sizes': set(),
                'code_sizes': set(),
                'sph_sha256': stage['sph_sha256'],
            })
            variant['count'] += 1
            variant['pipe_indices'].add(pipe_index)
            variant['pmf_offsets'].add(stage['offset'])
            variant['program_sizes'].add(stage['program_size'])
            variant['code_sizes'].add(stage['code_size'])
            record['raw_environment_records'].add(
                (tuple(stage['words'][:2] + stage['words'][5:]), key_hash))
            occurrences.append({
                'schema': 1,
                'shader_id': sid,
                'stage': entry['stage'],
                'pipeline_index': pipe_index,
                'pmf_offset': stage['offset'],
                'program_size': stage['program_size'],
                'code_size': stage['code_size'],
                'terminator': stage['code_size'] != stage['program_size'],
                'words': list(stage['words']),
                'environment_key': key_hash,
                'table_rows': [len(t) for t in tables],
                'type_table_sha256': table_digest(tables[0]),
                'format_table_sha256': table_digest(tables[1]),
                'cbuf_table_sha256': table_digest(tables[2]),
                'replacement_table_sha256': table_digest(tables[3]),
                'identity': 'full canonical SPK stage; PMF extent is not identity',
            })
    return header, stages, occurrences


def finalize_stages(stages):
    """Pick a representative environment and derive per-stage invariants."""
    for record in stages.values():
        variants = []
        for variant in record['variants'].values():
            variant['pmf_offsets'] = sorted(variant['pmf_offsets'])
            variant['program_sizes'] = sorted(variant['program_sizes'])
            variant['code_sizes'] = sorted(variant['code_sizes'])
            variant['pipe_indices'] = sorted(variant['pipe_indices'])
            variant['type_rows'] = variant['tables'][0]
            variant['format_rows'] = variant['tables'][1]
            variant['cbuf_rows'] = variant['tables'][2]
            variant['replacement_rows'] = variant['tables'][3]
            variant['bank1'] = [(k & 0xffffffff, v) for k, v in variant['cbuf_rows']
                                if (k >> 32) == 1]
            variants.append(variant)
        variants.sort(key=lambda v: (-v['count'],
                                     v['words'][0], v['words'][1], v['words'][5],
                                     v['environment_key']))
        record['variants'] = variants
        record['variants'].sort(key=lambda v: v['environment_key'])
        representative = variants[0]
        record['representative_environment_key'] = representative['environment_key']
        record['representative'] = representative
        record['pipeline_indices'] = sorted(record['pipeline_indices'])
        record['raw_environment_records'] = len(record['raw_environment_records'])
        record['variant_count'] = len(variants)
        # Bank-1 values are per-stage stable in this corpus.  If that ever
        # changes, the runtime contract below is not sufficient and the stage is
        # flagged rather than silently freezing one observation.
        bank1_sets = defaultdict(set)
        for variant in variants:
            for offset, value in variant['bank1']:
                bank1_sets[offset].add(value)
        record['bank1_offsets'] = sorted(bank1_sets)
        record['bank1_values'] = {str(offset): sorted(bank1_sets[offset])
                                  for offset in sorted(bank1_sets)}
        record['bank1_stable_across_occurrences'] = all(
            len(values) == 1 for values in bank1_sets.values())
        type_counts = {len(v['type_rows']) for v in variants}
        type_values = {tuple(sorted(v for _, v in v['type_rows'])) for v in variants}
        format_values = {tuple(sorted(v for _, v in v['format_rows'])) for v in variants}
        record['type_queries_stable'] = len(type_counts) == 1 and len(type_values) == 1
        record['type_values'] = sorted({v for tup in type_values for v in tup})
        record['format_values'] = sorted({v for tup in format_values for v in tup})
        record['observed_types'] = dict(sorted(Counter(
            v for variant in variants for _, v in variant['type_rows']).items()))
        record['observed_formats'] = dict(sorted(Counter(
            v for variant in variants for _, v in variant['format_rows']).items()))
    return stages


def representative_code(spk, entry, record):
    offset = record['representative']['pmf_offsets'][0]
    return spk[offset:offset + entry['maxwell_bytes']]


def exact_code(spk, record, variant):
    """PMF program extent plus its optional 16-byte terminator."""
    offset = variant['pmf_offsets'][0]
    program_size = variant['program_sizes'][0]
    code = spk[offset:offset + program_size]
    if variant['code_sizes'][0] != program_size:
        code += TERMINATOR
    return code


def environment_lines(variant, bound=None, drop_bank1=False):
    words = variant['words']
    tables = variant['tables']
    cbuf_rows = [(k, v) for k, v in tables[2]
                 if not (drop_bank1 and (k >> 32) == 1)]
    lines = [f'{words[6]} 16 {words[0]} '
             f'{words[1] if bound is None else bound} {words[5]} '
             f'{len(tables[0])} {len(tables[1])} {len(cbuf_rows)} {len(tables[3])}']
    for key, value in tables[0]:
        lines.append(f'{key} {value}')
    for key, value in tables[1]:
        lines.append(f'{key} {value}')
    for key, value in cbuf_rows:
        lines.append(f'{key} {value}')
    for key, value in tables[3]:
        lines.append(f'{key} {value}')
    return '\n'.join(lines) + '\n'


def run_frontend(frontend, code_path, environment_path, output_path, timeout=120):
    return subprocess.run([str(frontend), str(code_path), str(environment_path),
                           str(output_path)], capture_output=True, text=True,
                          timeout=timeout)


def parse_reflection(path):
    return read_json(path)


def parse_missing(path):
    return read_json(path)


def texture_signature(reflection):
    return sorted((d['type'], d['depth'], d['secondary'], d['count'],
                   d['shift'], d.get('size_shift'))
                  for d in reflection.get('textures', []))


def cbuf_signature(reflection, exclude_bank=None):
    return sorted((d['bank'], d['count'])
                  for d in reflection.get('constant_buffers', [])
                  if d['bank'] != exclude_bank)


def read_glsl(path):
    return Path(path).read_text(encoding='utf-8', errors='replace')


def run_compile(tag, frontend, code, environment, directory, runner=None):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    code_path = directory / f'code-{tag}.bin'
    env_path = directory / f'environment-{tag}.txt'
    glsl_path = directory / f'shader-{tag}.glsl'
    code_path.write_bytes(code)
    env_path.write_text(environment, encoding='utf-8', newline='\n')
    runner = runner or run_frontend
    proc = runner(frontend, code_path, env_path, glsl_path)
    result = {
        'tag': tag,
        'returncode': proc.returncode,
        'stdout': proc.stdout.strip(),
        'stderr': proc.stderr.strip(),
        'code_sha256': sha(code),
        'environment_sha256': digest(env_path),
        'glsl_sha256': None,
        'reflection': None,
        'missing': None,
    }
    if proc.returncode == 0 and glsl_path.is_file():
        result['glsl_sha256'] = digest(glsl_path)
        result['reflection'] = parse_reflection(Path(str(glsl_path) + '.resources.json'))
        result['missing'] = parse_missing(Path(str(glsl_path) + '.missing-cbufs.json'))
    return result


def missing_keys(missing):
    return sorted((k['bank'], k['offset']) for k in (missing or {}).get('keys', []))


def diff_summary(before, after):
    """Changed lines, with float literals from the 'before' side.

    The frontend folds a bank-1 value into a literal in A.  Zeroing that word
    produces P (or a single-word run); the removed A line contains exactly the
    literal that the word produced, unless the value is also zero (in which
    case the run is not differentiable).
    """
    removed, added = [], []
    for line in difflib.unified_diff(before.splitlines(), after.splitlines(),
                                     lineterm='', n=0):
        if line.startswith('---') or line.startswith('+++') or line.startswith('@@'):
            continue
        if line.startswith('-'):
            removed.append(line[1:])
        elif line.startswith('+'):
            added.append(line[1:])
    literals = []
    uint_literals = []
    integer_literals = []
    for line in removed:
        literals.extend(FLOAT_LITERAL_RE.findall(line))
        uint_literals.extend(UINT_LITERAL_RE.findall(line))
        integer_literals.extend(INT_LITERAL_RE.findall(line))
    return {
        'removed_lines': len(removed),
        'added_lines': len(added),
        'float_literals_in_removed': literals[:8],
        'uint_literals_in_removed': uint_literals[:8],
        'integer_literals_in_removed': integer_literals[:8],
        'changed': bool(removed or added),
    }


def classify_word(offset, value, observed_by_reference, frontend_read, zero_run):
    """Evidence categories; no semantic guess without a supporting observation."""
    bits = value & 0xffffffff
    finite = is_finite(bits)
    zero_same = bits == 0
    flags = {
        'observed_by_reference': observed_by_reference,
        'read_by_pinned_frontend': frontend_read,
        'non_finite_bits': not finite,
        'value_is_zero': zero_same,
        'epsilon_default_pattern': EPSILON_PATTERN.get(offset) == bits,
        'zeroing_changes_reflection': False,
        'zeroing_changes_glsl': False,
        'float_literal_in_folded_line': False,
        'uint_literal_in_folded_line': False,
        'integer_literal_in_folded_line': False,
        'not_differentiated': False,
    }
    if not observed_by_reference:
        category = 'frontend-only-read'
        basis = 'the pinned frontend reads this word but the reference table has no record'
    elif zero_run is None:
        flags['not_differentiated'] = True
        category = 'unresolved'
        basis = 'no single-word run (offset not read by the pinned frontend)'
    elif zero_same:
        flags['not_differentiated'] = True
        category = 'unresolved'
        basis = 'observed value is already zero; zeroing cannot distinguish uses'
    else:
        flags['zeroing_changes_reflection'] = not zero_run['reflection_equal']
        flags['zeroing_changes_glsl'] = not zero_run['glsl_equal']
        flags['float_literal_in_folded_line'] = bool(
            zero_run['diff']['float_literals_in_removed'])
        flags['uint_literal_in_folded_line'] = bool(
            zero_run['diff'].get('uint_literals_in_removed'))
        flags['integer_literal_in_folded_line'] = bool(
            zero_run['diff'].get('integer_literals_in_removed'))
        if not finite:
            category = 'bit-pattern-carrier'
            basis = 'raw u32 is NaN/Inf, so a float numeric path is not established'
        elif flags['zeroing_changes_reflection']:
            category = 'structure-affecting'
            basis = 'zeroing changes the reflected texture/constant-buffer shape'
        elif flags['zeroing_changes_glsl']:
            if flags['uint_literal_in_folded_line']:
                category = 'bit-pattern-carrier'
                basis = ('folded value appears as an unsigned/hex literal, '
                         'so the emitted program consumes raw bits')
            elif flags['float_literal_in_folded_line']:
                category = 'float-operation-input'
                basis = 'folded value appears as a float literal and changes emitted code'
            elif flags['integer_literal_in_folded_line']:
                category = 'integer-input'
                basis = 'value changes emitted code as a signed integer literal'
            else:
                category = 'bit-pattern-or-integer-input'
                basis = 'value changes emitted code without a typed literal'
        else:
            category = 'inert-or-dead'
            basis = 'zeroing changes neither reflection nor emitted GLSL'
    return category, basis, flags


def classify_format(value):
    name = PIXEL_FORMAT_NAMES.get(value)
    return {
        'format': value,
        'name': name,
        'integer_per_reference': value in INTEGER_PIXEL_FORMATS,
        'depth_or_stencil': value in DEPTH_PIXEL_FORMATS,
        'known': name is not None,
    }


def type_mapping(record, resolved):
    """Join observed type queries to descriptors through the raw cbuf handle.

    The PMF type table is keyed by the raw NVN handle, and its serialized order
    is not query order.  The observed cbuf table records the handle value at the
    descriptor's constant-buffer offset, so the deterministic join is:

        A2 descriptor (bank, offset) -> PMF cbuf value -> PMF type row -> A2 handle

    Two descriptor instructions can resolve to the same raw handle in one draw;
    the reference type map then has fewer rows than descriptors.  That is
    reported as handle aliasing, not as a missing query.
    """
    if resolved is None:
        return {'status': 'missing-a2-record'}
    descriptors = resolved.get('descriptors', [])
    queried = [d for d in descriptors if d.get('queried')]
    queried_types = sorted(d['instruction_type'] for d in queried)
    variants = []
    for variant in record['variants']:
        cbuf = {(key >> 32, key & 0xffffffff): value
                for key, value in variant['cbuf_rows']}
        type_by_handle = {key: value for key, value in variant['type_rows']}
        descriptor_rows = []
        matched = 0
        for descriptor in descriptors:
            bank = descriptor['source_bank']
            offset = descriptor['source_offset']
            raw_handle = cbuf.get((bank, offset))
            observed_type = type_by_handle.get(raw_handle) if raw_handle is not None else None
            row = {
                'a2_index': descriptor['index'],
                'a2_handle': descriptor['handle'],
                'a2_type': descriptor['instruction_type'],
                'queried': bool(descriptor.get('queried')),
                'source_bank': bank,
                'source_offset': offset,
                'pmf_raw_handle': raw_handle,
                'pmf_type': observed_type,
                'type_equal': observed_type == descriptor['instruction_type'],
            }
            if not row['queried']:
                row['status'] = 'not-queried-by-a2'
            elif raw_handle is None:
                row['status'] = 'no-pmf-cbuf-handle'
            elif observed_type is None:
                row['status'] = 'no-pmf-type-row'
            elif not row['type_equal']:
                row['status'] = 'type-differs'
            else:
                row['status'] = 'mapped'
                matched += 1
            descriptor_rows.append(row)
        mapped_handles = {row['pmf_raw_handle'] for row in descriptor_rows
                          if row['queried'] and row['pmf_raw_handle'] is not None}
        table_handles = set(type_by_handle)
        extra_type_rows = sorted(table_handles - mapped_handles)
        aliased = len(mapped_handles) < len(queried)
        issues = sorted({row['status'] for row in descriptor_rows
                         if row['queried'] and row['status'] != 'mapped'})
        handle_join_complete = not issues
        observed_types = sorted(v for _, v in variant['type_rows'])
        types_closed = (set(observed_types) <= set(queried_types) and
                        len(observed_types) <= len(queried))
        if not queried:
            status = 'no-type-queries'
        elif handle_join_complete and extra_type_rows:
            status = 'unresolved-extra-reference-queries'
        elif handle_join_complete and aliased:
            status = 'mapped-with-handle-aliasing'
        elif handle_join_complete:
            status = 'mapped'
        elif types_closed:
            # The observed table covers a subset of the descriptors and every
            # observed value equals an answer the port already supplies.  The
            # per-descriptor handle join is not available for this draw.
            status = 'mapped-types-only'
        else:
            status = 'unresolved'
        variants.append({
            'environment_key': variant['environment_key'],
            'status': status,
            'pmf_type_queries': len(variant['type_rows']),
            'a2_queried_descriptors': len(queried),
            'matched_descriptors': matched,
            'distinct_pmf_handles': len(mapped_handles),
            'handle_join_complete': handle_join_complete,
            'handle_aliasing': aliased,
            'type_values_closed': types_closed,
            'observed_types': observed_types,
            'extra_reference_type_handles': extra_type_rows,
            'issues': issues,
            'descriptors': descriptor_rows,
        })
    all_statuses = {variant['status'] for variant in variants}
    meaningful = all_statuses - {'no-type-queries'}
    mapped_states = {'mapped', 'mapped-with-handle-aliasing', 'mapped-types-only'}
    if not meaningful:
        status = 'no-type-queries'
    elif meaningful <= mapped_states:
        if 'mapped-types-only' in meaningful:
            status = 'mapped-types-only'
        elif 'mapped-with-handle-aliasing' in meaningful:
            status = 'mapped-with-handle-aliasing'
        else:
            status = 'mapped'
    else:
        status = 'unresolved'
    return {
        'status': status,
        'basis': ('A2 descriptor (bank,offset) -> PMF cbuf value -> PMF type row; '
                  'counts alone are not used when the handle join is available'),
        'all_variants_equal_a2': all(v['status'] in
                                     ('mapped', 'mapped-with-handle-aliasing')
                                     for v in variants),
        'a2_queried_descriptors': len(queried),
        'a2_queried_types': queried_types,
        'a2_synthetic_handles_sha256': sha(json.dumps(
            sorted(d['handle'] for d in queried)).encode()),
        'variants': variants,
    }


def format_analysis(record, pinned_frontend_queries_formats=False):
    formats = {}
    for value in record['format_values']:
        formats[str(value)] = classify_format(value)
    integer = [v for v in record['format_values']
               if v in INTEGER_PIXEL_FORMATS]
    depth = [v for v in record['format_values'] if v in DEPTH_PIXEL_FORMATS]
    if not record['format_values']:
        status = 'no-format-queries'
        reason = 'the reference recorded no pixel-format read for this stage'
    elif pinned_frontend_queries_formats:
        status = 'unresolved'
        reason = 'the pinned frontend queries formats; its answers must be supplied'
    elif integer:
        status = 'unresolved'
        reason = 'an observed format is integer under the reference predicate'
    else:
        status = 'format-neutral'
        reason = ('the reference format read only selects integer sampling '
                  '(IsPixelFormatInteger); every observed format is non-integer, '
                  'so the pinned frontend emits the same float sampling path')
    return {
        'status': status,
        'reason': reason,
        'pinned_frontend_queries_formats': pinned_frontend_queries_formats,
        'pinned_frontend_basis': ('the pinned shader-compiler-rs texture_pass has '
                                  'no ReadTexturePixelFormat/IsTexturePixelFormatInteger '
                                  'consumer; the adapter therefore never asks'),
        'observed_formats': formats,
        'integer_formats': integer,
        'depth_or_stencil_formats': depth,
        'depth_note': ('depth/stencil formats are not integer in the reference '
                       'predicate; descriptor depth is an instruction flag, not '
                       'this table'),
    }


def capture_reproduction(artifact_root, root):
    """Byte equality between A2 resolved GLSL and the committed playable captures."""
    overlays = {0: Path(artifact_root) / 'resolved-vp0',
                1: Path(artifact_root) / 'resolved'}
    captures = {0: Path(root) / 'build/playable-shaders/shader-vp0-final-20261021',
                1: Path(root) / 'build/playable-shaders/shader-vp1-final-20261021'}
    rows_path = Path(artifact_root) / 'audit-resolved/capture-map.jsonl'
    if not rows_path.is_file():
        return {'available': False, 'reason': 'capture-map.jsonl missing'}
    stats = Counter()
    checked = 0
    for row in read_jsonl(rows_path):
        viewport = row['viewport_transform']
        a = overlays[viewport] / row['shader_id'] / 'shader.glsl'
        b = captures[viewport] / row['program'] / 'shader.glsl'
        stage = row['shader_id'].split('_')[0]
        stats[f'vp{viewport}_{stage}_total'] += 1
        checked += 1
        if a.is_file() and b.is_file() and digest(a) == digest(b):
            stats[f'vp{viewport}_{stage}_matched'] += 1
    result = {k: stats[k] for k in sorted(stats)}
    result['available'] = True
    result['records_checked'] = checked
    result['all_matched'] = all(
        result.get(f'vp{viewport}_{stage}_matched', 0) ==
        result.get(f'vp{viewport}_{stage}_total', 0)
        for viewport in (0, 1) for stage in ('vert', 'frag'))
    result['comparison'] = 'A2 resolved GLSL SHA256 == committed playable capture GLSL SHA256'
    return result


def runtime_bank1_evidence(root, capture_resources):
    """Static code evidence for the upload path, plus local captured bank-1 bytes.

    The guest bridge keeps a raw pointer into guest constant memory and the GXM
    path passes that buffer to sceGxmSetUniformDataF.  No numeric conversion
    exists in this repository's code.  Whether the SDK preserves every NaN
    payload bit is a target question and stays open.
    """
    root = Path(root)
    guest = root / 'src/platform/guest_svc.c'
    gxm = root / 'src/platform/gxm_runtime.c'
    guest_text = guest.read_text(encoding='utf-8')
    gxm_text = gxm.read_text(encoding='utf-8')
    pointer_copy = bool(re.search(
        r'req\.stage_cbuf\[st\]\[bank\]\.data\s*=\s*\(const void \*\)\(uintptr_t\)ptr',
        guest_text))
    float_handoff = bool(re.search(
        r'sceGxmSetUniformDataF\(buffer,\s*p,\s*0,\s*floats,\s*banks\[bank\]\.data\)',
        gxm_text))
    captured = {}
    if capture_resources:
        directory = Path(capture_resources)
        files = sorted(directory.glob('creation-*-cbuf1.bin'))
        distinct = Counter()
        for path in files:
            data = path.read_bytes()
            distinct[data[:16].hex()] += 1
        captured = {
            'directory': str(directory),
            'files': len(files),
            'distinct_first_16_bytes': dict(sorted(distinct.items())),
            'first_8_bytes_float': sorted({
                f32_from_bits(struct.unpack('<I', bytes.fromhex(h)[:4])[0])
                for h in distinct if len(h) >= 8}),
            'note': ('draw-level local runtime bytes; these are not joined to a '
                     'PMF stage id and do not prove a target bit-preservation path'),
        }
    return {
        'guest_bridge_raw_pointer': pointer_copy,
        'gxm_float_handoff': float_handoff,
        'guest_svc_sha256': digest(guest),
        'gxm_runtime_sha256': digest(gxm),
        'captured_creation_bank1': captured,
        'open_question': ('sceGxmSetUniformDataF takes const float*; NaN payload '
                          'and subnormal preservation on the target is not '
                          'established by this host check'),
    }


def draw_capture_evidence(path):
    """Summarize Maxwell stage/bank structure from a locally captured draw list.

    The capture records ``[stage, bank, address, size, blob]`` rows.  It lets us
    corroborate that bank 1 is a separate 64 KiB program-block range and that
    user uniform buffers occupy banks >= 3, but it does not carry NVN call slot
    ids, so it cannot establish the name-to-slot join by itself.
    """
    if path is None:
        return {'available': False}
    source = Path(path)
    file = source / 'draws.jsonl' if source.is_dir() else source
    if not file.is_file():
        raise FileNotFoundError(f'draw capture not found: {file}')
    rows = [json.loads(line) for line in
            file.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    bindings = []
    for row in rows:
        for item in row.get('cbufs', []):
            if not isinstance(item, (list, tuple)) or len(item) < 4:
                raise ValueError('draw capture cbuf row is malformed')
            stage, bank, _address, size = item[:4]
            bindings.append({'stage': int(stage), 'bank': int(bank),
                             'size': int(size)})
    bank1 = [b for b in bindings if b['bank'] == 1]
    user = [b for b in bindings if b['bank'] >= 3]
    user_histogram = Counter(b['bank'] for b in user)
    return {
        'available': True,
        'file': str(file),
        'sha256': sha(file.read_bytes()),
        'draws': len(rows),
        'bindings': len(bindings),
        'bank1_bindings': len(bank1),
        'bank1_stages': sorted({b['stage'] for b in bank1}),
        'bank1_sizes': sorted({b['size'] for b in bank1}),
        'user_bank_bindings': len(user),
        'user_bank_histogram': {str(k): v for k, v in sorted(user_histogram.items())},
        'slot_join_available': False,
        'note': ('The capture has Maxwell stage/bank and sizes, not NVN slot ids. '
                 'The reference nvn_vk.cpp binds bank 1 from the program block and '
                 'maps user UB slot N to bank N+3; this file corroborates the '
                 'structure only, not a local name/slot join.'),
    }


def compile_record(record, entry, spk, resolved_dir, frontend, output, runner=None):
    sid = record['shader_id']
    variant = record['representative']
    full_code = representative_code(spk, entry, record)
    exact = exact_code(spk, record, variant)
    a_env = environment_lines(variant)
    p_env = environment_lines(variant, drop_bank1=True)
    stage_dir = Path(output) / sid
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    stage_dir.mkdir(parents=True)
    runs = {}
    runs['A'] = run_compile('A', frontend, full_code, a_env, stage_dir, runner)
    baseline_tag = 'A'
    if runs['A']['returncode'] != 0 or missing_keys(runs['A']['missing']):
        # The reference compiled a shorter cache extent; if our decoder reaches
        # past it, retry with the exact observed extent before declaring failure.
        runs['A_exact'] = run_compile('A_exact', frontend, exact, a_env, stage_dir, runner)
        if runs['A_exact']['returncode'] == 0 and not missing_keys(runs['A_exact']['missing']):
            baseline_tag = 'A_exact'
    baseline = runs[baseline_tag]
    baseline_code = full_code if baseline_tag == 'A' else exact
    runs['P'] = run_compile('P', frontend, baseline_code, p_env, stage_dir, runner)
    b_env_path = Path(resolved_dir) / sid / 'environment.txt'
    b_env = b_env_path.read_text(encoding='utf-8') if b_env_path.is_file() else None
    if b_env is not None:
        runs['B'] = run_compile('B', frontend, full_code, b_env, stage_dir, runner)
    else:
        runs['B'] = {'tag': 'B', 'returncode': None, 'error': 'A2 environment missing'}
    record_result = {
        'schema': 1,
        'shader_id': sid,
        'stage': entry['stage'],
        'representative_environment_key': variant['environment_key'],
        'observed_variants': len(record['variants']),
        'observed_occurrences': record['occurrences'],
        'runs': {tag: {k: run[k] for k in
                       ('returncode', 'code_sha256', 'environment_sha256',
                        'glsl_sha256', 'stdout', 'stderr')}
                 for tag, run in runs.items()},
    }
    if baseline['returncode'] != 0:
        record_result['status'] = 'frontend-failed'
        record_result['error'] = baseline['stderr'][:400]
        return record_result, runs
    reflection = baseline['reflection']
    record_result['a_missing_keys'] = missing_keys(baseline['missing'])
    record_result['a_missing_texture_types'] = baseline['missing']['texture_types']
    record_result['a_missing_texture_formats'] = baseline['missing']['texture_formats']

    # The P run records every bank-1 read the pinned frontend makes.
    p_missing = missing_keys(runs['P']['missing']) if runs['P']['returncode'] == 0 else []
    p_bank1 = sorted(offset for bank, offset in p_missing if bank == 1)
    record_result['pinned_frontend_bank1_reads'] = p_bank1
    record_result['pmf_bank1_offsets'] = record['bank1_offsets']
    record_result['bank1_read_sets'] = {
        'pmf_only': sorted(set(record['bank1_offsets']) - set(p_bank1)),
        'frontend_only': sorted(set(p_bank1) - set(record['bank1_offsets'])),
        'both': sorted(set(record['bank1_offsets']) & set(p_bank1)),
    }
    if runs['P']['returncode'] == 0:
        p_equal_reflection = (texture_signature(runs['P']['reflection']) ==
                              texture_signature(reflection) and
                              cbuf_signature(runs['P']['reflection'], 1) ==
                              cbuf_signature(reflection, 1))
        p_equal_glsl = runs['P']['glsl_sha256'] == baseline['glsl_sha256']
        record_result['all_bank1_zero'] = {
            'reflection_equal': p_equal_reflection,
            'glsl_equal': p_equal_glsl,
        }
    else:
        record_result['all_bank1_zero'] = {'reflection_equal': None,
                                           'glsl_equal': None,
                                           'error': runs['P'].get('stderr', '')[:200]}
    if runs['B']['returncode'] == 0:
        b = runs['B']
        record_result['a_vs_b'] = {
            'texture_structure_equal': texture_signature(b['reflection']) ==
                                       texture_signature(reflection),
            'constant_buffers_excluding_bank1_equal':
                cbuf_signature(b['reflection'], 1) == cbuf_signature(reflection, 1),
            'b_bank1_descriptor': [d for d in b['reflection'].get('constant_buffers', [])
                                   if d['bank'] == 1],
            'b_bank1_max_offset': (max(p_bank1) if p_bank1 else None),
            'b_bank1_components_needed': (max(p_bank1) // 4 + 1 if p_bank1 else 0),
            'missing_keys': missing_keys(b['missing']),
        }
    else:
        record_result['a_vs_b'] = {'status': 'failed',
                                   'error': runs['B'].get('error', '')}

    # Per-word sensitivity: zero exactly one bank-1 value.
    a_glsl = read_glsl(stage_dir / f'shader-{baseline_tag}.glsl') \
        if (stage_dir / f'shader-{baseline_tag}.glsl').is_file() else ''
    bank1_inventory = []
    for offset in p_bank1:
        observed_values = record['bank1_values'].get(str(offset), [])
        value = observed_values[0] if len(observed_values) == 1 else None
        zero_run = None
        category = basis = None
        if not observed_values:
            category = 'frontend-only-read'
            basis = ('the pinned frontend reads this word but no reference table '
                     'record exists for it')
        elif value is None:
            category = 'unresolved'
            basis = 'observed values differ across reference occurrences'
        elif value != 0:
            zero_variant = dict(variant)
            zero_tables = [list(table) for table in variant['tables']]
            zero_tables[2] = [(key,
                               0 if ((key >> 32) == 1 and
                                     (key & 0xffffffff) == offset) else val)
                              for key, val in variant['tables'][2]]
            zero_variant['tables'] = zero_tables
            zero_run_dir = stage_dir / f'zero-{offset}'
            run = run_compile('zero', frontend, baseline_code,
                              environment_lines(zero_variant), zero_run_dir, runner)
            if run['returncode'] == 0:
                zero_glsl = read_glsl(zero_run_dir / 'shader-zero.glsl')
                zero_run = {
                    'returncode': 0,
                    'glsl_equal': run['glsl_sha256'] == baseline['glsl_sha256'],
                    'reflection_equal': (
                        texture_signature(run['reflection']) == texture_signature(reflection)
                        and cbuf_signature(run['reflection'], 1) ==
                        cbuf_signature(reflection, 1)),
                    'diff': diff_summary(a_glsl, zero_glsl),
                }
            else:
                zero_run = {'returncode': run['returncode'],
                            'error': run['stderr'][:200]}
        if category is None:
            category, basis, flags = classify_word(offset, value, True, True, zero_run)
        else:
            flags = {
                'observed_by_reference': bool(observed_values),
                'read_by_pinned_frontend': True,
                'non_finite_bits': None,
                'value_is_zero': None if value is None else value == 0,
                'epsilon_default_pattern': None if value is None
                else EPSILON_PATTERN.get(offset) == value,
                'zeroing_changes_reflection': None,
                'zeroing_changes_glsl': None,
                'float_literal_in_folded_line': None,
                'not_differentiated': True,
            }
        finite = None if value is None else is_finite(value)
        if value is None:
            float_value, float_repr = None, None
        else:
            number = f32_from_bits(value)
            float_value = number if math.isfinite(number) else None
            float_repr = ('nan' if math.isnan(number) else
                          'inf' if number == math.inf else
                          '-inf' if number == -math.inf else repr(number))
        bank1_inventory.append({
            'offset': offset,
            'value': value,
            'value_hex': None if value is None else f'0x{value:08x}',
            'float': float_value,
            'float_repr': float_repr,
            'float_finite': finite,
            'observed_values': observed_values,
            'category': category,
            'basis': basis,
            'flags': flags,
            'zero_run': zero_run,
            'pmf_observed': bool(observed_values),
            'epsilon_pattern_match': (None if value is None else
                                       EPSILON_PATTERN.get(offset) == value),
        })
    record_result['bank1_inventory'] = bank1_inventory
    record_result['bank1_categories'] = dict(sorted(Counter(
        row['category'] for row in bank1_inventory).items()))
    return record_result, runs


def summarize(stages, results, reproductions, runtime_evidence, frontend_identity,
              inputs, draw_evidence=None):
    bank1_rows = [row for result in results for row in result.get('bank1_inventory', [])]
    categories = Counter(row['category'] for row in bank1_rows)
    by_offset = Counter(row['offset'] for row in bank1_rows)
    bit_pattern_values = Counter(
        (row['value_hex'], row['category']) for row in bank1_rows
        if row['category'] in ('bit-pattern-carrier', 'bit-pattern-or-integer-input'))
    read_sets = Counter()
    for result in results:
        sets = result.get('bank1_read_sets', {})
        for name in ('both', 'pmf_only', 'frontend_only'):
            read_sets[name] += len(sets.get(name, []))
    status = Counter(result['status'] if 'status' in result else 'compiled'
                     for result in results)
    a_vs_b = Counter()
    for result in results:
        if 'a_vs_b' in result and result['a_vs_b'].get('status') != 'failed':
            pair = result['a_vs_b']
            if pair.get('texture_structure_equal') and \
                    pair.get('constant_buffers_excluding_bank1_equal'):
                a_vs_b['structure-equal-except-bank1'] += 1
            else:
                a_vs_b['structure-differs'] += 1
    types = Counter(result.get('type_mapping', {}).get('status', 'absent')
                    for result in results)
    formats = Counter(result.get('format_analysis', {}).get('status', 'absent')
                      for result in results)
    unresolved = [result['shader_id'] for result in results
                  if result.get('status') == 'frontend-failed'
                  or result.get('type_mapping', {}).get('status') == 'unresolved'
                  or result.get('format_analysis', {}).get('status') == 'unresolved'
                  or result.get('bank1_read_sets', {}).get('frontend_only')]
    return {
        'schema': 1,
        'scope': 'S1/A2-R host cross-check; no target execution, pair linking or pixels',
        'inputs': inputs,
        'frontend': frontend_identity,
        'observed': {
            'stages': len(stages),
            'bank1_stages': sum(1 for s in stages.values() if s['bank1_offsets']),
            'type_query_stages': sum(1 for s in stages.values() if s['type_values']),
            'format_query_stages': sum(1 for s in stages.values() if s['format_values']),
            'stages_with_multiple_variants': sum(
                1 for s in stages.values() if s['variant_count'] > 1),
            'bank1_runtime_contract_stages': sum(
                1 for result in results if result.get('bank1_inventory')),
        },
        'compiled': dict(sorted(status.items())),
        'a_vs_b': dict(sorted(a_vs_b.items())),
        'bank1_read_sets': dict(sorted(read_sets.items())),
        'bank1_categories': dict(sorted(categories.items())),
        'bank1_words_by_offset': {str(k): v for k, v in sorted(by_offset.items())},
        'bank1_epsilon_pattern_matches': sum(
            1 for row in bank1_rows if row.get('epsilon_pattern_match')),
        'bank1_non_finite_words': sum(
            1 for row in bank1_rows if row.get('flags', {}).get('non_finite_bits')),
        'bank1_structure_affecting_words': sum(
            1 for row in bank1_rows
            if row.get('flags', {}).get('zeroing_changes_reflection')),
        'bank1_bit_pattern_values': {
            f'{value}:{category}': count
            for (value, category), count in sorted(bit_pattern_values.items())},
        'type_mapping': dict(sorted(types.items())),
        'format_analysis': dict(sorted(formats.items())),
        'capture_reproduction': reproductions,
        'runtime_bank1_evidence': runtime_evidence,
        'draw_capture_evidence': draw_evidence or {'available': False},
        'unresolved_stages': sorted(set(unresolved)),
        'evidence': {
            'observed-proven': [
                {'item': 'PMF environment records (raw four tables and seven '
                         'words, full SPK identity, session GPU VAs excluded)',
                 'count': len(stages)},
                {'item': 'bank-1 read-set closure (reference offsets == pinned '
                         'frontend reads)',
                 'count': sum(1 for result in results
                              if not result.get('bank1_read_sets', {}).get(
                                  'frontend_only')
                              and not result.get('bank1_read_sets', {}).get(
                                  'pmf_only'))},
                {'item': 'bank-1 value inventory (word categories and per-word '
                         'zero-run sensitivity)',
                 'count': sum(1 for result in results
                              if result.get('bank1_inventory'))},
                {'item': 'type-handle join to A2 descriptors',
                 'count': types.get('mapped', 0) +
                          types.get('mapped-with-handle-aliasing', 0)},
                {'item': 'stages with no type query (explicitly not applicable)',
                 'count': types.get('no-type-queries', 0)},
                {'item': 'observed pixel-format rows (all non-integer under the '
                         'reference predicate)',
                 'count': formats.get('format-neutral', 0)},
                {'item': 'stages with no format query (explicitly not applicable)',
                 'count': formats.get('no-format-queries', 0)},
                {'item': 'committed playable-capture windows reproduced',
                 'count': reproductions.get('records_checked', 0)},
            ],
            'statically-derived': [
                {'item': 'bank-1 is a live guest uniform in the port, not a '
                         'compile-time constant',
                 'count': sum(1 for result in results
                              if result.get('bank1_inventory')),
                 'basis': 'guest_svc.c stores the raw guest CB pointer; '
                          'gxm_runtime.c passes the bank bytes to '
                          'sceGxmSetUniformDataF; A/B structure is equal except '
                          'bank 1'},
                {'item': 'pixel-format integer predicate from the reference '
                         'source, not the pinned frontend',
                 'count': formats.get('format-neutral', 0),
                 'basis': 'the pinned shader-compiler-rs has no format query; '
                          'S8_UINT_D24_UNORM and D32_FLOAT are not integer'},
                {'item': 'program-block bank 1 structure in the local Maxwell '
                         'draw capture',
                 'count': (draw_evidence or {}).get('bank1_bindings', 0),
                 'basis': (draw_evidence or {}).get('note', 'draw capture absent')},
                {'item': 'user uniform-buffer bank 3+ structure in the local '
                         'Maxwell draw capture',
                 'count': (draw_evidence or {}).get('user_bank_bindings', 0),
                 'basis': (draw_evidence or {}).get('note', 'draw capture absent')},
            ],
            'unresolved': [
                {'item': 'stage-level environment closure',
                 'count': len(unresolved),
                 'basis': 'frontend failure, unresolved type/format mapping or '
                          'a frontend-only bank-1 read'},
                {'item': 'NVN slot id to shader bank local join',
                 'count': 1 if (draw_evidence or {}).get('available') else 0,
                 'basis': 'the draw capture records Maxwell stage/bank and sizes '
                          'but not NVN operation slot ids; the name-to-slot join '
                          'is reference-source derived only'},
                {'item': 'target NaN-payload and subnormal preservation through '
                         'sceGxmSetUniformDataF',
                 'count': 1,
                 'basis': 'open: source handoff is proven, no target execution'},
            ],
        },
        'limitations': [
            'A/P/B are host GLSL compilations with the pinned frontend; no shader was executed and no pixel was compared.',
            'The PMF records one reference session. Bank-1 values are stable per stage inside it, but the port still binds bank 1 from live guest memory every draw.',
            'Types are joined per draw through the PMF cbuf raw handle (318 exact, 1 aliased); raw NVN handles and synthetic A2 handles are not themselves numerically joinable, and 110 stages make no type query.',
            'The format table is interpreted through the reference integer predicate; S8_UINT_D24_UNORM is not integer there.',
            'sceGxmSetUniformDataF NaN payload/subnormal preservation is not established on the target.',
            'No GXM pair creation, runtime hit, performance or hardware claim is made.',
        ],
        'runtime_verified': False,
        'pixel_verified': False,
        'hardware_tested': False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--resolved', type=Path, required=True,
                        help='A2 resolved environment directory (vp1)')
    parser.add_argument('--artifact-root', type=Path, required=True,
                        help='A2 artifact root holding resolved-vp0/audit-resolved')
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--frontend', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--validation', type=Path)
    parser.add_argument('--capture-resources', type=Path)
    parser.add_argument('--draw-capture', type=Path,
                        help='optional locally captured draws.jsonl (or its directory)')
    parser.add_argument('--ledger', type=Path,
                        help='A3 stage ledger (vp1), for status columns')
    parser.add_argument('--limit', type=int, default=0)
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--reuse', action='store_true',
                        help='reuse an existing output directory')
    args = parser.parse_args()

    library = args.library.resolve()
    output = args.output.resolve()
    if output == library or output.is_relative_to(library):
        raise CrosscheckError('output must not modify the supplied library')
    if output.exists() and any(output.iterdir()) and not args.reuse:
        raise CrosscheckError('use a new output directory or pass --reuse')
    frontend = args.frontend.resolve()
    if not frontend.is_file():
        raise CrosscheckError('frontend binary is missing')

    spk, entries, shaders, extraction = load_library(library)
    header, stages, occurrences = collect_observations(args.manifest, spk, entries, shaders)
    finalize_stages(stages)

    resolved_rows = read_jsonl(args.resolved / 'resolution.jsonl')
    resolved_by_id = {row['shader_id']: row for row in resolved_rows}
    ledger = None
    if args.ledger:
        ledger = {row['shader_id']: row for row in read_jsonl(args.ledger)}

    selected = sorted(stages.values(), key=lambda s: s['shader_id'])
    if args.limit:
        selected = selected[:args.limit]

    build = read_json(frontend.parent / 'build.json') if (frontend.parent / 'build.json').is_file() else None
    frontend_identity = {'path': str(frontend), 'sha256': digest(frontend),
                         'build': build}

    output.mkdir(parents=True, exist_ok=True)
    selected_dir = output / 'stages'
    selected_dir.mkdir(exist_ok=True)

    def work(record):
        entry = entries[record['representative']['pmf_offsets'][0]]
        result, _ = compile_record(record, entry, spk, args.resolved, frontend,
                                   selected_dir)
        resolved = resolved_by_id.get(record['shader_id'])
        result['type_mapping'] = type_mapping(record, resolved)
        result['format_analysis'] = format_analysis(record)
        if ledger is not None and record['shader_id'] in ledger:
            row = ledger[record['shader_id']]
            result['a3_status'] = {
                'cg_lowering': row.get('cg_lowering'),
                'gxp_status': row.get('gxp_status'),
                'pipeline_linked': row.get('pipeline_linked'),
                'pixel_verified': row.get('pixel_verified'),
            }
        else:
            result['a3_status'] = {'cg_lowering': None, 'gxp_status': None,
                                   'pipeline_linked': None,
                                   'pixel_verified': None}
        return result

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {pool.submit(work, record): record for record in selected}
        results = []
        for future in futures:
            results.append(future.result())
    results.sort(key=lambda r: r['shader_id'])

    # Freeze the observation ledger beside the compile results.
    observation_rows = []
    for record in sorted(stages.values(), key=lambda s: s['shader_id']):
        observation_rows.append({
            'schema': 1,
            'shader_id': record['shader_id'],
            'stage': record['stage'],
            'origin': record['origin'],
            'maxwell_bytes': record['maxwell_bytes'],
            'maxwell_sha256': record['maxwell_sha256'],
            'sph_sha256': record['sph_sha256'],
            'occurrences': record['occurrences'],
            'pipeline_indices': record['pipeline_indices'],
            'variant_count': record['variant_count'],
            'representative_environment_key': record['representative_environment_key'],
            'bank1_offsets': record['bank1_offsets'],
            'bank1_values': record['bank1_values'],
            'bank1_stable_across_occurrences': record['bank1_stable_across_occurrences'],
            'observed_types': record['observed_types'],
            'observed_formats': record['observed_formats'],
            'type_queries_stable': record['type_queries_stable'],
            'identity': 'full canonical SPK stage',
        })
    variant_rows = []
    for record in sorted(stages.values(), key=lambda s: s['shader_id']):
        for variant in record['variants']:
            variant_rows.append({
                'schema': 1,
                'shader_id': record['shader_id'],
                'environment_key': variant['environment_key'],
                'count': variant['count'],
                'pipeline_indices': variant['pipe_indices'],
                'pmf_offsets': variant['pmf_offsets'],
                'program_sizes': variant['program_sizes'],
                'code_sizes': variant['code_sizes'],
                'words': variant['words'],
                'table_rows': [len(t) for t in variant['tables']],
                'tables': [list(map(list, t)) for t in variant['tables']],
                'note': ('session GPU addresses words[2..4] excluded from the key; '
                         'texture handle rows can differ per observed draw'),
            })
    write_jsonl(output / 'observed-stages.jsonl', observation_rows)
    write_jsonl(output / 'observed-environments.jsonl', variant_rows)
    write_jsonl(output / 'observed-occurrences.jsonl', occurrences)
    write_jsonl(output / 'compile-results.jsonl',
                [{k: v for k, v in result.items() if k != 'bank1_inventory'}
                 for result in results])
    write_jsonl(output / 'bank1-inventory.jsonl',
                [{**row, 'shader_id': result['shader_id']}
                 for result in results for row in result.get('bank1_inventory', [])])
    contract_rows = []
    for result in results:
        words = result.get('bank1_inventory', [])
        if not words:
            continue
        contract_rows.append({
            'schema': 1,
            'shader_id': result['shader_id'],
            'stage': result['stage'],
            'representative_environment_key': result['representative_environment_key'],
            'binding': {
                'shader_bank': 1,
                'kind': 'runtime uniform, not compile-time constant',
                'source': 'guest Maxwell constant buffer bank 1 at draw time',
                'upload': 'raw u32 bits; no float numeric conversion',
                'do_not_freeze': True,
            },
            'words': [{
                'offset': row['offset'],
                'value': row['value'],
                'value_hex': row['value_hex'],
                'float_repr': row['float_repr'],
                'category': row['category'],
                'basis': row['basis'],
            } for row in words],
            'note': ('PMF observations are one reference session. The port must '
                     'bind the live guest bank; these values are the expected '
                     'contract only when the draw supplies the same words.'),
        })
    write_jsonl(output / 'bank1-runtime-contract.jsonl', contract_rows)
    failures = []
    for result in results:
        reasons = []
        if result.get('status') == 'frontend-failed':
            reasons.append('frontend-failed')
        if result.get('type_mapping', {}).get('status') == 'unresolved':
            reasons.append('type-query-set-differs')
        if result.get('format_analysis', {}).get('status') == 'unresolved':
            reasons.append('format-sensitive')
        if result.get('bank1_read_sets', {}).get('frontend_only'):
            reasons.append('frontend-read-not-observed')
        if result.get('a_vs_b', {}).get('status') == 'failed':
            reasons.append('port-environment-failed')
        if result.get('a_vs_b', {}).get('texture_structure_equal') is False:
            reasons.append('texture-structure-differs')
        if reasons:
            failures.append({'shader_id': result['shader_id'], 'reasons': reasons})
    write_jsonl(output / 'failures.jsonl', failures)

    reproductions = capture_reproduction(args.artifact_root, args.root)
    runtime_evidence = runtime_bank1_evidence(args.root, args.capture_resources)
    draw_evidence = draw_capture_evidence(args.draw_capture)
    inputs = {
        'manifest_sha256': digest(args.manifest),
        'manifest_bytes': Path(args.manifest).stat().st_size,
        'spk_sha256': sha(spk),
        'library_extraction_sha256': digest(library / 'extraction.json'),
        'library_stage_entries_sha256': digest(library / 'stage_entries.jsonl'),
        'resolved_resolution_sha256': digest(args.resolved / 'resolution.jsonl'),
        'ledger_sha256': digest(args.ledger) if args.ledger else None,
        'audit_tool_sha256': digest(Path(manifest_audit.__file__)),
        'tool_sha256': digest(Path(__file__)),
    }
    summary = summarize(stages, results, reproductions, runtime_evidence,
                        frontend_identity, inputs, draw_evidence)
    summary['pmf_header'] = header
    summary['outputs'] = {
        name: digest(output / name) for name in
        ('observed-stages.jsonl', 'observed-environments.jsonl',
         'observed-occurrences.jsonl', 'compile-results.jsonl',
         'bank1-inventory.jsonl', 'bank1-runtime-contract.jsonl',
         'failures.jsonl')
    }
    write_json(output / 'summary.json', summary)
    if args.validation:
        write_json(args.validation, summary)
    print(json.dumps({
        'stages': len(stages),
        'compiled': summary['compiled'],
        'bank1_categories': summary['bank1_categories'],
        'type_mapping': summary['type_mapping'],
        'format_analysis': summary['format_analysis'],
        'unresolved': len(summary['unresolved_stages']),
        'capture_reproduction': reproductions.get('all_matched'),
    }, ensure_ascii=False))
    if summary['compiled'].get('frontend-failed'):
        raise SystemExit(1)


if __name__ == '__main__':
    main()

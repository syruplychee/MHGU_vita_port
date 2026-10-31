"""Read-only SPMF v1 audit against this port's exact SPK stage inventory.

The format is documented by SUYU-GURecomp/tools/a32recomp/pipeline_manifest.py.
This reader does not import upstream code or instantiate an emulator. Raw query
tables stay in the supplied manifest; the output contains identities and counts.
PMF offsets join to canonical SPK entries, never to a guessed creation order.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import zlib


class ManifestError(ValueError):
    pass


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, size):
        if size < 0 or size > len(self.data) - self.pos:
            raise ManifestError('truncated manifest')
        result = self.data[self.pos:self.pos + size]
        self.pos += size
        return result

    def unpack(self, fmt):
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))

    def string(self):
        size, = self.unpack('<H')
        try:
            return self.take(size).decode('utf-8')
        except UnicodeDecodeError as exc:
            raise ManifestError('invalid source UTF-8') from exc


def parse(data, spk):
    r = Reader(data)
    if r.take(4) != b'SPMF':
        raise ManifestError('invalid magic')
    version, cache, key_size, size, crc, count = r.unpack('<6I')
    if version != 1 or key_size != 1056:
        raise ManifestError('unsupported version or key size')
    if size != len(spk) or crc != zlib.crc32(spk):
        raise ManifestError('SPK identity mismatch')
    if count > len(data) // (4 + key_size):
        raise ManifestError('impossible pipeline count')
    header = dict(version=version, cache_version=cache, key_size=key_size,
                  spk_bytes=size, spk_crc32=f'{crc:08x}', pipeline_count=count,
                  archive=r.string(), entry=r.string())
    pipes = []
    for _ in range(count):
        stage_count, = r.unpack('<I')
        if stage_count not in (1, 2):
            raise ManifestError('unsupported stage count')
        stages = []
        for _ in range(stage_count):
            offset, code_size, program_size, same = r.unpack('<IIIB')
            counts = r.unpack('<4Q')
            words = r.unpack('<7I')
            # Shader::Stage::VertexB = 0, Fragment = 4 (not SPH's 1/5).
            if words[6] not in (0, 4) or same not in (0, 1):
                raise ManifestError('unsupported stage or header flag')
            if program_size < 80 or program_size > size or offset > size - program_size:
                raise ManifestError('SPK code range out of bounds')
            if code_size not in (program_size, program_size + 16):
                raise ManifestError('unexpected terminator extent')
            tables = []
            for n, fmt in zip(counts, ('<II', '<II', '<QI', '<QI')):
                row_size = struct.calcsize(fmt)
                blob = r.take(n * row_size)
                table = list(struct.iter_unpack(fmt, blob))
                if len({k for k, _ in table}) != n:
                    raise ManifestError('duplicate environment query key')
                tables.append(table)
            sph = spk[offset:offset + 80] if same else r.take(80)
            stages.append(dict(offset=offset, code_size=code_size,
                               program_size=program_size, words=words,
                               tables=tables, sph_sha256=sha(sph)))
        key = r.take(key_size)
        pipes.append((stages, sha(key)))
    if r.pos != len(data):
        raise ManifestError('trailing manifest bytes')
    return header, pipes


def sha(data):
    return hashlib.sha256(data).hexdigest()


def rows(path):
    return [json.loads(line) for line in Path(path).read_text(encoding='utf-8-sig').splitlines()
            if line.strip()]


def audit(manifest, library, ledger=None, details_path=None):
    library = Path(library)
    data = Path(manifest).read_bytes()
    spk = (library / 'source/root.spkg').read_bytes()
    expected = json.loads((library / 'extraction.json').read_text())['spk_sha256']
    if sha(spk) != expected:
        raise ManifestError('library SPK SHA256 mismatch')
    header, pipes = parse(data, spk)
    entries = {e['spk_sph_offset']: e for e in rows(library / 'stage_entries.jsonl')}
    shaders = {e['id']: e for e in rows(library / 'shaders.jsonl')}
    programs = rows(library / 'programs.jsonl')
    catalog_pairs = {(p['vertex_shader'], p['fragment_shader']) for p in programs}
    identities, pairs, envs = set(), set(), set()
    types, formats, banks, bank1_offsets, words_count = (Counter() for _ in range(5))
    extents, stage_kinds = Counter(), Counter()
    cb1_stages, type_stages, raw_cb1_values = set(), set(), set()
    cb1_reads, replacements, terminators, raw_headers = 0, 0, 0, 0
    for stages, key in pipes:
        pair = []
        for stage in stages:
            e = entries.get(stage['offset'])
            if e is None:
                raise ManifestError('manifest offset is not a catalog SPH boundary')
            sid = e['shader_id']
            canonical = spk[stage['offset']:stage['offset'] + e['maxwell_bytes']]
            if sha(canonical) != shaders[sid]['sha256']:
                raise ManifestError('canonical stage SHA mismatch')
            if (e['stage'], stage['words'][6]) not in (('vert', 0), ('frag', 4)):
                raise ManifestError('stage kind mismatch')
            if stage['program_size'] > e['maxwell_bytes']:
                raise ManifestError('PMF extent exceeds canonical stage')
            pair.append(sid)
            identities.add(sid)
            stage_kinds[e['stage']] += 1
            extents['equal' if stage['program_size'] == e['maxwell_bytes'] else 'shorter_prefix'] += 1
            terminators += stage['code_size'] != stage['program_size']
            raw_headers += stage['sph_sha256'] != sha(canonical[:80])
            tt, tf, cb, cr = stage['tables']
            types.update(value for _, value in tt)
            formats.update(value for _, value in tf)
            for k, value in cb:
                bank, offset = k >> 32, k & 0xffffffff
                banks[bank] += 1
                if bank == 1:
                    cb1_stages.add(sid)
                    cb1_reads += 1
                    bank1_offsets[offset] += 1
                    raw_cb1_values.add((offset, value))
            if tt:
                type_stages.add(sid)
            replacements += len(cr)
            # Raw environment count preserves session addresses and handles;
            # neither it nor the bound/viewport count is an executable variant key.
            words_count[(stage['words'][1], stage['words'][5])] += 1
            envs.add((sid, stage['words'], tuple(tuple(t) for t in stage['tables']), stage['sph_sha256']))
        pairs.add(tuple(pair))
    result = dict(schema=1, scope='offline manifest/source audit; no target execution',
                  manifest_sha256=sha(data), manifest_bytes=len(data), spk_sha256=sha(spk),
                  header=header, stage_occurrences=sum(stage_kinds.values()),
                  unique_stages=len(identities), unique_stages_by_kind=dict(sorted(Counter(shaders[s]['stage'] for s in identities).items())),
                  unique_pairs=len(pairs), pairs_in_spk_catalog=len(pairs & catalog_pairs),
                  unique_raw_environments=len(envs), extent_counts=dict(extents),
                  terminator_occurrences=terminators, distinct_header_occurrences=raw_headers,
                  texture_type_values=dict(sorted(types.items())), texture_format_values=dict(sorted(formats.items())),
                  stages_with_texture_type_queries=len(type_stages), cbuf_reads_by_bank=dict(sorted(banks.items())),
                  bank1_query_occurrences=cb1_reads, bank1_unique_stages=len(cb1_stages),
                  bank1_reads_by_offset=dict(sorted(bank1_offsets.items())),
                  bank1_distinct_offset_value_pairs=len(raw_cb1_values), replacement_query_occurrences=replacements,
                  bound_viewport_occurrences={f'{b}:{v}': n for (b, v), n in sorted(words_count.items())},
                  runtime_verified=False, pixel_verified=False, hardware_tested=False)
    if ledger:
        candidates = {e['shader_id']: e for e in rows(ledger)}
        missing = identities - candidates.keys()
        if missing:
            raise ManifestError('ledger lacks observed stages')
        result['ledger_sha256'] = sha(Path(ledger).read_bytes())
        result['ledger_input'] = Path(ledger).as_posix()
        result['observed_cg_lowering'] = dict(sorted(Counter(candidates[s]['cg_lowering'] for s in identities).items()))
        result['observed_gxp_status'] = dict(sorted(Counter(candidates[s]['gxp_status'] for s in identities).items()))
        result['pairs_with_both_gxp'] = sum(all(candidates[s]['gxp_status'] == 'compiled' for s in pair)
                                                for pair in pairs)
        result['observed_cg_rejections'] = dict(sorted(Counter(candidates[s]['cg_error'] for s in identities
                                                       if candidates[s]['cg_lowering'] != 'accepted').items()))
    if details_path:
        details = dict(schema=1, manifest_sha256=sha(data),
                       stages=sorted(identities), pairs=[list(p) for p in sorted(pairs)],
                       bank1_query_stages=sorted(cb1_stages),
                       notice='Reference observations, not a complete-game or executable variant manifest')
        details_path = Path(details_path)
        details_path.parent.mkdir(parents=True, exist_ok=True)
        details_path.write_text(json.dumps(details, indent=2) + '\n', encoding='utf-8')
        result['details_sha256'] = sha(details_path.read_bytes())
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--library', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--ledger', type=Path)
    p.add_argument('--details-out', type=Path)
    a = p.parse_args()
    result = audit(a.manifest, a.library, a.ledger, a.details_out)
    result['tool_sha256'] = sha(Path(__file__).read_bytes())
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    main()

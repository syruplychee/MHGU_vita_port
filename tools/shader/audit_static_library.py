"""Build a verified, offline shader catalogue from the extracted MHGU library.

Reads the supplied library and main audit without changing either. Outputs are
local build artifacts, not an installable GXM bundle: missing specialization,
vertex layout, compiler and pixel checks remain explicit. Captured 4096-byte
windows are checked against exact SPK program lengths, never used as content IDs.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import zlib

from shader_ir import Unsupported, emit_cg, lower
from translate_captured import normalize_glsl


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def read_jsonl(path):
    return [json.loads(line) for line in path.read_text(encoding='utf-8-sig').splitlines() if line.strip()]


def source_file(root, name):
    path = (root / name).resolve()
    if not path.is_relative_to(root) or not path.is_file():
        raise ValueError('invalid catalogue source path: ' + str(name))
    return path


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def write_jsonl(path, values):
    with path.open('w', encoding='utf-8', newline='\n') as stream:
        for value in values:
            stream.write(json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--main-audit', type=Path, required=True)
    parser.add_argument('--romfs-root', type=Path, required=True, help='Locked title RomFS tree used to verify root.arc and its decompressed SPK')
    parser.add_argument('--sdk-input', type=Path, required=True, help='Original locked SDK NSO for the auxiliary windows identity')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--capture', type=Path, action='append', default=[])
    parser.add_argument('--resolved-environments', type=Path,
        help='A2 resolver output; when supplied, use its GLSL and zero-missing environments')
    parser.add_argument('--emit-cg', action='store_true', help='Emit candidates only when no environment query is missing')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    library, audit = args.library.resolve(), args.main_audit.resolve()
    output = args.output.resolve()
    if output == library or output.is_relative_to(library) or output == audit or output.is_relative_to(audit):
        raise ValueError('output must not modify either supplied source directory')
    if output.exists() and any(output.iterdir()):
        raise ValueError('use a new or empty output directory; stale success artifacts are rejected')
    output.mkdir(parents=True, exist_ok=True)

    target = read_json(repo / 'config/target-1.4.0.json')
    expected = next(m for m in target['modules'] if m['name'] == 'main')
    main_path = source_file(audit, 'input/main.nso')
    main_bytes = main_path.read_bytes()
    if digest(main_path) != expected['sha256'] or main_bytes[0x40:0x60].hex() != expected['build_id']:
        raise ValueError('main audit is not the locked 1.4.0 input')
    metadata = read_json(audit / 'source-metadata.json')
    if metadata['input']['sha256'] != expected['sha256']:
        raise ValueError('main audit metadata identity mismatch')
    expected_sdk = next(m for m in target['modules'] if m['name'] == 'sdk')
    sdk_bytes = args.sdk_input.read_bytes()
    if hashlib.sha256(sdk_bytes).hexdigest() != expected_sdk['sha256'] or sdk_bytes[0x40:0x60].hex() != expected_sdk['build_id']:
        raise ValueError('SDK input is not the locked module')

    extraction = read_json(library / 'extraction.json')
    spk = source_file(library, 'source/root.spkg')
    spk_sha = digest(spk)
    if not extraction['sources'] or any(s['spk_sha256'] != spk_sha for s in extraction['sources']):
        raise ValueError('extracted SPK identity mismatch')
    arc_path = source_file(args.romfs_root.resolve(), 'nativeNX/sa/NX/root.arc')
    arc = arc_path.read_bytes()
    source = next(s for s in extraction['sources'] if s['version'] == 'update')
    arc_sha = hashlib.sha256(arc).hexdigest()
    if arc_sha != source['arc_sha256']:
        raise ValueError('library source ARC does not match the title RomFS')
    start, length = source['compressed_offset'], source['compressed_size']
    if start < 0 or length <= 0 or start + length > len(arc):
        raise ValueError('SPK compressed range is outside source ARC')
    if hashlib.sha256(zlib.decompress(arc[start:start + length])).hexdigest() != spk_sha:
        raise ValueError('SPK bytes do not match the source ARC payload')

    entries = read_jsonl(library / 'all_shaders.jsonl')
    programs = read_jsonl(library / 'programs_mapping.jsonl')
    stage_entries = read_jsonl(library / 'stage_mapping.jsonl')
    requirements = read_json(library / 'runtime_requirements.json')
    dependencies = {}
    for key, shader_ids in requirements['requests'].items():
        for shader_id in shader_ids:
            dependencies.setdefault(shader_id, []).append(key)
    by_id = {}
    for number, entry in enumerate(entries, 1):
        shader_id = entry['shader_id']
        if shader_id in by_id or not re.fullmatch(r'(vert|frag)_(sdk_)?[0-9a-f]+', shader_id):
            raise ValueError('duplicate or invalid shader id: ' + shader_id)
        by_id[shader_id] = entry
        for kind in ('maxwell', 'glsl', 'spirv'):
            if digest(source_file(library, entry[kind])) != entry[kind + '_sha256']:
                raise ValueError(f'{shader_id}: {kind} hash mismatch')
        reflection = source_file(library, entry['reflection'])
        entry['reflection_sha256'] = digest(reflection)
        if number % 300 == 0:
            print(f'verified source hashes {number}/{len(entries)}', flush=True)

    overlay = None
    overlay_root = None
    if args.resolved_environments:
        overlay_root = args.resolved_environments.resolve()
        overlay_summary_path = overlay_root / 'summary.json'
        if not overlay_summary_path.is_file():
            raise ValueError('resolved environment summary is missing')
        overlay_summary = read_json(overlay_summary_path)
        if overlay_summary.get('schema') != 1:
            raise ValueError('unsupported resolved environment schema')
        if overlay_summary['counts']['failed'] != 0:
            raise ValueError('resolved environment run has failures')
        if overlay_summary['counts']['resolved'] != len(entries):
            raise ValueError('resolved environment shader count mismatch')
        overlay_records = {r['shader_id']: r for r in read_jsonl(overlay_root / 'resolution.jsonl')}
        if len(overlay_records) != len(entries):
            raise ValueError('duplicate or missing resolved environment records')
        overlay = {
            'root': overlay_root,
            'summary': overlay_summary,
            'summary_sha256': digest(overlay_summary_path),
            'resolution_sha256': digest(overlay_root / 'resolution.jsonl'),
            'records': overlay_records,
        }
        for shader_id, entry in by_id.items():
            record = overlay_records.get(shader_id)
            if record is None or record['maxwell_sha256'] != entry['maxwell_sha256']:
                raise ValueError('resolved environment does not match shader content: ' + shader_id)
            resolved = record['resolved']
            missing_path = overlay_root / shader_id / 'shader.glsl.missing-cbufs.json'
            glsl_path = overlay_root / shader_id / resolved['glsl']
            env_path = overlay_root / shader_id / resolved['environment']
            for path in (missing_path, glsl_path, env_path):
                if not path.is_file():
                    raise ValueError('resolved environment file is missing: ' + str(path))
            if digest(missing_path) != resolved['missing_sha256']:
                raise ValueError('resolved missing sidecar hash mismatch: ' + shader_id)
            if digest(glsl_path) != resolved['glsl_sha256']:
                raise ValueError('resolved GLSL hash mismatch: ' + shader_id)
            if digest(env_path) != resolved['environment_sha256']:
                raise ValueError('resolved environment hash mismatch: ' + shader_id)
            missing = read_json(missing_path)
            if missing['count'] or missing['texture_types'] or missing['texture_formats']:
                raise ValueError('resolved environment still has missing queries: ' + shader_id)
        print(f'verified resolved environments {len(overlay_records)}/{len(entries)}', flush=True)

    token_records = []
    by_index = {}
    for program in programs:
        index = program['index']
        if index in by_index or not isinstance(index, int) or index < 0:
            raise ValueError('duplicate or invalid program index')
        by_index[index] = program
        for kind in ('vertex', 'fragment'):
            shader_id = program.get(kind + '_shader')
            if shader_id is not None and shader_id not in by_id:
                raise ValueError('unknown shader reference: ' + shader_id)
        token_records.append({
            'candidate_runtime_token': index + 1, 'spk_program_index': index,
            'vertex_shader': program.get('vertex_shader'),
            'fragment_shader': program.get('fragment_shader'),
            'spk_offset': program['spk_offset'], 'raw_words': program['raw_words'],
            'mapping_status': 'index-plus-one rule; captured subset verified separately',
        })
    for entry in stage_entries:
        shader = by_id[entry['shader_id']]
        if entry['maxwell'] != shader['maxwell']:
            raise ValueError('inconsistent stage mapping')

    captures = []
    for folder in args.capture:
        folder = folder.resolve()
        for record in read_json(folder / 'translate.json')['programs']:
            name = record['program']
            match = re.fullmatch(r's([15])_0x([0-9a-f]+)', name)
            if not match:
                raise ValueError('invalid captured program key: ' + name)
            program = by_index[int(match[2], 16) - 1]
            shader_id = program['vertex_shader' if match[1] == '1' else 'fragment_shader']
            canonical = source_file(library, by_id[shader_id]['maxwell']).read_bytes()
            window = source_file(folder, name + '/code.bin').read_bytes()
            if len(window) < len(canonical) or window[:len(canonical)] != canonical:
                raise ValueError('captured token/index/content mismatch: ' + name)
            captures.append({'program': name, 'runtime_token': int(match[2], 16),
                'spk_program_index': program['index'], 'shader_id': shader_id,
                'viewport_transform': record['viewport_transform'],
                'canonical_sha256': by_id[shader_id]['maxwell_sha256'],
                'canonical_bytes': len(canonical), 'capture_bytes': len(window),
                'capture_sha256': hashlib.sha256(window).hexdigest(),
                'verification': 'full canonical bytes equal capture prefix; extra window bytes excluded from identity'})

    counters = Counter()
    failures = Counter()
    catalogue = []
    for number, entry in enumerate(entries, 1):
        shader_id = entry['shader_id']
        conversion_path = source_file(library, str(Path(entry['maxwell']).parent / 'conversion.json'))
        conversion = read_json(conversion_path)
        library_environment_path = source_file(
            library, str(Path(entry['maxwell']).parent / 'environment.txt'))
        library_environment_sha = digest(library_environment_path)
        if 'environment_sha256' in conversion and library_environment_sha != conversion['environment_sha256']:
            raise ValueError('environment hash mismatch: ' + shader_id)
        if entry['specialization_required'] != conversion['specialization_required']:
            raise ValueError('specialization status mismatch: ' + shader_id)
        if entry['origin'] == 'SDK embedded shader window' and conversion['source']['module_sha256'] != expected_sdk['sha256']:
            raise ValueError('SDK window source identity mismatch: ' + shader_id)
        if overlay:
            record = overlay['records'][shader_id]
            glsl_path = overlay['root'] / shader_id / record['resolved']['glsl']
            resources_path = overlay['root'] / shader_id / 'shader.glsl.resources.json'
            environment_path = overlay['root'] / shader_id / record['resolved']['environment']
            missing_path = overlay['root'] / shader_id / 'shader.glsl.missing-cbufs.json'
            resources = read_json(resources_path)
            resolved_missing = read_json(missing_path)
            if 'resources' in conversion and conversion['resources'] != resources:
                library_resources_match = False
            else:
                library_resources_match = True
            normalized, hoists = normalize_glsl(glsl_path.read_text(encoding='utf-8'))
            specialization_required = False
            result = {**entry, 'missing_environment_keys': [],
                'conversion_sha256': digest(conversion_path),
                'environment_sha256': digest(environment_path),
                'library_environment_sha256': library_environment_sha,
                'environment_source': 'resolved',
                'resolution_sha256': digest(overlay['root'] / shader_id / 'resolution.json'),
                'missing_runtime_data': resolved_missing,
                'resources': resources, 'resources_sha256': digest(resources_path),
                'increment_hoists': hoists,
                'library_resources_match': library_resources_match,
                'library_specialization_required': entry['specialization_required'],
                'specialization_required': False,
                'gxp_compiled': False, 'pipeline_linked': False, 'pixel_verified': False}
            counters['resolved_environment'] += 1
        else:
            resources_path = source_file(library, entry['glsl'] + '.resources.json')
            resources = read_json(resources_path)
            if 'resources' in conversion and conversion['resources'] != resources:
                raise ValueError('resource sidecar mismatch: ' + shader_id)
            normalized, hoists = normalize_glsl(source_file(library, entry['glsl']).read_text(encoding='utf-8'))
            specialization_required = entry['specialization_required']
            result = {**entry, 'missing_environment_keys': dependencies.get(shader_id, []),
                'conversion_sha256': digest(conversion_path),
                'environment_sha256': library_environment_sha,
                'library_environment_sha256': library_environment_sha,
                'environment_source': 'library',
                'missing_runtime_data': conversion['missing_runtime_data'],
                'resources': resources, 'resources_sha256': digest(resources_path), 'increment_hoists': hoists,
                'library_specialization_required': entry['specialization_required'],
                'specialization_required': specialization_required,
                'gxp_compiled': False, 'pipeline_linked': False, 'pixel_verified': False}
        try:
            ir = lower(normalized, 'vertex' if entry['stage'] == 'vert' else 'fragment')
            cg, ir_info = emit_cg(ir)
            result['cg_lowering'] = 'accepted'
            result['cg_sha256'] = hashlib.sha256(cg.encode()).hexdigest()
            result['cg_metadata'] = ir_info
            result['lowering_notes'] = ir['lowering_notes']
            if args.emit_cg and not specialization_required:
                cg_path = output / 'cg-candidates' / (shader_id + '.cg')
                cg_path.parent.mkdir(exist_ok=True)
                cg_path.write_text(cg, encoding='utf-8', newline='\n')
                result['cg_candidate'] = cg_path.relative_to(output).as_posix()
                counters['cg_candidates_emitted'] += 1
            counters['cg_accepted'] += 1
        except Unsupported as error:
            result['cg_lowering'] = 'unsupported'
            result['cg_error'] = str(error)
            failures[str(error)] += 1
            counters['cg_unsupported'] += 1
        counters['needs_environment' if specialization_required else 'no_missing_environment_query'] += 1
        catalogue.append(result)
        if number % 300 == 0:
            print(f'checked {number}/{len(entries)} shaders', flush=True)

    write_jsonl(output / 'shader-catalogue.jsonl', catalogue)
    write_jsonl(output / 'program-map.jsonl', token_records)
    write_jsonl(output / 'capture-map.jsonl', captures)
    summary = {'schema': 1, 'scope': 'offline source identity, content mapping and current Cg lowering gate',
        'target_main': expected, 'target_sdk': expected_sdk,
        'main_audit_input_sha256': digest(main_path),
        'source_spk_sha256': spk_sha, 'source_arc_sha256': arc_sha,
        'source_arc': str(arc_path), 'library': str(library),
        'source_manifests': {name: digest(library / name) for name in
            ('all_shaders.jsonl', 'programs_mapping.jsonl', 'stage_mapping.jsonl',
             'runtime_requirements.json', 'toolchain.json', 'extraction.json')},
        'tool_sha256': digest(Path(__file__)),
        'shader_ir_sha256': digest(repo / 'tools/shader/shader_ir.py'),
        'normalizer_sha256': digest(repo / 'tools/shader/translate_captured.py'),
        'shader_count': len(entries), 'program_count': len(programs),
        'stage_entry_count': len(stage_entries),
        'unique_pairs': len({(p.get('vertex_shader'),p.get('fragment_shader')) for p in programs}),
        'capture_count': len(captures),
        'captured_unique_content': len({c['canonical_sha256'] for c in captures}),
        'counts': dict(counters), 'unsupported_reasons': dict(failures.most_common()),
        'outputs': {name: digest(output / name) for name in
            ('shader-catalogue.jsonl', 'program-map.jsonl', 'capture-map.jsonl')},
        'installable': False, 'gxm_compiler_verified': False, 'semantic_equivalence_verified': False,
        'limitations': ['Cg acceptance is a syntax/resource lowering gate, not Shacc compilation or pixel equivalence.',
            'Vertex attribute types, complete specialization signatures and broader pipelines remain to be proven.',
            'Only the observed token subset is verified; all other token=index+1 rows are static candidates.',
            'SDK shader windows have separately recorded boundaries and are not exact SPK entries.']}
    if overlay:
        summary['resolved_environments'] = {
            'root': str(overlay['root']),
            'summary_sha256': overlay['summary_sha256'],
            'resolution_sha256': overlay['resolution_sha256'],
            'texture_bound_buffer': overlay['summary']['texture_bound_buffer'],
            'driver_constant_folding': overlay['summary']['driver_constant_folding'],
            'counts': overlay['summary']['counts'],
        }
        summary['limitations'].insert(0,
            'Resolved environments come from the A2 overlay; bank 1 driver constants remain runtime uniforms, '
            'and texture type queries are answered from instruction descriptors after the recorded corpus census.')
    else:
        summary['limitations'].insert(0,
            'Existing GLSL uses its recorded environment, including fixed viewport=1 and defaulted queries.')
    write_json(output / 'summary.json', summary)
    print(json.dumps({key:summary[key] for key in ('shader_count','program_count','capture_count','counts')},ensure_ascii=False),flush=True)


if __name__ == '__main__':
    main()

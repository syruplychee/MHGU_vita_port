"""Resolve the static shader compile environment from locked library evidence.

The supplied library is read-only. Every shader is translated twice with the
pinned offline frontend:

1. discovery: synthetic texture handles are supplied, but no bound-type table;
   the frontend reports exactly which handles it asks ReadTextureType about.
2. resolution: each descriptor's instruction dimension is bound to its synthetic
   handle as the texture type, and the run must report zero missing queries.

The port runtime binds the guest texture descriptor table through the Maxwell
register, not through the shader's constant-buffer index, so the offline
environment uses the same bound-buffer convention as the existing playable
captures (bank 0). This also leaves the proprietary driver bank (bank 1) as a
normal runtime uniform instead of folding it to zero; see
docs/STATIC_ENVIRONMENT_20261022.md.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

TYPE_NAMES = {
    0: 'Color1D', 1: 'ColorArray1D', 2: 'Color2D', 3: 'ColorArray2D',
    4: 'Color3D', 5: 'ColorCube', 6: 'ColorArrayCube', 7: 'Buffer',
    8: 'Color2DRect',
}
HANDLE_BASE = 0x71000000
HANDLE_STRIDE = 0x100
ENVIRONMENT_FIELDS = 9


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def read_jsonl(path):
    return [json.loads(line) for line in Path(path).read_text(encoding='utf-8-sig').splitlines()
            if line.strip()]


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n',
                          encoding='utf-8', newline='\n')


def write_jsonl(path, values):
    with Path(path).open('w', encoding='utf-8', newline='\n') as stream:
        for value in values:
            stream.write(json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n')


class ResolutionError(Exception):
    pass


def source_file(root, name):
    root = Path(root).resolve()
    path = (root / name).resolve()
    if not path.is_relative_to(root) or not path.is_file():
        raise ResolutionError('invalid source path: ' + str(name))
    return path


def descriptor_plan(entry, resources_path):
    """Return the synthetic-handle plan for every texture descriptor."""
    resources = read_json(resources_path)
    plan = []
    seen = set()
    for index, desc in enumerate(resources['textures']):
        count = desc.get('count', 1)
        shift = desc.get('shift', 0)
        if not isinstance(count, int) or count < 1 or count > 64:
            raise ResolutionError(f'unsupported descriptor count {count} at {index}')
        if shift != 0:
            raise ResolutionError(f'unsupported descriptor shift {shift} at {index}')
        if desc.get('secondary'):
            raise ResolutionError(f'secondary texture descriptor at {index} is not supported')
        for n in range(count):
            offset = desc['offset'] + 4 * n
            handle = HANDLE_BASE | ((index + 1) * HANDLE_STRIDE) | (n + 1)
            key = offset  # bound texture buffer is bank 0 in the port convention
            if key in seen:
                raise ResolutionError(f'duplicate resolved cbuf key 0:{offset}')
            seen.add(key)
            plan.append({
                'index': index, 'array_index': n,
                'instruction_type': desc['type'],
                'instruction_type_name': TYPE_NAMES.get(desc['type'], 'unknown'),
                'source_bank': desc['bank'], 'source_offset': desc['offset'],
                'resolved_bank': 0, 'resolved_offset': offset,
                'count': count, 'shift': shift, 'handle': handle,
            })
    return plan


def write_environment(path, stage, local, cbuf_values, handle_types):
    lines = [f'{stage} 16 {local} 0 1 {len(handle_types)} 0 {len(cbuf_values)} 0']
    for handle, ttype in sorted(handle_types.items()):
        lines.append(f'{handle} {ttype}')
    for key, value in sorted(cbuf_values.items()):
        lines.append(f'{key} {value}')
    Path(path).write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')


def run_frontend(frontend, code, environment, glsl):
    return subprocess.run([str(frontend), str(code), str(environment), str(glsl)],
                          capture_output=True, text=True)


def parse_header(environment):
    values = Path(environment).read_text(encoding='utf-8').split()
    if len(values) < ENVIRONMENT_FIELDS:
        raise ResolutionError('truncated library environment header')
    return {
        'stage': int(values[0]), 'start_address': int(values[1]),
        'local_memory': int(values[2]), 'bound_buffer': int(values[3]),
        'viewport_transform': int(values[4]),
    }


def resolve_one(entry, library, frontend, output, frontend_identity, runner=None):
    runner = runner or run_frontend
    sid = entry['shader_id']
    code = source_file(library, entry['maxwell'])
    glsl_source = source_file(library, entry['glsl'])
    resources_path = Path(str(glsl_source) + '.resources.json')
    missing_path = Path(str(glsl_source) + '.missing-cbufs.json')
    conversion_path = glsl_source.parent / 'conversion.json'
    environment_path = glsl_source.parent / 'environment.txt'
    for path in (resources_path, missing_path, conversion_path, environment_path):
        if not path.is_file():
            raise ResolutionError('missing library sidecar ' + path.name)
    header = parse_header(environment_path)
    if header['start_address'] != 16 or header['viewport_transform'] != 1:
        raise ResolutionError('library environment has an unexpected start/viewport')
    if digest(code) != entry['maxwell_sha256']:
        raise ResolutionError('Maxwell content hash mismatch')
    conversion = read_json(conversion_path)
    environment_hash_verified = False
    if 'environment_sha256' in conversion:
        if conversion['environment_sha256'] != digest(environment_path):
            raise ResolutionError('library environment hash does not match conversion record')
        environment_hash_verified = True
    library_missing = read_json(missing_path)
    requirements = {f"cbuf:{k['bank']}:{k['offset']}" for k in library_missing['keys']}
    if library_missing['texture_types']:
        requirements.add('texture_type_handle:0')
    if library_missing['texture_types'] and library_missing['texture_type_handles'] != [0]:
        raise ResolutionError('library missing-texture handle is not the documented placeholder')

    plan = descriptor_plan(entry, resources_path)
    cbuf_values = {(slot['resolved_bank'] << 32) | slot['resolved_offset']: slot['handle']
                   for slot in plan}
    stage = 0 if entry['stage'] == 'vert' else 4
    local = header['local_memory']

    target = Path(output) / sid
    target.mkdir(parents=True, exist_ok=True)
    discovery = target / 'discovery'
    discovery.mkdir(exist_ok=True)
    discovery_env = discovery / 'environment.txt'
    write_environment(discovery_env, stage, local, cbuf_values, {})
    discovery_glsl = discovery / 'shader.glsl'
    proc = runner(frontend, code, discovery_env, discovery_glsl)
    if proc.returncode != 0:
        raise ResolutionError('discovery translation failed: ' + proc.stderr.strip()[:200])
    discovery_missing = read_json(str(discovery_glsl) + '.missing-cbufs.json')
    if discovery_missing['count'] != 0:
        keys = [(k['bank'], k['offset']) for k in discovery_missing['keys']]
        raise ResolutionError(f'unresolved cbuf queries after handle assignment: {keys[:8]}')
    if discovery_missing['texture_formats'] != 0:
        raise ResolutionError('texture pixel-format queries are not modelled')
    by_handle = {slot['handle']: slot for slot in plan}
    queried = []
    for handle in discovery_missing['texture_type_handles']:
        if handle not in by_handle:
            raise ResolutionError(f'texture type query for unknown handle {handle:#x}')
        queried.append(handle)
    if len(set(queried)) != len(queried):
        raise ResolutionError('duplicate texture type query handle')

    handle_types = {slot['handle']: slot['instruction_type'] for slot in plan}
    resolved_env = target / 'environment.txt'
    write_environment(resolved_env, stage, local, cbuf_values, handle_types)
    resolved_glsl = target / 'shader.glsl'
    proc = runner(frontend, code, resolved_env, resolved_glsl)
    if proc.returncode != 0:
        raise ResolutionError('resolution translation failed: ' + proc.stderr.strip()[:200])
    resolved_missing = read_json(str(resolved_glsl) + '.missing-cbufs.json')
    for field in ('count', 'texture_types', 'texture_formats'):
        if resolved_missing[field] != 0:
            raise ResolutionError(f'resolved run still has {field}={resolved_missing[field]}')
    resolved_resources = read_json(str(resolved_glsl) + '.resources.json')
    expected = [(d['type'], d['count']) for d in read_json(resources_path)['textures']]
    actual = [(d['type'], d['count']) for d in resolved_resources['textures']]
    if actual != expected:
        raise ResolutionError('resolved texture descriptor types changed')

    queried_set = set(queried)
    descriptors = []
    for slot in plan:
        descriptors.append({
            **slot,
            'queried': slot['handle'] in queried_set,
            'role': 'texture-handle',
            'type_source': 'instruction descriptor type (resources.json)',
        })
    record = {
        'schema': 1,
        'shader_id': sid,
        'stage': entry['stage'],
        'origin': entry.get('origin'),
        'maxwell_sha256': entry['maxwell_sha256'],
        'library': {
            'glsl': entry['glsl'],
            'resources_sha256': digest(resources_path),
            'missing_sha256': digest(missing_path),
            'environment_sha256': digest(environment_path),
            'environment_hash_verified': environment_hash_verified,
            'conversion_sha256': digest(conversion_path),
            'missing_keys': sorted(requirements),
            'missing_texture_types': library_missing['texture_types'],
        },
        'resolved': {
            'environment': 'environment.txt',
            'environment_sha256': digest(resolved_env),
            'glsl': 'shader.glsl',
            'glsl_sha256': digest(resolved_glsl),
            'resources_sha256': digest(str(resolved_glsl) + '.resources.json'),
            'missing_sha256': digest(str(resolved_glsl) + '.missing-cbufs.json'),
            'missing': resolved_missing,
        },
        'frontend': frontend_identity,
        'texture_bound_buffer': 0,
        'driver_constant_folding': 'disabled',
        'descriptors': descriptors,
        'discovery': {
            'missing_cbufs': discovery_missing['count'],
            'missing_texture_types': discovery_missing['texture_types'],
            'queried_handles': sorted(queried),
            'queried_descriptors': len(queried_set),
        },
        'stdout': proc.stdout.strip(),
    }
    write_json(target / 'resolution.json', record)
    shutil.rmtree(discovery)
    return record


def verify_library(library):
    extraction = read_json(library / 'extraction.json')
    spk = source_file(library, 'source/root.spkg')
    spk_sha = digest(spk)
    if not extraction.get('sources') or any(s['spk_sha256'] != spk_sha
                                            for s in extraction['sources']):
        raise ResolutionError('library SPK identity mismatch')
    return spk_sha, extraction


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--frontend', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--limit', type=int, default=0, help='diagnostic subset in catalogue order')
    args = parser.parse_args()
    library = args.library.resolve()
    output = args.output.resolve()
    if output == library or output.is_relative_to(library):
        raise ResolutionError('output must not modify the supplied library')
    if output.exists() and any(output.iterdir()):
        raise ResolutionError('use a new or empty output directory')
    frontend = args.frontend.resolve()
    if not frontend.is_file():
        raise ResolutionError('frontend binary is missing')
    build_path = frontend.parent / 'build.json'
    build = read_json(build_path) if build_path.is_file() else None
    frontend_identity = {'path': str(frontend), 'sha256': digest(frontend),
                         'build': build}
    spk_sha, extraction = verify_library(library)
    entries = read_jsonl(library / 'all_shaders.jsonl')
    if args.limit:
        entries = entries[:args.limit]
    output.mkdir(parents=True, exist_ok=True)
    records = []
    failures = []
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {pool.submit(resolve_one, e, library, frontend, output,
                               frontend_identity): e for e in entries}
        for future in futures:
            entry = futures[future]
            try:
                records.append(future.result())
            except Exception as error:  # noqa: BLE001
                failures.append({'shader_id': entry['shader_id'],
                                 'error': f'{type(error).__name__}: {error}'})
    records.sort(key=lambda r: r['shader_id'])
    failures.sort(key=lambda r: r['shader_id'])
    write_jsonl(output / 'resolution.jsonl', records)
    write_json(output / 'failures.json', failures)
    requirements = read_json(library / 'runtime_requirements.json')['requests']
    requirement_mismatch = []
    sdk_sidecar_only = []
    for record in records:
        sid = record['shader_id']
        expected = {key for key, shaders in requirements.items() if sid in shaders}
        actual = set(record['library']['missing_keys'])
        if expected - actual:
            requirement_mismatch.append(sid)
        elif actual - expected:
            if record['origin'] == 'SDK embedded shader window':
                sdk_sidecar_only.append(sid)
            else:
                requirement_mismatch.append(sid)
    query_types = Counter()
    queries = 0
    for record in records:
        for slot in record['descriptors']:
            if slot['queried']:
                query_types[slot['instruction_type']] += 1
                queries += 1
    summary = {
        'schema': 1,
        'scope': 'A2 static compile-environment resolution for the locked shader library',
        'library_spk_sha256': spk_sha,
        'library_extraction_sha256': digest(library / 'extraction.json'),
        'library_all_shaders_sha256': digest(library / 'all_shaders.jsonl'),
        'library_programs_mapping_sha256': digest(library / 'programs_mapping.jsonl'),
        'library_runtime_requirements_sha256': digest(library / 'runtime_requirements.json'),
        'frontend': frontend_identity,
        'tool_sha256': digest(Path(__file__)),
        'texture_bound_buffer': 0,
        'driver_constant_folding': 'disabled; bank 1 stays a runtime uniform binding',
        'counts': {
            'shaders': len(entries),
            'resolved': len(records),
            'failed': len(failures),
            'texture_type_queries': queries,
            'queries_by_instruction_type': {str(k): v for k, v in sorted(query_types.items())},
            'descriptors': sum(len(r['descriptors']) for r in records),
            'shaders_with_queries': sum(1 for r in records if r['discovery']['queried_handles']),
            'runtime_requirement_mismatches': len(requirement_mismatch),
            'sdk_sidecar_only_requirements': sdk_sidecar_only,
        },
        'outputs': {
            'resolution.jsonl': digest(output / 'resolution.jsonl'),
            'failures.json': digest(output / 'failures.json'),
        },
        'limitations': [
            'The bound texture descriptor table is placed at cbuf bank 0 to match the port runtime convention; the runtime resolves textures through Maxwell state, not through this offline bank index.',
            'Setting bound=0 disables the frontend proprietary-driver constant folding, so bank 1 remains a runtime uniform instead of being baked as zero.',
            'Texture type queries are answered with the instruction descriptor dimension after a corpus census found no non-normalized 2D or 1D-buffer texture source; a later evidence of such binding requires a new variant.',
            'No pair linking, pixel equivalence, hardware, or performance claim is made.',
        ],
    }
    write_json(output / 'summary.json', summary)
    print(json.dumps({'counts': summary['counts'], 'failures': len(failures)}, ensure_ascii=False))
    if requirement_mismatch:
        print(json.dumps({'runtime_requirement_mismatches': requirement_mismatch[:10]}))
    if failures or requirement_mismatch:
        print(json.dumps(failures[:10], ensure_ascii=False))
        raise SystemExit(1)


if __name__ == '__main__':
    main()

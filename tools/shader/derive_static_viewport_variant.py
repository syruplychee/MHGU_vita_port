"""Derive a viewport-transform variant of a resolved static environment.

The locked shader library records a single `viewport_transform=1` assumption,
but the runtime Maxwell state bit 0x64b[0] was observed with both values and
the vertex shader code changes: viewport=0 embeds the render-area transform and
requires the `render_area` uniform. The port binds that uniform and keys static
variants by (stage content, viewport transform), so the static compile set
needs both bounded variants.

This tool consumes an A2 resolver output (which must have zero missing
queries), rewrites the environment's viewport field, reruns the pinned
frontend, and refuses to emit a variant unless the rerun still has zero
missing cbuf/type/format queries and unchanged texture descriptors. Every
record names the source environment hash, the changed field, and its byte-level
rule; source and library inputs are never modified.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess

ENVIRONMENT_VIEWPORT_FIELD = 4
TYPE_NAMES = {
    0: 'Color1D', 1: 'ColorArray1D', 2: 'Color2D', 3: 'ColorArray2D',
    4: 'Color3D', 5: 'ColorCube', 6: 'ColorArrayCube', 7: 'Buffer',
    8: 'Color2DRect',
}


class VariantError(Exception):
    pass


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


def source_file(root, name):
    root = Path(root).resolve()
    path = (root / name).resolve()
    if not path.is_relative_to(root) or not path.is_file():
        raise VariantError('invalid source path: ' + str(name))
    return path


def rewrite_environment(text, viewport):
    tokens = text.split()
    if len(tokens) < 9:
        raise VariantError('truncated environment header')
    try:
        original = int(tokens[ENVIRONMENT_VIEWPORT_FIELD])
    except ValueError as error:
        raise VariantError('non-integer viewport field') from error
    tokens[ENVIRONMENT_VIEWPORT_FIELD] = str(viewport)
    # The environment parser is whitespace-delimited; canonical one-record-per-
    # line output keeps diffs reviewable without changing any other field.
    records = [tokens[:9]]
    position = 9
    texture_count = int(tokens[5])
    format_count = int(tokens[6])
    cbuf_count = int(tokens[7])
    replacement_count = int(tokens[8])
    if format_count or replacement_count:
        raise VariantError('format/replacement tables are not produced by the resolver')
    for count in (texture_count, cbuf_count):
        for _ in range(count):
            if position + 2 > len(tokens):
                raise VariantError('truncated environment table')
            records.append(tokens[position:position + 2])
            position += 2
    if position != len(tokens):
        raise VariantError('trailing environment tokens')
    return '\n'.join(' '.join(record) for record in records) + '\n', original


def run_frontend(frontend, code, environment, glsl):
    return subprocess.run([str(frontend), str(code), str(environment), str(glsl)],
                          capture_output=True, text=True)


def load_library(library):
    entries = {e['shader_id']: e for e in read_jsonl(library / 'all_shaders.jsonl')}
    return entries


def verify_source(source):
    summary = read_json(source / 'summary.json')
    if summary.get('schema') != 1:
        raise VariantError('unsupported source summary schema')
    if summary['counts']['failed'] != 0:
        raise VariantError('source resolution has failures')
    records = read_jsonl(source / 'resolution.jsonl')
    if summary['counts']['resolved'] != len(records):
        raise VariantError('source resolution count mismatch')
    for record in records:
        missing = record['resolved']['missing']
        if missing['count'] or missing['texture_types'] or missing['texture_formats']:
            raise VariantError('source still has missing queries: ' + record['shader_id'])
    return summary, records


def validate_output(source, library, output):
    if output == source or output.is_relative_to(source) or \
            output == library or output.is_relative_to(library):
        raise VariantError('output must not modify source or library')


def derive_one_checked(record, entries, library, frontend, source_root, output,
                       viewport, frontend_identity, runner=None):
    runner = runner or run_frontend
    sid = record['shader_id']
    entry = entries.get(sid)
    if entry is None:
        raise VariantError('shader is not in the locked library: ' + sid)
    if entry['maxwell_sha256'] != record['maxwell_sha256']:
        raise VariantError('library content hash mismatch: ' + sid)
    if record.get('frontend', {}).get('sha256') != frontend_identity['sha256']:
        raise VariantError('source frontend identity mismatch: ' + sid)
    code = source_file(library, entry['maxwell'])
    source_env = source_root / sid / record['resolved']['environment']
    source_glsl = source_root / sid / record['resolved']['glsl']
    if digest(source_env) != record['resolved']['environment_sha256']:
        raise VariantError('source environment hash mismatch: ' + sid)
    if digest(source_glsl) != record['resolved']['glsl_sha256']:
        raise VariantError('source GLSL hash mismatch: ' + sid)
    new_text, original_viewport = rewrite_environment(
        source_env.read_text(encoding='utf-8'), viewport)

    target = output / sid
    target.mkdir(parents=True, exist_ok=True)
    target_env = target / 'environment.txt'
    target_env.write_text(new_text, encoding='utf-8', newline='\n')
    target_glsl = target / 'shader.glsl'
    proc = runner(frontend, code, target_env, target_glsl)
    if proc.returncode != 0:
        raise VariantError('variant translation failed: ' + proc.stderr.strip()[:200])
    missing = read_json(str(target_glsl) + '.missing-cbufs.json')
    for field in ('count', 'texture_types', 'texture_formats'):
        if missing[field] != 0:
            raise VariantError(f'variant still has {field}={missing[field]}: {sid}')
    source_resources = read_json(str(source_root / sid / 'shader.glsl.resources.json'))
    variant_resources = read_json(str(target_glsl) + '.resources.json')
    if variant_resources != source_resources:
        raise VariantError('variant changed texture/constant descriptors: ' + sid)
    glsl_changed = digest(target_glsl) != record['resolved']['glsl_sha256']
    if glsl_changed and any(d.get('queried') for d in record.get('descriptors', [])):
        raise VariantError('viewport variant changed a shader with texture type queries: ' + sid)
    variant = {
        'viewport_transform': viewport,
        'source_viewport_transform': original_viewport,
        'source_environment': record['resolved']['environment'],
        'source_environment_sha256': record['resolved']['environment_sha256'],
        'source_glsl_sha256': record['resolved']['glsl_sha256'],
        'discovery_source': ('inherited: unchanged GLSL' if not glsl_changed
                             else 'inherited: changed GLSL has no texture type queries'),
        'field': {
            'name': 'viewport_transform',
            'environment_token_index': ENVIRONMENT_VIEWPORT_FIELD,
            'rule': 'replace whitespace-delimited header token 4 (0-based)',
            'from': original_viewport,
            'to': viewport,
        },
        'glsl_changed': glsl_changed,
        'evidence': [
            'runtime decode: guest_svc.c reads Maxwell state 0x64b bit 0',
            'runtime captures include both 0 and 1 for observed vertex programs',
            'library environment records viewport=1 as its fixed assumption',
        ],
    }
    derived = {
        'schema': 1,
        'shader_id': sid,
        'stage': record['stage'],
        'origin': record.get('origin'),
        'maxwell_sha256': record['maxwell_sha256'],
        'library': record['library'],
        'resolved': {
            'environment': 'environment.txt',
            'environment_sha256': digest(target_env),
            'glsl': 'shader.glsl',
            'glsl_sha256': digest(target_glsl),
            'resources_sha256': digest(str(target_glsl) + '.resources.json'),
            'missing_sha256': digest(str(target_glsl) + '.missing-cbufs.json'),
            'missing': missing,
        },
        'frontend': frontend_identity,
        'texture_bound_buffer': 0,
        'driver_constant_folding': 'disabled',
        'descriptors': record.get('descriptors', []),
        'discovery': record.get('discovery', {}),
        'variant': variant,
        'stdout': proc.stdout.strip(),
    }
    write_json(target / 'resolution.json', derived)
    return derived


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True,
                        help='A2 resolver output with summary/resolution.jsonl')
    parser.add_argument('--frontend', type=Path, required=True)
    parser.add_argument('--viewport-transform', type=int, choices=(0, 1), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()

    library = args.library.resolve()
    source = args.source.resolve()
    output = args.output.resolve()
    validate_output(source, library, output)
    if output.exists() and any(output.iterdir()):
        raise VariantError('use a new or empty output directory')
    frontend = args.frontend.resolve()
    if not frontend.is_file():
        raise VariantError('frontend binary is missing')
    build_path = frontend.parent / 'build.json'
    build = read_json(build_path) if build_path.is_file() else None
    frontend_identity = {'path': str(frontend), 'sha256': digest(frontend),
                         'build': build}
    summary, records = verify_source(source)
    entries = load_library(library)
    output.mkdir(parents=True, exist_ok=True)
    derived = []
    failures = []
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {pool.submit(derive_one_checked, r, entries, library, frontend,
                              source, output, args.viewport_transform,
                              frontend_identity): r for r in records}
        for future in futures:
            record = futures[future]
            try:
                derived.append(future.result())
            except Exception as error:  # noqa: BLE001
                failures.append({'shader_id': record['shader_id'],
                                 'error': f'{type(error).__name__}: {error}'})
    derived.sort(key=lambda r: r['shader_id'])
    failures.sort(key=lambda r: r['shader_id'])
    write_jsonl(output / 'resolution.jsonl', derived)
    write_json(output / 'failures.json', failures)
    changed = sum(1 for r in derived if r['variant']['glsl_changed'])
    summary_out = {
        'schema': 1,
        'scope': 'A2 bounded viewport-transform variant of a resolved static environment',
        'viewport_transform': args.viewport_transform,
        'source': {
            'root': str(source),
            'summary_sha256': digest(source / 'summary.json'),
            'resolution_sha256': digest(source / 'resolution.jsonl'),
            'resolved': summary['counts']['resolved'],
        },
        'library_spk_sha256': summary.get('library_spk_sha256'),
        'frontend': frontend_identity,
        'tool_sha256': digest(Path(__file__)),
        'texture_bound_buffer': 0,
        'driver_constant_folding': 'disabled',
        'counts': {
            'shaders': len(records),
            'resolved': len(derived),
            'derived': len(derived),
            'failed': len(failures),
            'glsl_changed': changed,
            'glsl_identical': len(derived) - changed,
            'missing_cbufs': sum(r['resolved']['missing']['count'] for r in derived),
            'missing_texture_types': sum(r['resolved']['missing']['texture_types']
                                         for r in derived),
            'missing_texture_formats': sum(r['resolved']['missing']['texture_formats']
                                           for r in derived),
        },
        'outputs': {
            'resolution.jsonl': digest(output / 'resolution.jsonl'),
            'failures.json': digest(output / 'failures.json'),
        },
        'limitations': [
            'Only the viewport_transform header field differs; cbuf values and texture descriptors are inherited from the source resolution.',
            'The variant is a bounded compile-time specialization observed in runtime Maxwell state, not a default.',
            'No pair linking, pixel equivalence, hardware, or performance claim is made.',
        ],
    }
    write_json(output / 'summary.json', summary_out)
    print(json.dumps({'counts': summary_out['counts'], 'failures': len(failures)}))
    if failures:
        print(json.dumps(failures[:10], ensure_ascii=False))
        raise SystemExit(1)


if __name__ == '__main__':
    main()

"""Strict per-program execution verification for GXM pair probes.

The probe's PAIR status is an API-path result.  This module adds the backend,
execution and content states as separate facts, correlated through the GXP
internal SHA-256 (the emulator's program hash) and the GL program id emitted by
the instrumented emulator build:

  VP_GXM_PROGRAM_READY frag=<sha> vert=<sha> prog=<id>
  VP_GXM_PROGRAM_CACHED ...
  VP_GXM_PROGRAM_FAIL ...
  VP_GXM_GLDRAW prog=<id>
  VP_GXM_GLSTATE ... linked=<0/1> err=0x...
  VP_GXM_GLDRAW_ERR err=0x...

No PAIR ok line, RT pixel hash or cache file alone can promote a program to
backend_program_ready or execution_passed.  Missing correlation is reported
as undetermined, never as a default pass.
"""
import hashlib
import json
import re
import struct
from collections import defaultdict
from pathlib import Path

LINE_RE = re.compile(
    r'^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\] \|(\w)\| \[([^\]]+)\]: (.*)$')
PROGRAM_RE = re.compile(
    r'^VP_GXM_PROGRAM_(READY|CACHED|FAIL) frag=([0-9a-f]{64}) '
    r'vert=([0-9a-f]{64})(?: prog=(\d+))?')
SHADER_RE = re.compile(
    r'^VP_GXM_SHADER_(READY|CACHED|FAIL) type=(vert|frag) '
    r'hash=([0-9a-f]{64})(?: id=(\d+))?')
GLDRAW_RE = re.compile(r'^VP_GXM_GLDRAW prog=(\d+)')
GLSTATE_RE = re.compile(
    r'^VP_GXM_GLSTATE .*?linked=(\d+) err=0x([0-9a-f]+)')
GLDRAWERR_RE = re.compile(r'^VP_GXM_GLDRAW_ERR err=0x([0-9a-f]+)')
GENERATING_RE = re.compile(r'Generating (vert|frag) shader ([0-9a-f]{64})')
ERROR_HASH_RE = re.compile(
    r'Error in (?:get/compiled|get/compile) (?:vertex|fragment vertex) shader:')
COMPILE_ERROR_RE = re.compile(r'Error in compile (vert|frag) shader:')
GLSL_ARRAY_RE = re.compile(r'vec4\s+r\[(\d+)\]\s*;')
GLSL_INDEX_RE = re.compile(r'r\[(\d+)\]')
HEX64_RE = re.compile(r'^[0-9a-f]{64}$')


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def ts_of(hour, minute, second, millisecond):
    return int(hour) * 3600 + int(minute) * 60 + int(second) + int(millisecond) / 1000.0


def parse_emulator_log(path):
    """Parse only evidence-relevant lines; keep their timestamps and order."""
    records = {
        'program_ready': {},   # (frag, vert) -> [{'prog': int, 'ts': float, 'cached': bool}]
        'program_fail': {},    # (frag, vert) -> count
        'shader_fail': [],     # {'type','hash','ts'}
        'draws': [],           # {'prog','ts','linked','state_err','draw_err','line'}
        'compile_errors': [],  # {'kind','hash','ts'}
        'c1068': [],           # {'hash', 'ts'}
        'fail_get_program': [],# ts
        'exceptions': [],      # {'ts','text'}
        'lines': 0,
        'first_ts': None,
        'last_ts': None,
        'game_closed_ts': None,
        'probe_completions': [],
    }
    lines = Path(path).read_text(encoding='utf-8', errors='replace').splitlines()
    records['lines'] = len(lines)
    last_generated = None
    pending_error = None
    active_programs = {}
    for number, raw in enumerate(lines):
        match = LINE_RE.match(raw)
        if match:
            hour, minute, second, millisecond, level, category, message = match.groups()
            ts = ts_of(hour, minute, second, millisecond)
        else:
            # Continuation lines (GLSL logs, hashes) have no timestamp.
            level = category = ''
            message = raw
            ts = records['last_ts'] if records['last_ts'] is not None else 0.0
        if records['first_ts'] is None:
            records['first_ts'] = ts
        records['last_ts'] = ts

        if pending_error is not None:
            token = message.strip()
            if HEX64_RE.match(token):
                records['compile_errors'].append(
                    {'kind': pending_error, 'hash': token, 'ts': ts})
            pending_error = None

        gen = GENERATING_RE.search(message)
        if gen:
            last_generated = gen.group(2)

        program = PROGRAM_RE.match(message)
        if program:
            state, frag, vert, prog = program.groups()
            key = (frag, vert)
            if state == 'FAIL':
                records['program_fail'][key] = records['program_fail'].get(key, 0) + 1
            else:
                records['program_ready'].setdefault(key, []).append(
                    {'prog': int(prog) if prog else None,
                     'ts': ts, 'cached': state == 'CACHED'})
                if prog is not None:
                    active_programs[int(prog)] = key

        shader = SHADER_RE.match(message)
        if shader and shader.group(1) == 'FAIL':
            records['shader_fail'].append(
                {'type': shader.group(2), 'hash': shader.group(3), 'ts': ts})

        draw = GLDRAW_RE.match(message)
        if draw:
            records['draws'].append(
                {'prog': int(draw.group(1)), 'ts': ts, 'linked': None,
                 'state_err': None, 'draw_err': None, 'line': number,
                 'key': active_programs.get(int(draw.group(1)))})

        state = GLSTATE_RE.match(message)
        if state and records['draws']:
            event = records['draws'][-1]
            if event['linked'] is None:
                event['linked'] = int(state.group(1))
                event['state_err'] = int(state.group(2), 16)

        draw_err = GLDRAWERR_RE.match(message)
        if draw_err and records['draws']:
            records['draws'][-1]['draw_err'] = int(draw_err.group(1), 16)

        if 'Fail to get program!' in message:
            records['fail_get_program'].append(ts)

        if 'array index out of bounds' in message:
            records['c1068'].append({'hash': last_generated, 'ts': ts})

        if ERROR_HASH_RE.search(message):
            pending_error = 'get'
        elif COMPILE_ERROR_RE.search(message):
            pending_error = 'compile'

        if 'EXCEPTION' in message:
            records['exceptions'].append({'ts': ts, 'text': message[:200]})
        if 'Game closed:' in message:
            records['game_closed_ts'] = ts
        if re.match(r'^VP_GXM_APP_EXIT status=0$', message):
            records['game_closed_ts'] = ts
        completion = re.fullmatch(r'VP_GXM_PROBE_COMPLETE terminal=(\d+)/(\d+)', message)
        if completion:
            records['probe_completions'].append(dict(ts=ts, terminal=int(completion[1]), total=int(completion[2])))

    return records


def gxp_info(path):
    """Return the declared temp counts and the emulator-visible hash."""
    data = Path(path).read_bytes()
    if len(data) < 60 or data[:4] != b'GXP\0':
        raise ValueError('not a GXP: ' + str(path))
    size = struct.unpack_from('<I', data, 8)[0]
    temp1 = struct.unpack_from('<I', data, 52)[0]
    temp2 = struct.unpack_from('<H', data, 56)[0]
    if size > len(data) or size < 60:
        raise ValueError('bad GXP program.size: ' + str(path))
    return {
        'path': str(path),
        'file_sha256': hashlib.sha256(data).hexdigest(),
        'internal_sha256': hashlib.sha256(data[:size]).hexdigest(),
        'file_bytes': len(data),
        'program_size': size,
        'temp_reg_count1': temp1,
        'temp_reg_count2': temp2,
        'temp_words': max(temp1, temp2),
        'temp_vec4_required': (max(temp1, temp2) + 3) // 4,
    }


def parse_device_gxp(device_log):
    result = {}
    for line in device_log.get('gxp', []):
        fields = dict(re.findall(r'(\w+)=([^ ]+)', line))
        try:
            token = int(fields['token'])
        except (KeyError, ValueError):
            continue
        result[token] = fields
    return result


def cache_lookup(cache_files, cache_dir):
    """Map shader hash -> GLSL text from the archived cache-after manifest."""
    out = {}
    if not cache_files or not cache_dir:
        return out
    base = Path(cache_dir)
    for item in cache_files:
        name = item.get('path', '')
        match = re.search(r'-([0-9a-f]{64})\.(vert|frag)$', name)
        if not match:
            continue
        path = (base / name).resolve()
        if not path.is_relative_to(base.resolve()):
            raise ValueError('cache path escapes archive: ' + name)
        if not path.is_file():
            raise ValueError('cache file missing: ' + name)
        raw = path.read_bytes()
        if len(raw) != item.get('bytes') or hashlib.sha256(raw).hexdigest() != item.get('sha256'):
            raise ValueError('cache receipt mismatch: ' + name)
        out[(match.group(2), match.group(1))] = raw.decode('utf-8', errors='replace')
    return out


def glsl_facts(text):
    if text is None:
        return None
    array = GLSL_ARRAY_RE.search(text)
    body = GLSL_ARRAY_RE.sub('', text)
    indexes = [int(value) for value in GLSL_INDEX_RE.findall(body)]
    return {
        'array_size': int(array.group(1)) if array else None,
        'max_index': max(indexes) if indexes else None,
        'dynamic_index': bool(re.search(r'r\[[^0-9\]]', body)),
    }


def _state_counts():
    return {'pass': 0, 'fail': 0, 'undetermined': 0}


def _add(counts, state):
    if state == 'pass':
        counts['pass'] += 1
    elif state == 'fail':
        counts['fail'] += 1
    else:
        counts['undetermined'] += 1


def evaluate_execution(device_log, manifest, bundle_dir, emulator_log,
                       process, cache_dir=None, expectations=None,
                       require_execution=False):
    """Compute independent per-program states.

    device_log is the dict returned by verify_gxm_s2_pairs.parse_log().
    process is the parsed process.json dict or None.
    """
    problems = []
    manifest_path = Path(bundle_dir) / 'pair-manifest.json'
    if manifest_path.is_file():
        manifest_sha = sha256_file(manifest_path)
    else:
        manifest_sha = None
    backend = parse_emulator_log(emulator_log) if emulator_log else None
    program_ready = backend['program_ready'] if backend else {}
    program_fail = backend['program_fail'] if backend else {}
    draws = backend['draws'] if backend else []
    exceptions = backend['exceptions'] if backend else []
    last_draw_ts = max((event['ts'] for event in draws), default=None)
    execution_exceptions = []
    post_run_exceptions = []
    for event in exceptions:
        # A timestamp after the last draw start is not proof of shutdown: an
        # exception can occur inside that very draw or its readback. Require
        # the emulator's explicit app-close marker and a completed probe.
        closed = backend.get('game_closed_ts') if backend else None
        expected_total = len(manifest['programs'])
        completions = [record['ts'] for record in (backend or {}).get('probe_completions', [])
                       if record['terminal'] == record['total'] == expected_total]
        if completions:
            closed = min(completions)
        if ((process or {}).get('result') == 'COMPLETE' and closed is not None
                and event['ts'] >= closed and last_draw_ts is not None
                and closed >= last_draw_ts):
            post_run_exceptions.append(event)
        else:
            execution_exceptions.append(event)
    global_failures = {
        'fail_to_get_program': len(backend['fail_get_program']) if backend else None,
        'compile_errors': len(backend['compile_errors']) if backend else None,
        'shader_fail': len(backend['shader_fail']) if backend else None,
        'c1068': len(backend['c1068']) if backend else None,
        'execution_exceptions': len(execution_exceptions),
        'post_last_draw_exceptions': len(post_run_exceptions),
        'post_validation_exceptions': len(post_run_exceptions),
    }

    cache_files = (process or {}).get('cache_after')
    try:
        cache = cache_lookup(cache_files, cache_dir)
    except ValueError as error:
        problems.append(str(error))
        cache = {}
    if emulator_log and (process or {}).get('emulator_log_sha256'):
        if sha256_file(emulator_log) != process['emulator_log_sha256'].lower():
            problems.append('emulator log receipt mismatch')
    elif require_execution:
        problems.append('missing emulator log receipt')
    gxp_lines = parse_device_gxp(device_log)
    stages = {stage['index']: stage for stage in manifest['stages']}
    programs = {program['token']: program for program in manifest['programs']}
    pairs = {int(pair['token']): pair for pair in device_log['pairs']
             if str(pair.get('token', '')).isdigit()}
    if len(pairs) != len(device_log['pairs']):
        problems.append('duplicate or invalid PAIR token')

    content_cases = 0
    for line in device_log.get('probe', []):
        fields = dict(re.findall(r'(\w+)=([^ ]+)', line))
        if 'content_cases' in fields:
            try:
                content_cases = int(fields['content_cases'])
            except ValueError:
                pass
    case_lines = {}
    for line in device_log.get('p5cases', []):
        fields = dict(re.findall(r'(\w+)=([^ ]+)', line))
        try:
            token = int(fields['token'])
            case = int(fields['case'])
        except (KeyError, ValueError):
            continue
        cases = case_lines.setdefault(token, {})
        if case in cases:
            problems.append('duplicate content case token=%d case=%d' % (token, case))
        cases[case] = fields

    state_counts = {name: _state_counts() for name in (
        'api_create_bind_draw_ok', 'backend_program_ready',
        'execution_passed', 'content_consumption_proven',
        'semantic_verified')}
    per_program = []
    used_draws = defaultdict(int)
    for token in sorted(programs):
        program = programs[token]
        pair = pairs.get(token)
        vs_stage = stages.get(program['vertex_stage'])
        fs_stage = stages.get(program['fragment_stage'])
        entry = {
            'token': token,
            'vertex_id': program.get('vertex_id'),
            'fragment_id': program.get('fragment_id'),
            'vs_stage': program['vertex_stage'],
            'fs_stage': program['fragment_stage'],
        }
        if vs_stage is None or fs_stage is None:
            problems.append('token %d has no complete stage identity' % token)
            entry['states'] = {}
            per_program.append(entry)
            continue
        vs_gxp_path = Path(bundle_dir) / 'shaders' / (vs_stage['variant_key'] + '.gxp')
        fs_gxp_path = Path(bundle_dir) / 'shaders' / (fs_stage['variant_key'] + '.gxp')
        try:
            vs_gxp = gxp_info(vs_gxp_path)
            fs_gxp = gxp_info(fs_gxp_path)
        except (OSError, ValueError) as error:
            problems.append('token %d GXP unreadable: %s' % (token, error))
            vs_gxp = fs_gxp = None
        entry['vs_gxp'] = vs_gxp
        entry['fs_gxp'] = fs_gxp
        if vs_gxp and vs_gxp['file_sha256'] != vs_stage['gxp_sha256']:
            problems.append('token %d VS GXP file hash mismatch' % token)
        if fs_gxp and fs_gxp['file_sha256'] != fs_stage['gxp_sha256']:
            problems.append('token %d FS GXP file hash mismatch' % token)

        gxp_line = gxp_lines.get(token)
        if require_execution and gxp_line is None:
            problems.append('token %d missing device GXP identity' % token)
        if gxp_line and vs_gxp and fs_gxp:
            try:
                if int(gxp_line['vs_size']) != vs_gxp['file_bytes'] or \
                        int(gxp_line['fs_size']) != fs_gxp['file_bytes']:
                    problems.append('token %d device GXP size mismatch' % token)
                if int(gxp_line['vs_crc'], 16) != vs_stage['gxp_crc32'] or \
                        int(gxp_line['fs_crc'], 16) != fs_stage['gxp_crc32']:
                    problems.append('token %d device GXP CRC mismatch' % token)
            except (KeyError, ValueError):
                problems.append('token %d device GXP line unreadable' % token)

        api_state = 'undetermined'
        if pair is None:
            api_state = 'undetermined'
        elif pair.get('status') != 'ok':
            api_state = 'fail'
        elif pair.get('api_create_bind_draw_ok') not in (None, '1'):
            api_state = 'fail'
        elif pair.get('vp') == '1' and pair.get('fp') == '1':
            api_state = 'pass'
        else:
            api_state = 'fail'

        backend_state = 'undetermined'
        execution_state = 'undetermined'
        backend_reason = 'no-emulator-log'
        execution_reason = 'no-emulator-log'
        prog_ids = []
        expected_draws = 2 * content_cases if content_cases else 2
        draw_facts = []
        glsl_facts_vs = None
        if backend is not None and vs_gxp and fs_gxp and pair is not None:
            key = (fs_gxp['internal_sha256'], vs_gxp['internal_sha256'])
            ready = program_ready.get(key, [])
            fails = program_fail.get(key, 0)
            prog_ids = sorted({item['prog'] for item in ready if item['prog'] is not None})
            matching_draws = [item for item in draws if item['key'] == key]
            # Repeated/aliased pairs must consume disjoint event ranges in
            # probe token order. One successful draw cannot certify every
            # token that happens to reuse the same backend program object.
            first_draw = used_draws[key]
            selected_draws = matching_draws[first_draw:first_draw + expected_draws]
            used_draws[key] += expected_draws
            draw_facts = [
                {'prog': item['prog'], 'linked': item['linked'],
                 'state_err': item['state_err'], 'draw_err': item['draw_err'],
                 'line': item['line']}
                for item in selected_draws]
            vs_glsl = cache.get(('vert', vs_gxp['internal_sha256']))
            fs_glsl = cache.get(('frag', fs_gxp['internal_sha256']))
            glsl_facts_vs = glsl_facts(vs_glsl)
            entry['glsl_vs'] = glsl_facts_vs
            entry['glsl_fs'] = glsl_facts(fs_glsl)
            shader_fail = any(
                item['hash'] in (vs_gxp['internal_sha256'], fs_gxp['internal_sha256'])
                for item in backend['shader_fail'])
            c1068_vs = any(
                item['hash'] == vs_gxp['internal_sha256'] for item in backend['c1068'])
            def temp_bank_valid(facts, gxp):
                if facts is None:
                    return False
                if facts['max_index'] is None and not facts['dynamic_index']:
                    return True  # no temp-bank access, e.g. a direct passthrough
                return (facts['array_size'] is not None
                        and facts['array_size'] >= gxp['temp_vec4_required']
                        and (facts['max_index'] is None or facts['max_index'] < facts['array_size']))
            cache_ok = (temp_bank_valid(glsl_facts_vs, vs_gxp)
                        and temp_bank_valid(entry['glsl_fs'], fs_gxp))
            if backend['fail_get_program']:
                backend_reason = 'global-fail-to-get-program'
            elif fails or shader_fail or c1068_vs:
                backend_reason = 'target-compile-or-link-error'
            elif backend['compile_errors']:
                backend_reason = 'global-compile-error'
            elif not ready:
                backend_reason = 'no-program-ready-line'
            elif not cache_ok:
                backend_reason = 'no-valid-cache-glsl-evidence'
            elif not draw_facts:
                backend_reason = 'no-draw-for-program'
            elif any(item['linked'] != 1 or item['state_err'] != 0 or
                     item['draw_err'] != 0 for item in draw_facts):
                backend_reason = 'draw-not-linked-or-gl-error'
            elif len(draw_facts) < expected_draws:
                backend_reason = 'fewer-draws-than-expected'
            else:
                backend_state = 'pass'
                backend_reason = 'program-ready-and-linked-draws'
            if backend_state != 'pass' and (
                    backend['fail_get_program'] or backend['compile_errors'] or
                    fails or shader_fail or c1068_vs):
                backend_state = 'fail'
            execution_reason = backend_reason
            if backend_state == 'pass' and api_state == 'pass':
                try:
                    cov = int(pair['cov'])
                    nonzero = int(pair['nonzero'])
                    stable = pair.get('stable') == '1'
                except (KeyError, ValueError):
                    cov = nonzero = 0
                    stable = False
                if cov <= 0 or nonzero <= 0:
                    execution_reason = 'no-content-pixels'
                elif not stable:
                    execution_reason = 'unstable-pixel'
                elif (process or {}).get('result') != 'COMPLETE':
                    execution_reason = 'process-not-complete'
                elif execution_exceptions:
                    execution_reason = 'exception-during-draws'
                else:
                    execution_state = 'pass'
                    execution_reason = 'program-executed-and-changed-pixels'
            elif backend_state == 'fail' or api_state == 'fail':
                execution_state = 'fail'

        content_state = 'undetermined'
        content_reason = 'not-a-content-run'
        expectation_facts = None
        if content_cases and token in case_lines:
            cases = case_lines[token]
            centers = []
            for case in sorted(cases):
                try:
                    centers.append(int(cases[case]['center'], 16))
                except (KeyError, ValueError):
                    pass
            distinct = len(set(centers))
            content_reason = 'content-cases=%d distinct=%d' % (len(cases), distinct)
            if expectations is not None:
                stage_expect = expectations.get(program['vertex_id'])
                if not stage_expect:
                    content_reason = 'no-independent-expectations'
                else:
                    expectation_facts = []
                    ok = (set(cases) == set(range(content_cases)) and
                          len(stage_expect) == content_cases)
                    for case in sorted(cases):
                        try:
                            index = int(cases[case]['case'])
                            center = int(cases[case]['center'], 16)
                        except (KeyError, ValueError):
                            ok = False
                            continue
                        expected = stage_expect[index]['expected_rgb'] if index < len(stage_expect) else None
                        if expected is None:
                            ok = False
                            continue
                        observed = [center & 0xff, (center >> 8) & 0xff,
                                    (center >> 16) & 0xff]
                        diff = [observed[i] - expected[i] for i in range(3)]
                        within = all(abs(value) <= 1 for value in diff)
                        within = (within and cases[case].get('stable') == '1'
                                  and int(cases[case].get('cov', 0)) > 0
                                  and int(cases[case].get('same', 0)) > 0)
                        expectation_facts.append(
                            {'case': index, 'observed_rgb': observed,
                             'expected_rgb': expected, 'diff': diff,
                             'within_1_lsb': within})
                        ok = ok and within
                    if execution_state == 'pass' and distinct >= 2 and ok:
                        content_state = 'pass'
                        content_reason = 'cases-consume-input-and-match-expectations'
                    elif execution_state != 'pass':
                        content_state = 'fail' if execution_state == 'fail' else 'undetermined'
                        content_reason = 'execution-not-passed'
                    else:
                        content_state = 'fail'
                        content_reason = 'case-output-mismatch-or-not-consuming-input'
        elif content_cases and execution_state == 'pass':
            content_state = 'undetermined'
            content_reason = 'no-p5-case-lines'

        entry['states'] = {
            'api_create_bind_draw_ok': api_state,
            'backend_program_ready': backend_state,
            'execution_passed': execution_state,
            'content_consumption_proven': content_state,
            'semantic_verified': 'undetermined',
        }
        entry['reasons'] = {
            'backend': backend_reason,
            'execution': execution_reason,
            'content': content_reason,
        }
        entry['prog_ids'] = prog_ids
        entry['draws'] = draw_facts
        entry['expected_draws'] = expected_draws
        entry['expectations'] = expectation_facts
        for name, state in entry['states'].items():
            _add(state_counts[name], state)
        per_program.append(entry)

    for key, expected in used_draws.items():
        actual = sum(item['key'] == key for item in draws)
        if actual != expected:
            problems.append('target draw count mismatch frag=%s vert=%s expected=%d actual=%d'
                            % (*key, expected, actual))

    semantic = {
        'state': 'undetermined',
        'reason': 'no-original-program-reference-or-O0-equivalence-proof',
    }
    if problems:
        # Preserve raw API results, but do not label an unbound/tampered
        # evidence set as a verified backend execution or content result.
        for entry in per_program:
            for name, reason in (('backend_program_ready', 'backend'),
                                 ('execution_passed', 'execution'),
                                 ('content_consumption_proven', 'content')):
                if entry['states'].get(name) == 'pass':
                    entry['states'][name] = 'undetermined'
                    entry['reasons'][reason] = 'evidence-integrity-failed'
        state_counts = {name: _state_counts() for name in state_counts}
        for entry in per_program:
            for name, state in entry['states'].items():
                _add(state_counts[name], state)
    execution_complete = not problems and all(
        entry['states'].get('execution_passed') == 'pass'
        for entry in per_program) and bool(per_program)
    content_complete = (not problems and bool(per_program) and all(
        entry['states'].get('content_consumption_proven') == 'pass'
        for entry in per_program))
    return {
        'schema': 1,
        'bundle_manifest_sha256': manifest_sha,
        'emulator_log_sha256': sha256_file(emulator_log) if emulator_log else None,
        'process': process,
        'content_cases': content_cases,
        'global_backend_errors': global_failures,
        'execution_exceptions': execution_exceptions,
        'post_last_draw_exceptions': post_run_exceptions,
        'states': state_counts,
        'semantic': semantic,
        'execution_complete': execution_complete,
        'content_complete': content_complete,
        'programs': per_program,
        'problems': problems,
    }

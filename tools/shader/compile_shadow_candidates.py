"""Compile the nine observed sampler2DShadow stages with the locked toolchain.

This is a compile/lowering gate, not runtime binding or pixel evidence.  It:

* verifies the resolved GLSL/resource identity against the locked catalogue;
* lowers only the observed ``textureLod(sampler2DShadow, vec3(uv,ref), 0.0)``
  shape and records the per-sampler shadow ABI;
* emits every stage Cg first, then compiles the requested optimization list
  (historical default: O0 and O1) with the locked Shacc service;
* supports an explicit service timeout (default 80 s preserves the historical
  classification) and a bounded outer timeout;
* can import a previously produced, fully verified GXP receipt when source,
  toolchain, flags, strategy, policy and emitter identity all match;
* writes a per-stage checkpoint after every terminal stage so an interrupted
  batch never loses completed entries, and can resume identity-matched stages;
* re-emits every one of the 2331 accepted catalogue stages and requires
  byte-identical historical Cg;
* verifies the 282-file candidate bank GXP/input-signature/flags/keys identity.

No GXP is installed, no runtime binding is added, and no GPU consumption is
claimed.  Classification is explicit: ``compiled``, ``service_timeout``,
``compiler_error``, ``interrupted``, ``invalid_output`` or ``not_requested``.
A service timeout is never reported as a compiler rejection.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from convert_cache import compile_serial
from shadow_helpers import (POLICY, STRATEGY, emitter_source_sha256,
                            helper_source_sha256, mechanism_source,
                            policy_sha256)
from shader_ir import Unsupported, emit_cg, lower

ROOT = Path(__file__).resolve().parents[2]
GXP_MAGIC = b'GXP\0'
SCHEMA = 1
TASK = 'M6-B24'
PROFILE_FRAGMENT = 'sce_fp_psp2'
OBSERVED = [
    'frag_03af1ac5efa36f74732f1263',
    'frag_0d45655f63c9a5922d06ea44',
    'frag_14905bcbbb89613d76a00208',
    'frag_64b44f9e813bdb8eb067eed5',
    'frag_aa4caac3ec191706ca19e7c0',
    'frag_b1d649c2889d6d8497bc7b50',
    'frag_cd5a92fb2987e1ab57fcb495',
    'frag_d5dd8db5ece6d7921a3d8efe',
    'frag_e71b9c5e3c7fad339fa1e1b1',
]
TERMINAL_COMPILED = 'compiled'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=False).encode()


def write_json(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=False) + '\n',
                         encoding='utf-8', newline='\n')
    temporary.replace(path)


def load_shacc_identity(shacc, shacc_config):
    config = {}
    for line in Path(shacc_config).read_text(encoding='utf-8').splitlines():
        key, value = line.split('=', 1)
        config[key.strip()] = value.strip()
    if set(config) != {'schema', 'root', 'service-vpk'} or config['schema'] != '1':
        raise ValueError('unsupported Shacc configuration')
    toolchain = json.loads((ROOT / 'config/graphics/shader-toolchain.json')
                           .read_text(encoding='utf-8'))
    compiler = Path(config['root']) / 'fs/ur0/data/external/libshacccg.suprx'
    identity = {
        'shacc_sha256': digest(shacc),
        'service_vpk_sha256': digest(config['service-vpk']),
        'libshacccg_sha256': digest(compiler),
        'config_sha256': digest(shacc_config),
    }
    if identity['shacc_sha256'] != toolchain['shacc']['executable_sha256']:
        raise ValueError('Shacc executable identity mismatch')
    if identity['service_vpk_sha256'] != toolchain['shacc']['service_vpk_sha256']:
        raise ValueError('Shacc service identity mismatch')
    if identity['libshacccg_sha256'] != toolchain['libshacccg']['sha256']:
        raise ValueError('libshacccg identity mismatch')
    return config, identity


def valid_gxp(path):
    path = Path(path)
    if not path.is_file():
        return False, 'missing'
    data = path.read_bytes()
    if len(data) < 32:
        return False, 'short'
    if data[:4] != GXP_MAGIC:
        return False, 'magic'
    declared = struct.unpack_from('<I', data, 8)[0]
    if declared > len(data) or declared < 32:
        return False, 'declared-size'
    return True, data


def classify_log_text(text, returncode):
    """Classify from log text and return code without inventing a cause."""
    lowered = text.lower()
    if 'compiler service timed out' in lowered or 'service timeout' in lowered:
        return 'service_timeout'
    if 'interrupted' in lowered:
        return 'interrupted'
    if returncode:
        return 'compiler_error'
    return 'invalid_output'


def result_record(flag, classification, data=None, reason=''):
    record = {
        'flag': flag,
        'compiled': classification == TERMINAL_COMPILED,
        'status': classification,
        'reason': reason,
        'gxp_sha256': None,
        'gxp_bytes': None,
    }
    if data is not None:
        record['gxp_sha256'] = hashlib.sha256(data).hexdigest()
        record['gxp_bytes'] = len(data)
    return record


def existing_result(path, flag, log_path=None):
    """Read an existing output without compiling; service timeout is named."""
    ok, data = valid_gxp(path)
    if ok:
        return result_record(flag, TERMINAL_COMPILED, data)
    reason = 'not compiled in this metadata pass (%s)' % data
    classification = 'not_attempted'
    if log_path is not None and Path(log_path).is_file():
        text = Path(log_path).read_text(encoding='utf-8', errors='replace')
        classification = classify_log_text(text, 0)
        if classification == 'invalid_output':
            classification = 'compiler_error'
        reason = {
            'service_timeout': 'Compiler service timed out',
            'interrupted': 'log records an interrupted compile',
            'compiler_error': 'log records a compiler error',
            'invalid_output': 'log present but no valid GXP',
        }[classification]
    return result_record(flag, classification, reason=reason)


def compile_cg(shacc, config, cg_path, out_path, flag, log_path,
               service_timeout, outer_timeout):
    out_path = Path(out_path)
    if out_path.exists():
        out_path.unlink()
    args = [str(shacc), '--config', config['_config_path'],
            '-profile', PROFILE_FRAGMENT, flag, '-o', str(out_path.resolve()),
            str(cg_path.resolve()), '--timeout', str(service_timeout)]
    started = time.monotonic()
    timed_out = False
    stdout = stderr = b''
    returncode = None
    try:
        result = compile_serial(args, config['root'], timeout=outer_timeout)
        stdout, stderr = result.stdout, result.stderr
        returncode = result.returncode
    except subprocess.TimeoutExpired as error:
        timed_out = True
        stdout = error.stdout or b''
        stderr = error.stderr or b''
    wall_seconds = round(time.monotonic() - started, 3)
    Path(log_path).write_bytes(stdout + stderr)
    text = (stdout + stderr).decode('utf-8', 'replace')
    ok, data = valid_gxp(out_path)
    if timed_out:
        classification = 'interrupted'
        reason = 'outer timeout after %.3f s; service timeout not observed' % wall_seconds
    elif ok and returncode == 0:
        classification = TERMINAL_COMPILED
        reason = ''
    else:
        classification = classify_log_text(text, returncode)
        reason = 'shacc returncode %s' % returncode
        if classification == 'service_timeout':
            reason = 'Compiler service timed out'
        elif classification == 'invalid_output':
            reason = 'no valid GXP output (%s)' % data
    record = result_record(flag, classification,
                           data if classification == TERMINAL_COMPILED else None,
                           reason)
    record.update({
        'wall_seconds': wall_seconds,
        'returncode': returncode,
        'outer_timed_out': timed_out,
        'log': str(log_path),
        'log_sha256': digest(log_path),
        'gxp': str(out_path),
        'gxp_present': bool(ok),
        'gxp_header_reason': data if not ok else 'valid',
    })
    return record


def stage_toolchain_identity(toolchain):
    return {
        'shacc_sha256': toolchain['shacc_sha256'],
        'service_vpk_sha256': toolchain['service_vpk_sha256'],
        'libshacccg_sha256': toolchain['libshacccg_sha256'],
        'config_sha256': toolchain['config_sha256'],
    }


def stage_identity(stage, flag, toolchain):
    """Identity that must match before a compiled stage may be resumed."""
    return hashlib.sha256(canonical({
        'schema': 1,
        'task': TASK,
        'shader_id': stage['shader_id'],
        'stage_sha256': stage.get('catalogue_maxwell_sha256'),
        'environment_sha256': stage.get('environment_sha256'),
        'cg_sha256': stage.get('cg_sha256'),
        'profile': PROFILE_FRAGMENT,
        'flags': [flag],
        'strategy': STRATEGY,
        'policy_sha256': policy_sha256(),
        'emitter_sha256': emitter_source_sha256(),
        'helper_sha256': helper_source_sha256(),
        'toolchain': stage_toolchain_identity(toolchain),
    })).hexdigest()


def receipt_identity(receipt):
    return hashlib.sha256(canonical({
        'schema': 1,
        'task': receipt.get('task'),
        'shader_id': receipt.get('shader_id'),
        'stage_sha256': receipt.get('stage_sha256'),
        'environment_sha256': receipt.get('environment_sha256'),
        'source_sha256': receipt.get('source_sha256'),
        'profile': receipt.get('profile'),
        'optimization': receipt.get('optimization'),
        'strategy': receipt.get('strategy'),
        'policy_sha256': receipt.get('policy_sha256'),
        'emitter_sha256': receipt.get('emitter_sha256'),
        'helper_sha256': receipt.get('helper_sha256'),
        'toolchain': receipt.get('toolchain'),
    })).hexdigest()


def load_import_receipts(paths, optimizations, toolchain, current_cg):
    """Validate imported receipts against the exact current batch identity."""
    imports = {}
    for raw_path in paths:
        path = Path(raw_path)
        receipt = json.loads(path.read_text(encoding='utf-8'))
        if receipt.get('schema') != 1:
            raise ValueError('import receipt schema unsupported: %s' % path)
        optimization = receipt.get('optimization')
        if optimization not in optimizations:
            raise ValueError('import receipt optimization %r is not requested'
                             % (optimization,))
        if receipt.get('strategy') != STRATEGY:
            raise ValueError('import receipt strategy mismatch: %s' % path)
        if receipt.get('policy_sha256') != policy_sha256():
            raise ValueError('import receipt policy mismatch: %s' % path)
        if receipt.get('emitter_sha256') != emitter_source_sha256():
            raise ValueError('import receipt emitter mismatch: %s' % path)
        if receipt.get('helper_sha256') != helper_source_sha256():
            raise ValueError('import receipt helper mismatch: %s' % path)
        if receipt.get('profile') != PROFILE_FRAGMENT:
            raise ValueError('import receipt profile mismatch: %s' % path)
        if receipt.get('classification') != TERMINAL_COMPILED:
            raise ValueError('import receipt is not compiled: %s' % path)
        if receipt.get('valid_header') is not True:
            raise ValueError('import receipt GXP header is not valid: %s' % path)
        receipt_toolchain = receipt.get('toolchain')
        if receipt_toolchain is None:
            receipt_toolchain = {key: receipt.get(key) for key in
                                 ('shacc_sha256', 'service_vpk_sha256',
                                  'libshacccg_sha256', 'config_sha256')}
        for key in ('shacc_sha256', 'service_vpk_sha256',
                    'libshacccg_sha256', 'config_sha256'):
            if receipt_toolchain.get(key) != toolchain[key]:
                raise ValueError('import receipt toolchain %s mismatch: %s'
                                 % (key, path))
        source_path = Path(receipt['source'])
        if not source_path.is_absolute():
            source_path = ROOT / source_path
        shader_id = source_path.parent.name
        if shader_id not in OBSERVED:
            raise ValueError('import receipt names an unknown stage: %s' % path)
        if receipt.get('shader_id') not in (None, shader_id):
            raise ValueError('import receipt shader_id contradicts source path: '
                             + str(path))
        if shader_id in imports:
            raise ValueError('duplicate import receipt for ' + shader_id)
        current = current_cg.get(shader_id)
        if current is None:
            raise ValueError('import receipt stage was not lowered: ' + shader_id)
        if receipt.get('source_sha256') != current['cg_sha256']:
            raise ValueError('import receipt source Cg SHA differs from the '
                             'current emitter output: ' + shader_id)
        if (receipt.get('stage_sha256') is not None and
                receipt['stage_sha256'] != current['catalogue_maxwell_sha256']):
            raise ValueError('import receipt Maxwell stage SHA mismatch: '
                             + shader_id)
        if (receipt.get('environment_sha256') is not None and
                receipt['environment_sha256'] != current['environment_sha256']):
            raise ValueError('import receipt environment SHA mismatch: '
                             + shader_id)
        flags = receipt.get('flags')
        if flags is not None and flags != [optimization]:
            raise ValueError('import receipt flags mismatch: ' + shader_id)
        gxp_path = Path(receipt['gxp'])
        if not gxp_path.is_absolute():
            gxp_path = ROOT / gxp_path
        ok, data = valid_gxp(gxp_path)
        if not ok:
            raise ValueError('import receipt GXP is invalid (%s): %s'
                             % (data, gxp_path))
        if hashlib.sha256(data).hexdigest() != receipt['gxp_sha256']:
            raise ValueError('import receipt GXP SHA mismatch: ' + str(gxp_path))
        if len(data) != receipt['gxp_bytes']:
            raise ValueError('import receipt GXP size mismatch: ' + str(gxp_path))
        log_path = Path(receipt['log'])
        if not log_path.is_absolute():
            log_path = ROOT / log_path
        log_sha = None
        if log_path.is_file():
            log_sha = digest(log_path)
            if receipt.get('log_sha256') and log_sha != receipt['log_sha256']:
                raise ValueError('import receipt log SHA mismatch: ' + str(log_path))
        imports[shader_id] = {
            'shader_id': shader_id,
            'optimization': optimization,
            'receipt_path': str(path),
            'receipt_sha256': digest(path),
            'receipt_identity_sha256': receipt_identity(receipt),
            'original_gxp_path': str(gxp_path),
            'original_gxp_sha256': receipt['gxp_sha256'],
            'original_gxp_bytes': receipt['gxp_bytes'],
            'original_log_path': str(log_path),
            'original_log_sha256': log_sha,
            'original_wall_seconds': receipt.get('wall_seconds'),
            'original_service_timeout_seconds':
                receipt.get('service_timeout_seconds'),
            'original_outer_timeout_seconds':
                receipt.get('outer_timeout_seconds'),
            'source_sha256': receipt['source_sha256'],
            'data': data,
        }
    return imports


def import_result(entry, receipt_path, stage_dir, flag):
    """Copy the verified GXP/log and return a compiled record with provenance."""
    stage_dir = Path(stage_dir)
    gxp_path = stage_dir / ('shader.frag%s.gxp' % flag.replace('-', ''))
    gxp_path.write_bytes(entry['data'])
    log_copy = None
    if entry['original_log_sha256']:
        source = Path(entry['original_log_path'])
        log_copy = stage_dir / ('shader.frag%s.imported.shacc.log'
                                % flag.replace('-', ''))
        log_copy.write_bytes(source.read_bytes())
        if digest(log_copy) != entry['original_log_sha256']:
            raise ValueError('imported log copy changed bytes')
    record = result_record(flag, TERMINAL_COMPILED, entry['data'])
    record.update({
        'wall_seconds': None,
        'returncode': 0,
        'outer_timed_out': False,
        'log': str(log_copy) if log_copy else entry['original_log_path'],
        'log_sha256': entry['original_log_sha256'],
        'gxp': str(gxp_path),
        'gxp_present': True,
        'gxp_header_reason': 'valid',
        'imported': True,
        'import_provenance': {
            'receipt': str(receipt_path),
            'receipt_sha256': entry['receipt_sha256'],
            'receipt_identity_sha256': entry['receipt_identity_sha256'],
            'original_gxp': entry['original_gxp_path'],
            'original_gxp_sha256': entry['original_gxp_sha256'],
            'original_gxp_bytes': entry['original_gxp_bytes'],
            'original_log': entry['original_log_path'],
            'original_log_sha256': entry['original_log_sha256'],
            'original_wall_seconds': entry['original_wall_seconds'],
            'original_service_timeout_seconds':
                entry['original_service_timeout_seconds'],
            'original_outer_timeout_seconds':
                entry['original_outer_timeout_seconds'],
            'source_sha256': entry['source_sha256'],
        },
    })
    return record


def resume_result(stage_dir, flag, identity_sha, toolchain):
    checkpoint_path = stage_dir / ('checkpoint%s.json' % flag.replace('-', ''))
    if not checkpoint_path.is_file():
        return None
    checkpoint = json.loads(checkpoint_path.read_text(encoding='utf-8'))
    if checkpoint.get('identity_sha256') != identity_sha:
        return None
    if checkpoint.get('status') != TERMINAL_COMPILED:
        return None
    if checkpoint.get('toolchain') != stage_toolchain_identity(toolchain):
        return None
    gxp_path = stage_dir / ('shader.frag%s.gxp' % flag.replace('-', ''))
    ok, data = valid_gxp(gxp_path)
    if not ok:
        return None
    if hashlib.sha256(data).hexdigest() != checkpoint.get('gxp_sha256'):
        return None
    if len(data) != checkpoint.get('gxp_bytes'):
        return None
    record = result_record(flag, TERMINAL_COMPILED, data)
    record.update({
        'gxp': str(gxp_path),
        'gxp_present': True,
        'resumed': True,
        'resume_identity_sha256': identity_sha,
    })
    receipt_path = stage_dir / ('receipt%s.json' % flag.replace('-', ''))
    if receipt_path.is_file():
        receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
        record.update({k: receipt.get(k) for k in
                       ('wall_seconds', 'returncode', 'log', 'log_sha256',
                        'outer_timed_out', 'imported', 'import_provenance')})
        record['receipt'] = str(receipt_path)
        record['receipt_sha256'] = digest(receipt_path)
    return record


def write_stage_checkpoint(stage_dir, flag, stage, identity_sha, result,
                           toolchain):
    checkpoint = {
        'schema': 1,
        'task': TASK,
        'shader_id': stage['shader_id'],
        'stage_sha256': stage.get('catalogue_maxwell_sha256'),
        'environment_sha256': stage.get('environment_sha256'),
        'cg_sha256': stage.get('cg_sha256'),
        'profile': PROFILE_FRAGMENT,
        'flags': [flag],
        'strategy': STRATEGY,
        'policy_sha256': policy_sha256(),
        'emitter_sha256': emitter_source_sha256(),
        'helper_sha256': helper_source_sha256(),
        'toolchain': stage_toolchain_identity(toolchain),
        'identity_sha256': identity_sha,
        'status': result['status'],
        'compiled': result['compiled'],
        'gxp_sha256': result.get('gxp_sha256'),
        'gxp_bytes': result.get('gxp_bytes'),
        'gxp': result.get('gxp'),
        'receipt_sha256': result.get('receipt_sha256'),
        'resumed': bool(result.get('resumed')),
        'imported': bool(result.get('imported')),
    }
    path = stage_dir / ('checkpoint%s.json' % flag.replace('-', ''))
    write_json(path, checkpoint)
    return path


def update_batch_checkpoint(path, state, stage_checkpoint_path, stage_checkpoint):
    state['stages'][stage_checkpoint['shader_id']] = {
        'checkpoint': str(stage_checkpoint_path),
        'identity_sha256': stage_checkpoint['identity_sha256'],
        'status': stage_checkpoint['status'],
        'compiled': stage_checkpoint['compiled'],
        'gxp_sha256': stage_checkpoint['gxp_sha256'],
        'gxp_bytes': stage_checkpoint['gxp_bytes'],
    }
    state['terminal_stages'] = sorted(
        shader_id for shader_id, row in state['stages'].items()
        if row['status'] in ('compiled', 'service_timeout', 'compiler_error',
                             'interrupted', 'invalid_output', 'rejected'))
    state['compiled_stages'] = sorted(
        shader_id for shader_id, row in state['stages'].items()
        if row['compiled'])
    state['complete'] = (
        len(state['compiled_stages']) == len(OBSERVED)
        and set(state['compiled_stages']) == set(OBSERVED))
    write_json(path, state)


def check_sampler_descriptors(ir, reflection, resources):
    """Match each lowered sampler to the locked resources.json descriptor."""
    descriptors = reflection['textures']
    if reflection['images'] or reflection['storage_buffers']:
        raise Unsupported('storage/images are not part of this contract')
    checked = []
    for resource in resources:
        if resource['storage'] == 'cbuf':
            bank = int(resource['name'].split('cbuf')[1])
            found = [item for item in reflection['constant_buffers'] if item['bank'] == bank]
            if len(found) != 1 or found[0]['count'] != 1:
                raise Unsupported('unknown cbuf descriptor for ' + resource['name'])
            resource['nvn_bank'] = bank
            resource['upload_vectors'] = resource['required_vectors']
            checked.append({'name': resource['name'], 'kind': 'cbuf', 'bank': bank})
            continue
        if not resource['name'].startswith('tex'):
            raise Unsupported('unrecognized sampler binding ' + resource['name'])
        index = int(resource['name'][3:])
        if index >= len(descriptors):
            raise Unsupported('sampler reflection missing for ' + resource['name'])
        descriptor = descriptors[index]
        shadow = resource['type'] == 'sampler2DShadow'
        expected_depth = 1 if shadow else 0
        if (descriptor['type'] != 2 or descriptor['secondary'] != 0
                or descriptor['count'] != 1 or descriptor['shift'] != 0
                or descriptor['size_shift'] != 3 or descriptor['depth'] != expected_depth):
            raise Unsupported('descriptor mismatch for %s: %r' %
                              (resource['name'], descriptor))
        resource['nvn_descriptor'] = descriptor
        checked.append({'name': resource['name'], 'kind': 'sampler',
                        'shadow': shadow, 'descriptor': descriptor})
    return checked


def stage_identity_checks(record, resolution_path, source_path, resources_path):
    resolution = json.loads(resolution_path.read_text(encoding='utf-8'))
    resolved = resolution['resolved']
    checks = {
        'maxwell_sha256': resolution['maxwell_sha256'] == record['maxwell_sha256'],
        'environment_sha256':
            resolved['environment_sha256'] == record['environment_sha256'],
        'resources_sha256':
            resolved['resources_sha256'] == record['resources_sha256'],
        'glsl_sha256': resolved['glsl_sha256'] == digest(source_path),
        'resources_file_sha256': resolved['resources_sha256'] == digest(resources_path),
    }
    return checks, resolution


def audit_nonregression(catalogue_path, resolved_root, cg_dir):
    entries = [json.loads(line) for line in
               catalogue_path.read_text(encoding='utf-8').splitlines() if line.strip()]
    accepted = [entry for entry in entries if entry.get('cg_lowering') == 'accepted']
    mismatches = []
    missing = []
    checked = 0
    for entry in accepted:
        shader_id = entry['shader_id']
        source = resolved_root / shader_id / 'shader.glsl'
        old = cg_dir / (shader_id + '.cg')
        if not source.is_file() or not old.is_file():
            missing.append(shader_id)
            continue
        stage = 'vertex' if entry['stage'] == 'vert' else 'fragment'
        cg, _ = emit_cg(lower(source.read_text(encoding='utf-8'), stage))
        new_sha = hashlib.sha256(cg.encode()).hexdigest()
        old_sha = hashlib.sha256(old.read_bytes()).hexdigest()
        checked += 1
        if new_sha != old_sha or new_sha != entry.get('cg_sha256'):
            mismatches.append({'shader_id': shader_id, 'new': new_sha,
                               'old': old_sha, 'catalogue': entry.get('cg_sha256')})
    ignored = [entry for entry in entries
               if entry.get('cg_lowering') != 'accepted']
    return {
        'accepted_catalogue': len(accepted),
        'checked': checked,
        'missing_fixture': missing,
        'mismatches': mismatches,
        'identical': not mismatches and not missing,
        'unsupported_catalogue': len(ignored),
        'unsupported_reasons': dict(collections.Counter(
            entry.get('cg_lowering_reason') or entry.get('unsupported_reason') or
            'recorded-unsupported' for entry in ignored)),
    }


def audit_bank(bank_root, manifest_sha_expected=None):
    manifest_path = bank_root / 'manifest.json'
    index_path = bank_root / 'shader-index.json'
    index_bin = bank_root / 'shader-index.bin'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    index = json.loads(index_path.read_text(encoding='utf-8'))
    stages = index['stages']
    variants = index['variants']
    problems = []
    flags = collections.Counter()
    signatures = collections.Counter()
    keys = []
    verified = 0
    for variant in variants:
        stage_index = variant['stage_index']
        if not 0 <= stage_index < len(stages):
            problems.append('bad stage index for ' + variant['key'])
            continue
        stage = stages[stage_index]
        flags[stage['source_flags']] += 1
        signatures[variant['input_signature']] += 1
        keys.append(variant['key'])
        path = bank_root / variant['gxp']
        if not path.is_file():
            problems.append('missing GXP ' + variant['key'])
            continue
        data = path.read_bytes()
        if (hashlib.sha256(data).hexdigest() != variant['gxp_sha256']
                or len(data) != variant['gxp_bytes']
                or (__import__('zlib').crc32(data) & 0xFFFFFFFF) != variant['gxp_crc32']):
            problems.append('GXP identity mismatch ' + variant['key'])
            continue
        verified += 1
    if len(keys) != len(set(keys)):
        problems.append('duplicate variant keys')
    if hashlib.sha256(index_bin.read_bytes()).hexdigest() != manifest['index_sha256']:
        problems.append('shader-index.bin hash mismatch')
    if hashlib.sha256(index_path.read_bytes()).hexdigest() != manifest['index_json_sha256']:
        problems.append('shader-index.json hash mismatch')
    if manifest_sha_expected and not digest(manifest_path).startswith(manifest_sha_expected):
        problems.append('manifest hash prefix mismatch')
    return {
        'bank_root': str(bank_root),
        'manifest_sha256': digest(manifest_path),
        'index_sha256': digest(index_bin),
        'index_json_sha256': digest(index_path),
        'variants': len(variants),
        'gxp_verified': verified,
        'flags_counts': {str(k): v for k, v in sorted(flags.items())},
        'input_signature_counts': {str(k): v for k, v in sorted(signatures.items())},
        'keys_sha256': hashlib.sha256(canonical(sorted(keys))).hexdigest(),
        'problems': problems,
        'identical': not problems and verified == len(variants),
    }


def requested_optimizations(choice):
    if choice == 'both':
        return ['-O0', '-O1']
    return ['-' + choice]


def result_key(flag):
    return flag.replace('-', '').lower()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalogue', type=Path,
                        default=ROOT / 'artifacts/p2-rebuild-20261025/audit-vp1/shader-catalogue.jsonl')
    parser.add_argument('--resolved-root', type=Path,
                        default=ROOT / 'artifacts/a2-20261022/resolved')
    parser.add_argument('--cg-candidates', type=Path,
                        default=ROOT / 'artifacts/p2-rebuild-20261025/audit-vp1/cg-candidates')
    parser.add_argument('--bank', type=Path,
                        default=ROOT / 'artifacts/m6b14-village-materials-20261026/candidate-bank')
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'artifacts/m6b24-shadow-bank-20261027/actual9')
    parser.add_argument('--summary', type=Path,
                        default=ROOT / 'config/validation/m6-b24-shadow-bank-20261027.json')
    parser.add_argument('--shacc', type=Path,
                        default=ROOT / 'local/shader-tools/shacc-bin/Vita3K-Shacc-windows-x86_64/vita3k-shacc.exe')
    parser.add_argument('--shacc-config', type=Path,
                        default=ROOT / 'local/shader-tools/shacc.conf')
    parser.add_argument('--expected-bank-manifest-prefix', default='b27d08')
    parser.add_argument('--service-timeout-seconds', type=int, default=80,
                        help='Shacc --timeout value; default 80 preserves the '
                             'historical classification')
    parser.add_argument('--outer-timeout-seconds', type=int, default=0,
                        help='process timeout; 0 means service timeout + 30 s')
    parser.add_argument('--optimizations', choices=('both', 'O0', 'O1'),
                        default='both',
                        help='bounded optimization selection; default both '
                             'preserves the historical batch')
    parser.add_argument('--import-receipt', type=Path, action='append', default=[],
                        help='verified receipt whose GXP may be imported for an '
                             'identity-matched stage')
    parser.add_argument('--resume', action='store_true',
                        help='reuse an identity-matched compiled checkpoint')
    parser.add_argument('--skip-compile', action='store_true',
                        help='reuse existing GXPs and only regenerate metadata/audits')
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=True)
    if arguments.service_timeout_seconds <= 0:
        raise SystemExit('service timeout must be positive')
    outer_timeout = arguments.outer_timeout_seconds or (
        arguments.service_timeout_seconds + 30)
    if outer_timeout <= arguments.service_timeout_seconds:
        raise SystemExit('outer timeout must exceed the service timeout')
    optimizations = requested_optimizations(arguments.optimizations)

    config, toolchain = load_shacc_identity(arguments.shacc, arguments.shacc_config)
    config['_config_path'] = str(arguments.shacc_config.resolve())

    catalogue = [json.loads(line) for line in
                 arguments.catalogue.read_text(encoding='utf-8').splitlines() if line.strip()]
    by_id = {entry['shader_id']: entry for entry in catalogue}

    stages = []
    current_cg = {}
    for shader_id in OBSERVED:
        record = by_id.get(shader_id)
        stage_dir = arguments.output / 'stages' / shader_id
        stage_dir.mkdir(parents=True, exist_ok=True)
        source_path = arguments.resolved_root / shader_id / 'shader.glsl'
        resources_path = arguments.resolved_root / shader_id / 'shader.glsl.resources.json'
        resolution_path = arguments.resolved_root / shader_id / 'resolution.json'
        entry = {
            'shader_id': shader_id,
            'catalogue_stage': record.get('stage') if record else None,
            'catalogue_cg_lowering_before': record.get('cg_lowering') if record else None,
            'catalogue_maxwell_sha256': record.get('maxwell_sha256') if record else None,
            'environment_sha256': record.get('environment_sha256') if record else None,
            'source_path': str(source_path),
            'source_sha256': digest(source_path),
            'resources_path': str(resources_path),
            'resources_sha256': digest(resources_path),
        }
        try:
            if record is None:
                raise Unsupported('catalogue entry missing')
            if record.get('stage') != 'frag':
                raise Unsupported('not a fragment stage')
            checks, resolution = stage_identity_checks(
                record, resolution_path, source_path, resources_path)
            entry['identity_checks'] = checks
            if not all(checks.values()):
                raise Unsupported('resolved source identity mismatch: %r' % checks)
            ir = lower(source_path.read_text(encoding='utf-8'), 'fragment')
            cg, resources = emit_cg(ir)
            reflection = json.loads(resources_path.read_text(encoding='utf-8'))
            checked = check_sampler_descriptors(ir, reflection, resources)
            entry['resources'] = checked
            entry['lowering_notes'] = ir['lowering_notes']
            entry['shadow_contract'] = ir['shadow_contract']
            entry['uniform_budget'] = ir['uniform_budget']
            entry['face_guard'] = {
                'builtin': 'gl_FrontFacing',
                'uniform': 'vp_face_state',
                'used': 'gl_FrontFacing' in ir['reads'],
                'culled_only_runtime_required': True,
                'note': 'unculled FACE use remains explicitly rejected by the runtime',
            }
            declared = sorted(name for name, symbol in ir['symbols'].items()
                              if symbol['storage'] == 'out')
            written = sorted(name for name in declared if name in ir['writes'])
            entry['outputs'] = {
                'declared': declared,
                'written': written,
                'color0_only': written == ['frag_color0'],
                'mrt_blocker': False,
            }
            entry['shadow_calls'] = [
                {'sampler': name, 'coord': 'vec3', 'lod': 0.0, 'return_type': 'float'}
                for name in ir['shadow_contract']['samplers']]
            cg_path = stage_dir / 'shader.frag.cg'
            cg_path.write_text(cg, encoding='utf-8', newline='\n')
            (stage_dir / 'shader.frag.ir.json').write_text(
                json.dumps(ir, indent=2) + '\n', encoding='utf-8', newline='\n')
            entry['cg_path'] = str(cg_path)
            entry['cg_sha256'] = hashlib.sha256(cg.encode()).hexdigest()
            entry['cg_bytes'] = len(cg)
            current_cg[shader_id] = entry
            entry['flags'] = list(optimizations)
            entry['variant_identity_sha256'] = hashlib.sha256(canonical({
                'shader_id': shader_id,
                'stage': 'frag',
                'maxwell_sha256': record['maxwell_sha256'],
                'environment_sha256': record['environment_sha256'],
                'resources_sha256': record['resources_sha256'],
                'cg_sha256': entry['cg_sha256'],
                'flags': entry['flags'],
                'strategy': ir['shadow_contract']['strategy'],
                'policy_sha256': ir['shadow_contract']['policy_sha256'],
                'emitter_sha256': ir['shadow_contract']['emitter_sha256'],
                'helper_sha256': ir['shadow_contract']['helper_sha256'],
                'shadow_abi': ir['shadow_contract']['samplers'],
                'uniform_budget': ir['uniform_budget'],
                'outputs': entry['outputs'],
                'face_guard': entry['face_guard'],
            })).hexdigest()
            entry['status'] = 'lowered'
            entry['lowering_ok'] = True
        except (Unsupported, ValueError, KeyError, FileNotFoundError) as error:
            entry['status'] = 'rejected'
            entry['reason'] = str(error)
            entry['compiled'] = False
            entry['lowering_ok'] = False
            for flag in optimizations:
                entry[result_key(flag)] = result_record(flag, 'not_attempted',
                                                        reason='lowering rejected')
        stages.append(entry)
        print('%s: %s' % (shader_id, entry['status']), flush=True)

    imports = load_import_receipts(
        arguments.import_receipt, optimizations, toolchain, current_cg)
    batch_checkpoint_path = arguments.output / 'checkpoint.json'
    batch_state = {
        'schema': 1,
        'task': TASK,
        'date': '2026-10-27',
        'strategy': STRATEGY,
        'policy_sha256': policy_sha256(),
        'emitter_sha256': emitter_source_sha256(),
        'helper_sha256': helper_source_sha256(),
        'profile': PROFILE_FRAGMENT,
        'optimizations': list(optimizations),
        'service_timeout_seconds': arguments.service_timeout_seconds,
        'outer_timeout_seconds': outer_timeout,
        'toolchain': stage_toolchain_identity(toolchain),
        'stages': {},
        'terminal_stages': [],
        'compiled_stages': [],
        'complete': False,
    }
    write_json(batch_checkpoint_path, batch_state)

    # Compile/import/resume per stage.  Every terminal result is checkpointed
    # immediately; an interruption cannot lose earlier completed entries.
    for entry in stages:
        if entry['status'] == 'rejected':
            continue
        stage_dir = arguments.output / 'stages' / entry['shader_id']
        for flag in optimizations:
            key = result_key(flag)
            identity = stage_identity(entry, flag, toolchain)
            entry[key + '_identity_sha256'] = identity
            result = None
            imported = imports.get(entry['shader_id'])
            if imported is not None and imported['optimization'] == flag:
                result = import_result(imported, imported['receipt_path'],
                                       stage_dir, flag)
                result['identity_sha256'] = identity
                print('%s %s: imported' % (entry['shader_id'], flag), flush=True)
            elif arguments.skip_compile:
                result = existing_result(
                    stage_dir / ('shader.frag%s.gxp' % key),
                    flag, stage_dir / ('shader.frag%s.shacc.log' % key))
                result['identity_sha256'] = identity
            elif arguments.resume:
                result = resume_result(stage_dir, flag, identity, toolchain)
                if result is not None:
                    print('%s %s: resumed' % (entry['shader_id'], flag),
                          flush=True)
            if result is None:
                result = compile_cg(
                    arguments.shacc, config, stage_dir / 'shader.frag.cg',
                    stage_dir / ('shader.frag%s.gxp' % key), flag,
                    stage_dir / ('shader.frag%s.shacc.log' % key),
                    arguments.service_timeout_seconds, outer_timeout)
                result['identity_sha256'] = identity
                print('%s %s: %s (%.3fs)' % (
                    entry['shader_id'], flag, result['status'],
                    result.get('wall_seconds') or 0.0), flush=True)
            receipt_path = stage_dir / ('receipt%s.json' % key)
            receipt = {
                'schema': 1,
                'task': TASK,
                'date': '2026-10-27',
                'shader_id': entry['shader_id'],
                'stage_sha256': entry['catalogue_maxwell_sha256'],
                'environment_sha256': entry['environment_sha256'],
                'source': entry['cg_path'],
                'source_sha256': entry['cg_sha256'],
                'source_bytes': entry['cg_bytes'],
                'profile': PROFILE_FRAGMENT,
                'optimization': flag,
                'flags': [flag],
                'strategy': STRATEGY,
                'policy_sha256': policy_sha256(),
                'emitter_sha256': emitter_source_sha256(),
                'helper_sha256': helper_source_sha256(),
                'toolchain': stage_toolchain_identity(toolchain),
                'service_timeout_seconds': arguments.service_timeout_seconds,
                'outer_timeout_seconds': outer_timeout,
                'identity_sha256': identity,
                'classification': result['status'],
                'compiled': result['compiled'],
                'wall_seconds': result.get('wall_seconds'),
                'returncode': result.get('returncode'),
                'outer_timed_out': result.get('outer_timed_out', False),
                'log': result.get('log'),
                'log_sha256': result.get('log_sha256'),
                'gxp': result.get('gxp'),
                'gxp_present': result.get('gxp_present', result['compiled']),
                'gxp_bytes': result.get('gxp_bytes'),
                'gxp_sha256': result.get('gxp_sha256'),
                'gxp_magic': 'GXP\x00' if result['compiled'] else None,
                'valid_header': bool(result['compiled']),
                'imported': bool(result.get('imported')),
                'import_provenance': result.get('import_provenance'),
                'resumed': bool(result.get('resumed')),
            }
            write_json(receipt_path, receipt)
            result['receipt'] = str(receipt_path)
            result['receipt_sha256'] = digest(receipt_path)
            entry[key] = result
            checkpoint_path = write_stage_checkpoint(
                stage_dir, flag, entry, identity, result, toolchain)
            checkpoint = json.loads(checkpoint_path.read_text(encoding='utf-8'))
            update_batch_checkpoint(batch_checkpoint_path, batch_state,
                                    checkpoint_path, checkpoint)
        entry['compiled'] = all(
            (entry.get(result_key(flag)) or {}).get('compiled')
            for flag in optimizations)
        primary = entry.get(result_key(optimizations[0])) or {}
        if not primary.get('compiled'):
            entry['status'] = {
                'service_timeout': 'compile-service-timeout',
                'compiler_error': 'compile-error',
                'interrupted': 'compile-interrupted',
                'invalid_output': 'compile-invalid-output',
                'not_attempted': 'compile-not-attempted',
            }.get(primary.get('status'), 'compile-failed')
        else:
            entry['status'] = 'lowered-compiled'

    mechanism_dir = arguments.output / 'mechanism'
    mechanism_dir.mkdir(parents=True, exist_ok=True)
    mechanism_cg = mechanism_dir / 'shadow_mechanism.cg'
    mechanism_cg.write_text(mechanism_source(), encoding='utf-8', newline='\n')
    mechanism = {}
    for flag in optimizations:
        stem = 'shadow_mechanism' + flag.replace('-', '')
        if arguments.skip_compile:
            mechanism[flag] = existing_result(
                mechanism_dir / (stem + '.gxp'), flag,
                mechanism_dir / (stem + '.shacc.log'))
        else:
            mechanism[flag] = compile_cg(
                arguments.shacc, config, mechanism_cg,
                mechanism_dir / (stem + '.gxp'), flag,
                mechanism_dir / (stem + '.shacc.log'),
                arguments.service_timeout_seconds, outer_timeout)
    mechanism['cg_sha256'] = hashlib.sha256(mechanism_cg.read_bytes()).hexdigest()
    mechanism['strategy'] = STRATEGY
    mechanism['emitter_sha256'] = emitter_source_sha256()
    mechanism['helper_sha256'] = helper_source_sha256()

    print('auditing 2331 accepted non-shadow Cg outputs...', flush=True)
    nonregression = audit_nonregression(
        arguments.catalogue, arguments.resolved_root, arguments.cg_candidates)
    print('auditing 282-file candidate bank...', flush=True)
    bank = audit_bank(arguments.bank, arguments.expected_bank_manifest_prefix)

    lowered = sum(1 for s in stages if s.get('lowering_ok'))
    compiled_o0 = sum(1 for s in stages if (s.get('o0') or {}).get('compiled'))
    compiled_o1 = sum(1 for s in stages if (s.get('o1') or {}).get('compiled'))
    rejected = sum(1 for s in stages if s.get('status') == 'rejected')
    imported_count = sum(
        1 for s in stages for flag in optimizations
        if (s.get(result_key(flag)) or {}).get('imported'))
    resumed_count = sum(
        1 for s in stages for flag in optimizations
        if (s.get(result_key(flag)) or {}).get('resumed'))
    status_counts = collections.Counter(
        (s.get(result_key(flag)) or {}).get('status', 'not_requested')
        for s in stages for flag in optimizations)
    manifest = {
        'schema': SCHEMA,
        'task': TASK,
        'date': '2026-10-27',
        'strategy': STRATEGY,
        'policy': POLICY,
        'policy_sha256': policy_sha256(),
        'emitter_sha256': emitter_source_sha256(),
        'helper_sha256': helper_source_sha256(),
        'profile': PROFILE_FRAGMENT,
        'optimizations': list(optimizations),
        'service_timeout_seconds': arguments.service_timeout_seconds,
        'outer_timeout_seconds': outer_timeout,
        'source_identity': {
            'strategy': STRATEGY,
            'catalogue': str(arguments.catalogue),
            'catalogue_sha256': digest(arguments.catalogue),
            'resolved_root': str(arguments.resolved_root),
            'shader_ir_sha256': digest(ROOT / 'tools/shader/shader_ir.py'),
            'shadow_helpers_sha256': digest(ROOT / 'tools/shader/shadow_helpers.py'),
            'shadow_reference_sha256': digest(ROOT / 'tools/shader/shadow_reference.py'),
            'compile_tool_sha256': digest(Path(__file__).resolve()),
            'policy_file_sha256': digest(ROOT / 'config/graphics/shadow-sampler-policy.json'),
            'mechanism_cg_sha256': mechanism['cg_sha256'],
        },
        'toolchain': toolchain,
        'imports': [
            {key: value for key, value in imported.items() if key != 'data'}
            for imported in imports.values()],
        'checkpoint': {
            'path': str(batch_checkpoint_path),
            'sha256': digest(batch_checkpoint_path),
            'complete': batch_state['complete'],
            'compiled_stages': batch_state['compiled_stages'],
        },
        'counts': {
            'observed': len(OBSERVED),
            'lowered': lowered,
            'rejected': rejected,
            'compiled_o0': compiled_o0,
            'compiled_o1': compiled_o1,
            'o1_requested': '-O1' in optimizations,
            'imported': imported_count,
            'resumed': resumed_count,
            'status_counts': dict(sorted(status_counts.items())),
            'mechanism_compiled': {
                flag: bool(mechanism[flag].get('compiled'))
                for flag in optimizations},
        },
        'stages': stages,
        'mechanism': mechanism,
        'nonregression': nonregression,
        'candidate_bank': bank,
        'claims': {
            'statement_inline_lowering_implemented': lowered == len(OBSERVED),
            'observed_stages_compiled_o0': compiled_o0 == len(OBSERVED),
            'observed_stages_compiled_o1':
                compiled_o1 == len(OBSERVED) if '-O1' in optimizations else False,
            'o1_not_attempted_in_this_batch': '-O1' not in optimizations,
            'non_shadow_cg_byte_identical': nonregression['identical'],
            'candidate_bank_identity_unchanged': bank['identical'],
            'runtime_binding_implemented': False,
            'runtime_gpu_ready': False,
            'gpu_consumed': False,
            'actual_atlas_observed': False,
            'game_pixels_observed': False,
            'batching_implemented': False,
            'hardware_verified': False,
        },
    }
    write_json(arguments.output / 'manifest.json', manifest)
    write_json(arguments.summary, {
        'schema': SCHEMA,
        'task': TASK,
        'date': '2026-10-27',
        'strategy': STRATEGY,
        'policy': POLICY,
        'policy_sha256': policy_sha256(),
        'emitter_sha256': emitter_source_sha256(),
        'helper_sha256': helper_source_sha256(),
        'profile': PROFILE_FRAGMENT,
        'optimizations': list(optimizations),
        'service_timeout_seconds': arguments.service_timeout_seconds,
        'outer_timeout_seconds': outer_timeout,
        'source_identity': manifest['source_identity'],
        'toolchain': toolchain,
        'imports': manifest['imports'],
        'checkpoint': manifest['checkpoint'],
        'counts': manifest['counts'],
        'stage_identity': [
            {'shader_id': s['shader_id'], 'status': s['status'],
             'cg_sha256': s.get('cg_sha256'),
             'environment_sha256': s.get('environment_sha256'),
             'stage_sha256': s.get('catalogue_maxwell_sha256'),
             'variant_identity_sha256': s.get('variant_identity_sha256'),
             'o0_status': (s.get('o0') or {}).get('status'),
             'gxp_o0_sha256': (s.get('o0') or {}).get('gxp_sha256'),
             'gxp_o0_bytes': (s.get('o0') or {}).get('gxp_bytes'),
             'o0_imported': (s.get('o0') or {}).get('imported', False),
             'o0_resumed': (s.get('o0') or {}).get('resumed', False),
             'o1_status': (s.get('o1') or {}).get('status'),
             'gxp_o1_sha256': (s.get('o1') or {}).get('gxp_sha256'),
             'gxp_o1_bytes': (s.get('o1') or {}).get('gxp_bytes'),
             'o1_imported': (s.get('o1') or {}).get('imported', False),
             'o1_resumed': (s.get('o1') or {}).get('resumed', False),
             'flags': s.get('flags'),
             'uniform_budget': s.get('uniform_budget'),
             'outputs': s.get('outputs'),
             'face_guard': s.get('face_guard'),
             'shadow_contract': s.get('shadow_contract')}
            for s in stages],
        'nonregression': {
            'accepted_catalogue': nonregression['accepted_catalogue'],
            'checked': nonregression['checked'],
            'identical': nonregression['identical'],
            'mismatches': nonregression['mismatches'],
            'unsupported_catalogue': nonregression['unsupported_catalogue'],
        },
        'candidate_bank': {k: bank[k] for k in
                           ('manifest_sha256', 'index_sha256', 'index_json_sha256',
                            'variants', 'gxp_verified', 'flags_counts',
                            'input_signature_counts', 'keys_sha256', 'identical',
                            'problems')},
        'claims': manifest['claims'],
        'open': [
            'runtime TSC decode/binding to the emitted uniforms',
            'real 1024x3072 atlas production and consumption',
            'GXM shadow content probe/readback',
            'game pixels and village performance',
            'bounded batching',
            'hardware/真机 equivalents',
        ],
    })
    print(json.dumps(manifest['counts'], sort_keys=True))
    print('nonregression identical=%s bank identical=%s' %
          (nonregression['identical'], bank['identical']))
    failures = (lowered != len(OBSERVED)
                or any(
                    sum(1 for s in stages
                        if (s.get(result_key(flag)) or {}).get('compiled'))
                    != len(OBSERVED)
                    for flag in optimizations)
                or not nonregression['identical'] or not bank['identical'])
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()

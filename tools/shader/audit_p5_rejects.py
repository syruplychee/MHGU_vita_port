"""Audit actual P5 blockers and preserve baseline candidate identities.

The optional parser diagnostic replaces sampler declarations in memory solely
to inspect the rest of the syntax. Its placeholder calls are never emitted or
compiled and cannot establish shadow/3D support.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

import shader_ir as ir
from translate_captured import normalize_glsl


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class SamplerSyntaxDiagnostic(ir.Parser):
    def __init__(self, source, stage):
        super().__init__(source, stage)
        self.source = source
        self.unsupported_samplers = {}
        for i, token in enumerate(self.ts):
            if token in ('sampler2DShadow', 'sampler3D'):
                self.unsupported_samplers[self.ts[i + 1]] = token
                self.ts[i] = 'sampler2D'

    def make_call(self, function, args):
        if (function in ir.TEXTURE and args and args[0].get('op') == 'ref'
                and args[0]['name'] in self.unsupported_samplers):
            sampler = self.unsupported_samplers[args[0]['name']]
            return dict(op='call', function='UNRESOLVED_' + sampler, args=args,
                        type='float' if sampler == 'sampler2DShadow' else 'vec4')
        return super().make_call(function, args)


def audit(catalogue, resolved, dependencies):
    catalogue, resolved, dependencies = map(Path, (catalogue, resolved, dependencies))
    records = [json.loads(line) for line in catalogue.read_text().splitlines() if line.strip()]
    dependency = json.loads(dependencies.read_text())
    selected = {s['shader_id']: s for s in dependency['stages']}
    baseline_changed, newly_accepted, rejects, observed = [], [], [], []
    for entry in records:
        sid = entry['shader_id']
        source_path = resolved / sid / 'shader.glsl'
        source, _ = normalize_glsl(source_path.read_text(encoding='utf-8'))
        stage = 'vertex' if entry['stage'] == 'vert' else 'fragment'
        error = None
        cg = None
        try:
            cg, _ = ir.emit_cg(ir.lower(source, stage))
        except ir.Unsupported as exc:
            error = str(exc)
        if entry['cg_lowering'] == 'accepted':
            if cg is None or hashlib.sha256(cg.encode()).hexdigest() != entry['cg_sha256']:
                baseline_changed.append(dict(shader_id=sid, error=error))
        elif cg is not None:
            newly_accepted.append(sid)
        if error:
            rejects.append(dict(shader_id=sid, error=error))
        if sid not in selected:
            continue
        samplers = dict((name, ty) for ty, name in re.findall(
            r'uniform\s+(sampler\w+)\s+(\w+)\s*;', source))
        declarations = re.findall(r'out\s+vec4\s+(frag_color\d+)\s*;', source)
        writes = sorted(set(re.findall(r'\b(frag_color\d+)(?:\.[xyzw]+)?\s*=(?!=)', source)))
        dynamic = sorted(set(m.group(0) for m in re.finditer(
            r'\b(?:vs|fs)_cbuf\d+\[(?!\d+\])[^\]]+\](?:\[[^\]]+\])?', source)))
        syntax_error, syntax_writes = None, None
        if any(ty in ('sampler2DShadow', 'sampler3D') for ty in samplers.values()):
            try:
                parsed = SamplerSyntaxDiagnostic(source, stage).parse()
                syntax_writes = sorted(n for n in parsed['writes']
                                       if parsed['symbols'][n]['storage'] == 'out')
            except ir.Unsupported as exc:
                syntax_error = str(exc)
        blockers = []
        if 'sampler2DShadow' in samplers.values():
            blockers.append('shadow depth representation, compare function, compare-before-filter/PCF, LOD and sampler-state binding unverified')
        if 'sampler3D' in samplers.values():
            blockers.append('3D layer/mip/filter/wrap representation and runtime resource binding unimplemented')
        if dynamic:
            blockers.append('dynamic UBO word/component addressing requires bounded storage-buffer ABI; fixed 128-vector uniform path is insufficient')
        if 'bitfield' in source or '<<' in source or '>>' in source:
            blockers.append('raw integer source bits/typed inputs must be available; float-pattern carriers cannot be reconstructed exactly by this profile')
        if any(name != 'frag_color0' for name in writes):
            blockers.append('multiple written render targets require an MRT contract')
        observed.append(dict(shader_id=sid, stage=entry['stage'],
                             resolved_glsl_sha256=digest(source_path),
                             production_gate_error=error, cg_accepted=cg is not None,
                             samplers=samplers, declared_outputs=declarations, written_outputs=writes,
                             non_color0_written=any(name != 'frag_color0' for name in writes),
                             syntax_diagnostic_only=True, syntax_diagnostic_error=syntax_error,
                             syntax_diagnostic_writes=syntax_writes,
                             dynamic_ubo_accesses=dynamic, semantic_blockers=blockers,
                             dependencies=selected[sid]['partners']))
    return dict(schema=1, scope='P5 rejection/identity audit, no target or semantic promotion',
                tool_sha256=digest(__file__), emitter_sha256=digest(Path(ir.__file__)),
                catalogue_sha256=digest(catalogue), dependencies_sha256=digest(dependencies),
                counts=dict(stages=len(records), baseline_accepted=sum(e['cg_lowering']=='accepted' for e in records),
                            baseline_changed=len(baseline_changed), newly_accepted=len(newly_accepted),
                            rejected=len(rejects), observed=len(observed),
                            observed_non_color0_written=sum(e['non_color0_written'] for e in observed)),
                baseline_changed=baseline_changed, newly_accepted=newly_accepted,
                rejected=rejects, observed=observed)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--catalogue', type=Path, required=True)
    ap.add_argument('--resolved', type=Path, required=True)
    ap.add_argument('--dependencies', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    result = audit(args.catalogue, args.resolved, args.dependencies)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result['counts']))
    raise SystemExit(1 if result['baseline_changed'] else 0)

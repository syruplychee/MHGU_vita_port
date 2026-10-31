"""Classify how each declared Cg output component is written.

Why this exists: 8 of 20 captured shaders compiled to GXM binaries, and compiling was
being treated as success. It is not. All seven translated vertex shaders initialise
`out float4 vp_position : POSITION` to a constant and then assign only .y/.z/.w - so
`vp_position.x` never receives a computed value, every vertex lands on x=0, and the
geometry collapses to a line. The companion attribute is short the same way
(`out_attr0.y`). A shader in that state links, compiles and loads and renders
nothing, which is why the gap survived a compile-only gate.

The classification is deliberately factual rather than a pass/fail verdict:

  computed      some assignment gives the component a value derived from a variable
  constant-only it is only ever set to a literal or a constructor of literals
  never         no assignment reaches it

`constant-only` is not by itself a defect - writing a fixed clip-space depth of 0 is
normal - so the tool reports the table and leaves the judgement to the reader. What
makes a program suspicious is a component that should derive from shader input sitting
at `constant-only` while its symmetric partner is `computed`.

Usage:
  python3 tools/shader/check_cg_outputs.py --dir contents/build/vp04-translate
"""

import argparse
import json
import re
from pathlib import Path

COMPONENTS = 'xyzw'
# Type names are not variables, so a right-hand side built only from these and
# literals counts as constant rather than computed.
TYPE_NAMES = {'float', 'float2', 'float3', 'float4', 'int', 'int2', 'int3', 'int4',
              'uint', 'uint2', 'uint3', 'uint4', 'bool', 'bool2', 'bool3', 'bool4',
              'sampler2D', 'samplerCUBE', 'true', 'false'}
PARAM = re.compile(r'\b(out|in)\s+(\w+)\s+(\w+)\s*:\s*([A-Z0-9_]+)')
ASSIGN = re.compile(r'^\s*([A-Za-z_]\w*)(?:\.([xyzw]{1,4}))?\s*=(?!=)([^;]*);',
                    re.MULTILINE)
IDENT = re.compile(r'[A-Za-z_]\w*')


def rhs_is_computed(rhs):
    """True when the value depends on a variable rather than only on literals."""
    for name in IDENT.findall(rhs):
        if name not in TYPE_NAMES:
            return True
    return False


def output_coverage(cg_text):
    """Return [(name, semantic, per-component classification)] for out params."""
    outs = [(m.group(3), m.group(4)) for m in PARAM.finditer(cg_text) if m.group(1) == 'out']
    state = {name: {c: 'never' for c in COMPONENTS} for name, _ in outs}
    for m in ASSIGN.finditer(cg_text):
        name, mask, rhs = m.group(1), m.group(2), m.group(3)
        if name not in state:
            continue
        kind = 'computed' if rhs_is_computed(rhs) else 'constant-only'
        targets = list(mask) if mask else list(COMPONENTS)
        for c in targets:
            # A computed write always wins; a later constant never demotes it.
            if state[name][c] == 'computed':
                continue
            if kind == 'computed':
                state[name][c] = 'computed'
            elif state[name][c] == 'never':
                state[name][c] = 'constant-only'
    rows = []
    for name, semantic in outs:
        rows.append({'output': name, 'semantic': semantic,
                     'components': state[name],
                     'computed': [c for c in COMPONENTS if state[name][c] == 'computed']})
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dir', type=Path, required=True,
                    help='directory holding <program>/shader.cg subdirectories')
    ap.add_argument('--output', type=Path, default=None)
    args = ap.parse_args()

    programs = []
    for cg in sorted(args.dir.glob('*/shader.cg')):
        name = cg.parent.name
        rows = output_coverage(cg.read_text(encoding='utf-8'))
        # A component that should follow shader input but only ever holds a literal
        # is what a lost instruction looks like. Symmetry is the tell: the tool flags
        # a position whose .x is constant-only while .y is computed.
        suspicious = []
        for r in rows:
            comp = r['components']
            if r['semantic'] == 'POSITION' and (comp['x'] != 'computed' or comp['y'] != 'computed'):
                suspicious.append(
                    f"{r['output']}.x is {comp['x']}, .y is {comp['y']}")
        programs.append({'program': name, 'outputs': rows,
                         'suspicious': suspicious})
        detail = '; '.join(suspicious)
        print(f"{'WARN' if suspicious else 'ok  '} {name:<14} {detail}")

    total = len(programs)
    flagged = sum(1 for p in programs if p['suspicious'])
    print()
    print(f'{flagged}/{total} programs have a POSITION component that never receives '
          f'a computed value')
    for p in programs:
        for r in p['outputs']:
            comp = r['components']
            print(f"    {p['program']:<14} {r['output']}.{'/'.join(COMPONENTS)}: "
                  f"{' '.join(f'{c}={comp[c]}' for c in COMPONENTS)}")

    payload = {
        'schema': 1,
        'task': 'VP04.1',
        'method': 'static classification of how each declared Cg output component is '
                  'written: computed / constant-only / never',
        'total_programs': total,
        'programs_with_constant_position_x': flagged,
        'programs': programs,
        'claim': 'a shader that leaves a POSITION component at a constant while its '
                 'partner component is computed links, compiles and loads but cannot '
                 'render the geometry it was translated from; compile success alone '
                 'is not evidence of translation',
        'limits': [
            'constant-only is not by itself a defect: a fixed clip-space depth or w '
            'of 1 is normal, so only the POSITION .x/.y asymmetry is flagged',
            'the check reads generated Cg, not the original SASS, so it says a value '
            'is missing but not which instruction was lost',
        ],
    }
    out = args.output or (args.dir / 'output-coverage.json')
    out.write_text(json.dumps(payload, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print('wrote', out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

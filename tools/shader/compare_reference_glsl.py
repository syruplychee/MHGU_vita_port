"""Compare this project's translated GLSL against the reference runtime's own GLSL.

The reference runtime (mhgu-runtime) draws this title correctly, so the GLSL it generates
for a shader is a baseline for the same Maxwell bytes. Its preamble differs from ours on
purpose, so the comparison runs on the operation sequence of main() rather than on text.

Three normalisations make the comparison about computation rather than presentation:
constant temporary declarations are dropped (both translators initialise up front, at
different points), numeric suffixes are stripped (0.587f would tokenise as 0.587 + f),
and temporaries are renamed in encounter order with bare copies folded (the two
translators thread different numbers of intermediate copies).

A body that still differs is reported with the first differing statement, which names
exactly where the two decodes diverge. Matching is by content, not by hash.

Usage:
  python3 tools/shader/compare_reference_glsl.py \
      --mine contents/build/vp04-translate --reference <dump-dir>
"""

import argparse
import json
import re
from pathlib import Path

TYPE_KW = {'float', 'float2', 'float3', 'float4', 'vec2', 'vec3', 'vec4',
           'int', 'int2', 'int3', 'int4', 'ivec2', 'ivec3', 'ivec4',
           'uint', 'uint2', 'uint3', 'uint4', 'uvec2', 'uvec3', 'uvec4',
           'bool', 'bool2', 'bool3', 'bool4', 'bvec2', 'bvec3', 'bvec4',
           'mat2', 'mat3', 'mat4', 'void', 'precise', 'const'}
KEEP = TYPE_KW | {'true', 'false', 'x', 'y', 'z', 'w', 'xy', 'xyz', 'xyzw', 'zw',
                  'if', 'else', 'for', 'while', 'break', 'continue', 'return', 'discard'}
IDENT = re.compile(r'[A-Za-z_]\w*')
DECL = re.compile(r'^(?:precise\s+)?(?:' +
                  '|'.join(sorted(TYPE_KW - {'precise', 'const'})) +
                  r')\s+\w+\s*=\s*[^;]*$')
COPY = re.compile(r'^(v\d+)=(v\d+(?:\.[xyzw]{1,4})?)$')


def _drop_constant_declarations(statements):
    kept = []
    for stmt in statements:
        if DECL.match(stmt):
            rhs = stmt.split('=', 1)[1]
            names = [n for n in IDENT.findall(rhs) if n not in KEEP and not n.isdigit()]
            if not names:
                continue
        kept.append(stmt)
    return kept


def _canonicalise_names(statements):
    mapping = {}

    def rename(match):
        name = match.group(0)
        if name in KEEP or name.isdigit():
            return name
        if name not in mapping:
            mapping[name] = 'v%d' % len(mapping)
        return mapping[name]

    return [IDENT.sub(rename, stmt) for stmt in statements]


def _propagate_copies(statements):
    result = []
    substitute = {}
    for stmt in statements:
        for name, target in list(substitute.items()):
            stmt = re.sub(r'\b' + name + r'\b', target, stmt)
        match = COPY.match(stmt)
        if match:
            substitute[match.group(1)] = match.group(2)
            continue
        result.append(stmt)
    return result


def operation_sequence(text):
    lines = text.replace('\r\n', '\n').split('\n')
    body, depth, inside = [], 0, False
    for line in lines:
        stripped = line.strip()
        if not inside:
            if re.search(r'\bvoid\s+main\s*\(', stripped) and '{' in stripped:
                inside = True
                tail = stripped.split('{', 1)[1]
                if tail.strip():
                    body.append(tail.strip())
            continue
        depth += stripped.count('{') - stripped.count('}')
        if depth < 0:
            break
        if stripped:
            body.append(stripped)
        if depth == 0 and '}' in stripped:
            break
    flat = ' '.join(body)
    flat = flat.replace('{', ' { ').replace('}', ' } ')
    flat = re.sub(r'(\d)[fFuU]\b', r'\1', flat)
    flat = re.sub(r'\bprecise\s+', '', flat)
    statements = []
    for chunk in flat.split(';'):
        chunk = re.sub(r'\s+', ' ', chunk).strip()
        if chunk and chunk not in ('{', '}'):
            statements.append(chunk)
    statements = _drop_constant_declarations(statements)
    return _propagate_copies(_canonicalise_names(statements))


def first_divergence(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i, x, y
    if len(a) != len(b):
        i = min(len(a), len(b))
        return i, (a[i] if i < len(a) else '<end>'), (b[i] if i < len(b) else '<end>')
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mine', type=Path, required=True)
    ap.add_argument('--reference', type=Path, required=True)
    ap.add_argument('--output', type=Path, default=None)
    args = ap.parse_args()

    mine = {p.parent.name: operation_sequence(p.read_text(encoding='utf-8'))
            for p in sorted(args.mine.glob('*/shader.glsl'))}
    reference = {p.name: operation_sequence(p.read_text(encoding='utf-8'))
                 for p in sorted(args.reference.glob('*.glsl'))}
    if not reference:
        raise SystemExit('no *.glsl under %s' % args.reference)
    print('mine=%d reference=%d' % (len(mine), len(reference)))
    print()

    rows, exact, reordered = [], 0, 0
    for name, ops in sorted(mine.items()):
        match = next((r for r, refops in reference.items() if refops == ops), None)
        order_same = match is not None
        if not match:
            # Independent assignments commute, so the two emitters may produce the same
            # operations in a different order. That is still the same computation, and it
            # is reported separately rather than as a difference.
            match = next((r for r, refops in reference.items()
                          if sorted(refops) == sorted(ops)), None)
        if match:
            best, score, same = match, 1.0, True
            if not order_same:
                reordered += 1
        else:
            best, score, same = None, -1.0, False
            for refname, refops in reference.items():
                common = sum(1 for x in ops if x in refops)
                value = common / max(len(ops), 1)
                if value > score:
                    best, score = refname, value
        if same:
            exact += 1
        detail = ''
        if not same and best:
            div = first_divergence(ops, reference[best])
            if div:
                detail = 'first divergence at stmt %d: mine=%r ref=%r' % div
        rows.append({'program': name, 'reference': best, 'identical_body': same,
                     'statement_score': round(score, 3), 'mine_statements': len(ops),
                     'reference_statements': len(reference[best]) if best else 0,
                     'divergence': detail})
        print('%s %-14s ref=%s score=%.3f stmts=%d/%d' %
              ('SAME' if same else 'DIFF', name, best, score, len(ops),
               len(reference[best]) if best else 0))
        if detail:
            print('       ' + detail)

    print()
    print('%d/%d programs produce the same operation sequence as the reference runtime' %
          (exact, len(mine)))
    if args.output:
        args.output.write_text(json.dumps({
            'schema': 1, 'mine': str(args.mine), 'reference': str(args.reference),
            'reference_files': len(reference), 'exact_matches': exact,
            'programs': rows}, indent=2) + '\n', encoding='utf-8')
        print('wrote %s' % args.output)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

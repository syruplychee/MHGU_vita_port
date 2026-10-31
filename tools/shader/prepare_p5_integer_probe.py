"""Generate runtime-input P5 shift/bitfield differential cases (no game assets).

Operands are uploaded as exact 16-bit halves. Independent Python reference
words are integer literals. The shader reconstructs full 32-bit input words
and compares integers, avoiding float result conversion or NaN readback.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

from shader_ir import emit_cg, lower


def cases():
    result = []
    def add(op, value, offset, bits=0, insert=0):
        mask = (1 << bits) - 1
        if op == 'ushr':
            expected = value >> offset
            expression = 'u>>%du' % offset
        elif op == 'shl':
            expected = (value << offset) & 0xffffffff
            expression = 'u<<%du' % offset
        elif op in ('extract_u', 'extract_s'):
            expected = (value >> offset) & mask
            if op == 'extract_s' and bits and expected & (1 << (bits - 1)):
                expected -= 1 << bits
            expression = ('bitfieldExtract(u,%d,%d)' if op == 'extract_u'
                          else 'uint(bitfieldExtract(int(u),%d,%d))') % (offset, bits)
        else:
            expected = (value & ~(mask << offset)) | ((insert & mask) << offset)
            expression = 'bitfieldInsert(u,ins,%d,%d)' % (offset, bits)
        result.append(dict(op=op, value=value, offset=offset, bits=bits, insert=insert,
                           expected=expected & 0xffffffff, expression=expression))
    for value in (0, 1, 0x80000000, 0xffffffff, 0x89abcdef):
        for shift in (0, 1, 16, 31):
            add('ushr', value, shift)
    for value in (1, 0x80000001, 0x89abcdef):
        for shift in (0, 1, 16, 31):
            add('shl', value, shift)
    for op in ('extract_u', 'extract_s'):
        for value in (0x80000000, 0xffffffff, 0x89abcdef):
            for offset, bits in ((0, 0), (0, 32), (0, 16), (16, 16), (31, 1)):
                add(op, value, offset, bits)
    for value in (0, 0xffffffff, 0x89abcdef):
        for offset, bits in ((0, 0), (0, 32), (8, 16), (31, 1)):
            add('insert', value, offset, bits, 0x76543210)
    return result


def prepare(output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    records = cases()
    n = len(records)
    assert n <= 76
    lines = ['#version 460', 'layout(location=0) out vec4 c;',
             'layout(binding=0) uniform vec4 p5_input[%d];' % n,
             'void main(){c=vec4(0.0,0.0,1.0,1.0);']
    inputs, expected = [], []
    for index, record in enumerate(records):
        x = 13 + index % 38
        row = '<' if index < 38 else '>='
        lines.append('if(int(gl_FragCoord.x)==%d && gl_FragCoord.y%s8.0){' % (x, row))
        # Identifiers are globally unique because the bounded parser does not
        # model shadowing. Inputs stay uniforms at compile time.
        u, ins = 'u%d' % index, 'ins%d' % index
        lines.extend([
            'uint %s=uint((int(p5_input[%d].x)<<16)|int(p5_input[%d].y));' % (u,index,index),
            'uint %s=uint((int(p5_input[%d].z)<<16)|int(p5_input[%d].w));' % (ins,index,index),
            '%s=%s;' % (u, re.sub(r'\b(u|ins)\b', lambda m: u if m[0]=='u' else ins,
                                  record['expression'])),
            'bool good%d=int(%s)==int(%du);' % (index,u,record['expected']),
            'c=good%d?vec4(0.0,1.0,0.0,1.0):vec4(1.0,0.0,0.0,1.0);}' % index,
        ])
        value, insert, answer = record['value'], record['insert'], record['expected']
        inputs.append([value >> 16, value & 65535, insert >> 16, insert & 65535])
        expected.append([answer >> 16, answer & 65535, 0, 0])
    lines.append('}')
    source = '\n'.join(lines) + '\n'
    ir = lower(source, 'fragment')
    cg, abi = emit_cg(ir)
    (output / 'probe.frag.glsl').write_text(source, encoding='utf-8', newline='\n')
    (output / 'probe.cg').write_text(cg, encoding='utf-8', newline='\n')
    header = ['/* Runtime uniforms for the independent P5 integer probe. */']
    for name, values in (('p5_inputs', inputs), ('p5_expected', expected)):
        header.append('static const float %s[%d][4] = {' % (name, n))
        header.extend('    {' + ','.join(str(v) + '.0f' for v in row) + '},' for row in values)
        header.append('};')
    (output / 'p5_integer_cases.h').write_text('\n'.join(header) + '\n', encoding='ascii')
    summary = dict(schema=1, cases=records, abi=abi,
                   tool_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                   emitter_sha256=hashlib.sha256(Path(__file__).with_name('shader_ir.py').read_bytes()).hexdigest(),
                   source_sha256=hashlib.sha256(source.encode()).hexdigest(),
                   cg_sha256=hashlib.sha256(cg.encode()).hexdigest())
    (output / 'cases.json').write_text(json.dumps(summary, indent=2) + '\n')
    return summary


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    print(json.dumps({'cases': len(prepare(args.output)['cases'])}))

"""Lossless vertex RGBA32F sampling through two adjacent GXM RG32F texels.

The selector is 0 for an ordinary texture, 1 for packed/repeat U and 2 for
packed/clamp U. Other packed U modes, linear filtering and mips are refused by
the runtime. Inverse extents always describe the guest's logical RGBA image.
"""
import re

def sample_statements(sampler, coord, tag, target):
    # Inline statements are deliberate. An O0 Cg function call produces a USSE
    # call graph unsupported by the selected local Vita3K translator.
    inv = sampler + '_inv_size'
    mode = sampler + '_rgba32f_packed'
    return [
        f'{tag}_coord = {coord};',
        f'if ({mode} > 0.5) {{',
        f'  {tag}_coord.x = floor({tag}_coord.x / {inv}.x);',
        f'  if ({mode} > 1.5) {tag}_coord.x = min(max({tag}_coord.x, 0.0), 1.0 / {inv}.x - 1.0);',
        f'  {tag}_coord.x = ({tag}_coord.x + 0.25) * {inv}.x;',
        '}',
        f'{target} = tex2Dlod({sampler}, {tag}_coord);',
        f'if ({mode} > 0.5) {{',
        f'  {tag}_coord.x = {tag}_coord.x + 0.5 * {inv}.x;',
        f'  {target}.ba = tex2Dlod({sampler}, {tag}_coord).rg;',
        '}',
    ]



def lower_cg(source):
    """Transform complete, already verified vertex Cg without changing its math.

    Balanced calls retain all existing UV/LOD/offset expressions. Refuse a
    sampled 2D texture using an unhandled builtin rather than partially patch
    one shader. Applying twice is an error, useful for content-key pipelines.
    """
    if '_rgba32f_packed' in source:
        raise ValueError('vertex RG32F lowering already present')
    samplers = set(re.findall(r'\buniform\s+sampler2D\s+(tex\d+)\b', source))
    replacements = []
    used = set()
    for match in re.finditer(r'\b(tex2D\w*)\s*\(\s*(tex\d+)\s*,', source):
        function, sampler = match.groups()
        if sampler not in samplers:
            raise ValueError('sampled sampler has no uniform declaration')
        if function != 'tex2Dlod':
            raise ValueError('unmeasured vertex sampler call: ' + function)
        start = match.end()
        depth = 1
        end = start
        while end < len(source) and depth:
            depth += (source[end] == '(') - (source[end] == ')')
            end += 1
        if depth:
            raise ValueError('unterminated texture call')
        argument = source[start:end-1]
        # The compiler emits a texture call as a standalone assignment. Keep
        # this boundary strict; inserting statements in a condition or loop
        # header would change the original control flow.
        line_start = source.rfind('\n', 0, match.start()) + 1
        line_end = source.find('\n', end)
        if line_end < 0:
            line_end = len(source)
        prefix = source[line_start:match.start()]
        suffix = source[end:line_end]
        assignment = re.fullmatch(r'(\s*)(?:(float4)\s+)?(\w+)\s*=\s*', prefix)
        if assignment is None or suffix.strip() != ';':
            raise ValueError('vertex texture call must be a standalone float4 assignment')
        indent, declaration, target = assignment.groups()
        tag = 'vp_rg32f'
        statements = sample_statements(sampler, argument, tag, target)
        replacement = (f'{indent}float4 {target};\n' if declaration else '')
        replacement += indent + '{\n'
        replacement += '\n'.join(indent + '  ' + line for line in statements)
        replacement += f'\n{indent}}}'
        replacements.append((line_start, line_end, replacement))
        used.add(sampler)
    if not replacements:
        return source, []
    for start, end, text in reversed(replacements):
        source = source[:start] + text + source[end:]
    for sampler in sorted(used):
        declaration = f'uniform sampler2D {sampler}'
        extra = f', uniform float {sampler}_rgba32f_packed'
        if not re.search(r'\buniform\s+float2\s+' + sampler + r'_inv_size\b', source):
            extra += f', uniform float2 {sampler}_inv_size'
        source, count = re.subn(r'\buniform\s+sampler2D\s+' + sampler + r'\b',
                                declaration + extra, source)
        if count != 1:
            raise ValueError('ambiguous sampler declaration')
    main = re.search(r'\bvoid\s+main\s*\(', source)
    if main is None:
        raise ValueError('vertex Cg main missing')
    body = source.index('{', main.end()) + 1
    declarations = '\n  float4 vp_rg32f_coord;\n'
    source = source[:body] + declarations + source[body:]
    return source, sorted(used)

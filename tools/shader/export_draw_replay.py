"""Turn one captured draw into a GXM replay bundle plus an expected image.

Input is a per-draw capture (tools/run_draw_capture.ps1) and the offline shader
conversion of the pipeline that draw used. Output is everything a GXM probe needs
to submit the same draw, and an image computed on the host from the *original*
shader source, which is what the probe's render is compared against.

The host model is a transcription of the original GLSL the reference runtime
decompiled (decompiled/*.glsl), not of the converted Cg: it has to be able to
disagree with the conversion for the comparison to mean anything.
"""
import argparse
import hashlib
import json
import math
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import maxwell_surface as ms

# Only this pipeline pair has a hand-written model. Any other draw is refused
# rather than replayed against a guess.
MODELED_PIPELINE = 'pipeline-0009'

_SC = 1.0 / 255.0


def _attr_float3(blob, offset):
    return list(struct.unpack_from('<3f', blob, offset)) + [1.0]


def _attr_float2(blob, offset):
    return list(struct.unpack_from('<2f', blob, offset)) + [0.0, 1.0]


def _attr_unorm4(blob, offset):
    return [value * _SC for value in struct.unpack_from('<4B', blob, offset)]


# Maxwell vertex fetch words -> (float4 components, reader, GXM attribute format,
# GXM component count, label). Only the layouts the captured draws use.
ATTRIBUTE_FORMATS = {
    (0x01, 7): (4, lambda b, o: list(struct.unpack_from('<4f', b, o)), 'F32', 4, 'R32G32B32A32_FLOAT'),
    (0x02, 7): (4, _attr_float3, 'F32', 3, 'R32G32B32_FLOAT'),
    (0x04, 7): (4, _attr_float2, 'F32', 2, 'R32G32_FLOAT'),
    (0x0A, 2): (4, _attr_unorm4, 'U8N', 4, 'R8G8B8A8_UNORM'),
}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_draws(capture):
    return [json.loads(line) for line in
            (capture / 'draws' / 'draws.jsonl').read_text(encoding='utf-8').splitlines() if line.strip()]


def pipeline_keys(inputs):
    names = {}
    for folder in sorted(Path(inputs).glob('pipeline-*')):
        key = folder / 'key.bin'
        if key.exists():
            names[key.read_bytes()[:48].hex()] = folder.name
    return names


def u32(blob, offset):
    return struct.unpack_from('<I', blob, offset)[0]


# The reference's own register enum names (`engines/maxwell_3d.h`), so the fixed
# state in the manifest can be read against the reference rather than a guess.
BLEND_EQUATION = {0: 'Unset_0', 1: 'Add_D3D', 2: 'Subtract_D3D', 3: 'ReverseSubtract_D3D', 4: 'Min_D3D',
                  5: 'Max_D3D', 0x8006: 'Add_GL', 0x8007: 'Min_GL', 0x8008: 'Max_GL',
                  0x800A: 'Subtract_GL', 0x800B: 'ReverseSubtract_GL'}
BLEND_FACTOR = {0: 'Unset_0', 0x1: 'Zero_D3D', 0x2: 'One_D3D', 0x3: 'SourceColor_D3D',
                0x4: 'OneMinusSourceColor_D3D', 0x5: 'SourceAlpha_D3D',
                0x6: 'OneMinusSourceAlpha_D3D', 0x7: 'DestAlpha_D3D',
                0x8: 'OneMinusDestAlpha_D3D', 0x9: 'DestColor_D3D',
                0xA: 'OneMinusDestColor_D3D', 0xB: 'SourceAlphaSaturate_D3D',
                0xC: 'BothSourceAlpha_D3D', 0xD: 'OneMinusBothSourceAlpha_D3D',
                0xE: 'BlendFactor_D3D', 0xF: 'OneMinusBlendFactor_D3D',
                0x10: 'Source1Color_D3D', 0x11: 'OneMinusSource1Color_D3D',
                0x12: 'Source1Alpha_D3D', 0x13: 'OneMinusSource1Alpha_D3D',
                0x4000: 'Zero_GL', 0x4001: 'One_GL', 0x4300: 'SourceColor_GL',
                0x4301: 'OneMinusSourceColor_GL', 0x4302: 'SourceAlpha_GL',
                0x4303: 'OneMinusSourceAlpha_GL', 0x4304: 'DestAlpha_GL',
                0x4305: 'OneMinusDestAlpha_GL', 0x4306: 'DestColor_GL',
                0x4307: 'OneMinusDestColor_GL', 0x4308: 'SourceAlphaSaturate_GL',
                0xC001: 'ConstantColor_GL', 0xC002: 'OneMinusConstantColor_GL',
                0xC003: 'ConstantAlpha_GL', 0xC004: 'OneMinusConstantAlpha_GL',
                0xC900: 'Source1Color_GL', 0xC901: 'OneMinusSource1Color_GL',
                0xC902: 'Source1Alpha_GL', 0xC903: 'OneMinusSource1Alpha_GL'}
COMPARISON_OP = {1: 'Never_D3D', 2: 'Less_D3D', 3: 'Equal_D3D', 4: 'LessEqual_D3D',
                 5: 'Greater_D3D', 6: 'NotEqual_D3D', 7: 'GreaterEqual_D3D', 8: 'Always_D3D',
                 0x200: 'Never_GL', 0x201: 'Less_GL', 0x202: 'Equal_GL', 0x203: 'LessEqual_GL',
                 0x204: 'Greater_GL', 0x205: 'NotEqual_GL', 0x206: 'GreaterEqual_GL',
                 0x207: 'Always_GL'}

# Register-file offsets, from the reference's `ASSERT_REG_POSITION` table.
REG_BLEND_PER_TARGET = 0x1E00
BLEND_PER_TARGET_SIZE = 0x20


def named(table, value, what):
    if value not in table:
        raise ValueError('%s value 0x%x has no name in the reference enums' % (what, value))
    return table[value]


def alpha_test_failures(expected, covered, func, ref):
    """Covered fragments whose alpha the captured alpha test would reject.

    The comparison is made on the value the render target stores, i.e. after the
    fragment alpha is quantized to the target's 8 bits; the reference compares the
    pre-quantization float, which can only differ for ref values that are not exact
    multiples of 1/255. GXM has no fixed-function alpha test at all, so this count
    is what decides whether dropping it can change a pixel.
    """
    test = {'Greater_D3D': lambda a, r: a > r, 'GreaterEqual_D3D': lambda a, r: a >= r,
            'Less_D3D': lambda a, r: a < r, 'LessEqual_D3D': lambda a, r: a <= r,
            'Equal_D3D': lambda a, r: a == r, 'NotEqual_D3D': lambda a, r: a != r,
            'Always_D3D': lambda a, r: True, 'Never_D3D': lambda a, r: False,
            'Greater_GL': lambda a, r: a > r, 'GreaterEqual_GL': lambda a, r: a >= r,
            'Less_GL': lambda a, r: a < r, 'LessEqual_GL': lambda a, r: a <= r,
            'Equal_GL': lambda a, r: a == r, 'NotEqual_GL': lambda a, r: a != r,
            'Always_GL': lambda a, r: True, 'Never_GL': lambda a, r: False}[func]
    limit = ref * 255.0
    failing = 0
    for index, on in enumerate(covered):
        if on and not test(expected[index * 4 + 3], limit):
            failing += 1
    return failing


def blend_per_target(regs, index):
    """`Regs::blend_per_target[index]` out of the captured register block."""
    base = REG_BLEND_PER_TARGET + index * BLEND_PER_TARGET_SIZE
    separate_alpha = u32(regs, base)
    entry = {'separate_alpha': separate_alpha,
             'color_op': named(BLEND_EQUATION, u32(regs, base + 4), 'blend equation'),
             'color_src': named(BLEND_FACTOR, u32(regs, base + 8), 'blend factor'),
             'color_dst': named(BLEND_FACTOR, u32(regs, base + 12), 'blend factor'),
             'alpha_op': named(BLEND_EQUATION, u32(regs, base + 16), 'blend equation'),
             'alpha_src': named(BLEND_FACTOR, u32(regs, base + 20), 'blend factor'),
             'global_color_key': u32(regs, base + 24),
             'alpha_dst': named(BLEND_FACTOR, u32(regs, base + 28), 'blend factor')}
    return entry


def f32(blob, offset, count=1):
    values = struct.unpack_from('<%df' % count, blob, offset)
    return values[0] if count == 1 else list(values)


class Draw:
    """One captured draw, resolved against the offline conversion."""

    def __init__(self, capture, inputs, converted, draw):
        self.dir = capture / 'draws'
        self.memory = (self.dir / 'memory.bin').read_bytes()
        self.keys = (self.dir / 'keys.bin').read_bytes()
        self.draw = draw
        names = pipeline_keys(inputs)
        key = self.blob(self.keys, draw['key'])[:48].hex()
        self.pipeline = names.get(key)
        if self.pipeline is None:
            raise ValueError('draw %d uses a pipeline with no offline conversion' % draw['seq'])
        if self.pipeline != MODELED_PIPELINE:
            raise ValueError('%s has no written model in this tool' % self.pipeline)
        self.converted = Path(converted) / self.pipeline
        # Each blob lives in its own file: `regs` is an offset into regs.bin, not
        # into memory.bin, and both files start at zero.
        self.regs = self.blob((self.dir / 'regs.bin').read_bytes(), draw['regs'])
        self.tic = self.blob(self.memory, draw['tic']['blob'])
        self.tsc = self.blob(self.memory, draw['tsc']['blob'])
        self.state = draw['state']

    def blob(self, source, entry):
        if not entry:
            raise ValueError('required capture range is missing')
        return source[entry['off']:entry['off'] + entry['size']]

    def cbuf(self, stage, slot):
        for entry in self.draw['cbufs']:
            if entry[0] == stage and entry[1] == slot:
                if not entry[4]:
                    raise ValueError('constant buffer %d.%d was not captured' % (stage, slot))
                return self.blob(self.memory, entry[4])
        raise ValueError('constant buffer %d.%d is not bound' % (stage, slot))

    def surface(self, tic_index):
        for entry in self.draw['tex']:
            if entry['tic'] == tic_index:
                return self.blob(self.memory, entry['bytes']), entry
        raise ValueError('TIC %d has no captured surface window' % tic_index)

    def sampler_handle(self):
        """The texture word the fragment shader reads, from NVN bank 2 offset 32."""
        return u32(self.cbuf(4, 2), 32)

    def texture(self):
        via_header = self.state['sampler_binding'] == 1
        tic_index, tsc_index = ms.texture_handle(self.sampler_handle(), via_header)
        entry = ms.read_entry(self.tic, tic_index)
        sampler = ms.read_entry(self.tsc, tsc_index, cls=ms.TscEntry)
        window, record = self.surface(tic_index)
        needed = ms.surface_size(entry)
        if needed > len(window):
            raise ValueError('surface window holds %d of %d bytes' % (len(window), needed))
        return entry, sampler, window[:needed], record


def parse_cg_uniforms(cg_path):
    """`uniform float4 name[count]` declarations, in declaration order."""
    text = Path(cg_path).read_text(encoding='utf-8')
    head = text.split(') {')[0]
    found = re.findall(r'uniform\s+float4\s+(\w+)\s*\[\s*(\d+)\s*\]', head)
    return [(name, int(count)) for name, count in found]


def vs_model(attr0, attr1, attr2, c3, c4, c5, c6):
    """The captured pipeline's vertex GLSL, transcribed operation for operation.

    `c3`..`c6` are the constant banks the shader declares (`vs_cbuf3` and so on).
    Returns (clip position, out_attr0, out_attr1).
    """
    f0 = attr0[1]
    f1 = -c5[17 * 4 + 3]
    f2 = c3[3 * 4 + 2]
    pf0 = f2 * f1 + 0.0
    f1, f2, f3, f4 = attr0[0], attr0[2], attr1[1], attr1[0]
    f5 = c3[1 * 4 + 0]
    pf1 = f0 * f5
    f5 = attr2[0]
    f6 = c3[1 * 4 + 1]
    pf2 = f0 * f6
    f0 = attr2[1]
    f6 = c3[0 * 4 + 0]
    pf1 = f1 * f6 + pf1
    f6 = attr1[2]
    f7 = c3[0 * 4 + 1]
    pf2 = f1 * f7 + pf2
    f1 = attr1[3]
    pf0 = pf0 + c5[18 * 4 + 3]
    out0 = [0.0, f3, 0.0, 0.0]
    f3 = -c5[17 * 4 + 2]
    f7 = c3[3 * 4 + 2]
    pf3 = f7 * f3 + 0.0
    f3 = 1.0 / pf0
    pf0 = f2 * c3[2 * 4 + 0] + pf1
    out0[0] = f4
    pf1 = f2 * c3[2 * 4 + 1] + pf2
    out0[2] = f6
    pf2 = pf3 + c5[18 * 4 + 2]
    out0[3] = f1
    gl_w = 1.0
    pf0 = pf0 + c3[3 * 4 + 0]
    pf1 = pf1 + c3[3 * 4 + 1]
    pf2 = pf2 * f3
    out1 = [0.0, 0.0, 0.0, 0.0]
    pf3 = f5 * c6[1 * 4 + 0]
    pf4 = f0 * c6[1 * 4 + 1]
    out1[0] = pf3
    pf0 = pf0 * c4[0] + c4[2]
    out1[1] = pf4
    pf1 = pf1 * c4[1] + c4[3]
    return (pf0, pf1, min(pf2, 1.0), gl_w), out0, out1


def fs_model(in_attr0, in_attr1, c3, sample):
    """The captured pipeline's fragment GLSL; `sample(u, v)` returns RGBA floats."""
    span_x = c3[2] - c3[0]
    span_y = c3[3] - c3[1]
    inv_x = 1.0 / span_x
    inv_y = 1.0 / span_y
    pf2 = inv_x * in_attr1[0]
    pf3 = inv_y * in_attr1[1]
    pf0 = span_x * (-math.floor(pf2)) + in_attr1[0]
    pf1 = span_y * (-math.floor(pf3)) + in_attr1[1]
    colour = sample(pf0 + c3[0], pf1 + c3[1])
    return [colour[i] * in_attr0[i] for i in range(4)]


def texture_sampler(descriptor, sampler, pixels):
    """Bilinear or point sampling with the captured wrap mode, in linear RGBA."""
    width, height = descriptor.width, descriptor.height
    wrap = {'wrap': lambda v, n: v % n,
            'mirror': lambda v, n: (v if int(v // n) % 2 == 0 else n - 1 - v) % n,
            'clamp_to_edge': lambda v, n: min(max(v, 0), n - 1),
            'clamp': lambda v, n: min(max(v, 0), n - 1)}

    def texel_address(x, y):
        x = int(wrap[sampler.wrap_u](x, width)) if sampler.wrap_u in wrap else int(x)
        y = int(wrap[sampler.wrap_v](y, height)) if sampler.wrap_v in wrap else int(y)
        return (y * width + x) * 4

    def texel(x, y):
        offset = texel_address(x, y)
        return [pixels[offset + i] / 255.0 for i in range(4)]

    def sample(u, v):
        if sampler.min_filter == 'nearest' and sampler.mag_filter == 'nearest':
            return texel(int(math.floor(u * width)), int(math.floor(v * height)))
        # Texel centres sit at (i + 0.5) / size.
        fx, fy = u * width - 0.5, v * height - 0.5
        x0, y0 = math.floor(fx), math.floor(fy)
        tx, ty = fx - x0, fy - y0
        out = [0.0] * 4
        for dy in (0, 1):
            for dx in (0, 1):
                weight = (tx if dx else 1.0 - tx) * (ty if dy else 1.0 - ty)
                corner = texel(x0 + dx, y0 + dy)
                for i in range(4):
                    out[i] += weight * corner[i]
        return out

    return sample


def rasterize(vertices, viewport, width, height, fragment):
    """Fill the two strip triangles at pixel centres with the top-left rule."""
    positions = [(entry[0][0], entry[0][1]) for entry in vertices]
    scaled = [(x * viewport['scale'][0] + viewport['translate'][0],
               y * viewport['scale'][1] + viewport['translate'][1]) for x, y in positions]
    image = bytearray(width * height * 4)
    covered = bytearray(width * height)
    triangles = [(0, 1, 2), (2, 1, 3)] if len(vertices) == 4 else [(0, 1, 2)]
    for a, b, c in triangles:
        ax, ay = scaled[a]
        bx, by = scaled[b]
        cx, cy = scaled[c]
        area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
        if area == 0.0:
            continue
        min_x = max(0, int(math.floor(min(ax, bx, cx))))
        max_x = min(width, int(math.ceil(max(ax, bx, cx))))
        min_y = max(0, int(math.floor(min(ay, by, cy))))
        max_y = min(height, int(math.ceil(max(ay, by, cy))))
        for py in range(min_y, max_y):
            for px in range(min_x, max_x):
                x, y = px + 0.5, py + 0.5
                w0 = (bx - x) * (cy - y) - (by - y) * (cx - x)
                w1 = (cx - x) * (ay - y) - (cy - y) * (ax - x)
                w2 = (ax - x) * (by - y) - (ay - y) * (bx - x)
                inside = all((weight >= 0.0) if area > 0.0 else (weight <= 0.0)
                             for weight in (w0, w1, w2))
                if not inside:
                    continue
                covered[py * width + px] = 1
                lambdas = (w0 / area, w1 / area, w2 / area)
                attributes = []
                for attribute in (1, 2):  # out_attr0 and out_attr1
                    attributes.append(
                        tuple(lambdas[0] * vertices[a][attribute][k] +
                              lambdas[1] * vertices[b][attribute][k] +
                              lambdas[2] * vertices[c][attribute][k] for k in range(4)))
                colour = fragment(attributes[0], attributes[1], None)
                target = (py * width + px) * 4
                image[target:target + 4] = bytes(
                    max(0, min(255, int(round(value * 255.0)))) for value in colour)
    return bytes(image), bytes(covered)


def write_png(path, rgba, width, height):
    import zlib

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += rgba[y * width * 4:(y + 1) * width * 4]

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + \
            struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    header = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
                           chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--converted', type=Path, required=True)
    parser.add_argument('--draw', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    draws = load_draws(args.capture)
    record = next((d for d in draws if d['seq'] == args.draw), None)
    if record is None:
        raise SystemExit('draw %d is not in this capture' % args.draw)
    draw = Draw(args.capture, args.inputs, args.converted, record)

    # Vertex data: the captured stream slice the draw's attributes read.
    streams = record['streams']
    if len(streams) != 1:
        raise ValueError('the model expects a single vertex stream')
    stream = streams[0]
    stride = stream[2]
    first = record['vertex_first']
    count = record['vertex_count']
    if stride == 0 or count == 0:
        raise ValueError('empty vertex range')
    stream_bytes = draw.blob(draw.memory, stream[4])
    vertices_raw = stream_bytes[first * stride:(first + count) * stride]

    # Attributes, from the vertex_fetch words the capture decoded. Maxwell fills
    # the components a fetch does not supply with (0, 0, 0, 1).
    attributes = []
    for index, buffer, offset, size, kind, constant, _bgra in record['state']['vattrs']:
        if constant == 1:
            # Fixed-value attribute: the runtime supplies it from its replacement
            # table, so no stream slice is needed. The converted shader does not
            # declare these slots either.
            continue
        if buffer != 0:
            raise ValueError('attribute %d reads stream %d' % (index, buffer))
        key = (size, kind)
        if key not in ATTRIBUTE_FORMATS:
            raise ValueError('attribute %d has unsupported size 0x%02x type %d'
                             % (index, size, kind))
        components, decoder, gxm_format, gxm_components, name = ATTRIBUTE_FORMATS[key]
        attributes.append((index, offset, decoder, name, gxm_format, gxm_components))
    if [index for index, *_ in attributes] != [0, 1, 2]:
        raise ValueError('unexpected attribute set %s' % [index for index, *_ in attributes])

    def read_vertex(v):
        base = v * stride
        return [decoder(vertices_raw, base + offset) for _index, offset, decoder, *_ in attributes]

    # Uniforms come from the draw's own constant buffers, at the bank the offline
    # conversion bound each name to, sliced to the vectors that conversion needs.
    declared = {'vertex': parse_cg_uniforms(draw.converted / '0-vertex_b.cg'),
                'fragment': parse_cg_uniforms(draw.converted / '1-fragment.cg')}
    report = json.loads((Path(args.converted) / 'report.json').read_text(encoding='utf-8'))
    pair = next(item for item in report['pipelines'] if item['index'] == int(draw.pipeline[-4:]))
    needed = {(stage['stage'], resource['name']): resource['upload_vectors']
              for stage in pair['stages'] for resource in stage['resources']
              if resource['storage'] == 'cbuf'}
    stage_stage = {'vertex': ('vertex_b', 0), 'fragment': ('fragment', 4)}
    uniforms, uniform_list = {}, []
    for stage, items in declared.items():
        stage_name, cbuf_stage = stage_stage[stage]
        uniforms[stage] = {}
        for name, expected in items:
            bank = int(name.split('cbuf')[1])
            wanted = needed.get((stage_name, name))
            if wanted is None:
                raise ValueError('%s is not in the conversion report' % name)
            if wanted != expected:
                raise ValueError('%s: conversion wants %d vectors, Cg declares %d'
                                 % (name, wanted, expected))
            if expected > 128:
                raise ValueError('%s: refusing to upload %d vectors' % (name, expected))
            values = struct.unpack_from('<%df' % (expected * 4), draw.cbuf(cbuf_stage, bank), 0)
            uniforms[stage][name] = list(values)
            uniform_list.append((name, expected, expected))
    c3 = uniforms['vertex']['vs_cbuf3']
    c4 = uniforms['vertex']['vs_cbuf4']
    c5 = uniforms['vertex']['vs_cbuf5']
    c6 = uniforms['vertex']['vs_cbuf6']
    fs_c3 = uniforms['fragment']['fs_cbuf3']

    descriptor, sampler, surface, surface_record = draw.texture()
    pixels = ms.untile(descriptor, surface)

    vertex_out = [vs_model(*read_vertex(v), c3=c3, c4=c4, c5=c5, c6=c6) for v in range(count)]
    positions = [entry[0] for entry in vertex_out]
    outputs = [(entry[0], entry[1], entry[2]) for entry in vertex_out]

    viewport = record['state']['viewport'][0]
    rt = record['rt']['targets'][0]
    rt_width, rt_height = rt[2], rt[3]
    sampler_fn = texture_sampler(descriptor, sampler, pixels)

    def fragment(in_attr0, in_attr1, _in_attr2):
        return fs_model(in_attr0, in_attr1, fs_c3, sampler_fn)

    expected, covered = rasterize(outputs, viewport, rt_width, rt_height, fragment)

    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'vertex.bin').write_bytes(vertices_raw)
    (args.output / 'texture.rgba').write_bytes(pixels)
    (args.output / 'oracle.rgba').write_bytes(expected)
    (args.output / 'oracle-coverage.bin').write_bytes(covered)
    (args.output / 'replay_v.gxp').write_bytes((draw.converted / '0-vertex_b.gxp').read_bytes())
    (args.output / 'replay_f.gxp').write_bytes((draw.converted / '1-fragment.gxp').read_bytes())

    uniforms_raw = bytearray()
    uniform_offsets = {}
    for name, _expected, _upload in uniform_list:
        values = uniforms['vertex'].get(name) or uniforms['fragment'][name]
        uniform_offsets[name] = len(uniforms_raw) // 4
        uniforms_raw += struct.pack('<%df' % len(values), *values)
    (args.output / 'uniforms.bin').write_bytes(bytes(uniforms_raw))

    covered_count = sum(covered)
    alpha_failing = alpha_test_failures(
        expected, covered, named(COMPARISON_OP, record['state']['alpha_test']['func'], 'comparison'),
        record['state']['alpha_test']['ref'])
    covered_alpha = [expected[index * 4 + 3] for index, on in enumerate(covered) if on]
    alpha_min, alpha_max = (min(covered_alpha), max(covered_alpha)) if covered_alpha else (0, 0)
    manifest = {
        'schema': 1,
        'task': 'VP01.6.5',
        'capture': str(args.capture),
        'draw_seq': record['seq'],
        'pipeline': draw.pipeline,
        'pipeline_key_prefix': draw.blob(draw.keys, record['key'])[:48].hex(),
        'primitive': record['topology'],
        'vertex_count': count,
        'vertex_stride': stride,
        'vertex_first': first,
        'attributes': [{'location': index, 'stream': 0, 'offset': offset, 'label': name,
                        'gxm_format': gxm_format, 'gxm_components': gxm_components}
                       for index, offset, _decoder, name, gxm_format, gxm_components in attributes],
        'render_target': {'width': rt_width, 'height': rt_height, 'format': rt[4], 'tile': rt[5],
                          'format_name': 'A8B8G8R8_UNORM'},
        'viewport': viewport,
        'scissor': record['state']['scissor'][0],
        'color_mask': struct.unpack('<I', struct.pack('<I', record['state']['color_mask'][0]))[0],
        'depth': {'test': record['state']['depth']['test'],
                  'write': record['state']['depth']['write'],
                  'func': named(COMPARISON_OP, record['state']['depth']['func'], 'comparison'),
                  'bounds_enable': record['state']['depth']['bounds_enable'],
                  'bounds': record['state']['depth']['bounds'],
                  'effect': 'depth test is disabled, so the reference never writes depth '
                            '(Vulkan ignores depthWriteEnable when depthTestEnable is false); '
                            'the probe matches that with FUNCTION_ALWAYS and writes off'},
        'blend': {'enable_of_attachment0': record['state']['blend']['enable'][0],
                  'per_target_enabled': record['state']['blend']['per_target'],
                  'equations_from': 'blend_per_target[0]' if record['state']['blend']['per_target']
                                    else 'blend',
                  **blend_per_target(draw.regs, 0),
                  'notes': ['The reference reads blend.enable[attachment] regardless of '
                            'blend_per_target_enabled (fixed_pipeline_state.cpp '
                            'BlendingAttachment::Refresh), so attachment 0 blends here.',
                            'separate_alpha 0 does not make the reference reuse the colour '
                            'equations: setup_blend reads the alpha fields unconditionally.',
                            'alpha_dest is 0, which is not a Factor value. The reference '
                            'PackBlendFactor falls through its switch (UNIMPLEMENTED_MSG) and '
                            'returns 0, i.e. Zero.',
                            'enable_global_color_key is set in blend_per_target[0] but is only '
                            'declared in the reference (engines/maxwell_3d.h) and never read, '
                            'so it has no effect on the reference output.',
                            'The colour equation keeps the destination term, so this draw only '
                            'reduces to the shader output where the source alpha is 1; that is '
                            'checked against the oracle in alpha_test/covered.']},
        'alpha_test': {'enable': record['state']['alpha_test']['enable'],
                       'ref': record['state']['alpha_test']['ref'],
                       'func': named(COMPARISON_OP, record['state']['alpha_test']['func'],
                                     'comparison'),
                       'failing_pixels': alpha_failing, 'covered_pixels': covered_count,
                       'note': 'GXM has no fixed-function alpha test; the state is only '
                               'reproducible through the shader. This capture is replayed '
                               'without one, which the replay script accepts only while '
                               'failing_pixels is 0.'},
        'alpha_stats': {'min': alpha_min, 'max': alpha_max, 'covered': covered_count},
        'cull': record['state']['cull'],
        'texture': {'tic': surface_record['tic'], 'width': descriptor.width,
                    'height': descriptor.height, 'format': descriptor.format,
                    'format_name': 'DXT1', 'decoded': 'A8B8G8R8',
                    'surface_bytes': ms.surface_size(descriptor),
                    'window_bytes': surface_record['bytes']['size'],
                    'address': surface_record['address'],
                    'tic_srgb_conversion': descriptor.srgb_conversion,
                    'srgb_note': 'The reference decides sampling sRGB from the TIC entry '
                                 '(image_info.cpp passes config.srgb_conversion into '
                                 'PixelFormatFromTextureInfo); this TIC has 0, so the sampled '
                                 'values are the stored 8-bit ones with no sRGB decode. The TSC '
                                 'srgb bit is only read for the border colour, which is not used '
                                 'with the captured repeat wrap mode.'},
        'sampler': sampler.describe(),
        'uniforms': {name: {'offset_floats': uniform_offsets[name],
                            'count_vec4': expected,
                            'upload_vectors': uploads}
                     for name, expected, uploads in uniform_list},
        'covered_pixels': covered_count,
        'covered_fraction': covered_count / float(rt_width * rt_height),
        'hashes': {'vertex_bin': sha256(args.output / 'vertex.bin'),
                   'texture_rgba': sha256(args.output / 'texture.rgba'),
                   'oracle_rgba': sha256(args.output / 'oracle.rgba'),
                   'uniforms_bin': sha256(args.output / 'uniforms.bin'),
                   'replay_v_gxp': sha256(args.output / 'replay_v.gxp'),
                   'replay_f_gxp': sha256(args.output / 'replay_f.gxp')},
        'oracle_model': 'original GLSL of %s, transcribed; float64 host arithmetic' % draw.pipeline,
        'oracle_is_not_the_emulator_frame': True,
        'gxm_render_is_not_warranted_by_this_file': True,
        'captured_bytes_are_game_content': True,
    }
    (args.output / 'replay.json').write_text(json.dumps(manifest, indent=2) + '\n',
                                             encoding='utf-8', newline='\n')
    write_png(args.output / 'texture.png', pixels, descriptor.width, descriptor.height)
    write_png(args.output / 'oracle.png', expected, rt_width, rt_height)
    print(json.dumps({'draw': record['seq'], 'pipeline': draw.pipeline,
                      'texture': '%s %dx%d' % (manifest['texture']['format_name'],
                                               descriptor.width, descriptor.height),
                      'covered': covered_count, 'output': str(args.output)}))


if __name__ == '__main__':
    main()

"""Verify a GXM replay bundle and emit the probe's contract header.

The probe reads the binary parts (vertex.bin, texture.rgba, uniforms.bin) and the
two converted GXP programs; every scalar it needs is baked into the generated
header so the on-device code has no parser and no defaults of its own.
"""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

REQUIRED = ('vertex.bin', 'texture.rgba', 'uniforms.bin', 'replay_v.gxp', 'replay_f.gxp',
            'oracle.rgba', 'replay.json')
GXM_PRIMITIVE = {5: 'SCE_GXM_PRIMITIVE_TRIANGLE_STRIP', 4: 'SCE_GXM_PRIMITIVE_TRIANGLES'}
GXM_FORMAT = {'F32': 'SCE_GXM_ATTRIBUTE_FORMAT_F32', 'U8N': 'SCE_GXM_ATTRIBUTE_FORMAT_U8N'}
GXM_WRAP = {'wrap': 'SCE_GXM_TEXTURE_ADDR_REPEAT', 'clamp_to_edge': 'SCE_GXM_TEXTURE_ADDR_CLAMP',
            'clamp': 'SCE_GXM_TEXTURE_ADDR_CLAMP', 'mirror': 'SCE_GXM_TEXTURE_ADDR_MIRROR'}
GXM_FILTER = {'nearest': 'SCE_GXM_TEXTURE_FILTER_POINT', 'linear': 'SCE_GXM_TEXTURE_FILTER_LINEAR'}

# Maxwell blend state, as the reference names it, to the GXM enum that means the
# same thing. `Unset_0` is a register the game never wrote; the reference treats a
# 0 equation as Add and a 0 factor as Zero, so those are the spellings used here.
GXM_BLEND_FUNC = {'Add_D3D': 'SCE_GXM_BLEND_FUNC_ADD', 'Subtract_D3D': 'SCE_GXM_BLEND_FUNC_SUBTRACT',
                  'ReverseSubtract_D3D': 'SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT',
                  'Min_D3D': 'SCE_GXM_BLEND_FUNC_MIN', 'Max_D3D': 'SCE_GXM_BLEND_FUNC_MAX',
                  'Unset_0': 'SCE_GXM_BLEND_FUNC_ADD'}
GXM_BLEND_FACTOR = {'Zero_D3D': 'SCE_GXM_BLEND_FACTOR_ZERO', 'One_D3D': 'SCE_GXM_BLEND_FACTOR_ONE',
                    'SourceColor_D3D': 'SCE_GXM_BLEND_FACTOR_SRC_COLOR',
                    'OneMinusSourceColor_D3D': 'SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR',
                    'SourceAlpha_D3D': 'SCE_GXM_BLEND_FACTOR_SRC_ALPHA',
                    'OneMinusSourceAlpha_D3D': 'SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA',
                    'DestAlpha_D3D': 'SCE_GXM_BLEND_FACTOR_DST_ALPHA',
                    'OneMinusDestAlpha_D3D': 'SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA',
                    'DestColor_D3D': 'SCE_GXM_BLEND_FACTOR_DST_COLOR',
                    'OneMinusDestColor_D3D': 'SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR',
                    'SourceAlphaSaturate_D3D': 'SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE',
                    'Unset_0': 'SCE_GXM_BLEND_FACTOR_ZERO'}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def cfloat(value):
    """A float literal that is valid C, unlike '%df' % 960.0 which prints '960f'."""
    return repr(float(value)) + 'f'


def verify(bundle):
    for name in REQUIRED:
        if not (bundle / name).exists():
            raise ValueError('replay bundle is missing %s' % name)
    manifest = json.loads((bundle / 'replay.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or manifest.get('task') != 'VP01.6.5':
        raise ValueError('unrecognized replay bundle')
    for name, expected in manifest['hashes'].items():
        file = {'vertex_bin': 'vertex.bin', 'texture_rgba': 'texture.rgba',
                'oracle_rgba': 'oracle.rgba', 'uniforms_bin': 'uniforms.bin',
                'replay_v_gxp': 'replay_v.gxp', 'replay_f_gxp': 'replay_f.gxp'}[name]
        if sha256(bundle / file) != expected:
            raise ValueError('%s does not match its recorded hash' % file)
    if manifest['primitive'] not in GXM_PRIMITIVE:
        raise ValueError('primitive %d has no GXM spelling' % manifest['primitive'])
    for attribute in manifest['attributes']:
        if attribute['gxm_format'] not in GXM_FORMAT:
            raise ValueError('attribute format %s has no GXM spelling' % attribute['gxm_format'])
    if manifest['sampler']['mag_filter'] not in GXM_FILTER or \
            manifest['sampler']['min_filter'] not in GXM_FILTER:
        raise ValueError('sampler filter has no GXM spelling: %s' % manifest['sampler'])
    if manifest['sampler']['wrap_u'] not in GXM_WRAP or manifest['sampler']['wrap_v'] not in GXM_WRAP:
        raise ValueError('sampler wrap has no GXM spelling: %s' % manifest['sampler'])
    # Fixed-function state. Blend is translated; the rest is either checked against
    # what the probe writes or refused, never assumed.
    blend = manifest['blend']
    for name in ('color_op', 'color_src', 'color_dst', 'alpha_op', 'alpha_src', 'alpha_dst'):
        table = GXM_BLEND_FACTOR if name.endswith(('_src', '_dst')) else GXM_BLEND_FUNC
        if blend[name] not in table:
            raise ValueError('blend %s %s has no GXM equivalent' % (name, blend[name]))
    if not blend['per_target_enabled']:
        raise ValueError('shared (non per-target) blend registers are not decoded yet')
    if blend['separate_alpha']:
        raise ValueError('separate_alpha would need the alpha equations re-read from '
                         'blend_per_target[0]')
    if manifest['alpha_test']['enable'] and manifest['alpha_test']['failing_pixels']:
        raise ValueError('the captured alpha test rejects %d covered fragments and GXM has no '
                         'fixed-function alpha test' % manifest['alpha_test']['failing_pixels'])
    if manifest['depth']['bounds_enable'] and manifest['depth']['bounds'] != [0.0, 1.0]:
        raise ValueError('unexpected depth bounds: %s' % manifest['depth']['bounds'])
    # The probe keeps depth testing off. That is only faithful while the capture has
    # it off too: with depthTestEnable false the reference writes no depth whatever
    # depth_write_enabled says (Vulkan ignores depthWriteEnable when depthTestEnable
    # is false), which is exactly what the probe's always-pass, write-off pair does.
    if manifest['depth']['test']:
        raise ValueError('the probe does not translate an enabled depth test (func %s)'
                         % manifest['depth']['func'])
    if manifest['cull']['gl_enable'] or manifest['cull']['d3d_mode']:
        raise ValueError('the probe does not translate the captured cull state')
    if manifest['color_mask'] != 0x1111:
        raise ValueError('the probe assumes an all-channels colour mask, not 0x%x'
                         % manifest['color_mask'])
    if manifest['texture']['tic_srgb_conversion']:
        raise ValueError('an sRGB-decoding TIC entry is not replayed by this probe')
    uniform_bytes = (bundle / 'uniforms.bin').stat().st_size
    for name, entry in manifest['uniforms'].items():
        end = (entry['offset_floats'] + entry['count_vec4'] * 4) * 4
        if end > uniform_bytes:
            raise ValueError('%s runs past uniforms.bin' % name)
    manifest['uniform_bytes'] = uniform_bytes
    return manifest


def emit_header(manifest, header):
    blend = manifest['blend']
    lines = ['/* Generated by tools/shader/prepare_gxm_replay.py. Do not edit. */',
             '#pragma once', '',
             '#define VP_REPLAY_DRAW_SEQ %d' % manifest['draw_seq'],
             '#define VP_REPLAY_PIPELINE "%s"' % manifest['pipeline'],
             '#define VP_REPLAY_PRIMITIVE %s' % GXM_PRIMITIVE[manifest['primitive']],
             '#define VP_REPLAY_VERTEX_COUNT %d' % manifest['vertex_count'],
             '#define VP_REPLAY_VERTEX_STRIDE %d' % manifest['vertex_stride'],
             '#define VP_REPLAY_RT_WIDTH %d' % manifest['render_target']['width'],
             '#define VP_REPLAY_RT_HEIGHT %d' % manifest['render_target']['height'],
             '#define VP_REPLAY_TEX_WIDTH %d' % manifest['texture']['width'],
             '#define VP_REPLAY_TEX_HEIGHT %d' % manifest['texture']['height'],
             '#define VP_REPLAY_VIEWPORT_X %s' % cfloat(manifest['viewport']['scale'][0]),
             '#define VP_REPLAY_VIEWPORT_Y %s' % cfloat(manifest['viewport']['scale'][1]),
             '#define VP_REPLAY_VIEWPORT_CX %s' % cfloat(manifest['viewport']['translate'][0]),
             '#define VP_REPLAY_VIEWPORT_CY %s' % cfloat(manifest['viewport']['translate'][1]),
             '#define VP_REPLAY_SCISSOR_X0 %d' % manifest['scissor']['min_x'],
             '#define VP_REPLAY_SCISSOR_Y0 %d' % manifest['scissor']['min_y'],
             '#define VP_REPLAY_SCISSOR_X1 %d' % (manifest['scissor']['max_x'] - 1),
             '#define VP_REPLAY_SCISSOR_Y1 %d' % (manifest['scissor']['max_y'] - 1),
             '#define VP_REPLAY_WRAP_U %s' % GXM_WRAP[manifest['sampler']['wrap_u']],
             '#define VP_REPLAY_WRAP_V %s' % GXM_WRAP[manifest['sampler']['wrap_v']],
             '#define VP_REPLAY_MAG_FILTER %s' % GXM_FILTER[manifest['sampler']['mag_filter']],
             '#define VP_REPLAY_MIN_FILTER %s' % GXM_FILTER[manifest['sampler']['min_filter']],
             '#define VP_REPLAY_BLEND %d' % (1 if blend['enable_of_attachment0'] else 0),
             '#define VP_REPLAY_BLEND_COLOR_FUNC %s' % GXM_BLEND_FUNC[blend['color_op']],
             '#define VP_REPLAY_BLEND_COLOR_SRC %s' % GXM_BLEND_FACTOR[blend['color_src']],
             '#define VP_REPLAY_BLEND_COLOR_DST %s' % GXM_BLEND_FACTOR[blend['color_dst']],
             '#define VP_REPLAY_BLEND_ALPHA_FUNC %s' % GXM_BLEND_FUNC[blend['alpha_op']],
             '#define VP_REPLAY_BLEND_ALPHA_SRC %s' % GXM_BLEND_FACTOR[blend['alpha_src']],
             '#define VP_REPLAY_BLEND_ALPHA_DST %s' % GXM_BLEND_FACTOR[blend['alpha_dst']],
             '#define VP_REPLAY_DEPTH_TEST %d' % (1 if manifest['depth']['test'] else 0),
             '/* depth_write_enabled is %d in the capture; the reference writes no depth at all'
             ' while the test is off, so the effective value is what the probe uses. */'
             % manifest['depth']['write'],
             '#define VP_REPLAY_DEPTH_WRITE %d'
             % (1 if (manifest['depth']['test'] and manifest['depth']['write']) else 0),
             '#define VP_REPLAY_COLOR_MASK %d' % manifest['color_mask'],
             '#define VP_REPLAY_UNIFORM_BYTES %d' % manifest['uniform_bytes'],
             '#define VP_REPLAY_ORACLE_SHA256 "%s"' % manifest['hashes']['oracle_rgba'],
             '#define VP_REPLAY_VERTEX_SHA256 "%s"' % manifest['hashes']['vertex_bin'],
             '#define VP_REPLAY_TEXTURE_SHA256 "%s"' % manifest['hashes']['texture_rgba'],
             '#define VP_REPLAY_UNIFORMS_SHA256 "%s"' % manifest['hashes']['uniforms_bin'],
             '', 'typedef struct { unsigned location; unsigned offset; const char *format;'
                 ' unsigned components; } vp_replay_attribute;',
             'typedef struct { const char *name; unsigned offset_floats; unsigned count_vec4;'
             ' } vp_replay_uniform;', '',
             'static const vp_replay_attribute VP_REPLAY_ATTRIBUTES[] = {']
    for attribute in manifest['attributes']:
        lines.append('    {%d, %d, "%s", %d},' % (
            attribute['location'], attribute['offset'], attribute['gxm_format'],
            attribute['gxm_components']))
    lines.append('};')
    lines.append('#define VP_REPLAY_ATTRIBUTE_COUNT %d' % len(manifest['attributes']))
    lines.append('')
    lines.append('static const vp_replay_uniform VP_REPLAY_UNIFORMS[] = {')
    for name, entry in manifest['uniforms'].items():
        lines.append('    {"%s", %d, %d},' % (name, entry['offset_floats'], entry['count_vec4']))
    lines.append('};')
    lines.append('#define VP_REPLAY_UNIFORM_COUNT %d' % len(manifest['uniforms']))
    lines.append('')
    Path(header).write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--header', type=Path)
    parser.add_argument('--stage', type=Path, help='directory to copy the on-device files into')
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    manifest = verify(args.bundle)
    if args.header:
        args.header.parent.mkdir(parents=True, exist_ok=True)
        emit_header(manifest, args.header)
    if args.stage:
        args.stage.mkdir(parents=True, exist_ok=True)
        for name in ('vertex.bin', 'texture.rgba', 'uniforms.bin', 'replay_v.gxp', 'replay_f.gxp'):
            shutil.copyfile(args.bundle / name, args.stage / name)
    print(json.dumps({'draw': manifest['draw_seq'], 'pipeline': manifest['pipeline'],
                      'attributes': len(manifest['attributes']),
                      'uniforms': len(manifest['uniforms']), 'verified': True}))


if __name__ == '__main__':
    main()

"""Offline sampler2DShadow policy, ABI and statement-inline Cg emitter (M6-B21).

This module is the host-pure contract for the observed shadow call shape::

    textureLod(sampler2DShadow, vec3(uv, ref), 0.f)

Only that explicit LOD0 form is lowered.  The emitted Cg binds the real depth
allocation as an ordinary ``sampler2D`` (depth is read from R), performs the
Maxwell wrap modes and point/linear comparison filtering itself, and never
assumes a target-side hardware compare sampler.  The native sampler state is
therefore point, clamp-to-edge, no mip and no compare.

The B21 lowering expands each call to scoped statements through
``InlineEmitter`` instead of calling nested Cg helper functions.  Every call
evaluates its ``float3`` coordinate once, every tap names its depth read once,
and no shadow helper function is emitted or called.  The canonical emitter
sample (not a callable helper library) is the identity hashed into the ABI
contract.

The Maxwell TSC bit positions come from the primary reference implementation
(yuzu ``src/video_core/textures/texture.h`` TSCEntry and
``renderer_opengl/gl_texture_cache.cpp`` Sampler::Sampler); the border depth is
the R (first) float of the TSC border colour words 4..7, which is the component
the primary configures as ``GL_TEXTURE_BORDER_COLOR``.  This module does not
invent a value when the descriptor is outside the measured set: every
unsupported field produces a named rejection.

The policy document is the shared runtime contract for the next round.  It is
also the input to ``shadow_policy_sha256`` so a variant identity cannot silently
change compare/filter/wrap semantics.
"""
import hashlib
import json
import math
import struct

SCHEMA = 1
POLICY = 'vp-shadow-policy-2'

COMPARE_NAMES = ('Never', 'Less', 'Equal', 'LessEqual',
                 'Greater', 'NotEqual', 'GreaterEqual', 'Always')
WRAP_NAMES = ('Repeat', 'Mirror', 'ClampToEdge', 'Border',
              'Clamp', 'MirrorOnceClampToEdge', 'MirrorOnceBorder',
              'MirrorOnceClampOGL')
# Maxwell enums (yuzu texture.h): Nearest=1, Linear=2; Mipmap None=1,
# Nearest=2, Linear=3.  Zero and three are not defined filter values.
TSC_FILTER = {1: 'point', 2: 'linear'}
TSC_MIP_FILTER = {1: 'none', 2: 'nearest', 3: 'linear'}
WRAP_SUPPORTED = (0, 1, 2, 3)


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=False).encode()


def digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def f32_from_bits(bits):
    return struct.unpack('<f', struct.pack('<I', bits & 0xFFFFFFFF))[0]


def f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', float(value)))[0]


def sign_extend(value, width):
    sign = 1 << (width - 1)
    return (value ^ sign) - sign


# --- policy document -------------------------------------------------------

def policy_document():
    """Return the exact accepted/rejected ranges and the emitted Cg ABI."""
    return {
        'schema': SCHEMA,
        'policy': POLICY,
        'tsc_layout': {
            'word0': {
                'wrap_u': [0, 3],
                'wrap_v': [3, 3],
                'wrap_p': [6, 3],
                'compare_enabled': [9, 1],
                'compare_func': [10, 3],
                'srgb_conversion': [13, 1],
                'max_anisotropy': [20, 3],
            },
            'word1': {
                'mag_filter': [0, 2],
                'min_filter': [4, 2],
                'mipmap_filter': [6, 2],
                'cubemap_anisotropy': [8, 1],
                'cubemap_interface_filtering': [9, 1],
                'reduction_filter': [10, 2],
                'mip_lod_bias': [12, 13],
                'float_coord_normalization': [25, 1],
                'trilinear_optimization': [26, 5],
            },
            'word2': {
                'min_lod_clamp': [0, 12],
                'max_lod_clamp': [12, 12],
            },
            'word3': {
                'srgb_border_color_g': [12, 8],
                'srgb_border_color_b': [20, 8],
            },
            'word4': 'border_color_r_float32',
            'word5': 'border_color_g_float32',
            'word6': 'border_color_b_float32',
            'word7': 'border_color_a_float32',
        },
        'accepted': {
            'glsl_call': 'textureLod(sampler2DShadow, vec3(uv, ref), literal 0.0)',
            'return_type': 'float',
            'texture_type': 2,
            'depth_flag': 1,
            'compare_enabled': 1,
            'compare_func': [0, 7],
            'wrap_u': list(WRAP_SUPPORTED),
            'wrap_v': list(WRAP_SUPPORTED),
            'min_filter_equals_mag_filter': True,
            'filter': ['point', 'linear'],
            'mipmap_filter': ['none', 'nearest', 'linear'],
            'max_mip_level': 0,
            'reduction_filter': 0,
            'max_anisotropy': 0,
            'mip_lod_bias': 0,
            'min_lod_clamp': 0,
            'float_coord_normalization': 0,
            'trilinear_optimization': 0,
            'inert_for_2d_depth': {
                'srgb_conversion': 'depth-format TIC has no sRGB transfer; helper reads .r raw',
                'cubemap_anisotropy': 'cube-only bit; the accepted view is 2D',
                'cubemap_interface_filtering': 'cube-only bit; the accepted view is 2D',
                'evidence': 'B36 village shadow TSC w0=0x00027292 w1=0x000003a2 on TIC Z32 1024x3072; policy-1 refused every shadow receiver',
            },
            'border_depth': {
                'used_when': 'wrap_u == Border or wrap_v == Border',
                'source': 'TSC word4 as float32 (R component)',
                'range': [0.0, 1.0],
                'finite': True,
            },
        },
        'rejected': {
            'glsl_calls': ['texture', 'textureOffset', 'textureLodOffset',
                           'non-literal LOD', 'non-zero LOD'],
            'shadow_sampler_types': ['samplerCubeShadow', 'sampler2DArrayShadow',
                                     'sampler3DShadow'],
            'texture_type_not_2': True,
            'depth_flag_not_1': True,
            'compare_disabled': True,
            'wrap_u_v': [4, 5, 6, 7],
            'filter_mismatch_min_mag': True,
            'invalid_filter_enum': [0, 3],
            'invalid_mipmap_enum': [0],
            'max_mip_level_nonzero_or_unknown': True,
            'non_weighted_reduction': True,
            'anisotropy_nonzero': True,
            'lod_bias_nonzero': True,
            'min_lod_clamp_nonzero': True,
            'float_coord_normalization_set': True,
            'trilinear_optimization_set': True,
            'border_depth_nonfinite_or_out_of_range_when_used': True,
        },
        'abi': {
            'sampler': {
                'source_glsl_type': 'sampler2DShadow',
                'cg_type': 'sampler2D',
                'depth_component': 'r',
                'native_state': {
                    'min_filter': 'point',
                    'mag_filter': 'point',
                    'mip_filter': 'none',
                    'compare_mode': 'none',
                    'wrap_u': 'clamp-to-edge',
                    'wrap_v': 'clamp-to-edge',
                },
                'why_native_clamp': 'the helper wraps integer texel coordinates; '
                                    'a second native wrap would alias the tap',
            },
            'uniforms': [
                {'suffix': '_shadow_extent', 'cg_type': 'float4',
                 'layout': ['width', 'height', 'inv_width', 'inv_height']},
                {'suffix': '_shadow_state', 'cg_type': 'float4',
                 'layout': ['compare_func', 'filter_code', 'wrap_u', 'wrap_v']},
                {'suffix': '_shadow_border', 'cg_type': 'float4',
                 'layout': ['border_depth', '0', '0', '0']},
            ],
            'filter_code': {'point': 0.0, 'linear': 1.0},
            'vector_count_per_sampler': 3,
            'extent_source': 'TIC width and height of the bound depth view',
        },
        'semantics': {
            'compare_direction': 'ref OP depth',
            'point': 'index = floor(uv * size); read texel centre (index + 0.5) / size',
            'linear': 't = uv * size - 0.5; i = floor(t); f = t - i; compare four taps '
                      'individually, then bilinear mix (never filter depth first)',
            'repeat': 'positive modulo period (mod n)',
            'mirror': 'period 2n; fold indices >= n with 2n - 1 - index',
            'clamp_to_edge': 'clamp index to [0, n-1]',
            'border': 'any out-of-range tap compares ref against border_depth',
        },
        'evidence': {
            'tsc_layout': 'yuzu src/video_core/textures/texture.h TSCEntry',
            'border_and_compare_state':
                'yuzu src/video_core/renderer_opengl/gl_texture_cache.cpp '
                'Sampler::Sampler lines 1271-1300; GL_TEXTURE_BORDER_COLOR at 1297',
            'border_color_source':
                'yuzu src/video_core/textures/texture.cpp TSCEntry::BorderColor '
                'returns the four float32 words; R is the depth comparison border',
            'depth_component':
                'GL depth texture sampling exposes depth in R; the B18 depth-view '
                'usage audit only proves .r, so G/B/A are never selected',
            'limits':
                'host tests do not claim target NaN/subnormal preservation or '
                'hardware equivalence of float operations',
        },
    }


def policy_sha256():
    return digest_bytes(canonical(policy_document()))


# --- host-pure TSC planner ------------------------------------------------

def plan_tsc_shadow(tsc, texture_type, depth, max_mip_level):
    """Decode one raw 32-byte TSC descriptor into a shadow sampler plan.

    Returns a dict with ``supported`` plus named ``rejections``.  Every field
    that can change the sample is either honoured or explicitly rejected; the
    planner never falls back to a guessed filter/wrap/compare state.
    """
    if len(tsc) != 8:
        raise ValueError('a TSC descriptor is exactly eight 32-bit words')
    words = [int(word) & 0xFFFFFFFF for word in tsc]
    w0, w1, w2, w3, w4, w5, w6, w7 = words

    decoded = {
        'wrap_u': w0 & 7,
        'wrap_v': (w0 >> 3) & 7,
        'wrap_p': (w0 >> 6) & 7,
        'compare_enabled': (w0 >> 9) & 1,
        'compare_func': (w0 >> 10) & 7,
        'srgb_conversion': (w0 >> 13) & 1,
        'max_anisotropy': (w0 >> 20) & 7,
        'mag_filter': w1 & 3,
        'min_filter': (w1 >> 4) & 3,
        'mipmap_filter': (w1 >> 6) & 3,
        'cubemap_anisotropy': (w1 >> 8) & 1,
        'cubemap_interface_filtering': (w1 >> 9) & 1,
        'reduction_filter': (w1 >> 10) & 3,
        'mip_lod_bias': sign_extend((w1 >> 12) & 0x1FFF, 13),
        'float_coord_normalization': (w1 >> 25) & 1,
        'trilinear_optimization': (w1 >> 26) & 0x1F,
        'min_lod_clamp': w2 & 0xFFF,
        'max_lod_clamp': (w2 >> 12) & 0xFFF,
        'srgb_border_color_r': (w2 >> 24) & 0xFF,
        'srgb_border_color_g': (w3 >> 12) & 0xFF,
        'srgb_border_color_b': (w3 >> 20) & 0xFF,
        'border_rgba': [f32_from_bits(w4), f32_from_bits(w5),
                        f32_from_bits(w6), f32_from_bits(w7)],
        'texture_type': texture_type,
        'depth_flag': depth,
        'max_mip_level': max_mip_level,
    }
    rejections = []

    def reject(code, detail):
        rejections.append({'code': code, 'detail': detail})

    if texture_type != 2:
        reject('texture-type-not-2d', 'texture type %r is not Color2D (2)' % (texture_type,))
    if depth != 1:
        reject('texture-not-depth', 'depth flag %r is not 1' % (depth,))
    if decoded['compare_enabled'] != 1:
        reject('compare-disabled', 'TSC word0 bit 9 is 0; sampler2DShadow compare is off')
    if decoded['wrap_u'] not in WRAP_SUPPORTED:
        reject('wrap-mode-unsupported',
               'wrap_u %d (%s) is not in the measured 0..3 set' %
               (decoded['wrap_u'], WRAP_NAMES[decoded['wrap_u']]))
    if decoded['wrap_v'] not in WRAP_SUPPORTED:
        reject('wrap-mode-unsupported',
               'wrap_v %d (%s) is not in the measured 0..3 set' %
               (decoded['wrap_v'], WRAP_NAMES[decoded['wrap_v']]))
    if decoded['mag_filter'] not in TSC_FILTER:
        reject('filter-enum-unsupported',
               'mag_filter %d is not Nearest(1) or Linear(2)' % decoded['mag_filter'])
    if decoded['min_filter'] not in TSC_FILTER:
        reject('filter-enum-unsupported',
               'min_filter %d is not Nearest(1) or Linear(2)' % decoded['min_filter'])
    if (decoded['mag_filter'] in TSC_FILTER and decoded['min_filter'] in TSC_FILTER
            and decoded['mag_filter'] != decoded['min_filter']):
        reject('filter-mismatch',
               'min_filter %d and mag_filter %d differ; LOD0 min/mag selection is unproven'
               % (decoded['min_filter'], decoded['mag_filter']))
    if decoded['mipmap_filter'] not in TSC_MIP_FILTER:
        reject('mipmap-filter-unsupported',
               'mipmap_filter %d is not None(1), Nearest(2) or Linear(3)'
               % decoded['mipmap_filter'])
    if max_mip_level is None:
        reject('mip-level-unknown', 'TIC max mip level was not supplied')
    elif max_mip_level != 0:
        reject('mip-level-present',
               'TIC max mip level is %r; the contract only proves single-level LOD0'
               % (max_mip_level,))
    if decoded['reduction_filter'] != 0:
        reject('reduction-unsupported',
               'reduction_filter %d is not WeightedAverage(0)'
               % decoded['reduction_filter'])
    if decoded['max_anisotropy'] != 0:
        reject('anisotropy-unsupported',
               'max_anisotropy %d is not 1x(0)' % decoded['max_anisotropy'])
    # policy-2: cubemap-only filtering bits and sRGB conversion are inert for
    # the accepted 2D depth sampler.  Captured B36 village shadow TSCs carry
    # both cubemap bits and bit13 while sampling a Z32 surface; policy-1
    # refused every shadow receiver.  The values stay decoded for audit.
    if decoded['mip_lod_bias'] != 0:
        reject('lod-bias-unsupported',
               'mip_lod_bias %d/256 is not zero' % decoded['mip_lod_bias'])
    if decoded['float_coord_normalization'] != 0:
        reject('float-coord-normalization-unsupported',
               'float_coord_normalization bit is set')
    if decoded['trilinear_optimization'] != 0:
        reject('trilinear-optimization-unsupported',
               'trilinear optimization field is %d, not zero'
               % decoded['trilinear_optimization'])
    if decoded['min_lod_clamp'] != 0:
        reject('min-lod-clamp-unsupported',
               'min_lod_clamp %d/256 is not zero' % decoded['min_lod_clamp'])
    border_used = decoded['wrap_u'] == 3 or decoded['wrap_v'] == 3
    border_depth = decoded['border_rgba'][0]
    if border_used:
        if not math.isfinite(border_depth):
            reject('border-depth-nonfinite',
                   'border wrap needs a finite R border value, got %r' % (border_depth,))
        elif not 0.0 <= border_depth <= 1.0:
            reject('border-depth-out-of-range',
                   'border R %r is outside [0,1]; clamping is not assumed'
                   % (border_depth,))

    if rejections:
        return {
            'schema': SCHEMA,
            'policy': POLICY,
            'policy_sha256': policy_sha256(),
            'supported': False,
            'rejections': rejections,
            'decoded': decoded,
        }
    return {
        'schema': SCHEMA,
        'policy': POLICY,
        'policy_sha256': policy_sha256(),
        'supported': True,
        'compare_func': decoded['compare_func'],
        'compare_name': COMPARE_NAMES[decoded['compare_func']],
        'filter': TSC_FILTER[decoded['min_filter']],
        'filter_code': 0.0 if TSC_FILTER[decoded['min_filter']] == 'point' else 1.0,
        'wrap_u': decoded['wrap_u'],
        'wrap_v': decoded['wrap_v'],
        'wrap_p': decoded['wrap_p'],
        'wrap_p_ignored_2d': decoded['wrap_p'] != 0,
        'border_used': border_used,
        'border_depth': border_depth if border_used else None,
        'border_rgba': decoded['border_rgba'],
        'lod': {
            'mode': 'explicit-zero',
            'mip_level': 0,
            'max_mip_level': max_mip_level,
            'mipmap_filter': TSC_MIP_FILTER[decoded['mipmap_filter']],
            'min_lod_clamp': decoded['min_lod_clamp'],
            'max_lod_clamp': decoded['max_lod_clamp'],
            'mip_lod_bias': decoded['mip_lod_bias'],
        },
        'abi': abi_for_sampler('<sampler>'),
        'decoded': decoded,
    }


def abi_for_sampler(name):
    """The per-sampler Cg uniform contract, shared with the runtime planner."""
    return {
        'sampler': {
            'name': name,
            'source_glsl_type': 'sampler2DShadow',
            'cg_type': 'sampler2D',
            'depth_component': 'r',
            'native_state': {
                'min_filter': 'point',
                'mag_filter': 'point',
                'mip_filter': 'none',
                'compare_mode': 'none',
                'wrap_u': 'clamp-to-edge',
                'wrap_v': 'clamp-to-edge',
            },
        },
        'uniforms': [
            {'name': name + '_shadow_extent', 'cg_type': 'float4',
             'layout': ['width', 'height', 'inv_width', 'inv_height']},
            {'name': name + '_shadow_state', 'cg_type': 'float4',
             'layout': ['compare_func', 'filter_code', 'wrap_u', 'wrap_v']},
            {'name': name + '_shadow_border', 'cg_type': 'float4',
             'layout': ['border_depth', '0', '0', '0']},
        ],
        'filter_code': {'point': 0.0, 'linear': 1.0},
        'vector_count': 3,
        'extent_source': 'TIC width and height of the bound depth view',
    }


# --- statement-inline Cg emitter -------------------------------------------
# The inline form deliberately uses only constructs already measured by
# probe_cg_capability (ternary, min, max, floor, tex2Dlod) and emits no shadow
# helper functions.  Every local has a module-wide unique name so a declaration
# inside an if/for body cannot alias a value from another call.

STRATEGY = 'vp-shadow-inline-direct-dest-1'
TEMP_PREFIX = 'vp_shadow'


def is_shadow_call(node):
    """True only for the already-resolved standalone shadow textureLod node."""
    return (isinstance(node, dict) and node.get('op') == 'call'
            and node.get('function') == 'textureLod'
            and node.get('shadow') is True)


def contains_shadow_call(node):
    if isinstance(node, dict):
        if is_shadow_call(node):
            return True
        return any(contains_shadow_call(value) for value in node.values())
    if isinstance(node, list):
        return any(contains_shadow_call(value) for value in node)
    return False


def compare_expr(ref, depth, func):
    """ref OP depth for all eight compare functions (no helper call)."""
    expr = '1.0'  # Always
    for threshold, operator in ((6.5, '>='), (5.5, '!='), (4.5, '>'),
                                (3.5, '<='), (2.5, '=='), (1.5, '<')):
        expr = (f'(({func} < {threshold}) ? '
                f'(({ref} {operator} {depth}) ? 1.0 : 0.0) : {expr})')
    return f'(({func} < 0.5) ? 0.0 : {expr})'


class InlineEmitter:
    """Expand a shadow sample to uniquely named Cg statements.

    The call shape is ``textureLod(sampler2DShadow, vec3(uv, ref), 0.0)``.
    ``emit_sample`` writes a ``float3`` coordinate temporary first, so a target
    that also appears inside the coordinate expression keeps its old value.
    The returned result name is assigned by the caller only after the complete
    inline block.  All temporaries stay in the lexical if/for scope in which
    the statement appeared.
    """

    def __init__(self, prefix=TEMP_PREFIX):
        self.prefix = prefix
        self.serial = 0

    def _tmp(self, stem):
        self.serial += 1
        return f'{self.prefix}_{self.serial:03d}_{stem}'

    @staticmethod
    def _uniforms(sampler):
        return (f'{sampler}_shadow_extent', f'{sampler}_shadow_state',
                f'{sampler}_shadow_border')

    def _wrap(self, index_expr, size, mode, indent):
        raw = self._tmp('wrap_value')
        repeat = self._tmp('wrap_repeat')
        period = self._tmp('wrap_period')
        mirror = self._tmp('wrap_mirror')
        folded = self._tmp('wrap_folded')
        clamp = self._tmp('wrap_clamp')
        border = self._tmp('wrap_border')
        result = self._tmp('wrap_result')
        lines = [
            f'{indent}float {raw} = {index_expr};',
            f'{indent}float {repeat} = {raw} - floor({raw} / {size}) * {size};',
            f'{indent}float {period} = {size} + {size};',
            f'{indent}float {mirror} = {raw} - floor({raw} / {period}) * {period};',
            f'{indent}float {folded} = ({mirror} >= {size}) ? '
            f'({period} - 1.0 - {mirror}) : {mirror};',
            f'{indent}float {clamp} = min(max({raw}, 0.0), {size} - 1.0);',
            # Border samples only the out-of-range taps against the border
            # depth.  In-range taps keep their real integer index independently
            # for U and V; returning -1 for mode 3 unconditionally would turn
            # every Border-mode tap into the border value.
            f'{indent}float {border} = '
            f'({raw} >= 0.0 && {raw} <= {size} - 1.0) ? {raw} : -1.0;',
            f'{indent}float {result} = ({mode} < 0.5) ? {repeat} : '
            f'(({mode} < 1.5) ? {folded} : '
            f'(({mode} < 2.5) ? {clamp} : '
            f'(({mode} < 3.5) ? {border} : -1.0)));',
        ]
        return lines, result

    def _tap(self, sampler, coord, index_x, index_y, indent):
        extent, state, border = self._uniforms(sampler)
        x_lines, x = self._wrap(index_x, extent + '.x', state + '.z', indent)
        y_lines, y = self._wrap(index_y, extent + '.y', state + '.w', indent)
        depth = self._tmp('tap_depth')
        uv = self._tmp('tap_uv')
        value = self._tmp('tap_compare')
        lines = x_lines + y_lines
        lines.extend([
            f'{indent}float {depth} = {border}.x;',
            f'{indent}if ({x} >= 0.0 && {y} >= 0.0) {{',
            f'{indent}  float2 {uv} = (float2({x}, {y}) + 0.5) * {extent}.zw;',
            f'{indent}  {depth} = '
            f'tex2Dlod({sampler}, float4({uv}, 0.0, 0.0)).x;',
            f'{indent}}}',
            f'{indent}float {value} = '
            f'{compare_expr(coord + ".z", depth, state + ".x")};',
        ])
        return lines, value

    def _point_body(self, sampler, coord, dest, indent):
        extent, _, _ = self._uniforms(sampler)
        index = self._tmp('point_index')
        lines = [f'{indent}float2 {index} = floor({coord}.xy * {extent}.xy);']
        tap_lines, value = self._tap(
            sampler, coord, index + '.x', index + '.y', indent)
        lines.extend(tap_lines)
        lines.append(f'{indent}{dest} = {value};')
        return lines

    def _linear_body(self, sampler, coord, dest, indent):
        extent, _, _ = self._uniforms(sampler)
        t = self._tmp('linear_t')
        index = self._tmp('linear_index')
        fraction = self._tmp('linear_fraction')
        lines = [
            f'{indent}float2 {t} = {coord}.xy * {extent}.xy - 0.5;',
            f'{indent}float2 {index} = floor({t});',
            f'{indent}float2 {fraction} = {t} - {index};',
        ]
        taps = []
        for suffix, dx, dy in (('c00', '0.0', '0.0'),
                               ('c10', '1.0', '0.0'),
                               ('c01', '0.0', '1.0'),
                               ('c11', '1.0', '1.0')):
            tap_lines, value = self._tap(
                sampler, coord,
                f'{index}.x + {dx}', f'{index}.y + {dy}', indent)
            lines.extend(tap_lines)
            taps.append(value)
        c00, c10, c01, c11 = taps
        top = self._tmp('linear_top')
        bottom = self._tmp('linear_bottom')
        lines.extend([
            f'{indent}float {top} = {c00} + ({c10} - {c00}) * {fraction}.x;',
            f'{indent}float {bottom} = {c01} + ({c11} - {c01}) * {fraction}.x;',
            f'{indent}{dest} = {top} + ({bottom} - {top}) * {fraction}.y;',
        ])
        return lines

    def emit_sample(self, sampler, coord_expr, indent='', mode='dispatch',
                    destination=None):
        """Return ``(lines, result_name)`` for point, linear or dispatch.

        ``destination`` is an optional lvalue (for example ``color.x``) that
        receives the final point/linear/mix value directly.  The result
        temporary serial is still allocated so the generated temp names are
        identical whether or not a destination is used; only its declaration
        and final assignment differ.  This is the clean form of the hand
        patched direct-output variant established in B21.
        """
        if mode not in ('point', 'linear', 'dispatch'):
            raise ValueError('unknown shadow sample mode ' + mode)
        _, state, _ = self._uniforms(sampler)
        coord = self._tmp('coord')
        result = self._tmp(mode + '_result')
        target = destination if destination is not None else result
        lines = [f'{indent}float3 {coord} = {coord_expr};']
        if destination is None:
            lines.append(f'{indent}float {result} = 0.0;')
        if mode == 'point':
            lines.extend(self._point_body(sampler, coord, target, indent))
        elif mode == 'linear':
            lines.extend(self._linear_body(sampler, coord, target, indent))
        else:
            lines.append(f'{indent}if ({state}.y < 0.5) {{')
            lines.extend(self._point_body(sampler, coord, target, indent + '  '))
            lines.append(f'{indent}}} else {{')
            lines.extend(self._linear_body(sampler, coord, target, indent + '  '))
            lines.append(f'{indent}}}')
        return lines, target


def emitter_source():
    """Canonical emitter sample used as the lowering identity hash.

    This text is not included in generated shaders; it is a deterministic
    derivation of the point, linear and dispatch statement expansion.
    """
    emitter = InlineEmitter()
    lines = [f'// VP shadow statement-inline emitter (strategy {STRATEGY}).']
    for mode in ('point', 'linear', 'dispatch'):
        block, result = emitter.emit_sample('tex', 'coord.xyz', '', mode)
        lines.append(f'// {mode} -> {result}')
        lines.extend(block)
    # Direct-destination form: the final value is written into the caller's
    # lvalue instead of a result temporary.  This is the established B21
    # boundary-safe output shape and must be part of the lowering identity.
    direct = InlineEmitter()
    lines.append('// direct-destination samples')
    for mode, component in (('point', 'out.x'), ('linear', 'out.y'),
                            ('dispatch', 'out.z')):
        block, result = direct.emit_sample(
            'tex', 'coord.xyz', '', mode, destination=component)
        lines.append(f'// {mode} -> {result}')
        lines.extend(block)
    return '\n'.join(lines) + '\n'


def helper_source():
    """Compatibility name for the emitter identity sample.

    Generated Cg does not include this text and defines no shadow helper.
    """
    return emitter_source()


def emitter_source_sha256():
    return digest_bytes(emitter_source().encode())


def helper_source_sha256():
    return emitter_source_sha256()


def mechanism_source(name='tex_shadow'):
    """Self-authored Cg exercising point, linear and dispatch inline paths."""
    extent = name + '_shadow_extent'
    state = name + '_shadow_state'
    border = name + '_shadow_border'
    uniforms = '\n'.join([
        f'uniform sampler2D {name};',
        f'uniform float4 {extent};',
        f'uniform float4 {state};',
        f'uniform float4 {border};',
    ])
    emitter = InlineEmitter()
    body = ['  color = float4(0.0, 0.0, 0.0, 1.0);']
    for mode, component in (('point', 'color.x'), ('linear', 'color.y'),
                            ('dispatch', 'color.z')):
        block, _ = emitter.emit_sample(name, 'coord.xyz', '  ', mode,
                                       destination=component)
        body.extend(block)
    body.append('  color.w = 1.0;')
    return (
        f'// Generated by shadow_helpers.InlineEmitter '
        f'(strategy {STRATEGY}, policy {POLICY}).\n'
        + uniforms + '\n'
        + 'void main(out float4 color : COLOR0, float4 coord : TEXCOORD0) {\n'
        + '\n'.join(body) + '\n'
        + '}\n')



def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write-policy', type=str,
                        help='write the policy document JSON to this path')
    parser.add_argument('--write-mechanism-cg', type=str,
                        help='write the mechanism Cg to this path')
    args = parser.parse_args()
    if args.write_policy:
        from pathlib import Path
        Path(args.write_policy).write_text(
            json.dumps(policy_document(), indent=2) + '\n',
            encoding='utf-8', newline='\n')
    if args.write_mechanism_cg:
        from pathlib import Path
        Path(args.write_mechanism_cg).write_text(
            mechanism_source(), encoding='utf-8', newline='\n')
    print(json.dumps({
        'strategy': STRATEGY,
        'policy': POLICY,
        'policy_sha256': policy_sha256(),
        'emitter_sha256': emitter_source_sha256(),
        'helper_sha256': helper_source_sha256(),
    }, sort_keys=True))


if __name__ == '__main__':
    main()

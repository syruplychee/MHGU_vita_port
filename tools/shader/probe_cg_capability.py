"""Probe which Cg constructs the locked Shacc compiler accepts on PSP2 profiles.

Every case is a self-authored snippet. A rejected case is recorded with the
compiler message; a case that compiles is only recorded as "accepted by the
compiler", never as "numerically correct". The Shacc workspace is a serial
compiler service: do not run two instances against the same root.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path

def filesystem_path(value):
    """Resolve native Windows paths when this probe is driven from WSL."""
    if os.name=='nt':return Path(value)
    match=re.match(r'^([a-zA-Z]):[\\/](.*)$',value)
    if match:return Path('/mnt/'+match.group(1).lower()+'/'+match.group(2).replace('\\','/'))
    return Path(value)

F_HEAD = 'void main(out float4 frag : COLOR0, uniform float4 c17[1]%s) {\n'
F_TAIL = '}\n'
V_HEAD = 'void main(float4 position : POSITION, out float4 out_position : POSITION, out float4 out_attr0 : TEXCOORD0, uniform float4 c17[1]%s) {\n'
V_TAIL = '  out_position = position;\n}\n'


def f(name, body, extra='', note=''):
    return (name, 'sce_fp_psp2', F_HEAD % extra + body + F_TAIL, note)


def v(name, body, extra='', note=''):
    return (name, 'sce_vp_psp2', V_HEAD % extra + body + V_TAIL, note)


CASES = [
    f('control_passthrough', '  frag = float4(0.25, 0.5, 0.75, 1.0);\n', note='compiler control: must pass'),
    f('if_else', '  if (c17[0].x > 0.5) { frag = float4(1.0,0.0,0.0,1.0); } else { frag = float4(0.0,0.0,1.0,1.0); }\n', note='GLSL if/else'),
    f('ternary', '  frag = float4((c17[0].x > 0.5) ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n', note='GLSL ?:'),
    f('bool_var_logic', '  bool a = c17[0].x > 0.5;\n  bool b = c17[0].y > 0.5;\n  frag = float4((a && !b) ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n', note='bool locals, &&, !'),
    f('bool_or', '  bool a = c17[0].x > 0.5;\n  bool b = c17[0].y > 0.5;\n  frag = float4((a || b) ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n'),
    f('bool_from_int', '  int i = 3;\n  bool b = bool(i);\n  frag = float4(b ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n',
      note='GLSL bool(int) used by the capture; must test nonzero'),
    f('wpos_fragcoord', '  frag = float4((fp.x > 31.5) ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n',
      extra=', float4 fp : WPOS', note='window position input, needed by the differential probe'),
    f('int_declare_from_literal', '  int slot = 3;\n  frag = float4((slot == 3) ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n',
      note='scalar int local from a literal'),
    f('int_cast_from_float', '  int slot = (int)fp.x;\n  frag = float4((slot == 0) ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n',
      extra=', float4 fp : WPOS', note='cast spelling of a float-to-int conversion'),
    f('float_from_int_call', '  int slot = 3;\n  frag = float4(float(slot) * 0.5, 0.0, 0.0, 1.0);\n',
      note='call spelling of an int-to-float conversion'),
    f('bool2_from_int2', '  int2 v = int2(3, 0);\n  bool2 b = bool2(v);\n'
      '  frag = float4(b.x ? 1.0 : 0.0, b.y ? 1.0 : 0.0, 0.0, 1.0);\n', note='vector form of bool(int)'),
    f('compare_lt_ge_eq_ne', '  float x = c17[0].x;\n  float y = c17[0].y;\n'
      '  frag = float4((x < y) ? 1.0 : 0.0, (x >= y) ? 1.0 : 0.0, (x == y) ? 1.0 : 0.0, (x != y) ? 1.0 : 0.0);\n'),
    f('isnan_intrinsic', '  float x = c17[0].x;\n  frag = float4(isnan(x) ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n', note='GLSL isnan used 98x by capture'),
    f('nan_self_compare', '  float x = c17[0].x;\n  bool n = (x != x);\n  frag = float4(n ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n', note='exact NaN test without intrinsic'),
    f('fma_intrinsic', '  frag = float4(fma(c17[0].x, c17[0].y, c17[0].z), 0.0, 0.0, 1.0);\n', note='GLSL fma used 76x'),
    f('mul_add_unfused', '  frag = float4(c17[0].x * c17[0].y + c17[0].z, 0.0, 0.0, 1.0);\n', note='unfused fallback shape'),
    f('abs_intrinsic', '  frag = float4(abs(c17[0].x), 0.0, 0.0, 1.0);\n'),
    f('max_intrinsic', '  frag = float4(max(c17[0].x, c17[0].y), 0.0, 0.0, 1.0);\n'),
    f('min_intrinsic', '  frag = float4(min(c17[0].x, c17[0].y), 0.0, 0.0, 1.0);\n'),
    f('floor_intrinsic', '  frag = float4(floor(c17[0].x), 0.0, 0.0, 1.0);\n'),
    f('log2_intrinsic', '  frag = float4(log2(c17[0].x), 0.0, 0.0, 1.0);\n'),
    f('exp2_intrinsic', '  frag = float4(exp2(c17[0].x), 0.0, 0.0, 1.0);\n'),
    f('sqrt_intrinsic', '  frag = float4(sqrt(c17[0].x), 0.0, 0.0, 1.0);\n'),
    f('rsqrt_intrinsic', '  frag = float4(rsqrt(c17[0].x), 0.0, 0.0, 1.0);\n',
      note='Cg spelling considered for GLSL inversesqrt'),
    f('face_semantic', '  frag = float4(front_face ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n',
      extra=', bool front_face : FACE', note='GLSL gl_FrontFacing input'),
    f('texCUBE_sampler', '  frag = texCUBE(tex0, float3(c17[0].x, c17[0].y, c17[0].z));\n',
      extra=', uniform samplerCUBE tex0', note='GLSL samplerCube/texture used by character shaders'),
    f('texCUBElod_sampler', '  frag = texCUBElod(tex0, float4(c17[0].x, c17[0].y, c17[0].z, 0.0));\n',
      extra=', uniform samplerCUBE tex0', note='GLSL samplerCube/textureLod candidate'),
    f('frac_intrinsic', '  frag = float4(frac(c17[0].x), 0.0, 0.0, 1.0);\n'),
    f('uint_type_arith', '  uint a = uint(c17[0].x);\n  uint b = a + 2u;\n  frag = float4(float(b), 0.0, 0.0, 1.0);\n', note='GLSL uint used 106x'),
    f('uint_bitwise_and_or_xor_not', '  uint a = uint(c17[0].x);\n  uint b = uint(c17[0].y);\n  frag = float4(float((a & b) | (a ^ (~b))), 0.0, 0.0, 1.0);\n'),
    f('uint_shift', '  uint a = uint(c17[0].x);\n  frag = float4(float((a >> 1u) << 2u), 0.0, 0.0, 1.0);\n'),
    f('uint_mod', '  uint a = uint(c17[0].x);\n  frag = float4(float(a %% 4u), 0.0, 0.0, 1.0);\n'),
    f('bitcast_asuint', '  uint a = asuint(c17[0].x);\n  frag = float4(float(a), 0.0, 0.0, 1.0);\n', note='GLSL floatBitsToUint used 95x'),
    f('bitcast_asfloat', '  float x = asfloat(uint(c17[0].x));\n  frag = float4(x, 0.0, 0.0, 1.0);\n', note='GLSL uintBitsToFloat used 157x'),
    f('bitcast_glsl_names', '  uint a = floatBitsToUint(c17[0].x);\n  float b = uintBitsToFloat(a);\n  frag = float4(b, 0.0, 0.0, 1.0);\n', note='GLSL spelling; expected rejection'),
    f('uint2_type', '  uint2 o = uint2(uint(c17[0].x), uint(c17[0].y));\n  frag = float4(float(o.x), float(o.y), 0, 1);\n', note='GLSL uvec2'),
    f('int2_type', '  int2 o = int2(int(c17[0].x), int(c17[0].y));\n  frag = float4(float(o.x), float(o.y), 0, 1);\n', note='GLSL ivec2'),
    f('float_to_uint_roundtrip', '  float x = c17[0].x;\n  uint a = asuint(x);\n  float y = asfloat(a);\n  frag = float4(y, 0.0, 0.0, 1.0);\n', note='the utof(ftou(f)) roundtrip used by the capture'),
    f('loop_for', '  float s = 0.0;\n  for (int i = 0; i < 4; i = i + 1) { s = s + c17[0].x; }\n  frag = float4(s, 0.0, 0.0, 1.0);\n', note='for loop'),
    f('loop_while', '  float s = 0.0;\n  int i = 0;\n  while (i < 4) { s = s + c17[0].x; i = i + 1; }\n  frag = float4(s, 0.0, 0.0, 1.0);\n', note='while loop'),
    f('loop_break', '  float s = 0.0;\n  int i = 0;\n  while (i < 4) { if (i == 2) { break; } s = s + c17[0].x; i = i + 1; }\n  frag = float4(s, 0.0, 0.0, 1.0);\n', note='break/continue'),
    f('early_return', '  if (c17[0].x > 0.5) { frag = float4(1.0,0.0,0.0,1.0); return; }\n  frag = float4(0.0,0.0,1.0,1.0);\n', note='mid-function return'),
    f('tex2D_baseline', '  frag = tex2D(tex0, float2(c17[0].x, c17[0].y));\n', extra=', uniform sampler2D tex0', note='compiler control'),
    f('tex2Dlod', '  frag = tex2Dlod(tex0, float4(c17[0].x, c17[0].y, 0.0, 0.0));\n', extra=', uniform sampler2D tex0', note='GLSL textureLod used 14x'),
    f('tex2Dproj', '  frag = tex2Dproj(tex0, float4(c17[0].x, c17[0].y, 1.0, 1.0));\n', extra=', uniform sampler2D tex0'),
    f('tex2D_offset_name', '  frag = tex2Doff(tex0, float2(c17[0].x, c17[0].y), int2(0, 1));\n', extra=', uniform sampler2D tex0', note='offset sampling spelling probe; expected rejection'),
    f('tex2Dlod_offset_name', '  frag = tex2Dlodoff(tex0, float4(c17[0].x, c17[0].y, 0.0, 0.0), int2(0, 1));\n', extra=', uniform sampler2D tex0', note='offset sampling spelling probe; expected rejection'),
    f('depth_output_semantic', '  out_depth = c17[0].x;\n  frag = float4(1, 0.0, 0.0, 1.0);\n', note='GLSL gl_FragDepth; needs a DEPTH out parameter, see body rewrite below'),
    v('v_control_passthrough', '  out_attr0 = float4(0.0,0.0,0.0,1.0);\n', note='compiler control'),
    v('v_fma', '  out_attr0 = float4(fma(c17[0].x, c17[0].y, c17[0].z), 0.0, 0.0, 1.0);\n'),
    v('v_uint_bitwise', '  uint a = uint(c17[0].x);\n  out_attr0 = float4(float(a | (~a)), 0.0, 0.0, 1.0);\n'),
    v('v_if_else', '  if (c17[0].x > 0.5) { out_attr0 = float4(1.0,0.0,0.0,1.0); } else { out_attr0 = float4(0.0,0.0,1.0,1.0); }\n'),
    # Round 2: the exact spellings the unsigned/integer and fused-multiply paths need.
    f('asint_only', '  int a = asint(c17[0].x);\n  frag = float4(float(a), 0.0, 0.0, 1.0);\n', note='bit reinterpret via signed int'),
    f('asfloat_only', '  float y = asfloat(int(c17[0].x));\n  frag = float4(y, 0.0, 0.0, 1.0);\n'),
    f('asint_asfloat_roundtrip', '  int a = asint(c17[0].x);\n  float y = asfloat(a);\n  frag = float4(y, 0.0, 0.0, 1.0);\n', note='the utof(ftou(f)) roundtrip'),
    f('int_bitwise', '  int a = int(c17[0].x);\n  int b = int(c17[0].y);\n'
      '  frag = float4(float(a & b), float(a | b), float(a ^ b), float(~a));\n'),
    f('int_shift', '  int a = int(c17[0].x);\n  int r = (a >> 1) << 2;\n  frag = float4(float(r), 0.0, 0.0, 1.0);\n'),
    f('int_arith_mod', '  int a = int(c17[0].x);\n  int b = a * 3 + 7;\n  int r = b % 4;\n  frag = float4(float(r), 0.0, 0.0, 1.0);\n'),
    f('int_compare', '  int a = int(c17[0].x);\n  int b = int(c17[0].y);\n  frag = float4((a < b) ? 1.0 : 0.0, (a == b) ? 1.0 : 0.0, 0.0, 1.0);\n'),
    f('int2_bitwise', '  int2 v = int2(int(c17[0].x), int(c17[0].y));\n'
      '  int2 w = (v & int2(1, 2)) | (v ^ int2(3, 4));\n  frag = float4(float(w.x), float(w.y), 0.0, 1.0);\n', note='uvec2 stand-in'),
    f('float_int_convert', '  int a = int(c17[0].x);\n  float f = float(a);\n  int i = int(f);\n  frag = float4(float(i), 0.0, 0.0, 1.0);\n'),
    f('mad_intrinsic', '  frag = float4(mad(c17[0].x, c17[0].y, c17[0].z), 0.0, 0.0, 1.0);\n', note='Cg spelling of fused multiply-add'),
    v('v_mad_intrinsic', '  out_attr0 = float4(mad(c17[0].x, c17[0].y, c17[0].z), 0.0, 0.0, 1.0);\n'),
    f('tex2D_grad', '  frag = tex2D(tex0, float2(c17[0].x, c17[0].y), float2(0.001, 0.001), float2(0.001, 0.001));\n',
      extra=', uniform sampler2D tex0', note='explicit-gradient sampling'),
    f('tex2Dlod_offset_4arg', '  frag = tex2Dlod(tex0, float4(c17[0].x, c17[0].y, 0.0, 0.0), int2(1, 0));\n',
      extra=', uniform sampler2D tex0', note='offset sampling spelling probe'),
    f('tex2Doff_int2', '  frag = tex2Doff(tex0, float2(c17[0].x, c17[0].y), int2(1, 0));\n',
      extra=', uniform sampler2D tex0', note='offset sampling spelling probe'),
    f('depth_output_omitted', '  frag = float4(1.0, 0.0, 0.0, 1.0);\n',
      note='byte-compare control for the DEPTH semantic'),
    # Round 3: the compiler hints "'uintBitsToFloat', did you mean 'intBitsToFloat'?".
    f('bitcast_int_names', '  int a = floatBitsToInt(c17[0].x);\n  float y = intBitsToFloat(a);\n'
      '  frag = float4(y, 0.0, 0.0, 1.0);\n', note='signed-int spelling of the bit reinterpretation'),
    f('bitcast_float_to_int_only', '  int a = floatBitsToInt(c17[0].x);\n  frag = float4(float(a), 0.0, 0.0, 1.0);\n'),
    f('bitcast_int_to_float_only', '  float y = intBitsToFloat(int(c17[0].x));\n  frag = float4(y, 0.0, 0.0, 1.0);\n'),
    f('bitcast_uint_names', '  uint a = floatBitsToUint(c17[0].x);\n  float y = uintBitsToFloat(a);\n'
      '  frag = float4(y, 0.0, 0.0, 1.0);\n', note='unsigned spelling; expected rejection'),
    f('int2_from_bits', '  int2 o = int2(floatBitsToInt(c17[0].x), floatBitsToInt(c17[0].y));\n'
      '  int2 t = (o & int2(-1, 0)) | (o >> 1);\n  frag = float4(float(t.x), float(t.y), 0.0, 1.0);\n',
      note='uvec2 stand-in built from bit patterns'),
    v('v_mul_add_unfused', '  out_attr0 = float4(c17[0].x * c17[0].y + c17[0].z, 0.0, 0.0, 1.0);\n',
      note='vertex form of a*b+c, the fma replacement candidate'),
    f('v_mul_add_fragment_shape', '  frag = float4(c17[0].x * c17[0].y + c17[0].z, 0.0, 0.0, 1.0);\n',
      note='fragment form of the same shape, for GXP byte comparison'),
    f('tex2Dlod_uv_offset', '  frag = tex2Dlod(tex0, float4(c17[0].x + 0.001, c17[0].y + 0.001, 0.0, 0.0));\n',
      extra=', uniform sampler2D tex0', note='manual UV offset instead of an offset sampler'),
    f('tex2D_uv_offset', '  frag = tex2D(tex0, float2(c17[0].x + 0.001, c17[0].y));\n',
      extra=', uniform sampler2D tex0'),
    f('int_shift_right_negative', '  int a = floatBitsToInt(c17[0].x);\n  int r = a >> 1;\n'
      '  frag = float4(float(r), 0.0, 0.0, 1.0);\n', note='signed shift of a bit pattern'),
    # Round 4: alternate spellings for the float -> bit-pattern direction.
    f('bitcast_alt_as_int', '  int a = as_int(c17[0].x);\n  frag = float4(float(a), 0.0, 0.0, 1.0);\n'),
    f('bitcast_alt_float_as_int', '  int a = float_as_int(c17[0].x);\n  frag = float4(float(a), 0.0, 0.0, 1.0);\n'),
    f('bitcast_alt_underscore', '  int a = __float_as_int(c17[0].x);\n  frag = float4(float(a), 0.0, 0.0, 1.0);\n'),
    f('bitcast_int2_to_float2', '  float2 v = intBitsToFloat(int2(int(c17[0].x), int(c17[0].y)));\n'
      '  frag = float4(v.x, v.y, 0.0, 1.0);\n', note='vector form of int -> float reinterpretation'),
    f('bitcast_int3_to_float3', '  float3 v = intBitsToFloat(int3(int(c17[0].x), int(c17[0].y), int(c17[0].z)));\n'
      '  frag = float4(v.x, v.y, v.z, 1.0);\n'),
    # Round 5 (A3): the sampler dimensions and qualifiers the locked library still contains.
    f('sampler1D_tex1D', '  frag = tex1D(tex0, c17[0].x);\n',
      extra=', uniform sampler1D tex0', note='GLSL sampler1D + texture()'),
    f('sampler1D_tex1Dlod_float2', '  frag = tex1Dlod(tex0, float2(c17[0].x, 0.0));\n',
      extra=', uniform sampler1D tex0', note='GLSL textureLod on sampler1D'),
    f('sampler1D_tex1Dlod_float4', '  frag = tex1Dlod(tex0, float4(c17[0].x, 0.0, 0.0, 0.0));\n',
      extra=', uniform sampler1D tex0', note='Cg float4 lod spelling probe'),
    f('sampler1D_tex1Dproj', '  frag = tex1Dproj(tex0, float2(c17[0].x, 1.0));\n',
      extra=', uniform sampler1D tex0', note='projected 1D sampling'),
    f('sampler3D_tex3D', '  frag = tex3D(tex0, float3(c17[0].x, c17[0].y, c17[0].z));\n',
      extra=', uniform sampler3D tex0', note='GLSL sampler3D + texture()'),
    f('sampler3D_tex3Dlod', '  frag = tex3Dlod(tex0, float4(c17[0].x, c17[0].y, c17[0].z, 0.0));\n',
      extra=', uniform sampler3D tex0', note='GLSL textureLod on sampler3D'),
    f('sampler2DShadow_type_tex2D', '  frag = tex2D(tex0, float2(c17[0].x, c17[0].y));\n',
      extra=', uniform sampler2DShadow tex0', note='shadow sampler type, ordinary sample spelling'),
    f('sampler2DShadow_tex2DShadow', '  frag = tex2DShadow(tex0, float3(c17[0].x, c17[0].y, c17[0].z));\n',
      extra=', uniform sampler2DShadow tex0', note='comparison sampling spelling probe'),
    f('sampler2DShadow_tex2Dproj', '  frag = tex2Dproj(tex0, float4(c17[0].x, c17[0].y, c17[0].z, 1.0));\n',
      extra=', uniform sampler2DShadow tex0', note='DX9-style projected shadow comparison probe'),
    f('sampler2DShadow_tex2Dlod', '  frag = tex2Dlod(tex0, float4(c17[0].x, c17[0].y, 0.0, 0.0));\n',
      extra=', uniform sampler2DShadow tex0', note='lod sampling on a shadow sampler'),
    f('noperspective_input', '  frag = in0;\n',
      extra=', noperspective float4 in0 : TEXCOORD0', note='GLSL noperspective fragment input'),
    f('linear_input', '  frag = in0;\n',
      extra=', linear float4 in0 : TEXCOORD0', note='Cg linear interpolation modifier'),
    f('nointerpolation_input', '  frag = in0;\n',
      extra=', nointerpolation float4 in0 : TEXCOORD0', note='Cg flat input modifier'),
    f('int_of_trunc', '  int a = int(trunc(c17[0].x));\n  frag = float4(float(a), 0.0, 0.0, 1.0);\n',
      note='exact GLSL int(float) spelling candidate: truncate then convert'),
    f('uint_of_trunc_sign_split', '  float x = trunc(c17[0].x);\n'
      '  int low = int(x < 2147483648.0 ? x : x - 2147483648.0);\n'
      '  int u = (x < 2147483648.0) ? low : (low | (-2147483648));\n'
      '  frag = float4(float(u), 0.0, 0.0, 1.0);\n', note='uint(float) above 2^31 via a signed carrier'),
    v('v_gl_VertexID', '  uint vid = uint(gl_VertexID);\n  out_attr0 = float4(float(vid), 0.0, 0.0, 1.0);\n',
      note='GLSL gl_VertexID builtin probe'),
    v('v_vertexid_semantic', '  out_attr0 = float4(float(vertex_id), 0.0, 0.0, 1.0);\n',
      extra=', int vertex_id : VERTEXID', note='explicit vertex id semantic probe'),
    f('bitcast_inline_asm_mov', '  float x = c17[0].x;\n  int a;\n  asm { mov a, x; };\n'
      '  frag = float4(float(a), 0.0, 0.0, 1.0);\n', note='Cg inline asm bit move probe'),
    f('bitcast_float_to_uint_builtin', '  uint a = floatToUintBits(c17[0].x);\n'
      '  frag = float4(float(a), 0.0, 0.0, 1.0);\n', note='alternative float->bits spelling'),
    f('bitcast_int_as_float_underscore', '  int a = __float_as_int(c17[0].x);\n  float y = __int_as_float(a);\n'
      '  frag = float4(y, 0.0, 0.0, 1.0);\n', note='CUDA-style paired intrinsics probe'),
    # Round 6 (A3): the vertex varying budget, which the translated library exposes
    # once the sampler and carrier errors no longer stop parsing first.
    ('v_eight_varyings', 'sce_vp_psp2',
     'void main(out float4 p : POSITION, '
     + ', '.join(f'out float4 o{i} : TEXCOORD{i}' for i in range(8))
     + ') { p=float4(0,0,0,1); '
     + ' '.join(f'o{i}=float4(0,0,0,1);' for i in range(8)) + ' }\n',
     'control below the measured limit'),
    ('v_ten_varyings', 'sce_vp_psp2',
     'void main(out float4 p : POSITION, '
     + ', '.join(f'out float4 o{i} : TEXCOORD{i}' for i in range(10))
     + ') { p=float4(0,0,0,1); '
     + ' '.join(f'o{i}=float4(0,0,0,1);' for i in range(10)) + ' }\n',
     'ten TEXCOORD outputs, the locked library writes eleven'),
    ('v_eleven_varyings_texcoord', 'sce_vp_psp2',
     'void main(out float4 p : POSITION, '
     + ', '.join(f'out float4 o{i} : TEXCOORD{i}' for i in range(11))
     + ') { p=float4(0,0,0,1); '
     + ' '.join(f'o{i}=float4(0,0,0,1);' for i in range(11)) + ' }\n',
     'expected rejection: TEXCOORD10 out of range'),
    ('v_eleven_varyings_color0', 'sce_vp_psp2',
     'void main(out float4 p : POSITION, '
     + ', '.join(f'out float4 o{i} : TEXCOORD{i}' for i in range(10))
     + ', out float4 o10 : COLOR0'
     + ') { p=float4(0,0,0,1); '
     + ' '.join(f'o{i}=float4(0,0,0,1);' for i in range(10))
     + ' o10=float4(0,0,0,1); }\n',
     'eleventh varying spelled COLOR0'),
    ('v_twelve_varyings_color', 'sce_vp_psp2',
     'void main(out float4 p : POSITION, '
     + ', '.join(f'out float4 o{i} : TEXCOORD{i}' for i in range(10))
     + ', out float4 o10 : COLOR0, out float4 o11 : COLOR1'
     + ') { p=float4(0,0,0,1); '
     + ' '.join(f'o{i}=float4(0,0,0,1);' for i in range(10))
     + ' o10=float4(0,0,0,1); o11=float4(0,0,0,1); }\n',
     'twelve varying outputs via COLOR0/COLOR1'),
    ('f_eleven_inputs_color0', 'sce_fp_psp2',
     'void main(out float4 c : COLOR0, uniform float4 u[1], '
     + ', '.join(f'float4 i{i} : TEXCOORD{i}' for i in range(10))
     + ', float4 i10 : COLOR0'
     + ') { c=float4(i0.x+i10.x+u[0].x,0,0,1); }\n',
     'fragment side of the COLOR0 spelling'),
    # Round 7 (A3): separate "unknown sampler type" from the uniform-block parse path.
    ('sampler2DShadow_bare_param', 'sce_fp_psp2',
     'void main(out float4 c : COLOR0, sampler2DShadow tex0) { c = tex2D(tex0, float2(0.5, 0.5)); }\n',
     'shadow sampler type as a bare parameter'),
    ('sampler2DShadow_global', 'sce_fp_psp2',
     'sampler2DShadow tex0;\nvoid main(out float4 c : COLOR0) { c = tex2D(tex0, float2(0.5, 0.5)); }\n',
     'shadow sampler type in a global declaration'),
]

# The depth case needs the out parameter inserted, so it is rewritten here.
CASES = [(n, pr, src.replace('out float4 frag : COLOR0',
                             'out float4 frag : COLOR0, out float out_depth : DEPTH')
          if n == 'depth_output_semantic' else src, note) for (n, pr, src, note) in CASES]


def digest(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def diagnostic(log):
    """Return the compiler's own lines; the shacc log is mostly emulator startup noise."""
    lines = [line.strip() for line in log.splitlines() if line.strip()]
    diag = [line for line in lines if re.search(r'error \d+:|warning \d+:|Shacc compilation', line)]
    if diag:
        return ' | '.join(diag[:6])[:600]
    return ' | '.join(lines[-3:])[:300] or '(no output)'


def compile_one(shacc, config, root, name, profile, source, work):
    cg = work / f'{name}.cg'
    cg.write_text(source, encoding='utf-8', newline='\n')
    gxp = work / f'{name}.gxp'
    if gxp.exists():
        gxp.unlink()
    def native_path(path):
        value=str(path.resolve())
        if os.name=='nt':return value
        match=re.match(r'/mnt/([a-z])/(.*)',value)
        if not match:raise ValueError('not on a /mnt/<drive> mount: '+value)
        return match.group(1).upper()+':\\'+match.group(2).replace('/','\\')
    args = [str(shacc), '--config', native_path(config), '-profile', profile, '-O0',
            '-o', native_path(gxp), native_path(cg), '--timeout', '40']
    lock = Path(root) / '.vp-shacc.lock'
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        os.write(fd, str(os.getpid()).encode())
        os.close(fd)
        r = subprocess.run(args, capture_output=True, timeout=50)
    finally:
        lock.unlink()
    log = (r.stdout + r.stderr).decode('utf-8', 'replace')
    (work / f'{name}.log').write_text(log, encoding='utf-8', newline='\n')
    if r.returncode or not gxp.exists():
        return {'status': 'rejected', 'returncode': r.returncode, 'message': diagnostic(log)}
    binary = gxp.read_bytes()
    if len(binary) < 32 or binary[:4] != b'GXP\x00':
        return {'status': 'rejected', 'returncode': r.returncode, 'message': 'invalid GXP output header'}
    return {'status': 'accepted_by_compiler', 'gxp_bytes': len(binary),
            'gxp_sha256': digest(gxp), 'message': ''}


def collect(report_path, validation_path):
    """Normalise an existing run into the checked-in summary; no compiler is invoked."""
    report = json.loads(Path(report_path).read_text(encoding='utf-8'))
    cases = []
    for row in report['cases']:
        entry = {'case': row['case'], 'profile': row['profile'], 'status': row['status']}
        if row['note']:
            entry['note'] = row['note']
        if row['status'] == 'rejected':
            entry['diagnostic'] = re.sub(r'^"[^"]*cases[\\/]', '"', row['message'])
        else:
            entry['gxp_bytes'] = row['gxp_bytes']
        cases.append(entry)
    summary = {'schema': 1, 'task': report['task'], 'toolchain': report['toolchain'],
               'artifact_report_sha256': digest(report_path),
               'artifact_report_cases': report['case_count'],
               'scope_note': report['scope_note'], 'cases': cases}
    Path(validation_path).write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps({'collected': len(cases), 'validation': str(validation_path)}))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--shacc', type=Path, required=True)
    p.add_argument('--shacc-config', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--only', default=None, help='comma separated case names; default is every case')
    p.add_argument('--collect', type=Path, default=None,
                   help='normalise an existing report into this checked-in summary path and exit')
    a = p.parse_args()
    if a.collect is not None:
        collect(a.output / 'report.json', a.collect)
        return
    cases = [c for c in CASES if a.only is None or c[0] in a.only.split(',')]
    if a.only is not None:
        unknown = set(a.only.split(',')) - {c[0] for c in CASES}
        if unknown:
            raise SystemExit('unknown case name: ' + ','.join(sorted(unknown)))
    lock = json.loads((Path(__file__).resolve().parents[2] / 'config/graphics/shader-toolchain.json').read_text())
    config = {}
    for line in a.shacc_config.read_text(encoding='utf-8').splitlines():
        key, value = line.split('=', 1)
        if key in config:
            raise ValueError('duplicate Shacc configuration key')
        config[key] = value
    if set(config) != {'schema', 'root', 'service-vpk'} or config['schema'] != '1':
        raise ValueError('unsupported Shacc configuration')
    config_root=filesystem_path(config['root'])
    service_vpk=filesystem_path(config['service-vpk'])
    compiler = config_root / 'fs/ur0/data/external/libshacccg.suprx'
    if (digest(a.shacc) != lock['shacc']['executable_sha256']
            or digest(service_vpk) != lock['shacc']['service_vpk_sha256']
            or digest(compiler) != lock['libshacccg']['sha256']):
        raise ValueError('compiler toolchain identity mismatch')

    work = a.output / 'cases'
    work.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, profile, source, note in cases:
        row = {'case': name, 'profile': profile, 'note': note,
               'cg_sha256': hashlib.sha256(source.encode()).hexdigest()}
        try:
            row.update(compile_one(a.shacc, a.shacc_config.resolve(), config_root, name, profile, source, work))
        except subprocess.TimeoutExpired:
            row.update(status='rejected', returncode=None, message='compiler timeout')
        rows.append(row)
        print(f'{name:32s} {row["status"]:22s} {row["message"][:90]}', flush=True)

    report = {'schema': 1, 'task': 'VP01.6.2-CG-CAPABILITY',
        'toolchain': {'shacc_sha256': digest(a.shacc),
                            'service_sha256': digest(service_vpk),
                            'libshacccg_sha256': digest(compiler),
                            'cg_flags': ['-O0']},
              'case_count': len(rows),
              'accepted': sum(1 for r in rows if r['status'] == 'accepted_by_compiler'),
              'rejected': sum(1 for r in rows if r['status'] == 'rejected'),
              'scope_note': 'Compiler acceptance only; no numeric correctness, GXM run or hardware claim.',
              'cases': rows}
    a.output.mkdir(parents=True, exist_ok=True)
    (a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps({'accepted': report['accepted'], 'rejected': report['rejected']}))


if __name__ == '__main__':
    main()

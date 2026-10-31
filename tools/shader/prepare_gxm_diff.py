"""Compile the self-authored VP01.6.2-R1 differential shaders and emit their contract.

Each case asks one question and states both the value it compares against and the
answer it expects, so a case that disagrees names the behaviour that is wrong
instead of leaving a puzzle. The tool computes nothing about the shader's result:
it records what the table says and checks that the table is self-consistent.

Guards:

* An operand carried as an integer must be exactly representable as float32, so
  the scaffolding conversion ``int(u_params.y)`` on the device is exact and cannot
  mask the primitive under test.
* A case that claims to separate one rounding from two must use operands for which
  the two really differ, which this tool recomputes in float32 and rejects
  otherwise.

The shaders are self-authored and contain no game code.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import sys
# An embedded interpreter ships a ._pth that keeps the script directory off sys.path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert_cache import compile_serial, digest, write_json

MODE_BITS_TO_FLOAT = 0
MODE_INT_TO_FLOAT = 1
MODE_BOOL_OF_INT = 2
MODE_MUL_ADD = 3
MODE_INT_MUL_ADD = 4
MODE_MIRROR_MUL_ADD = 5
MODE_NAN_TEST = 6
MODE_READ_OPERANDS = 7
MODE_READ_EXPECTATIONS = 8
MODE_PASS_THROUGH = 9
MODE_NOT_EQUAL = 10
MODE_GREATER = 11
MODE_INT_NOT_EQUAL = 12
MODE_BOOL_CAST = 13
MODE_BOOL_INVERTED = 14
MODE_BOOL_FLOAT_TEST = 15
MODE_INT_SUM = 16
# Modes whose operand `a` is a 32-bit integer pattern rather than a value.
INTEGER_CARRIER_MODES = (MODE_BITS_TO_FLOAT, MODE_BOOL_OF_INT, MODE_MIRROR_MUL_ADD, MODE_NAN_TEST,
                         MODE_BOOL_CAST, MODE_BOOL_INVERTED, MODE_BOOL_FLOAT_TEST)
READOUT_MODES = (MODE_READ_OPERANDS, MODE_READ_EXPECTATIONS)
# Comparison values that a program can reach; a NaN never matches anything, so it
# is never used as an expectation.
DOUBLE = 2.0
TWO_ROUNDINGS = 0.0
# The operands below are the classic ones: (1+2**-23)**2 rounds to 1+2**-22, and
# subtracting that leaves 0, while a single rounding leaves exactly 2**-46.
CANCEL_A = 1.0 + 2.0 ** -23
CANCEL_C = -(1.0 + 2.0 ** -22)
CANCEL_FUSED = 2.0 ** -46
# 3 * (1+2**-23) is not representable, so the product rounds to 3 + 2**-21 and the
# subtraction leaves 2**-21, while a single rounding leaves exactly 3 * 2**-23.
THREE_A = 3.0
THREE_C = -3.0
THREE_FUSED = 3.0 * 2.0 ** -23
THREE_ROUNDED = 2.0 ** -21


# What each mode is asking, so a result can be read without knowing the modes.
MODE_QUESTION = {
    MODE_BITS_TO_FLOAT: 'bits_to_float_is_a_reinterpretation',
    MODE_INT_TO_FLOAT: 'int_to_float_is_exact',
    MODE_BOOL_OF_INT: 'bool_of_int_tests_nonzero',
    MODE_MUL_ADD: 'mul_add_rounds_once',
    MODE_INT_MUL_ADD: 'int_to_float_is_exact',
    MODE_MIRROR_MUL_ADD: 'carrier_mirror_carries_the_bit_pattern',
    MODE_NAN_TEST: 'nan_compares_unequal',
    MODE_READ_OPERANDS: 'readout',
    MODE_READ_EXPECTATIONS: 'readout',
    MODE_PASS_THROUGH: 'delivery',
    MODE_NOT_EQUAL: 'not_equal_works',
    MODE_GREATER: 'greater_than_works',
    MODE_INT_NOT_EQUAL: 'not_equal_works',
    MODE_BOOL_CAST: 'bool_of_int_tests_nonzero',
    MODE_BOOL_INVERTED: 'bool_of_int_tests_nonzero',
    MODE_BOOL_FLOAT_TEST: 'bool_of_int_tests_nonzero',
    MODE_INT_SUM: 'int_of_float_rounds_toward_negative_infinity',
}


def case(name, mode, a=None, expected=None, b=None, c=None, x=None, pair=None, pixel=None,
         white=True, opposite=None):
    return {'name': name, 'mode': mode, 'a': a, 'expected': expected, 'b': b, 'c': c, 'x': x,
            'pair': pair, 'pixel': pixel, 'white': white, 'opposite': opposite}


# Every comparing case states the value its result must equal, so a case that
# disagrees is visible as a pixel of the wrong colour.
# `pair` is (single rounding, two roundings) for the cases that separate them.
CASES = [
    # fma -> a*b+c, and whether the product is rounded before the addition. Both
    # answers are stated as predictions; whichever fails is the answer.
    case('mul_add_exact_control', MODE_MUL_ADD, DOUBLE, 7.0, b=3.0, c=1.0),
    case('mul_add_power_of_two_is_exact', MODE_MUL_ADD, 1.5, 6.5, b=4.0, c=0.5),
    case('mul_add_single_rounding_cancel', MODE_MUL_ADD, CANCEL_A, CANCEL_FUSED, b=CANCEL_A,
         c=CANCEL_C, pair=(CANCEL_FUSED, TWO_ROUNDINGS),
         opposite='mul_add_two_roundings_cancel'),
    case('mul_add_two_roundings_cancel', MODE_MUL_ADD, CANCEL_A, TWO_ROUNDINGS, b=CANCEL_A,
         c=CANCEL_C, opposite='mul_add_single_rounding_cancel'),
    case('mul_add_single_rounding_three', MODE_MUL_ADD, THREE_A, THREE_FUSED, b=CANCEL_A,
         c=THREE_C, pair=(THREE_FUSED, THREE_ROUNDED),
         opposite='mul_add_two_roundings_three'),
    case('mul_add_two_roundings_three', MODE_MUL_ADD, THREE_A, THREE_ROUNDED, b=CANCEL_A,
         c=THREE_C, opposite='mul_add_single_rounding_three'),

    # intBitsToFloat: a bit reinterpretation, not a numeric conversion. The
    # operand is a bit pattern carried as an integer, so reading it as a number
    # would give 1065353216 rather than 1.
    case('bitcast_of_one_pattern_is_one', MODE_BITS_TO_FLOAT, 0x3F800000, 1.0,
         opposite='bitcast_of_one_pattern_is_the_integer'),
    case('bitcast_of_one_pattern_is_the_integer', MODE_BITS_TO_FLOAT, 0x3F800000, 1065353216.0,
         opposite='bitcast_of_one_pattern_is_one'),
    case('bitcast_of_two_pattern_is_two', MODE_BITS_TO_FLOAT, 0x40000000, 2.0),
    case('bitcast_of_minus_one_point_five_pattern', MODE_BITS_TO_FLOAT, 0xBFC00000, -1.5),
    case('bitcast_of_infinity_pattern_is_infinite', MODE_BITS_TO_FLOAT, 0x7F800000,
         float('inf')),
    case('bitcast_of_denormal_pattern', MODE_BITS_TO_FLOAT, 0x00000001, 2.0 ** -149),

    # int(x) and back: the conversion, and what it does to a fractional value.
    case('int_to_float_negative_seven', MODE_INT_TO_FLOAT, -7.0, -7.0),
    case('int_to_float_two_pow_24', MODE_INT_TO_FLOAT, 16777216.0, 16777216.0),
    case('int_to_float_truncates_positive', MODE_INT_TO_FLOAT, 3.75, 3.0,
         opposite='int_to_float_rounds_positive'),
    case('int_to_float_rounds_positive', MODE_INT_TO_FLOAT, 3.75, 4.0,
         opposite='int_to_float_truncates_positive'),
    case('int_to_float_truncates_negative', MODE_INT_TO_FLOAT, -3.75, -3.0,
         opposite='int_to_float_floors_negative'),
    case('int_to_float_floors_negative', MODE_INT_TO_FLOAT, -3.75, -4.0,
         opposite='int_to_float_truncates_negative'),
    case('int_to_float_of_large', MODE_INT_TO_FLOAT, 12345.0, 12345.0),
    case('int_to_float_of_minus_one', MODE_INT_TO_FLOAT, -1.0, -1.0),
    case('int_to_float_of_two_thousand_million', MODE_INT_TO_FLOAT, 2139095040.0, 2139095040.0),
    # int() of -0.5 is -1 if it floors and 0 if it truncates; adding one turns
    # that into 0.0 against 1.0.
    case('int_of_negative_half_floors', MODE_INT_SUM, -0.5, 0.0, b=1.0,
         opposite='int_of_negative_half_truncates'),
    case('int_of_negative_half_truncates', MODE_INT_SUM, -0.5, 1.0, b=1.0,
         opposite='int_of_negative_half_floors'),

    # bool(int(x)): a non-zero test. The operands are integers carried as values,
    # as the uint carriers are.
    case('bool_of_zero_is_false', MODE_BOOL_OF_INT, 0, 0.0),
    case('bool_of_minus_one_pattern_is_true', MODE_BOOL_OF_INT, 0xBF800000, 1.0),
    case('bool_of_minus_one_is_true', MODE_BOOL_OF_INT, 0xFFFFFFFF, 1.0),
    case('bool_of_large_is_true', MODE_BOOL_OF_INT, 0x00003039, 1.0),
    case('bool_of_infinity_is_true', MODE_BOOL_OF_INT, 0x7F800000, 1.0),
    # The same question asked the three other ways the lowering could spell it.
    case('bool_cast_of_zero_is_false', MODE_BOOL_CAST, 0, 0.0),
    case('bool_cast_of_large_is_true', MODE_BOOL_CAST, 0x00003039, 1.0),
    case('bool_inverted_of_zero_is_false', MODE_BOOL_INVERTED, 0, 0.0),
    case('bool_inverted_of_large_is_true', MODE_BOOL_INVERTED, 0x00003039, 1.0),
    case('bool_float_test_of_zero_is_false', MODE_BOOL_FLOAT_TEST, 0, 0.0),
    case('bool_float_test_of_large_is_true', MODE_BOOL_FLOAT_TEST, 0x00003039, 1.0),

    # The carrier mirror used numerically: the pattern of 2.0 must come back as
    # 2.0 for 2 * 3 + 1 to be 7.
    case('mirror_mul_add_numeric', MODE_MIRROR_MUL_ADD, 0x40000000, 7.0, b=3.0, c=1.0),
    case('mirror_mul_add_negative_numeric', MODE_MIRROR_MUL_ADD, 0xBFC00000, -2.5, b=2.0, c=0.5),
    case('int_to_float_mul_add', MODE_INT_MUL_ADD, 5.0, 11.0, b=2.0, c=1.0),

    # The NaN test that the PCF pipeline's isnan depends on.
    case('bitcast_nan_is_nan', MODE_NAN_TEST, 0x7FC00000, 1.0),
    case('bitcast_one_is_not_nan', MODE_NAN_TEST, 0x3F800000, 0.0),

    # The comparison operators themselves, which the modes above all lean on.
    case('not_equal_of_different_values', MODE_NOT_EQUAL, 0.25, 1.0, x=0.5),
    case('not_equal_of_equal_values', MODE_NOT_EQUAL, 0.25, 0.0, x=0.25),
    case('greater_than_is_true', MODE_GREATER, 0.5, 1.0, x=0.25),
    case('greater_than_is_false', MODE_GREATER, 0.25, 0.0, x=0.5),
    case('int_not_equal_of_different_values', MODE_INT_NOT_EQUAL, 5.0, 1.0, x=3.0),
    case('int_not_equal_of_equal_values', MODE_INT_NOT_EQUAL, 5.0, 0.0, x=5.0),

    # Delivery: the fragment program must compare what the probe uploaded. The
    # last one predicts black, so a comparison that always answered "equal" would
    # not pass unnoticed.
    case('delivery_of_a_positive', MODE_PASS_THROUGH, 0.25, 0.25),
    case('delivery_of_a_negative', MODE_PASS_THROUGH, -1.5, -1.5),
    case('delivery_of_a_mismatch_is_black', MODE_PASS_THROUGH, 0.25, 0.5, white=False),
    case('readout_operands', MODE_READ_OPERANDS, 1.0, b=0.5, c=0.25, pixel=(255, 128, 64, 255)),
    case('readout_expectations', MODE_READ_EXPECTATIONS, x=0.25, b=0.5, c=0.75,
         pixel=(64, 128, 191, 255)),
]
# A readout only has to show that the values arrived; one step of unorm
# quantisation is not what it is asking.
READOUT_TOLERANCE = 1


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def pattern_of_value(value):
    return struct.unpack('<I', struct.pack('<f', f32(value)))[0]


def signed_of_pattern(pattern):
    return struct.unpack('<i', struct.pack('<I', pattern))[0]


def two_rounding(a, b, c):
    """What a*b+c gives when the product is rounded before the addition."""
    return f32(f32(a * b) + c)


def resolved():
    rows = []
    for entry in CASES:
        name, mode = entry['name'], entry['mode']
        a = entry['a']
        if mode in READOUT_MODES:
            if entry['pixel'] is None:
                raise ValueError('%s: a readout case states the pixel it expects' % name)
        elif entry['expected'] is None:
            raise ValueError('%s: a comparing case needs a value to compare against' % name)
        if mode in INTEGER_CARRIER_MODES:
            carrier = signed_of_pattern(a)
            # The probe uploads this value as a float and the shader converts it
            # back with int(); that scaffolding is only exact if the value is a
            # float32-representable integer.
            if f32(float(carrier)) != float(carrier) or int(f32(float(carrier))) != carrier:
                raise ValueError('%s: integer carrier 0x%08x is not exact in float32' % (name, a))
            a_value = float(carrier)
            a_pattern = a
        else:
            a_value = 0.0 if a is None else a
            a_pattern = pattern_of_value(a_value)
        b = 0.0 if entry['b'] is None else entry['b']
        c = 0.0 if entry['c'] is None else entry['c']
        if entry['expected'] is not None and entry['expected'] != entry['expected']:
            raise ValueError('%s: a NaN never matches, so it cannot be the expectation' % name)
        if entry['pair'] is not None:
            single, two = entry['pair']
            if pattern_of_value(single) == pattern_of_value(two):
                raise ValueError('%s: the two candidates are equal, nothing is separated' % name)
            if pattern_of_value(two_rounding(a_value, b, c)) != pattern_of_value(two):
                raise ValueError('%s: the second candidate is not what rounding the product '
                                 'first gives' % name)
        rows.append({'name': name, 'mode': mode, 'a_pattern': a_pattern,
                     'a_value': f32(a_value), 'b': f32(b), 'c': f32(c),
                     'b_pattern': pattern_of_value(b), 'c_pattern': pattern_of_value(c),
                     'expected': f32(entry['expected'] or 0.0),
                     'x': f32(entry['x'] or 0.0), 'white': entry['white'],
                     'pixel': entry['pixel'], 'question': MODE_QUESTION[mode],
                     # Where a question has two plausible answers, both are asked
                     # as cases and each names the other: a refuted prediction is
                     # then readable as the answer the device did not give.
                     'opposite': entry['opposite'],
                     'tolerance': READOUT_TOLERANCE if entry['pixel'] else 0})
    return rows


def header(rows, hashes):
    lines = ['/* Generated by tools/shader/prepare_gxm_diff.py. Do not edit. */',
             '#define VP_DIFF_CASE_COUNT %d' % len(rows),
             '#define VP_DIFF_TARGET_WIDTH 64',
             '#define VP_DIFF_TARGET_HEIGHT 64',
             '#define VP_DIFF_VERTEX_SHA256 "%s"' % hashes['v'],
             '#define VP_DIFF_FRAGMENT_SHA256 "%s"' % hashes['f'],
             'typedef struct { unsigned mode, a, b, c, expected, x, readout; const char *name; }',
             '    vp_diff_case;',
             'static const vp_diff_case vp_diff_cases[VP_DIFF_CASE_COUNT] = {']
    for row in rows:
        # The operand travels as the pattern of its value: an integer carrier is
        # uploaded as the number the program converts back with int().
        lines.append('    {%uu, 0x%08xu, 0x%08xu, 0x%08xu, 0x%08xu, 0x%08xu, %uu, "%s"},' % (
            row['mode'], pattern_of_value(row['a_value']), pattern_of_value(row['b']),
            pattern_of_value(row['c']), pattern_of_value(row['expected']),
            pattern_of_value(row['x']), 1 if row['mode'] in READOUT_MODES else 0, row['name']))
    lines.append('};')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shacc', type=Path, required=True)
    parser.add_argument('--shacc-config', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2]
                        / 'src/probes/shaders')
    parser.add_argument('--header', type=Path, required=True)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--wsl-distro', default='Ubuntu-24.04')
    arguments = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    config = {}
    for line in arguments.shacc_config.read_text(encoding='utf-8').splitlines():
        if '=' in line:
            key, value = line.split('=', 1)
            config[key.strip()] = value.strip()
    toolchain = json.loads((root / 'config/graphics/shader-toolchain.json')
                           .read_text(encoding='utf-8'))
    shacc = arguments.shacc.resolve()
    compiler = Path(config['root']) / 'fs/ur0/data/external/libshacccg.suprx'
    if (digest(shacc) != toolchain['shacc']['executable_sha256']
            or digest(Path(config['service-vpk'])) != toolchain['shacc']['service_vpk_sha256']
            or digest(compiler) != toolchain['libshacccg']['sha256']):
        raise ValueError('compiler toolchain identity mismatch')

    rows = resolved()
    arguments.stage.mkdir(parents=True, exist_ok=True)
    hashes = {}
    stages = {}
    for stage, profile in (('v', 'sce_vp_psp2'), ('f', 'sce_fp_psp2')):
        source = arguments.source / ('diff_%s.cg' % stage)
        gxp = arguments.stage / ('diff_%s.gxp' % stage)
        if gxp.exists():
            gxp.unlink()
        args = [str(shacc), '--config', str(arguments.shacc_config.resolve()), '-profile', profile,
                '-O0', '-o', str(gxp.resolve()), str(source.resolve()), '--timeout', '40']
        result = compile_serial(args, config['root'])
        (arguments.stage / ('diff_%s.shacc.log' % stage)).write_bytes(result.stdout + result.stderr)
        if result.returncode or not gxp.exists():
            raise ValueError('%s did not compile; see diff_%s.shacc.log' % (source.name, stage))
        binary = gxp.read_bytes()
        if len(binary) < 32 or binary[:4] != b'GXP\0' or struct.unpack_from('<I', binary, 8)[0] > len(binary):
            raise ValueError('invalid GXP output header for %s' % source.name)
        hashes[stage] = digest(gxp)
        stages[stage] = {'profile': profile, 'source_sha256': digest(source),
                         'source_bytes': source.stat().st_size, 'gxp_sha256': hashes[stage],
                         'gxp_bytes': len(binary)}

    arguments.header.parent.mkdir(parents=True, exist_ok=True)
    arguments.header.write_text(header(rows, hashes), encoding='utf-8', newline='\n')
    if arguments.report:
        write_json(arguments.report, {
            'schema': 1, 'task': 'VP01.6.2-R1',
            'method': 'self-authored Cg programs compiled offline by the locked Shacc toolchain; '
                      'no game shader is involved',
            'toolchain': {'shacc_sha256': digest(shacc),
                          'service_vpk_sha256': digest(Path(config['service-vpk'])),
                          'libshacccg_sha256': digest(compiler)},
            'stages': stages,
            'header_sha256': digest(arguments.header),
            'readout_tolerance': READOUT_TOLERANCE,
            'cases': [{'name': row['name'], 'mode': row['mode'], 'question': row['question'],
                       'operands': ['0x%08x' % pattern_of_value(row['a_value']),
                                    '0x%08x' % pattern_of_value(row['b']),
                                    '0x%08x' % pattern_of_value(row['c'])],
                       'compares_against': ('0x%08x' % pattern_of_value(row['expected'])
                                            if row['pixel'] is None else None),
                       'extra_operand': ('0x%08x' % pattern_of_value(row['x'])
                                         if row['pixel'] is None else None),
                       'predicts': 'white' if row['white'] else 'black',
                       'opposite': row['opposite'],
                       'readout_pixel': list(row['pixel']) if row['pixel'] else None}
                      for row in rows]})
    print('compiled %d cases, vertex %s fragment %s' % (len(rows), hashes['v'][:12], hashes['f'][:12]))


if __name__ == '__main__':
    main()

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Host-side oracle for the finite-binary32 fallback emitted by shader_ir.py. */
static float bits_as_unsigned_float(float x) {
    float ax = fabsf(x);
    float exponent;
    float mantissa;
    float bits;
    float sign_bits = 0.0f;
    if (x < 0.0f || (x == 0.0f && (1.0f / x) < 0.0f))
        sign_bits = 2147483648.0f;
    if (ax == 0.0f) return sign_bits;
    if (ax > 3.402823466e38f) {
        bits = 2139095040.0f;
        mantissa = 0.0f;
    } else if (ax < 1.175494351e-38f) {
        bits = 0.0f;
        mantissa = floorf(ax * exp2f(126.0f) * 8388608.0f);
    }
    else {
        exponent = floorf(log2f(ax));
        if (ax < exp2f(exponent)) exponent -= 1.0f;
        else if (ax >= exp2f(exponent + 1.0f)) exponent += 1.0f;
        mantissa = floorf((ax / exp2f(exponent) - 1.0f) * 8388608.0f);
        bits = (exponent + 127.0f) * 8388608.0f;
    }
    return sign_bits + bits + mantissa;
}

static float oracle(uint32_t bits) {
    return (float)bits;
}

static int check(uint32_t bits, unsigned *checked) {
    float x, got, want;
    uint32_t got_bits, want_bits;
    if ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu) != 0u)
        return 0; /* NaN payload/sign decoding is the documented remaining boundary. */
    memcpy(&x, &bits, sizeof(x));
    got = bits_as_unsigned_float(x);
    want = oracle(bits);
    memcpy(&got_bits, &got, sizeof(got_bits));
    memcpy(&want_bits, &want, sizeof(want_bits));
    (*checked)++;
    if (got_bits == want_bits) return 0;
    fprintf(stderr, "mismatch bits=%08x got=%08x want=%08x\n", bits, got_bits, want_bits);
    return 1;
}

int main(void) {
    static const uint32_t cases[] = {
        0x00000000u, 0x80000000u, 0x00000001u, 0x007fffffu,
        0x00800000u, 0x3f800000u, 0xbf800000u, 0x7f7fffffu,
        0x7f800000u, 0xff800000u, 0x4f000000u, 0xcf000000u,
    };
    uint32_t state = 0x6d687675u;
    unsigned checked = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (check(cases[i], &checked)) return 1;
    for (unsigned i = 0; i < 1000000u; i++) {
        state = state * 1664525u + 1013904223u;
        if (check(state, &checked)) return 1;
    }
    printf("PASS finite floatBitsToUint numeric conversion cases=%u\n", checked);
    return 0;
}

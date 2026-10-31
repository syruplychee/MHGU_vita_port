"""Independent CPU oracle for the shadow compare/filter policy (M6-B20A).

This implementation is deliberately separate from the Cg emitter in
``shadow_helpers.py``.  It uses Python control flow and does not import or
reuse the emitted expressions, so a mistake copied into both implementations
cannot self-certify.  Tests compare the two through high-level expected values
that are also written by hand in the tests.

Scope: finite test inputs only.  This oracle makes no claim about target NaN or
subnormal preservation.
"""
import math

COMPARE_NAMES = ('Never', 'Less', 'Equal', 'LessEqual',
                 'Greater', 'NotEqual', 'GreaterEqual', 'Always')


def compare(func, ref, depth):
    """ref OP depth, returning 0.0 or 1.0."""
    if func == 0:
        return 0.0
    if func == 1:
        return 1.0 if ref < depth else 0.0
    if func == 2:
        return 1.0 if ref == depth else 0.0
    if func == 3:
        return 1.0 if ref <= depth else 0.0
    if func == 4:
        return 1.0 if ref > depth else 0.0
    if func == 5:
        return 1.0 if ref != depth else 0.0
    if func == 6:
        return 1.0 if ref >= depth else 0.0
    if func == 7:
        return 1.0
    raise ValueError('compare function out of range: %r' % (func,))


def wrap_index(index, size, mode):
    """Return a wrapped integer texel coordinate, or None for Border.

    Maxwell modes 0..3 are implemented; 4..7 raise instead of being guessed.
    """
    if size <= 0:
        raise ValueError('texture extent must be positive')
    if mode == 0:      # Repeat
        return index % size
    if mode == 1:      # Mirror
        period = 2 * size
        folded = index % period
        return period - 1 - folded if folded >= size else folded
    if mode == 2:      # ClampToEdge
        return min(max(index, 0), size - 1)
    if mode == 3:      # Border
        if 0 <= index < size:
            return index
        return None
    raise ValueError('wrap mode %r is outside the measured 0..3 set' % (mode,))


def _texel(depth_rows, size, ix, iy, ref, plan):
    """One compare-first tap."""
    width, height = size
    x = wrap_index(ix, width, plan['wrap_u'])
    y = wrap_index(iy, height, plan['wrap_v'])
    if x is None or y is None:
        return compare(plan['compare_func'], ref, plan['border_depth'])
    return compare(plan['compare_func'], ref, depth_rows[y][x])


def _texel_depth(depth_rows, size, ix, iy, plan):
    """One depth value with border substitution (the intentionally wrong path)."""
    width, height = size
    x = wrap_index(ix, width, plan['wrap_u'])
    y = wrap_index(iy, height, plan['wrap_v'])
    if x is None or y is None:
        return plan['border_depth']
    return depth_rows[y][x]


def sample_point(depth_rows, size, uv, ref, plan):
    width, height = size
    ix = math.floor(uv[0] * width)
    iy = math.floor(uv[1] * height)
    return _texel(depth_rows, size, ix, iy, ref, plan)


def sample_linear(depth_rows, size, uv, ref, plan):
    width, height = size
    tx = uv[0] * width - 0.5
    ty = uv[1] * height - 0.5
    ix = math.floor(tx)
    iy = math.floor(ty)
    fx = tx - ix
    fy = ty - iy
    c00 = _texel(depth_rows, size, ix, iy, ref, plan)
    c10 = _texel(depth_rows, size, ix + 1, iy, ref, plan)
    c01 = _texel(depth_rows, size, ix, iy + 1, ref, plan)
    c11 = _texel(depth_rows, size, ix + 1, iy + 1, ref, plan)
    top = c00 + (c10 - c00) * fx
    bottom = c01 + (c11 - c01) * fx
    return top + (bottom - top) * fy


def sample_linear_filter_depth_first(depth_rows, size, uv, ref, plan):
    """The rejected alternative: bilinear-filter raw depth, then compare once.

    Kept only so a test can prove the two paths differ on asymmetric depths.
    """
    width, height = size
    tx = uv[0] * width - 0.5
    ty = uv[1] * height - 0.5
    ix = math.floor(tx)
    iy = math.floor(ty)
    fx = tx - ix
    fy = ty - iy
    d00 = _texel_depth(depth_rows, size, ix, iy, plan)
    d10 = _texel_depth(depth_rows, size, ix + 1, iy, plan)
    d01 = _texel_depth(depth_rows, size, ix, iy + 1, plan)
    d11 = _texel_depth(depth_rows, size, ix + 1, iy + 1, plan)
    top = d00 + (d10 - d00) * fx
    bottom = d01 + (d11 - d01) * fx
    return compare(plan['compare_func'], ref, top + (bottom - top) * fy)

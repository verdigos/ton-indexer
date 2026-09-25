import math

import pytest

from dex_tap import curves


def implicit_slope(invariant, x, y, h=1e-7):
    """dy/dx along invariant(x, y) = const, from central finite differences."""
    fx = (invariant(x * (1 + h), y) - invariant(x * (1 - h), y)) / (2 * x * h)
    fy = (invariant(x, y * (1 + h)) - invariant(x, y * (1 - h))) / (2 * y * h)
    return fx / fy


@pytest.mark.parametrize("x, y", [(1e12, 3e12), (5e9, 5e9), (2.5e15, 1.7e9)])
def test_constant_product_matches_invariant(x, y):
    assert curves.constant_product(x, y) == pytest.approx(implicit_slope(lambda a, b: a * b, x, y), rel=1e-6)


@pytest.mark.parametrize("x, y", [(1e12, 1.2e12), (7e9, 3e9)])
def test_solidly_matches_invariant(x, y):
    k = lambda a, b: a ** 3 * b + a * b ** 3
    assert curves.solidly(x, y) == pytest.approx(implicit_slope(k, x, y), rel=1e-6)


@pytest.mark.parametrize("x, y, amp", [(1.01e11, 1.03e11, 100), (5e11, 1e11, 50), (1e9, 1e9, 10)])
def test_stableswap_matches_invariant(x, y, amp):
    d = curves.stableswap_d(x, y, amp)
    assert amp * (x + y) + d == pytest.approx(amp * d + d ** 3 / (4 * x * y), rel=1e-12)
    # D stays fixed along a trade, so the slope is that of F(x, y) = amp*(x+y) - d^3/(4xy)
    f = lambda a, b: amp * (a + b) - d ** 3 / (4 * a * b)
    assert curves.stableswap(x, y, amp) == pytest.approx(implicit_slope(f, x, y), rel=1e-6)


def test_stableswap_is_near_one_when_balanced():
    assert curves.stableswap(1e12, 1e12, 100) == pytest.approx(1.0, rel=1e-12)


@pytest.mark.parametrize("x, y, w", [(3.5e13, 3.1e14, 0.25), (1e9, 1e9, 0.5)])
def test_weighted_constant_product_matches_invariant(x, y, w):
    f = lambda a, b: w * math.log(a) + (1 - w) * math.log(b)
    assert curves.weighted_constant_product(x, y, w) == pytest.approx(implicit_slope(f, x, y), rel=1e-6)


@pytest.mark.parametrize("x, y, amp, rate, w", [(5.26e10, 1.03e11, 50, 1.304, 0.25), (1e12, 2e12, 5, 1.0, 0.5)])
def test_weighted_stableswap_matches_invariant(x, y, amp, rate, w):
    f = lambda a, b: amp * (a + rate * b) + a ** w * (rate * b) ** (1 - w)
    assert curves.weighted_stableswap(x, y, amp, rate, w) == pytest.approx(implicit_slope(f, x, y), rel=1e-6)

"""Spot prices of the pool curves in scope (docs/dex-prices/mvp-plan.md, §5).

Every function takes raw reserves x (asset0) and y (asset1) and returns the marginal price of
one raw unit of asset0 in raw units of asset1, dy/dx along the curve. Fees leave the pool
outside the reserves on every DEX in scope, so this is the fee-free mid price.
"""
from __future__ import annotations

import math


def constant_product(x: float, y: float) -> float:
    return y / x


def solidly(x: float, y: float) -> float:
    """DeDust v1 stable: x^3*y + x*y^3 = k."""
    return y * (3 * x * x + y * y) / (x * (x * x + 3 * y * y))


def stableswap_d(x: float, y: float, amp: float) -> float:
    """Invariant D of STON.fi v2 stableswap: amp*(x+y) + D = amp*D + D^3/(4xy)."""
    d = x + y
    for _ in range(256):
        f = d ** 3 / (4 * x * y) + (amp - 1) * d - amp * (x + y)
        step = f / (3 * d * d / (4 * x * y) + amp - 1)
        d -= step
        if abs(step) <= d * 1e-15:
            break
    return d


def stableswap(x: float, y: float, amp: float) -> float:
    d3 = stableswap_d(x, y, amp) ** 3
    xy4 = 4 * amp * x * x * y * y
    return (xy4 + d3 * y) / (xy4 + d3 * x)


def weighted_constant_product(x: float, y: float, w0: float) -> float:
    """STON.fi v2 weighted_const_product: x^w0 * y^(1-w0) = k; w0 as a fraction."""
    return w0 * y / ((1 - w0) * x)


def weighted_stableswap(x: float, y: float, amp: float, rate: float, w0: float) -> float:
    """STON.fi v2 weighted_stableswap: amp*(x + r*y) + x^w0 * (r*y)^(1-w0) = k.

    amp, rate and w0 are plain numbers (the pool reports them scaled by 1e18).
    """
    ry = rate * y
    fx = amp + w0 * math.exp((w0 - 1) * math.log(x) + (1 - w0) * math.log(ry))
    fy = amp * rate + (1 - w0) * rate * math.exp(w0 * math.log(x) - w0 * math.log(ry))
    return fx / fy

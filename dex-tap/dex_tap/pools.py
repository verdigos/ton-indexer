"""Pool states of the DEXes in scope, parsed from get-method stacks.

Stack values are what pytoniq's VmStack yields: int, Cell, Slice or None. Jetton sides of
STON.fi pools are router jetton wallets; the caller resolves them to masters (assets.py).
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Sequence

from pytoniq_core import Address, Cell, Slice

from dex_tap import curves

TON = ""
E18 = 10 ** 18

# Pool code hash (hex) -> DEX. Collected from live pools on 2026-09-25; the worker's DexTap
# forwards exactly these.
CODE_HASHES = {
    "82566AD72B6568FE7276437D3B0C911AAB65ED701C13601941B2917305E81C11": "stonfi_v1",
    "EC614EA4AAEA3F7768606F1C1632B3374D3DE096A1E7C4BA43C8009C487FEE9D": "stonfi_v2",
    "CF5D0B99FA704E7CF2C9D50A8FF8B8BC7CE0B8A74E414B9C279AC544E7AADE05": "stonfi_v2",
    "FBC7E8FCCA72C2B9C078B359FFA936F46384491B895B6577B0A6CB3F569040BC": "stonfi_v2",
    "F04A14C3231221056C3499965E4604417E324F8E9121D840120D803288715594": "stonfi_v2",
    "DAC47636AE899081EBD4F47DC90EF9DE98456B1000591069773F683C6D601FA9": "stonfi_v2",
    "1275095B6DA3911292406F4F4386F9E780099B854C6DEE9EE2895DDCE70927C1": "dedust_v1",
    "778F0D3FE6482C50888970DF5E787F40F3A4AB282170C035A5920877058C99D3": "dedust_v1",
    "C0F9D14FBC8E14F0D72CBA2214165EEE35836AB174130912BAF9DBFA43EAD562": "dedust_v1",
    "3997A5C1EE8923E93CF3A6A98EA3EF9482C64DD8AEA2BA8365689E14D99C760D": "dedust_v2",
    "5851780D2386E989B9AB956DA7CEC5C69A667B9E525F628DCBA5EB02C1EE0367": "dedust_v2",
}

# Get methods each DEX needs, in call order.
METHODS = {
    "stonfi_v1": ("get_pool_data",),
    "stonfi_v2": ("get_pool_type", "get_pool_data"),
    "dedust_v1": ("get_assets", "get_reserves", "is_stable", "get_trade_fee"),
    "dedust_v2": ("get_pool_data",),
}

CURVES = {
    "constant_product": "volatile",
    "stableswap": "stable",
    "solidly": "stable",
    "weighted_const_product": "weighted",
    "weighted_stableswap": "weighted",
}


class UnsupportedPool(ValueError):
    pass


@dataclass
class PoolState:
    dex: str
    pool_type: str            # constant_product | stableswap | solidly | weighted_const_product | weighted_stableswap
    side0: str                # TON, a jetton master, or (STON.fi) a router jetton wallet until resolved
    side1: str
    reserve0: int
    reserve1: int
    lp_fee: float             # fraction of the input
    protocol_fee: float
    params: dict = field(default_factory=dict)
    sides_are_wallets: bool = False

    @property
    def curve(self) -> str:
        return CURVES[self.pool_type]

    def spot_price(self) -> float:
        """Raw asset1 per raw asset0."""
        x, y = float(self.reserve0), float(self.reserve1)
        if x <= 0 or y <= 0:
            raise UnsupportedPool("empty reserves")
        p = self.params
        if self.pool_type == "constant_product":
            return curves.constant_product(x, y)
        if self.pool_type == "solidly":
            return curves.solidly(x, y)
        if self.pool_type == "stableswap":
            return curves.stableswap(x, y, p["amp"])
        if self.pool_type == "weighted_const_product":
            return curves.weighted_constant_product(x, y, p["weight0"])
        if self.pool_type == "weighted_stableswap":
            return curves.weighted_stableswap(x, y, p["amp"], p["rate"], p["weight0"])
        raise UnsupportedPool(self.pool_type)


def raw_address(value: Cell | Slice | None) -> str:
    """Address from a cell or slice; addr_none means TON."""
    if value is None:
        return TON
    s = value.begin_parse() if isinstance(value, Cell) else value.copy()
    address = s.load_address()
    return TON if address is None else address_str(address)


def address_str(address: Address) -> str:
    return address.to_str(is_user_friendly=False).upper()


def dedust_asset(value: Cell | Slice) -> str:
    """DeDust v1 Asset: native$0000 | jetton$0001 workchain_id:int8 address:uint256."""
    s = value.begin_parse() if isinstance(value, Cell) else value.copy()
    tag = s.load_uint(4)
    if tag == 0:
        return TON
    if tag != 1:
        raise UnsupportedPool(f"DeDust asset tag {tag}")
    workchain = s.load_int(8)
    return f"{workchain}:{s.load_bytes(32).hex().upper()}"


def snake_string(value: Cell | Slice) -> str:
    s = value.begin_parse() if isinstance(value, Cell) else value.copy()
    return s.load_snake_string()


def parse_pool(dex: str, stacks: dict[str, Sequence]) -> PoolState:
    if dex == "stonfi_v1":
        s = stacks["get_pool_data"]
        return PoolState(dex, "constant_product", raw_address(s[2]), raw_address(s[3]), s[0], s[1],
                         lp_fee=s[4] / 10000, protocol_fee=(s[5] + s[6]) / 10000, sides_are_wallets=True)
    if dex == "stonfi_v2":
        s = stacks["get_pool_data"]
        pool_type = snake_string(stacks["get_pool_type"][0])
        params: dict = {}
        if pool_type == "stableswap":
            params = {"amp": s[12]}
        elif pool_type == "weighted_const_product":
            params = {"weight0": s[12] / E18}
        elif pool_type == "weighted_stableswap":
            params = {"amp": s[12] / E18, "rate": s[13] / E18, "weight0": s[14] / E18}
        elif pool_type != "constant_product":
            raise UnsupportedPool(f"STON.fi v2 pool type {pool_type}")
        return PoolState(dex, pool_type, raw_address(s[5]), raw_address(s[6]), s[3], s[4],
                         lp_fee=s[7] / 10000, protocol_fee=s[8] / 10000, params=params, sides_are_wallets=True)
    if dex == "dedust_v1":
        a, r = stacks["get_assets"], stacks["get_reserves"]
        num, den = stacks["get_trade_fee"]
        pool_type = "solidly" if stacks["is_stable"][0] else "constant_product"
        return PoolState(dex, pool_type, dedust_asset(a[0]), dedust_asset(a[1]), r[0], r[1],
                         lp_fee=num / den, protocol_fee=0.0)
    if dex == "dedust_v2":
        s = stacks["get_pool_data"]
        return PoolState(dex, "constant_product", raw_address(s[3]), raw_address(s[4]), s[9], s[10],
                         lp_fee=s[8] / 10000, protocol_fee=0.0)
    raise UnsupportedPool(dex)


def read_pool(dex: str, run_method: Callable[[str], Sequence | None]) -> PoolState:
    """Call the DEX's get methods through run_method(name) -> stack and parse the result."""
    stacks = {}
    for name in METHODS[dex]:
        stack = run_method(name)
        if stack is None:
            raise UnsupportedPool(f"{dex}: {name} failed")
        stacks[name] = stack
    return parse_pool(dex, stacks)

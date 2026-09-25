"""Capture DEX pool states as JSON fixtures for pool parsing and spot-price tests.

For each pool the fixture holds the account state at one masterchain seqno (code, data,
balance), the raw stacks of the pool get methods, the jetton master behind every jetton
wallet the pool references, and reference quotes for a small amount where the DEX has an
on-chain estimate method. Get methods run locally in the TVM emulator, as in api_fixture.

    python -m tests.utils.pool_fixture <pool> [<pool> ...] [--seqno N] [--out-dir tests/pools]

The API key comes from the API_KEY environment variable, else from the repository .env.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
from pathlib import Path
from typing import Iterable

from pytoniq_core import Cell

from tests.utils.api_fixture import (ApiClient, ApiError, _method, _number_entry, normalize_address,
                                     stack_address, stack_cell, stack_number)


DEFAULT_OUT_DIR = Path(__file__).resolve().parents[1] / "pools"

# Stack length of get_pool_data per DEX, used when the pool has no more specific probe.
STONFI_V1_POOL_DATA_LEN = 10
DEDUST_V2_POOL_DATA_LEN = 19

# Reference quotes use this fraction of the input-side reserve, small enough to sit on the
# spot price and large enough to survive integer rounding.
QUOTE_FRACTION = 10 ** 6


def detect_dex(methods: dict[str, list[dict] | None]) -> str | None:
    if methods.get("get_pool_type"):
        return "stonfi_v2"
    if methods.get("get_assets") and methods.get("get_reserves"):
        return "dedust_v1"
    pool_data = methods.get("get_pool_data")
    if pool_data and len(pool_data) == STONFI_V1_POOL_DATA_LEN:
        return "stonfi_v1"
    if pool_data and len(pool_data) == DEDUST_V2_POOL_DATA_LEN:
        return "dedust_v2"
    return None


def stonfi_wallets(dex: str, pool_data: list[dict]) -> list[str]:
    """Router jetton wallets a STON.fi pool trades, in token0/token1 order."""
    indexes = (2, 3) if dex == "stonfi_v1" else (5, 6)
    return [_address_from_cell(pool_data[i]) for i in indexes]


def _address_from_cell(entry: dict) -> str:
    cell, _ = stack_cell(entry)
    address = normalize_address(cell.begin_parse().load_address())
    if address is None:
        raise ValueError("pool references an empty address")
    return address


def _slice_entry(entry: dict) -> dict:
    _, boc = stack_cell(entry)
    return {"@type": "tvm.stackEntrySlice", "slice": {"bytes": boc}}


def _wallet_master(client: ApiClient, wallet: str, seqno: int) -> str | None:
    data = _method(client, wallet, "get_wallet_data", seqno)
    return stack_address(data[2]) if data and len(data) >= 3 else None


def _quotes(client: ApiClient, pool: str, dex: str, seqno: int,
            methods: dict[str, list[dict] | None]) -> list[dict]:
    quotes = []
    if dex == "stonfi_v1":
        pool_data = methods["get_pool_data"]
        for side, wallet in enumerate(stonfi_wallets(dex, pool_data)):
            amount = max(1, stack_number(pool_data[side]) // QUOTE_FRACTION)
            args = [_number_entry(amount), _slice_entry(pool_data[2 + side])]
            quotes.append({"method": "get_expected_outputs", "side": side, "amount_in": str(amount),
                           "wallet_in": wallet,
                           "stack": _method(client, pool, "get_expected_outputs", seqno, args)})
    elif dex == "dedust_v1":
        assets = methods["get_assets"]
        reserves = methods["get_reserves"]
        for side in (0, 1):
            amount = max(1, stack_number(reserves[side]) // QUOTE_FRACTION)
            args = [assets[side], _number_entry(amount)]
            quotes.append({"method": "estimate_swap_out", "side": side, "amount_in": str(amount),
                           "stack": _method(client, pool, "estimate_swap_out", seqno, args)})
    return quotes


def build_pool_fixture(client: ApiClient, pool: str, seqno: int) -> dict:
    pool = normalize_address(pool)
    state = client.get_address_information(pool, seqno)
    if not state or not state.get("code") or not state.get("data"):
        raise ValueError(f"no active state for {pool} at seqno {seqno}")
    code = Cell.one_from_boc(base64.b64decode(state["code"]))
    data = Cell.one_from_boc(base64.b64decode(state["data"]))

    probes = ("get_pool_type", "get_pool_data", "get_assets", "is_stable", "get_reserves", "get_trade_fee")
    methods = {name: _method(client, pool, name, seqno) for name in probes}
    dex = detect_dex(methods)
    if dex is None:
        raise ValueError(f"{pool} does not look like a supported DEX pool")

    wallet_masters = {}
    if dex in ("stonfi_v1", "stonfi_v2"):
        for wallet in stonfi_wallets(dex, methods["get_pool_data"]):
            wallet_masters[wallet] = _wallet_master(client, wallet, seqno)

    return {
        "pool": pool,
        "dex": dex,
        "mc_seqno": seqno,
        "sync_utime": state.get("sync_utime"),
        "balance": str(state.get("balance", "0")),
        "code_hash": base64.b64encode(code.hash).decode(),
        "data_hash": base64.b64encode(data.hash).decode(),
        "code_boc": state["code"],
        "data_boc": state["data"],
        "methods": {name: stack for name, stack in methods.items() if stack is not None},
        "wallet_masters": wallet_masters,
        "quotes": _quotes(client, pool, dex, seqno, methods),
    }


def fixture_path(out_dir: Path, fixture: dict) -> Path:
    short = fixture["pool"].split(":")[1][:8].lower()
    return out_dir / f"{fixture['dex']}-{short}-{fixture['mc_seqno']}.json"


def write_pool_fixtures(client: ApiClient, pools: Iterable[str], seqno: int, out_dir: Path) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    paths = []
    for pool in pools:
        fixture = build_pool_fixture(client, pool, seqno)
        path = fixture_path(out_dir, fixture)
        path.write_text(json.dumps(fixture, indent=1, sort_keys=True) + "\n")
        paths.append(path)
    return paths


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Capture DEX pool states as JSON fixtures")
    parser.add_argument("pools", nargs="+", help="pool addresses in any form")
    parser.add_argument("--seqno", type=int, help="masterchain seqno of the state (default: latest)")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR, help="output directory")
    args = parser.parse_args(argv)
    try:
        client = ApiClient(api_key=os.environ.get("API_KEY"))
        seqno = args.seqno or int(client.request(2, "getMasterchainInfo")["last"]["seqno"])
        for path in write_pool_fixtures(client, args.pools, seqno, args.out_dir):
            print(path)
    except (ApiError, ValueError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Asset identity and decimals, resolved once per asset and cached on disk."""
from __future__ import annotations

import base64
from pathlib import Path

from pytoniq_core import Cell

from dex_tap.pools import TON, PoolState
from dex_tap.toncenter import DiskCache, Toncenter

# Wrappers DEXes use for TON inside pools (ton-mch-engine/src/host/DexPton.cpp); all mean TON.
PTON_MASTERS = {
    "0:8CDC1D7640AD5EE326527FC1AD0514F468B30DC84B0173F0E155F451B4E11F7C",  # STON.fi pTON v1
    "0:671963027F7F85659AB55B821671688601CDCF1EE674FC7FBBB1A776A18D34A3",  # STON.fi pTON v2
    "0:949C4C66760C002800E2FA3D8A3CA4E1C90A9373B53AE7472033483BF14CD95E",  # Tonco wTTON
    "0:7019100C8D363F08C3562A1CC178703E54F8E9DAB0FDBC1DEBC1EA46D9CA3979",  # Tonco wGRAM
}
USDT_MASTER = "0:B113A994B5024A16719F69139328EB759596C38A25F59028B146FECDC3621DFE"
KNOWN = {TON: {"decimals": 9, "symbol": "TON"}, USDT_MASTER: {"decimals": 6, "symbol": "USDT"}}


class AssetResolver:
    def __init__(self, toncenter: Toncenter, cache_dir: Path):
        self.toncenter = toncenter
        self.wallets = DiskCache(cache_dir / "wallet_masters.json")
        self.assets = DiskCache(cache_dir / "assets.json")
        self.libraries = DiskCache(cache_dir / "libraries.json")

    def library(self, lib_hash: bytes) -> Cell:
        key = lib_hash.hex().upper()
        if key not in self.libraries:
            self.libraries.put(key, self.toncenter.library(key))
        return Cell.one_from_boc(base64.b64decode(self.libraries.get(key)))

    def wallet_master(self, wallet: str) -> str:
        if wallet not in self.wallets:
            master = self.toncenter.jetton_wallet_master(wallet)
            if master is None:
                raise LookupError(f"unknown jetton wallet {wallet}")
            self.wallets.put(wallet, master)
        master = self.wallets.get(wallet)
        return TON if master in PTON_MASTERS else master

    def resolve_sides(self, pool: PoolState) -> tuple[str, str]:
        if not pool.sides_are_wallets:
            return pool.side0, pool.side1
        return self.wallet_master(pool.side0), self.wallet_master(pool.side1)

    def info(self, asset: str) -> dict:
        """{"decimals", "symbol"}; decimals default to 9 per TEP-64."""
        if asset in KNOWN:
            return KNOWN[asset]
        if asset not in self.assets:
            self.assets.put(asset, self.toncenter.jetton_master(asset) or {"decimals": 9, "symbol": ""})
        return self.assets.get(asset)

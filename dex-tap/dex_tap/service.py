"""Turns tap events into pool snapshot rows."""
from __future__ import annotations

import logging
import time
from collections import Counter
from datetime import datetime, timezone

from dex_tap import pools
from dex_tap.assets import KNOWN, AssetResolver
from dex_tap.protocol import GapCounter, TapEvent
from dex_tap.sink import Sink
from dex_tap.tvm import Contract

log = logging.getLogger("dex_tap")


class Stats:
    def __init__(self):
        self.events = 0
        self.bytes = 0
        self.snapshots = 0
        self.errors: Counter[str] = Counter()
        self.by_type: Counter[str] = Counter()
        self.process_us: list[int] = []
        self.gaps = GapCounter()
        self.started = time.monotonic()

    def report(self, sink: Sink) -> str:
        elapsed = max(time.monotonic() - self.started, 1e-9)
        us = sorted(self.process_us)
        p50 = us[len(us) // 2] if us else "-"
        p99 = us[min(len(us) - 1, int(len(us) * 0.99))] if us else "-"
        line = (f"events={self.events} ({self.events / elapsed:.1f}/s, {self.bytes / 1e6:.1f} MB) "
                f"snapshots={self.snapshots} lost={self.gaps.lost} sessions={self.gaps.sessions} "
                f"process_us p50={p50} p99={p99} flush_ms={sink.flush_ms_total:.0f} "
                f"types={dict(self.by_type)} errors={dict(self.errors)}")
        self.process_us.clear()
        return line


class Processor:
    def __init__(self, resolver: AssetResolver, sink: Sink):
        self.resolver = resolver
        self.sink = sink
        self.stats = Stats()
        self.known_pools: dict[str, tuple] = {}
        self.known_assets: set[str] = set()
        for asset, info in KNOWN.items():
            self._asset(asset, info)

    def _asset(self, asset: str, info: dict | None = None) -> None:
        if asset in self.known_assets:
            return
        info = info or self.resolver.info(asset)
        self.sink.add("dex_assets", {"asset": asset, "symbol": info.get("symbol") or "", "decimals": info["decimals"]})
        self.known_assets.add(asset)

    def handle(self, event: TapEvent, size: int = 0) -> dict | None:
        started = time.perf_counter()
        self.stats.events += 1
        self.stats.bytes += size
        self.stats.gaps.observe(event)
        dex = pools.CODE_HASHES.get(event.code_hash)
        if dex is None:
            self.stats.errors["unknown_code"] += 1
            return None
        try:
            contract = Contract(event.account, event.code, event.data, event.balance, event.utime,
                                self.resolver.library)
            pool = pools.read_pool(dex, contract.run)
            asset0, asset1 = self.resolver.resolve_sides(pool)
            spot = pool.spot_price()
        except Exception as exc:  # one bad pool must not stop the stream
            self.stats.errors[type(exc).__name__] += 1
            log.debug("pool %s failed: %s", event.account, exc)
            return None

        self._asset(asset0)
        self._asset(asset1)
        identity = (dex, pool.pool_type, asset0, asset1)
        if self.known_pools.get(event.account) != identity:
            self.sink.add("dex_pools", {
                "pool": event.account, "dex": dex, "pool_type": pool.pool_type, "curve": pool.curve,
                "asset0": asset0, "asset1": asset1, "code_hash": event.code_hash,
                "first_mc_seqno": event.mc_seqno, "updated_mc_seqno": event.mc_seqno,
            })
            self.known_pools[event.account] = identity

        process_us = int((time.perf_counter() - started) * 1e6)
        row = {
            "pool": event.account, "mc_seqno": event.mc_seqno,
            "block_utime": datetime.fromtimestamp(event.utime, tz=timezone.utc),
            "last_tx_lt": event.last_trans_lt, "dex": dex, "pool_type": pool.pool_type,
            "asset0": asset0, "asset1": asset1, "reserve0": pool.reserve0, "reserve1": pool.reserve1,
            "spot_price": spot, "amp": float(pool.params.get("amp", 0)),
            "weight0": float(pool.params.get("weight0", 0)), "rate": float(pool.params.get("rate", 0)),
            "lp_fee": pool.lp_fee, "protocol_fee": pool.protocol_fee,
            "tx_count": min(event.tx_count, 65535), "process_us": process_us,
        }
        self.sink.add("dex_pool_snapshots", row)
        self.stats.snapshots += 1
        self.stats.by_type[f"{dex}/{pool.pool_type}"] += 1
        self.stats.process_us.append(process_us)
        return row

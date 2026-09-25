"""Where processed rows go: ClickHouse, or NDJSON files when no ClickHouse is at hand."""
from __future__ import annotations

import json
import time
from pathlib import Path

SNAPSHOT_COLUMNS = [
    "pool", "mc_seqno", "block_utime", "last_tx_lt", "dex", "pool_type", "asset0", "asset1",
    "reserve0", "reserve1", "spot_price", "amp", "weight0", "rate", "lp_fee", "protocol_fee",
    "tx_count", "process_us",
]
POOL_COLUMNS = ["pool", "dex", "pool_type", "curve", "asset0", "asset1", "code_hash", "first_mc_seqno",
                "updated_mc_seqno"]
ASSET_COLUMNS = ["asset", "symbol", "decimals"]


class Sink:
    """Buffers rows per table and flushes every `flush_rows` rows or `flush_seconds`."""

    def __init__(self, flush_rows: int = 1000, flush_seconds: float = 1.0):
        self.flush_rows = flush_rows
        self.flush_seconds = flush_seconds
        self.buffers: dict[str, list[dict]] = {"dex_pool_snapshots": [], "dex_pools": [], "dex_assets": []}
        self.last_flush = time.monotonic()
        self.written = 0
        self.flush_ms_total = 0.0

    def add(self, table: str, row: dict) -> None:
        self.buffers[table].append(row)
        if (sum(len(rows) for rows in self.buffers.values()) >= self.flush_rows
                or time.monotonic() - self.last_flush >= self.flush_seconds):
            self.flush()

    def flush(self) -> None:
        started = time.monotonic()
        for table, rows in self.buffers.items():
            if rows:
                self._write(table, rows)
                self.written += len(rows) if table == "dex_pool_snapshots" else 0
                self.buffers[table] = []
        self.last_flush = time.monotonic()
        self.flush_ms_total += (self.last_flush - started) * 1000

    def _write(self, table: str, rows: list[dict]) -> None:
        raise NotImplementedError


class ClickHouseSink(Sink):
    COLUMNS = {"dex_pool_snapshots": SNAPSHOT_COLUMNS, "dex_pools": POOL_COLUMNS, "dex_assets": ASSET_COLUMNS}

    def __init__(self, url: str, database: str = "dex_prices", **kwargs):
        import clickhouse_connect

        super().__init__(**kwargs)
        self.client = clickhouse_connect.get_client(dsn=url, database=database)

    def _write(self, table: str, rows: list[dict]) -> None:
        columns = self.COLUMNS[table]
        self.client.insert(table, [[row[c] for c in columns] for row in rows], column_names=columns)


class NdjsonSink(Sink):
    def __init__(self, out_dir: Path, **kwargs):
        super().__init__(**kwargs)
        self.out_dir = out_dir
        out_dir.mkdir(parents=True, exist_ok=True)

    def _write(self, table: str, rows: list[dict]) -> None:
        with (self.out_dir / f"{table}.ndjson").open("a") as f:
            for row in rows:
                f.write(json.dumps(row, default=str) + "\n")

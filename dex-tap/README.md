# dex-tap

Prototype of the DEX price pipeline (stage M-1 in [`docs/dex-prices/mvp-plan.md`](../docs/dex-prices/mvp-plan.md)).
The index worker sends the state of every DEX pool that changed in a masterchain block to a unix
datagram socket; this service runs the pool get methods in the TVM emulator, computes spot prices and
writes snapshots to ClickHouse. `ton-index-go` serves prices from those tables.

```
ton-index-postgres --dex-tap-socket /run/dex-tap.sock ...   (or ton-index-clickhouse for a history range)
        │  one JSON datagram per pool per masterchain block, lossy, never blocks indexing
        ▼
python -m dex_tap --socket /run/dex-tap.sock --clickhouse http://localhost:8123
        │  dex_prices.dex_pool_snapshots, dex_pools, dex_assets (+ latest and 1m candle MVs)
        ▼
ton-index-go -clickhouse-dex-prices clickhouse://localhost:9000/dex_prices
        GET /api/v3/experimental/dex/prices?asset=<master>|ton
        GET /api/v3/experimental/dex/pool_snapshots?pool=<address>
```

## Run

On the machine with the TON node and the worker:

1. Python 3.11+, `pip install -r requirements.txt`, and a current TVM emulator:

   ```
   curl -L -o ~/libemulator-linux-x86_64.so \
       https://github.com/ton-blockchain/ton/releases/latest/download/libemulator-linux-x86_64.so
   ```

2. ClickHouse schema: `clickhouse client --multiquery < sql/schema.sql`.

3. The listener. `API_KEY` is a toncenter key, used once per library cell, jetton wallet and jetton
   master; results are cached in `--cache-dir`.

   ```
   API_KEY=... python -m dex_tap --socket /run/dex-tap.sock --clickhouse http://localhost:8123
   ```

   Every `--stats-every` seconds it logs events per second, lost datagrams (from sequence numbers),
   processing time percentiles, pool types and errors. `--ndjson DIR` writes files instead of ClickHouse.

4. The worker with `--dex-tap-socket /run/dex-tap.sock`, the same path, as a user that can write to it.
   Order does not matter; datagrams sent while the listener is down are dropped and counted.

5. Prices: `clickhouse client --param_max_age_sec=604800 --param_min_liquidity_usdt=1000
   --param_assets="[]" --queries-file sql/prices.sql`, or the API endpoints above.

Without a node, `python -m dex_tap.replay --socket /run/dex-tap.sock ../indexer/tests/pools/*.json` sends
the captured pool fixtures in the worker's format.

## Tests

`python -m pytest tests` (curve math against the invariants, pool parsing and spot prices against the
fixtures' on-chain quotes, the wire format). The worker side is `ctest -R dex_tap` in the worker build.

## Limits of the prototype

- Pools are recognized by code hash only; the router or factory check of ADR-0007 is not done.
- `process_us` includes the first toncenter lookups for a new pool's assets; steady-state values are
  what matters for sizing.
- `sql/prices.sql` is copied into `ton-index-go/index/dexprices/`; a Go test keeps the copies equal.

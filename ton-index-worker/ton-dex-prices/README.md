# DEX prices

Trusted STON.fi v1/v2 and DeDust v1/v2 pools provide jetton prices in GRAM (Toncoin) and USDT, with minute history.

- The indexer writes pool snapshots with spot and depth for supported curves to ClickHouse.
- The aggregator turns those pool snapshots into minute prices in ClickHouse.
- The API serves current prices and price history.

Missing prices are NULL with a reason.

Freshness uses the live head of indexed blocks, never the clock. A minute becomes final about 60 s after it starts, plus up to 5 s. Until then, the current minute has a history point with `final: false`, recomputed every 5 s.

## Components and data flow

### Indexer

- `ton-index-clickhouse --dex-prices-database dex_prices` detects and verifies pools. It writes their pool snapshots into that database on the same server.
- Each insert batch writes pool snapshots just before the batch's `blocks` rows.
- At startup, `ton-index-clickhouse` creates the price schema and the view `mc_blocks` over its own masterchain blocks.
- A failed pool snapshot insert logs an ERROR line with the dropped masterchain range and never stops indexing.
- Without `--dex-prices-database`, `ton-index-clickhouse` skips STON.fi pool detection and builds no pool snapshots. It still detects DeDust pools.
- `ton-index-postgres` does not write pool snapshots.

### ClickHouse tables

[sql/schema.sql](sql/schema.sql) defines the ClickHouse tables and views.

| Table or view | Contents |
|---|---|
| `pool_snapshots` | Pool snapshots |
| `pool_latest` | Each pool's newest state (filled by materialized view) |
| `pool_daily` | Each pool's newest state per day (filled by materialized view) |
| `pool_fees` | Each pool's newest swap fee (filled by materialized view) |
| `anchor_1m` | TON/USDT anchor for every minute |
| `price_1m` | Minute asset prices |
| `price_latest` | Each asset's newest price (filled by materialized view) |
| `ingest_head` | Live head |
| `aggregate_state` | Last computed minute |
| `anchor_now` | Current minute's anchor |
| `price_now` | Current minute's asset prices |
| `anchor_all` | View that adds `anchor_now` to the stored anchor rows |

- `price_1m` gets a row for an asset at a minute where the asset's pools changed, if its price, pools, tvl or state differ from its previous row.
- At the minute the anchor gets its first price, every asset gets a row. USDT came to TON in April 2024, so older prices get a USDT price this way.
- Each row stores the anchor of its minute. Its USDT price is the TON price times that anchor, so an asset's USDT price changes only at its own rows.
- Materialized views fill `pool_latest`, `pool_daily`, `pool_fees`, and `price_latest` on insert. Refill them after adding data with ATTACH or RESTORE.

### Aggregator

The [aggregator](../../ton-index-price-api/cmd/dex-prices-aggregator/) runs as one instance per database.

- It moves the live head in `ingest_head` to the newest block in `mc_blocks` with no missing block below it.
- That prevents the live head from passing a block whose pool snapshots are still in flight.
- Once the live head passes a minute's end, the aggregator computes that minute.
- It computes the anchor first, then prices for assets whose pools changed, and for every asset at the anchor's first minute.
- If a range contains outputs from an earlier attempt, it deletes them first.
- Once caught up, it recomputes the current minute at each interval when the live head has moved.
- It logs one line per finished range.
- Stored prices use raw quote units per raw asset unit. The API scales them when decimals are known.

### API

`ton-index-price-api -clickhouse <dsn> -kvrocks <addr>` serves `/api/v3/jetton/prices` and `/api/v3/jetton/price_history`. You can also pass `-kvrocks-sentinels`, `-kvrocks-password`, and the other `-kvrocks-*` flags.

- The API reads jetton decimals from kvrocks and keeps them in memory for 5 minutes per asset.
- Assets are jetton master addresses or `gram` (Toncoin, by its current name; `ton` also works).
- Quotes are `gram` and `usdt`.
- Responses use `gram`, while ClickHouse tables keep the key `TON`.
- In responses, `value` is in whole units and needs known decimals. `value_raw` is in raw units and is there whenever the asset has a price.

#### `/api/v3/jetton/prices`

- Serves a cached price snapshot of the newest prices for all assets.
- After `-latest-refresh` (default 2 s), an incoming request starts a reload.
- The reload reads the prices first and then the live head.
- Requests use the previous price snapshot until its age reaches the larger of `-latest-refresh` and 30 s.
- After that limit, requests wait for the reload and get 500 if it fails.
- The endpoint reads decimals only for assets that have a price.

#### `/api/v3/jetton/price_history`

- Reads the row in force at the range start and the newest row of each step in one query.
- Rounds the range outward to whole steps and treats any bound after now as now.
- Clients asking for "the last 7 days" at different seconds therefore share one response.
- The request timeout (`X-Timeout`) is at least 500 ms. A query keeps running for the cache after its request times out, so a client with a near-zero timeout could start queries without waiting for any of them.
- Each cached response expires after 80% to 120% of `-history-cache-ttl` (24 to 36 s at the 30 s default).
- The cache holds up to `-history-cache-mb` (default 1024 MiB).
- It runs one query at a time per distinct request.
- At 3,000 requests/s over 3,997 jettons, that keeps ClickHouse at about 3 threads.

| Range length | Step |
|---|---|
| Under 6 h | 1 min |
| Under 24 h | 5 min |
| Under 15 days | 15 min |
| Under 60 days | 1 h |
| Under 240 days | 4 h |
| Under 1440 days | 1 day |
| Longer ranges | 1 week |

## Rules

### Pool trust

- Store a pool only when both its getters and trust checks pass.
- Write a removal row when a cached trusted pool stops passing.
- At startup, `ton-index-clickhouse` loads the identity of every pool in `pool_latest` into its cache.
- A pool removed after a restart therefore still gets a removal row.
- Detection checks a loaded pool again before trusting it.
- **DeDust v1**: must match the factory's derived pool address.
- **DeDust v2**: must have the DeDust admin and match its original StateInit address in workchain 0.
- **STON.fi**: must have an allowlisted router that derives the pool address. Both wallets must belong to that router and match their masters' derived addresses.

### Curves and pricing

- Price only unlocked pools that use a supported curve and have positive reserves.
- A DeDust v2 pool counts as locked until its status is ready (2), and while its swaps are off.
- Invariant curves use `amp`, `rate`, and `w0` as the getters return them.
- For DeDust stable, `rate` is asset1's storage rate over asset0's × 1e18, which carries the decimals.
- Native TON and pTON aliases merge into TON.

| Curve family | Curves | Pricing method |
|---|---|---|
| Constant product | STON.fi v1/v2 `constant_product`, DeDust v1/v2 `volatile` | Closed form |
| Invariant | STON.fi v2 `stableswap`, `weighted_stableswap`, `weighted_const_product`; DeDust v1 `stable` | From invariants; depths found by bisection |

### Aggregation

- A pool counts only with a swap fee (LP plus protocol) of at most 1%. A pool with a higher fee can stay that far off the market, because arbitrage does not pay.
- For a jetton, a pool also needs at least 100 TON on its smaller side, valued at the pool's own price.
- The reference price is the weighted median of those pools, so one pool cannot set it alone. Pools within 7.5% of it are kept.
- The price is the weighted median of the kept pools, or their weighted mean when fewer than 3 remain. A pool's weight is its smaller side, valued in TON at the reference price.
- The TON/USDT anchor takes the weighted median of TON/USDT pools with at least 100,000 TON, and averages the last 5 minute medians.

### State, age, and constants

- A pool state counts until the pool changes, however old it is.
- Removed pools end their earlier state.
- For a jetton price, `age_sec` counts from the newest state change among the pools used to the live head.
- For GRAM/USDT and USDT/GRAM, `age_sec` counts from the start of the anchor minute to the live head.
- `DexConstants.{h,cpp}` holds the compiled allowlists and quotes.
- The USDT master and the TON aliases (pTON, DeDust native) are constants in `ton-index-price-api/prices`.
- Aggregation thresholds live in [config.go](../../ton-index-price-api/aggregate/config.go).

## Operations

### Launch order

B and S are masterchain block seqnos. The aggregator takes unix timestamps (in seconds).

1. Restart `ton-index-clickhouse` with `--dex-prices-database dex_prices`. It keeps running and writes live pool snapshots from block S onward with writer `live`.
2. Backfill beside it, into the same ClickHouse database:

   ```sh
   ton-dex-prices-backfill --db <node db> --secondary <new dir> --from B --to <S plus about 1000> \
     --accounts-file <pool list> --dex-prices-clickhouse clickhouse://<user>:<password>@<host>:9000/dex_prices
   ```

   - `--secondary` takes a new directory for every write (RocksDB state and stats files). Backfill only reads the node's database.
   - At block B, backfill snapshots every listed pool (see [Pool list](#pool-list)). History therefore starts with all pools that exist at B.
   - Later blocks add pool changes, written as `backfill`.
   - `-t` and `--parallel` set the reading threads.
   - Overlap with the live indexer is safe. Rows of the same pool and block collapse, and prices use each pool's newest block.
   - Every `--progress-sec` seconds (default 10), it logs listed accounts read, then blocks done, chain time reached, speed, and an ETA.
   - It writes no Postgres or kvrocks data. It exits non-zero if any blocks were dropped.

3. Start the aggregator. It walks history in ranges of up to one day, catches up, and keeps running:

   ```sh
   dex-prices-aggregator -dsn clickhouse://<user>:<password>@<host>:9000/dex_prices -start <B's minute>
   ```

4. Start `ton-index-price-api -clickhouse <dsn> -kvrocks ...`. Route `/api/v3/jetton/prices` and `/api/v3/jetton/price_history` to it at the gateway.

Verify these signals after startup:

- The `live` row of `ingest_head` moves about every 5 s while the indexer is in sync.
- `aggregate_state` follows `ingest_head` about one minute behind.
- `as_of_utime` in `/api/v3/jetton/prices` stays close to the clock.

### Pool list

`--accounts-file` lists raw addresses (`0:HEX`), one per line. Backfill skips listed accounts that are not trusted pools at block B. It finds pools created after block B when they first change.

Collect today's pools from these sources:

- **DeDust v1**: take the `address` fields of `https://api.dedust.io/v2/pools` (user-friendly form) and convert them to raw.
- **kvrocks code-hash index**: read `ton-index:v1:idx:latest_account_states:code_hash:<base64 code hash>:by_account`, a sorted set of raw addresses. The hashes below and in `DexConstants.cpp` are hex; convert them to base64 for the key.
  - **STON.fi v1/v2**: use `stonfi_v1_pool_code_hashes` and `stonfi_v2_pool_code_hashes` from `DexConstants.cpp`.
  - **DeDust v2**: pool code is a library reference cell. Index keys use the hashes of those cells, not the library hashes:
    - `3997A5C1EE8923E93CF3A6A98EA3EF9482C64DD8AEA2BA8365689E14D99C760D` (CPMM v2.0)
    - `5851780D2386E989B9AB956DA7CEC5C69A667B9E525F628DCBA5EB02C1EE0367` (CPMM v2.1)
  - **DeDust v1**: the index also covers DeDust v1 (`dedust_v1_pool_code_hashes`).

### Aggregator flags

| Flag | Default | Use |
|---|---|---|
| `-dsn` | required | ClickHouse DSN of the prices database |
| `-start` | oldest pool snapshot's minute | First minute when the job never ran (unix seconds) |
| `-until` | none | Last minute to compute; turns the current minute off |
| `-max-range` | 1440 | Minutes per statement run |
| `-interval` | 5s | Pause when there is nothing to do or after an error |
| `-once` | false | Exit when ready minutes and the current minute are done |
| `-rewind-from` | none | Recompute from this minute (see [Outage refill](#outage-refill)) |

### Outage refill

`ton-index-clickhouse` logs one ERROR line per dropped masterchain range (`DEX prices writer: dropped mc seqno`) with its reason.

To refill a dropped range:

1. Run `ton-dex-prices-backfill` over that range.
2. Stop the aggregator.
3. Start the aggregator once with the first affected minute (in unix seconds):

   ```sh
   dex-prices-aggregator -dsn <dsn> -rewind-from <first-minute>
   ```

4. Remove `-rewind-from` afterwards, because each startup with it rewinds again.

Rewind behavior:

- The rewind minute can be at most one minute after `done_minute`.
- The aggregator sets `done_minute` to the minute before the rewind. You can therefore retry the same rewind after a stop.
- It deletes prices and anchors from the rewind minute onward.
- It restores the last price before that minute for each affected asset.
- It then recomputes prices up to the live head and continues.
- A rewind to the start of history rebuilds all prices.

### Tests

#### C++ tests

From the repository root, with `build/` configured as in CI:

```sh
cmake --build build --target ton-dex-prices-test ton-dex-prices-backfill ton-index-clickhouse
DEX_PRICES_TEST_DSN=clickhouse://127.0.0.1:9000 ctest --test-dir build/ton-index-worker/ton-dex-prices --output-on-failure
```

Omitting `DEX_PRICES_TEST_DSN` skips the pool cache restart test.

#### Go tests

From `ton-index-price-api`, with a ClickHouse server for the price SQL tests:

```sh
DEX_PRICES_TEST_DSN=clickhouse://127.0.0.1:9000 go test ./aggregate/... ./prices/...
```

- Without a ClickHouse server, the run skips the price SQL tests.
- The tests run the real aggregation and read SQL on synthetic pools with prices worked out by hand.
- A plain `mc_blocks` table stands in for the indexer's view.
- Each test creates and drops its own database.
- Before running `go test ./...`, generate the API docs. Build the `ton-index-price-api` target or run `swag init --parseDependency` from `ton-index-price-api`.

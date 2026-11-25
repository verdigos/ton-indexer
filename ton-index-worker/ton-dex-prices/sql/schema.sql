-- Addresses use uppercase raw 0:HEX, with native TON as '' and pTON masters unchanged.
-- The database name is a parameter of the writer (`dex_prices` below).

CREATE DATABASE IF NOT EXISTS dex_prices;

-- Each changed pool has one trusted snapshot or removal per masterchain block.
CREATE TABLE IF NOT EXISTS dex_prices.pool_snapshots
(
    pool            String CODEC(ZSTD(3)),
    mc_seqno        UInt32 CODEC(Delta, ZSTD(3)),
    block_utime     DateTime CODEC(Delta, ZSTD(3)),
    last_tx_lt      UInt64 CODEC(Delta, ZSTD(3)),
    dex             LowCardinality(String),               -- dedust | stonfi
    dex_version     UInt8,
    pool_type       LowCardinality(String),               -- stonfi: constant_product, stableswap, ... | dedust: volatile, stable
    code_hash       FixedString(44),                      -- base64
    router          String CODEC(ZSTD(3)),                -- STON.fi router, '' otherwise

    removed         Bool,

    asset0          String CODEC(ZSTD(3)),                -- in pool order: jetton master, '' = DeDust native TON
    asset1          String CODEC(ZSTD(3)),
    reserve0        UInt128,                              -- raw units (Coins < 2^120)
    reserve1        UInt128,
    lp_fee          UInt32,                               -- numerators over fee_denominator
    protocol_fee    UInt32,
    ref_fee         UInt32,
    fee_denominator UInt32,
    amp             Nullable(UInt128),                    -- curve-specific parameters
    rate            Nullable(UInt128),
    w0              Nullable(UInt128),
    is_locked       Bool,
    lp_supply       Nullable(UInt128),

    spot            Nullable(Float64),                    -- raw asset1 per raw asset0, mid price without fees
    depth0          Nullable(Float64),                    -- raw asset0 in that lowers spot by 2%
    depth1          Nullable(Float64),                    -- raw asset1 in that lowers the inverse spot by 2%
    math_version    UInt16,                               -- 0 = not priced (curve not supported or removed)

    writer          LowCardinality(String),               -- live | backfill
    ingested_at     DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(math_version)
PARTITION BY toYYYYMM(block_utime)
ORDER BY (pool, mc_seqno);

CREATE TABLE IF NOT EXISTS dex_prices.pool_latest
(
    pool         String,
    version      UInt64,                                  -- mc_seqno << 16 | math_version
    block_utime  DateTime,
    removed      Bool,
    asset0       String,
    asset1       String,
    reserve0     UInt128,
    reserve1     UInt128,
    is_locked    Bool,
    spot         Nullable(Float64),
    math_version UInt16
)
ENGINE = ReplacingMergeTree(version)
ORDER BY pool;

CREATE MATERIALIZED VIEW IF NOT EXISTS dex_prices.pool_latest_mv TO dex_prices.pool_latest
AS SELECT
    pool,
    bitOr(bitShiftLeft(toUInt64(mc_seqno), 16), toUInt64(math_version)) AS version,
    block_utime, removed, asset0, asset1, reserve0, reserve1, is_locked, spot, math_version
FROM dex_prices.pool_snapshots;

-- Each pool's newest state per UTC day, so the pool states before a given minute need no scan of all older snapshots.
CREATE TABLE IF NOT EXISTS dex_prices.pool_daily
(
    pool         String,
    day          Date,
    version      UInt64,                                  -- mc_seqno << 16 | math_version
    block_utime  DateTime,
    removed      Bool,
    reserve0     UInt128,
    reserve1     UInt128,
    is_locked    Bool,
    spot         Nullable(Float64),
    math_version UInt16
)
ENGINE = ReplacingMergeTree(version)
ORDER BY (pool, day)
SETTINGS index_granularity = 1024;

CREATE MATERIALIZED VIEW IF NOT EXISTS dex_prices.pool_daily_mv TO dex_prices.pool_daily
AS SELECT
    pool, toDate(block_utime, 'UTC') AS day,
    bitOr(bitShiftLeft(toUInt64(mc_seqno), 16), toUInt64(math_version)) AS version,
    block_utime, removed, reserve0, reserve1, is_locked, spot, math_version
FROM dex_prices.pool_snapshots;

-- Each pool's newest swap fee, one row per pool.
CREATE TABLE IF NOT EXISTS dex_prices.pool_fees
(
    pool         String,
    version      UInt64,                                  -- mc_seqno << 16 | math_version
    fee          Float64                                  -- (lp_fee + protocol_fee) / fee_denominator, e.g. 0.003
)
ENGINE = ReplacingMergeTree(version)
ORDER BY pool;

CREATE MATERIALIZED VIEW IF NOT EXISTS dex_prices.pool_fees_mv TO dex_prices.pool_fees
AS SELECT
    pool,
    bitOr(bitShiftLeft(toUInt64(mc_seqno), 16), toUInt64(math_version)) AS version,
    (lp_fee + protocol_fee) / fee_denominator AS fee
FROM dex_prices.pool_snapshots
WHERE NOT removed AND fee_denominator > 0;

-- The aggregator's work table, filled by aggregate/sql/pool_records.sql for each range.
CREATE TABLE IF NOT EXISTS dex_prices.pool_records
(
    pool         String,
    minute       DateTime,
    ts           DateTime,
    r0           Float64,
    r1           Float64,
    spot         Nullable(Float64),
    usable       Bool,
    priced_asset String,                                  -- the non-quote asset, '' when both or neither are quotes
    inv          Bool,                                    -- asset0 is the quote: invert spot
    via_usdt     Bool,                                    -- the quote is USDT
    is_anchor    Bool,                                    -- a TON/USDT pool
    ton0         Bool                                     -- asset0 is TON
)
ENGINE = Memory;

-- The live head: the newest block the prices use. Ages count from it, not from the clock. The aggregator moves it forward
-- from mc_blocks, a view over the indexer's masterchain blocks. ton-index-clickhouse --dex-prices-database creates
-- that view, because it names the indexer's database.
CREATE TABLE IF NOT EXISTS dex_prices.ingest_head
(
    writer      LowCardinality(String),
    mc_seqno    UInt32,
    block_utime DateTime,
    updated_at  DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(mc_seqno)
ORDER BY writer;

CREATE TABLE IF NOT EXISTS dex_prices.anchor_1m
(
    minute         DateTime,
    price_raw      Nullable(Float64),                     -- raw USDT per raw TON (TWAP of minute medians)
    pools          UInt16,
    stale          Bool,                                  -- no eligible anchor pool, price_raw is the last good value
    tvl_raw        Nullable(Float64),                     -- both reserves of the anchor pools of the minute, raw TON (NULL when stale)
    computed_at    DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(computed_at)
ORDER BY minute
SETTINGS min_age_to_force_merge_seconds = 600;

-- Prices at the minutes where an asset's pools changed, a row when the price, pools, tvl or state changes. The
-- USDT price is the TON price times the anchor of the same minute. Nothing is written between pool changes.
CREATE TABLE IF NOT EXISTS dex_prices.price_1m
(
    asset           String,                               -- jetton master
    minute          DateTime,
    price_raw       Nullable(Float64),                    -- raw TON per raw asset at the minute's end
    pools           UInt16,
    tvl_quote       Nullable(Float64),                    -- both reserves of the included pools, raw TON units
    newest_utime    DateTime,                             -- newest snapshot among included pools
    stale           Bool,                                 -- the anchor used for USDT pools was stale (last good value)
    reason          LowCardinality(String),               -- empty when priced, else no_eligible_pools | no_anchor
    anchor_raw      Nullable(Float64),                    -- anchor_1m price of the minute (raw USDT per raw TON)
    anchor_stale    Bool,                                 -- that anchor was stale
    computed_at     DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(computed_at)
ORDER BY (asset, minute)
-- Each part costs every history read: parts older than 10 minutes are merged into one.
SETTINGS min_age_to_force_merge_seconds = 600;

-- The newest price row per asset, so current prices need no scan of the history.
CREATE TABLE IF NOT EXISTS dex_prices.price_latest
(
    asset           String,
    version         UInt64,                               -- minute << 32 | computed_at
    minute          DateTime,
    price_raw       Nullable(Float64),
    pools           UInt16,
    tvl_quote       Nullable(Float64),
    newest_utime    DateTime,
    stale           Bool,
    reason          LowCardinality(String),
    anchor_raw      Nullable(Float64),
    anchor_stale    Bool,
    computed_at     DateTime
)
ENGINE = ReplacingMergeTree(version)
ORDER BY asset;

CREATE MATERIALIZED VIEW IF NOT EXISTS dex_prices.price_latest_mv TO dex_prices.price_latest
AS SELECT
    asset,
    bitOr(bitShiftLeft(toUInt64(toUInt32(minute)), 32), toUInt64(toUInt32(computed_at))) AS version,
    minute, price_raw, pools, tvl_quote, newest_utime, stale, reason, anchor_raw, anchor_stale, computed_at
FROM dex_prices.price_1m;

-- Forward job price_1m has written every minute through done_minute. The row with the highest seq counts.
CREATE TABLE IF NOT EXISTS dex_prices.aggregate_state
(
    job         LowCardinality(String),
    done_minute DateTime,
    seq         UInt64,
    updated_at  DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(seq)
ORDER BY job;

-- The current minute, which is not over yet. The aggregator writes it into the *_next copies every few seconds, then
-- swaps the tables.
CREATE TABLE IF NOT EXISTS dex_prices.anchor_now
(
    minute         DateTime,
    price_raw      Nullable(Float64),
    pools          UInt16,
    stale          Bool,
    tvl_raw        Nullable(Float64)
)
ENGINE = Memory;

CREATE TABLE IF NOT EXISTS dex_prices.anchor_next AS dex_prices.anchor_now;

CREATE TABLE IF NOT EXISTS dex_prices.price_now
(
    asset           String,
    minute          DateTime,
    price_raw       Nullable(Float64),
    pools           UInt16,
    tvl_quote       Nullable(Float64),
    newest_utime    DateTime,
    stale           Bool,
    reason          LowCardinality(String),
    anchor_raw      Nullable(Float64),
    anchor_stale    Bool
)
ENGINE = Memory;

CREATE TABLE IF NOT EXISTS dex_prices.price_next AS dex_prices.price_now;

-- The stored rows, then the rows of the current minute (is_final = false).
CREATE OR REPLACE VIEW dex_prices.anchor_all AS
SELECT minute, price_raw, pools, stale, tvl_raw, true AS is_final FROM dex_prices.anchor_1m FINAL
UNION ALL
SELECT minute, price_raw, pools, stale, tvl_raw, false FROM dex_prices.anchor_now
WHERE minute > (SELECT max(minute) FROM dex_prices.anchor_1m);

-- Readers use price_1m, price_latest and price_now.
DROP VIEW IF EXISTS dex_prices.price_all;

-- DEX prices prototype schema (docs/dex-prices/mvp-plan.md, §6 and stage M-1).
-- Assets: '' is TON; jettons are raw master addresses '0:HEX'.

CREATE DATABASE IF NOT EXISTS dex_prices;

CREATE TABLE IF NOT EXISTS dex_prices.dex_pools
(
    pool             String,
    dex              LowCardinality(String),
    pool_type        LowCardinality(String),
    curve            LowCardinality(String),
    asset0           String,
    asset1           String,
    code_hash        String,
    first_mc_seqno   UInt32,
    updated_mc_seqno UInt32
)
ENGINE = ReplacingMergeTree(updated_mc_seqno)
ORDER BY pool;

CREATE TABLE IF NOT EXISTS dex_prices.dex_assets
(
    asset      String,
    symbol     String,
    decimals   UInt8,
    updated_at DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(updated_at)
ORDER BY asset;

-- One row per pool per masterchain block in which the pool changed.
CREATE TABLE IF NOT EXISTS dex_prices.dex_pool_snapshots
(
    pool         String CODEC(ZSTD(3)),
    mc_seqno     UInt32 CODEC(Delta, ZSTD(3)),
    block_utime  DateTime CODEC(Delta, ZSTD(3)),
    last_tx_lt   UInt64 CODEC(Delta, ZSTD(3)),
    dex          LowCardinality(String),
    pool_type    LowCardinality(String),
    asset0       String CODEC(ZSTD(3)),
    asset1       String CODEC(ZSTD(3)),
    reserve0     UInt128,
    reserve1     UInt128,
    spot_price   Float64,          -- raw asset1 per raw asset0
    amp          Float64 DEFAULT 0,
    weight0      Float64 DEFAULT 0,
    rate         Float64 DEFAULT 0,
    lp_fee       Float32,
    protocol_fee Float32,
    tx_count     UInt16,
    process_us   UInt32,           -- listener time from datagram to row, for sizing the real thing
    received_at  DateTime64(3) DEFAULT now64(3)
)
ENGINE = ReplacingMergeTree
PARTITION BY toYYYYMM(block_utime)
ORDER BY (pool, mc_seqno);

CREATE TABLE IF NOT EXISTS dex_prices.dex_pool_latest AS dex_prices.dex_pool_snapshots
ENGINE = ReplacingMergeTree(mc_seqno)
ORDER BY pool;

CREATE MATERIALIZED VIEW IF NOT EXISTS dex_prices.dex_pool_latest_mv TO dex_prices.dex_pool_latest
AS SELECT * FROM dex_prices.dex_pool_snapshots;

CREATE TABLE IF NOT EXISTS dex_prices.dex_pool_candles_1m
(
    pool      String,
    minute    DateTime,
    open      AggregateFunction(argMin, Float64, UInt64),
    high      SimpleAggregateFunction(max, Float64),
    low       SimpleAggregateFunction(min, Float64),
    close     AggregateFunction(argMax, Float64, UInt64),
    snapshots SimpleAggregateFunction(sum, UInt64),
    txs       SimpleAggregateFunction(sum, UInt64)
)
ENGINE = AggregatingMergeTree
PARTITION BY toYYYYMM(minute)
ORDER BY (pool, minute);

CREATE MATERIALIZED VIEW IF NOT EXISTS dex_prices.dex_pool_candles_1m_mv TO dex_prices.dex_pool_candles_1m
AS SELECT
    pool,
    toStartOfMinute(block_utime) AS minute,
    argMinState(spot_price, last_tx_lt) AS open,
    max(spot_price) AS high,
    min(spot_price) AS low,
    argMaxState(spot_price, last_tx_lt) AS close,
    toUInt64(count()) AS snapshots,
    toUInt64(sum(tx_count)) AS txs
FROM dex_prices.dex_pool_snapshots
GROUP BY pool, minute;

-- USDT/TON pools that define the TON price in USDT (ADR-0002).
CREATE TABLE IF NOT EXISTS dex_prices.dex_price_anchors
(
    pool String,
    note String
)
ENGINE = ReplacingMergeTree
ORDER BY pool;

INSERT INTO dex_prices.dex_price_anchors VALUES
    ('0:FC4C9F311160754A99D113877CF583B78E5D16D048819CD4B820168769499D7E', 'STON.fi v1 USDT/TON'),
    ('0:8649CAD97B5C5BC96A960EF748EA6CCFF8601C01616FE995EE6893AE4AA7A6C6', 'STON.fi v2 USDT/TON'),
    ('0:3E5FFCA8DDFCF36C36C9FF46F31562AAB51B9914845AD6C26CBDE649D58A5588', 'DeDust v1 TON/USDT');

-- Current jetton prices in TON and USDT from the latest pool states (docs/dex-prices/mvp-plan.md, §6.2).
--
-- Parameters:
--   {max_age_sec:UInt32}        eligible pools changed within this window (604800 = 7 days)
--   {min_liquidity_usdt:Float64} minimum pool liquidity, both sides, in USDT (1000)
--   {assets:Array(String)}      assets to return; empty = all
--
-- Output prices are per whole token (decimals applied from dex_assets, default 9); *_raw columns are
-- per raw unit. TON is the asset ''.

WITH
    '0:B113A994B5024A16719F69139328EB759596C38A25F59028B146FECDC3621DFE' AS usdt,

    -- raw USDT per nanoTON, liquidity-weighted over the eligible anchor pools
    (
        SELECT sum(if(asset0 = '', spot_price, 1 / spot_price) * toFloat64(if(asset0 = '', reserve0, reserve1)))
             / sum(toFloat64(if(asset0 = '', reserve0, reserve1)))
        FROM dex_prices.dex_pool_latest FINAL
        WHERE pool IN (SELECT pool FROM dex_prices.dex_price_anchors)
          AND ((asset0 = '' AND asset1 = usdt) OR (asset0 = usdt AND asset1 = ''))
          AND reserve0 > 0 AND reserve1 > 0
          AND block_utime >= now() - {max_age_sec:UInt32}
          AND 2 * toFloat64(if(asset0 = '', reserve1, reserve0)) >= {min_liquidity_usdt:Float64} * 1e6
    ) AS ton_usdt,

    {min_liquidity_usdt:Float64} * 1e6 / ton_usdt AS min_liquidity_ton,

    legs AS
    (
        -- each pool seen from both sides: price of `asset` in raw `quote` units, and the quote-side reserve
        SELECT pool, asset0 AS asset, asset1 AS quote, spot_price AS price, toFloat64(reserve1) AS quote_reserve
        FROM dex_prices.dex_pool_latest FINAL
        WHERE reserve0 > 0 AND reserve1 > 0 AND block_utime >= now() - {max_age_sec:UInt32}
        UNION ALL
        SELECT pool, asset1, asset0, 1 / spot_price, toFloat64(reserve0)
        FROM dex_prices.dex_pool_latest FINAL
        WHERE reserve0 > 0 AND reserve1 > 0 AND block_utime >= now() - {max_age_sec:UInt32}
    ),

    hop1 AS
    (
        -- pools against TON or USDT; USDT sides are converted to TON through the anchors
        SELECT asset,
               sum(price_ton * w) / sum(w) AS price_ton,
               sum(w) AS liquidity_ton,
               count() AS pools,
               toUInt8(1) AS hops
        FROM
        (
            SELECT asset,
                   price * if(quote = '', 1, 1 / ton_usdt) AS price_ton,
                   2 * quote_reserve * if(quote = '', 1, 1 / ton_usdt) AS w
            FROM legs
            WHERE quote IN ('', usdt) AND asset NOT IN ('', usdt)
        )
        WHERE w >= min_liquidity_ton
        GROUP BY asset
    ),

    hop2 AS
    (
        -- pools against an asset priced in hop1
        SELECT l.asset AS asset,
               sum(l.price * h.price_ton * 2 * l.quote_reserve * h.price_ton) / sum(2 * l.quote_reserve * h.price_ton) AS price_ton,
               sum(2 * l.quote_reserve * h.price_ton) AS liquidity_ton,
               count() AS pools,
               toUInt8(2) AS hops
        FROM legs AS l
        INNER JOIN hop1 AS h ON l.quote = h.asset
        WHERE l.asset NOT IN ('', usdt)
          AND l.asset NOT IN (SELECT asset FROM hop1)
          AND 2 * l.quote_reserve * h.price_ton >= min_liquidity_ton
        GROUP BY l.asset
    ),

    priced AS
    (
        SELECT asset, price_ton, liquidity_ton, pools, hops FROM hop1
        UNION ALL
        SELECT asset, price_ton, liquidity_ton, pools, hops FROM hop2
        UNION ALL
        SELECT '', 1, 0, 0, toUInt8(0)
        UNION ALL
        SELECT usdt, 1 / ton_usdt, 0, 0, toUInt8(0)
    )

SELECT
    p.asset AS asset,
    multiIf(p.asset = '', 'TON', p.asset = usdt, 'USDT', a.symbol) AS symbol,
    multiIf(p.asset = '', 9, p.asset = usdt, 6, a.asset = '', 9, a.decimals) AS decimals,
    p.price_ton * pow(10, decimals - 9) AS price_ton,
    p.price_ton * ton_usdt * pow(10, decimals - 6) AS price_usdt,
    p.price_ton AS price_ton_raw,
    p.price_ton * ton_usdt AS price_usdt_raw,
    p.liquidity_ton / 1e9 AS liquidity_ton,
    p.pools AS pools,
    p.hops AS hops
FROM priced AS p
LEFT JOIN (SELECT asset, symbol, decimals FROM dex_prices.dex_assets FINAL) AS a ON a.asset = p.asset
WHERE empty({assets:Array(String)}) OR has({assets:Array(String)}, p.asset)
ORDER BY liquidity_ton DESC

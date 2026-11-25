-- Loads each pool's state just before the range and its last state per minute in the range, keeping removed and
-- unusable states to end earlier ones. Reads only USDT pools and pools of assets changed in the range, or all pools
-- until anchor_1m has its first price.
INSERT INTO pool_records (pool, minute, ts, r0, r1, spot, usable, priced_asset, inv, via_usdt, is_anchor, ton0)
WITH
    60 AS minute_seconds,
    {records_from:DateTime} AS records_from,
    {records_to:DateTime} AS records_to,
    -- Clamp to live head so the current minute excludes blocks past it.
    least(toDateTime(toInt64(records_to) + minute_seconds), toDateTime(toInt64({head_utime:DateTime}) + 1)) AS records_end,
    {ton_aliases:Array(String)} AS ton_aliases,
    {usdt:String} AS usdt,
    {max_pool_fee:Float64} AS max_pool_fee,
    (SELECT count() FROM (SELECT 1 FROM anchor_1m WHERE price_raw IS NOT NULL LIMIT 1)) = 0 AS all_pools,
    -- Non-quote assets changed in the range (excluding TON and USDT avoids loading every pool).
    keep_assets AS
    (
        SELECT a
        FROM
        (
            SELECT arrayJoin([asset0, asset1]) AS a FROM pool_snapshots
            WHERE block_utime >= records_from AND block_utime < records_end
        )
        WHERE NOT has(arrayPushBack(ton_aliases, usdt), a)
    ),
    changed_pools AS (SELECT pool, asset0, asset1 FROM pool_latest FINAL WHERE block_utime >= records_from),
    (removed, math_version, locked, r0, r1, spot) ->
        NOT removed AND math_version > 0 AND NOT locked AND r0 > 0 AND r1 > 0 AND ifNull(spot, 0) > 0 AS pool_usable
-- Picks the non-quote asset to price (when paired with TON or USDT) and marks USDT and TON/USDT pools.
SELECT pool, minute, ts, r0, r1, spot, usable AND fee <= max_pool_fee,
       multiIf(q1 AND NOT q0, asset0, q0 AND NOT q1, asset1, ''), q0, if(q0, c0, c1) = usdt,
       (c0 = 'TON' AND c1 = usdt) OR (c0 = usdt AND c1 = 'TON'), c0 = 'TON'
FROM
(
    SELECT *, if(has(ton_aliases, asset0), 'TON', asset0) AS c0, if(has(ton_aliases, asset1), 'TON', asset1) AS c1,
           c0 IN ('TON', usdt) AS q0, c1 IN ('TON', usdt) AS q1
    FROM
    (
        -- Pools that did not change from records_from on: their newest state.
        SELECT pool, toDateTime(toInt64(records_from) - minute_seconds) AS minute, block_utime AS ts,
               asset0, asset1, toFloat64(reserve0) AS r0, toFloat64(reserve1) AS r1, spot,
               pool_usable(removed, math_version, is_locked, reserve0, reserve1, spot) AS usable
        FROM pool_latest FINAL
        WHERE block_utime < records_from
          AND (all_pools OR asset0 = usdt OR asset1 = usdt OR asset0 IN keep_assets OR asset1 IN keep_assets)
        UNION ALL
        -- Pools that changed: their state just before records_from. It is the newest of the daily states before that day
        -- and the snapshots of that day before records_from.
        SELECT pool, toDateTime(toInt64(records_from) - minute_seconds), state.block_utime, l.asset0, l.asset1,
               toFloat64(state.reserve0), toFloat64(state.reserve1), state.spot,
               pool_usable(state.removed, state.math_version, state.is_locked, state.reserve0, state.reserve1, state.spot)
        FROM
        (
            SELECT pool,
                   argMax(CAST((block_utime, reserve0, reserve1, spot, removed, math_version, is_locked)
                               AS Tuple(block_utime DateTime, reserve0 UInt128, reserve1 UInt128, spot Nullable(Float64),
                                        removed Bool, math_version UInt16, is_locked Bool)),
                          version) AS state
            FROM
            (
                SELECT pool, version, block_utime, reserve0, reserve1, spot, removed, math_version, is_locked
                FROM pool_daily
                WHERE day < toDate(records_from, 'UTC') AND pool IN (SELECT pool FROM changed_pools)
                UNION ALL
                SELECT pool, bitOr(bitShiftLeft(toUInt64(mc_seqno), 16), toUInt64(math_version)),
                       block_utime, reserve0, reserve1, spot, removed, math_version, is_locked
                FROM pool_snapshots
                WHERE block_utime >= toStartOfDay(records_from, 'UTC') AND block_utime < records_from
                  AND pool IN (SELECT pool FROM changed_pools)
            )
            GROUP BY pool
        ) AS b
        INNER JOIN changed_pools AS l USING (pool)
        UNION ALL
        -- The last state of each pool in each minute of the range. A block can have rows of several math versions:
        -- the highest one wins.
        SELECT pool, minute, state.block_utime, state.asset0, state.asset1, toFloat64(state.reserve0),
               toFloat64(state.reserve1), state.spot,
               pool_usable(state.removed, state.math_version, state.is_locked, state.reserve0, state.reserve1, state.spot)
        FROM
        (
            SELECT pool, toStartOfMinute(block_utime) AS minute,
                   argMax(CAST((block_utime, asset0, asset1, reserve0, reserve1, spot, removed, math_version, is_locked)
                               AS Tuple(block_utime DateTime, asset0 String, asset1 String, reserve0 UInt128,
                                        reserve1 UInt128, spot Nullable(Float64), removed Bool, math_version UInt16,
                                        is_locked Bool)),
                          (mc_seqno, math_version)) AS state
            FROM pool_snapshots
            WHERE block_utime >= records_from AND block_utime < records_end
            GROUP BY pool, minute
        )
    ) AS states
    -- A pool with a fee above max_pool_fee can sit that far off the market without arbitrage, so it does not count.
    LEFT JOIN (SELECT pool, fee FROM pool_fees FINAL) AS fees USING (pool)
)

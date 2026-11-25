-- Writes the TON/USDT anchor per minute into anchor_out: weighted median of TON/USDT pools with at least the TON
-- floor, averaged over anchor_twap minutes. Minutes with no such pool carry forward the last anchor as stale with
-- NULL tvl_raw. Otherwise tvl_raw is both reserves in raw TON, with USDT valued at the minute's median.
INSERT INTO {anchor_out:Identifier} (minute, price_raw, pools, stale, tvl_raw)
WITH
    60 AS minute_seconds,
    1e9 AS ton_raw,
    {from_minute:DateTime} AS f,
    {to_minute:DateTime} AS t,
    {anchor_floor_ton:Float64} * ton_raw AS floor_raw,
    {anchor_twap:UInt16} AS twap,
    toDateTime(toInt64(f) - (toInt64(twap) - 1) * minute_seconds) AS f0,
    anchor_pools AS (SELECT DISTINCT pool FROM pool_records WHERE is_anchor),
    minutes AS
    (
        SELECT toDateTime(toInt64(f0) + number * minute_seconds) AS m
        FROM numbers(toUInt64(intDiv(toInt64(t) - toInt64(f0), minute_seconds) + 1))
    ),
    pool_states AS
    (
        SELECT g.m AS m, g.pool AS pool, p.usable AS usable,
               p.r0 AS r0, p.r1 AS r1, p.spot AS spot, p.ton0 AS ton0, p.is_anchor AS is_anchor
        FROM (SELECT m, pool FROM minutes CROSS JOIN anchor_pools) AS g
        ASOF INNER JOIN pool_records AS p ON g.pool = p.pool AND g.m >= p.minute
    ),
    pool_eligible AS
    (
        SELECT m, pool,
               if(ton0, assumeNotNull(spot), 1 / assumeNotNull(spot)) AS pool_price,   -- raw USDT per raw TON
               if(ton0, r0, r1) AS ton_reserve,
               if(ton0, r1, r0) AS usdt_reserve
        FROM pool_states
        WHERE is_anchor AND usable AND ton_reserve >= floor_raw
    ),
    -- Weight is each pool's smaller side in raw TON.
    minute_medians AS
    (
        SELECT m, count() AS n,
               arraySort(z -> (z.price, z.pool),
                         groupArray(CAST((pool_price, least(ton_reserve, usdt_reserve / pool_price), pool)
                                         AS Tuple(price Float64, weight Float64, pool String)))) AS sorted,
               arrayCumSum(arrayMap(z -> z.weight, sorted)) AS cum,
               sorted[arrayFirstIndex(c -> c >= cum[-1] / 2, cum)].price AS median,
               sum(ton_reserve) + sum(usdt_reserve) / median AS tvl
        FROM pool_eligible
        GROUP BY m
    ),
    -- Sorting before averaging makes the float sum deterministic across runs.
    -- Identical medians bypass arrayAvg because summing equal floats can drift in the last bit.
    anchor_twaps AS
    (
        SELECT m, n, median_raw, tvl_raw,
               groupArray(median_raw) OVER (ORDER BY m ROWS BETWEEN twap - 1 PRECEDING AND CURRENT ROW) AS window_medians,
               if(n > 0, if(arrayMin(window_medians) = arrayMax(window_medians), window_medians[1], arrayAvg(arraySort(window_medians))), NULL) AS live
        FROM
        (
            SELECT mi.m AS m, minute_medians.n AS n, if(minute_medians.n > 0, minute_medians.median, NULL) AS median_raw,
                   if(minute_medians.n > 0, minute_medians.tvl, NULL) AS tvl_raw
            FROM minutes AS mi LEFT JOIN minute_medians ON minute_medians.m = mi.m
        )
    ),
    anchor_values AS
    (
        SELECT m, n, live, tvl_raw,
               anyLast(live) OVER (ORDER BY m ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) AS carry
        FROM anchor_twaps
        WHERE m >= f
    )
SELECT
    m,
    if(n > 0, live,
       coalesce(carry, (SELECT argMax(price_raw, minute) FROM anchor_1m FINAL WHERE minute < f AND price_raw IS NOT NULL))),
    toUInt16(n),
    n = 0,
    if(n > 0, tvl_raw, NULL)
FROM anchor_values
ORDER BY m

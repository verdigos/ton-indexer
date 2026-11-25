-- Writes changed jetton TON prices for the range into price_out (requires anchor.sql first), plus every asset at the
-- anchor's first priced minute. For each minute where an asset's pools changed: drops pools below min_pool_liquidity_ton,
-- takes the weighted median of the rest as the reference, drops pools outside the band around it, and takes the weighted
-- median (weighted mean under 3 pools). Emits a row only when it differs from the previous row, and skips rows before an asset's first price.
INSERT INTO {price_out:Identifier} (asset, minute, price_raw, pools, tvl_quote, newest_utime, stale, reason,
                                    anchor_raw, anchor_stale)
WITH
    60 AS minute_seconds,
    1e9 AS ton_raw,
    {from_minute:DateTime} AS f,
    {to_minute:DateTime} AS t,
    {band:Float64} AS band,
    {min_pool_liquidity_ton:Float64} * ton_raw AS floor_raw,
    {current:UInt8} AS current,
    -- Current minute reads its anchor from anchor_next.
    anchor AS
    (
        SELECT minute AS m, price_raw AS anchor_price, stale AS anchor_stale
        FROM
        (
            SELECT minute, price_raw, stale FROM anchor_1m FINAL
            UNION ALL
            SELECT minute, price_raw, stale FROM anchor_next WHERE current = 1
        )
        WHERE minute >= f AND minute <= t
    ),
    -- The minute the anchor gets its first price, when it is in this range. NULL otherwise.
    (SELECT if((SELECT count() FROM anchor_1m WHERE minute < f AND price_raw IS NOT NULL) = 0, minOrNull(m), NULL)
     FROM anchor WHERE anchor_price IS NOT NULL) AS anchor_start,
    minutes_to_price AS
    (
        SELECT DISTINCT asset, m
        FROM
        (
            SELECT priced_asset AS asset, minute AS m FROM pool_records WHERE minute >= f
            UNION ALL
            SELECT priced_asset, assumeNotNull(anchor_start) FROM pool_records WHERE anchor_start IS NOT NULL
        )
        WHERE asset != ''
    ),
    asset_pools AS (SELECT DISTINCT priced_asset AS asset, pool FROM pool_records WHERE priced_asset != ''),
    pool_states AS
    (
        SELECT asset, m, c.pool AS pool, ts, l.usable AS eligible, inv, via_usdt, r0, r1, spot
        FROM (SELECT asset, m, pool FROM minutes_to_price INNER JOIN asset_pools USING (asset)) AS c
        ASOF INNER JOIN pool_records AS l ON c.pool = l.pool AND c.m >= l.minute
    ),
    -- inv means asset0 is the quote, so spot is inverted.
    pool_prices AS
    (
        SELECT asset, m, pool, ts, via_usdt, anchor_price, anchor_stale, eligible,
               if(via_usdt, 1 / ifNull(anchor_price, 0), 1.) AS quote_to_ton,   -- raw TON per raw quote
               if(inv, 1 / ifNull(spot, 0), ifNull(spot, 0)) AS price_in_quote, -- raw quote per raw asset
               if(inv, r1, r0) AS asset_reserve,
               if(inv, r0, r1) AS quote_reserve,
               eligible AND (NOT via_usdt OR ifNull(anchor_price, 0) > 0) AS usable
        FROM pool_states LEFT JOIN anchor USING (m)
    ),
    candidates AS
    (
        SELECT asset, m,
               countIf(eligible) AS eligible_count,
               countIf(eligible AND via_usdt) AS eligible_usdt_count,
               any(anchor_price) AS anchor_price,
               max(anchor_stale) AS stale_anchor,
               groupArrayIf(CAST((price_in_quote * quote_to_ton, quote_reserve * quote_to_ton, asset_reserve,
                                  via_usdt, ts, pool)
                                 AS Tuple(price Float64, quote_reserve_ton Float64, asset_reserve Float64,
                                          via_usdt Nullable(UInt8), snapshot_time DateTime, pool String)), usable) AS candidate_pools
        FROM pool_prices
        GROUP BY asset, m
    ),
    -- Pools whose smaller side at their own price holds the floor vote: the reference is their weighted median, so no
    -- single pool sets it. A kept pool's weight is its smaller side valued in TON at the reference price.
    computed AS
    (
        SELECT asset, m, eligible_count, eligible_usdt_count, anchor_price, stale_anchor,
               arraySort(e -> (e.price, e.pool),
                         arrayFilter(e -> least(e.quote_reserve_ton, e.asset_reserve * e.price) >= floor_raw,
                                     candidate_pools)) AS voters,
               arrayCumSum(arrayMap(e -> least(e.quote_reserve_ton, e.asset_reserve * e.price), voters)) AS voter_cum,
               voters[arrayFirstIndex(c -> c >= voter_cum[-1] / 2, voter_cum)].price AS reference_price,
               arrayFilter(z -> abs(z.price / reference_price - 1) <= band,
                           arrayMap(e -> CAST((e.price, least(e.quote_reserve_ton, e.asset_reserve * reference_price),
                                               e.via_usdt, e.snapshot_time, e.pool, e.quote_reserve_ton, e.asset_reserve)
                                              AS Tuple(price Float64, weight Float64,
                                                       via_usdt Nullable(UInt8), snapshot_time DateTime, pool String,
                                                       quote_reserve_ton Float64, asset_reserve Float64)), voters)) AS kept,
               length(kept) AS kept_count,
               arraySort(z -> (z.price, z.pool), kept) AS sorted,
               arrayCumSum(arrayMap(z -> z.weight, sorted)) AS cum,
               arraySum(arrayMap(z -> z.weight, sorted)) AS total_weight,
               if(kept_count >= 3,
                  sorted[arrayFirstIndex(c -> c >= total_weight / 2, cum)].price,  -- weighted median
                  arraySum(arrayMap(z -> z.price * z.weight, sorted)) / total_weight) AS price,
               -- tvl is the value of both reserves of the kept pools, the asset side at the price.
               arraySum(arrayMap(z -> z.quote_reserve_ton, kept)) + arraySum(arrayMap(z -> z.asset_reserve, kept)) * price AS tvl
        FROM candidates
    ),
    new_rows AS
    (
        SELECT asset, m,
               if(kept_count = 0, NULL, price) AS price_raw,
               toUInt16(kept_count) AS pools,
               if(kept_count = 0, NULL, tvl) AS tvl_quote,
               if(kept_count = 0, toDateTime(0), arrayMax(z -> z.snapshot_time, kept)) AS newest_utime,
               -- stale: a USDT pool was used with an anchor that is not current.
               stale_anchor AND arrayExists(z -> z.via_usdt, kept) AS stale,
               multiIf(kept_count > 0, '',
                       eligible_count = 0, 'no_eligible_pools',
                       eligible_count = eligible_usdt_count AND ifNull(anchor_price, 0) <= 0, 'no_anchor',
                       'no_eligible_pools') AS reason,
               if(kept_count = 0, NULL, anchor_price) AS anchor_raw,
               kept_count > 0 AND stale_anchor AS anchor_stale
        FROM computed
    ),
    -- The last stored row of each asset before the range, to compare the first new row with.
    previous_rows AS
    (
        SELECT asset, price_raw, pools, tvl_quote, newest_utime, stale, toString(reason) AS reason, anchor_raw,
               anchor_stale
        FROM price_latest FINAL
        WHERE minute < f AND asset IN (SELECT asset FROM minutes_to_price)
    ),
    all_rows AS
    (
        SELECT asset, m, toUInt8(1) AS is_new, price_raw, pools, tvl_quote, newest_utime, stale,
               toString(reason) AS reason, anchor_raw, anchor_stale
        FROM new_rows
        UNION ALL
        SELECT asset, toDateTime(toInt64(f) - minute_seconds), toUInt8(0), price_raw, pools, tvl_quote,
               newest_utime, stale, reason, anchor_raw, anchor_stale
        FROM previous_rows
    ),
    compared AS
    (
        SELECT *,
               (ifNull(price_raw, -1.), pools, ifNull(tvl_quote, -1.), newest_utime, stale, reason,
                ifNull(anchor_raw, -1.), anchor_stale) AS row_value,
               lagInFrame(toUInt8(1), 1, toUInt8(0)) OVER w AS has_previous,
               lagInFrame(row_value) OVER w AS previous_value,
               -- Rows so far that count as history: a stored row, or a new row with a price.
               countIf(is_new = 0 OR price_raw IS NOT NULL) OVER (PARTITION BY asset ORDER BY m
                   ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS history_rows
        FROM all_rows
        WINDOW w AS (PARTITION BY asset ORDER BY m ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)
    )
SELECT asset, m, price_raw, pools, tvl_quote, newest_utime, stale, reason, anchor_raw, anchor_stale
FROM compared
WHERE is_new = 1 AND history_rows > 0 AND (has_previous = 0 OR row_value != previous_value)
ORDER BY asset, m

-- Returns one asset's points: the last row per step in the range plus the row in force at the range start.
-- The store adds the current minute and sorts them. Wrapping columns in a tuple prevents argMax from skipping
-- rows with a NULL price.
WITH
    {asset:String} AS x,
    toDateTime({from_minute:UInt32}) AS f,
    toDateTime({to_minute:UInt32}) AS t,
    {step:UInt32} AS step
SELECT x, 'TON', toUInt32(r.1), r.2, r.3, r.4, toUInt32(r.5), r.6, r.7, true, r.8, r.9
FROM
(
    SELECT argMax(tuple(minute, price_raw, pools, tvl_quote, newest_utime, stale, toString(reason), anchor_raw,
                        anchor_stale), minute) AS r
    FROM price_1m
    WHERE asset = x AND minute >= f AND minute <= t
    GROUP BY intDiv(toUInt32(minute), step)
    UNION ALL
    SELECT tuple(minute, price_raw, pools, tvl_quote, newest_utime, stale, toString(reason), anchor_raw, anchor_stale)
    FROM (SELECT * FROM price_1m WHERE asset = x AND minute < f ORDER BY minute DESC LIMIT 1)
)

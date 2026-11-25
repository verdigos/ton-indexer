-- Returns the anchor points: TON in USDT, or USDT in TON when inverse is true. Each point is the last anchor with a price
-- in its step, with the tvl of its anchor pools in raw quote units.
WITH
    toDateTime({from_minute:UInt32}) AS f,
    toDateTime({to_minute:UInt32}) AS t,
    {step:UInt32} AS step
SELECT {asset:String}, {quote:String}, toUInt32(r.1), if({inverse:Bool}, 1 / r.2, r.2), r.3,
       if({inverse:Bool}, r.6, r.6 * r.2), toUInt32(r.1), r.4, '', r.5, CAST(NULL AS Nullable(Float64)), false
FROM
(
    SELECT argMax(tuple(minute, price_raw, pools, stale, is_final, tvl_raw), minute) AS r
    FROM anchor_all
    WHERE minute >= f AND minute <= t AND price_raw > 0
    GROUP BY intDiv(toUInt32(minute), step)
)
ORDER BY r.1

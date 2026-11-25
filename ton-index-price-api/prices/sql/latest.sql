-- Returns each asset's newest TON row (price_latest or price_now if newer), plus the newest anchor as TON in USDT
-- with tvl_raw in raw TON. The store derives the USDT rows and converts the anchor tvl to raw USDT.
WITH (SELECT argMax(done_minute, seq) FROM aggregate_state WHERE job = 'price_1m') AS final_minute
SELECT asset, 'TON', toUInt32(minute), price_raw, pools, tvl_quote, toUInt32(newest_utime), stale,
       toString(reason), is_final, anchor_raw, anchor_stale
FROM
(
    SELECT asset, minute, price_raw, pools, tvl_quote, newest_utime, stale, reason, true AS is_final, anchor_raw,
           anchor_stale
    FROM price_latest FINAL
    UNION ALL
    SELECT asset, minute, price_raw, pools, tvl_quote, newest_utime, stale, reason, false, anchor_raw, anchor_stale
    FROM price_now
    WHERE minute > final_minute
)
ORDER BY minute DESC, is_final DESC
LIMIT 1 BY asset
UNION ALL
SELECT 'TON', 'USDT', toUInt32(minute), price_raw, pools, tvl_raw, toUInt32(minute), stale, '',
       is_final, NULL, false
FROM anchor_all
WHERE minute >= final_minute
ORDER BY minute DESC
LIMIT 1

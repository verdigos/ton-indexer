-- Restores each affected asset's last row before from_minute, before deleting newer rows.
INSERT INTO price_latest (asset, version, minute, price_raw, pools, tvl_quote, newest_utime, stale,
                          reason, anchor_raw, anchor_stale, computed_at)
SELECT asset, bitOr(bitShiftLeft(toUInt64(toUInt32(minute)), 32), toUInt64(toUInt32(computed_at))),
       minute, price_raw, pools, tvl_quote, newest_utime, stale, reason, anchor_raw, anchor_stale, computed_at
FROM price_1m
WHERE minute < {from_minute:DateTime} AND asset IN (SELECT asset FROM price_1m WHERE minute >= {from_minute:DateTime})
ORDER BY asset, minute DESC, computed_at DESC
LIMIT 1 BY asset

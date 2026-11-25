-- Returns ready_minute (last minute the live head has passed by 60 s), done_minute, and head_utime.
WITH
    60 AS head_margin,
    (SELECT if(count() = 0, NULL, max(block_utime)) FROM ingest_head FINAL WHERE writer = 'live') AS head_utime
SELECT
    if(head_utime IS NULL, NULL, toStartOfMinute(toDateTime(toInt64(assumeNotNull(head_utime)) - head_margin))) AS ready_minute,
    (SELECT if(count() = 0, NULL, argMax(done_minute, seq)) FROM aggregate_state WHERE job = 'price_1m') AS done_minute,
    head_utime

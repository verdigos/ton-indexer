-- Advances the live head to the last contiguous block in mc_blocks before any gap, since a missing block may
-- still be in flight. Each run scans up to 100,000 blocks from the saved head, or the newest 100,000 blocks
-- on the first run.
INSERT INTO ingest_head (writer, mc_seqno, block_utime)
WITH
    (SELECT max(mc_seqno) FROM ingest_head WHERE writer = 'live') AS saved,
    if(saved > 0, saved, (SELECT mc_seqno FROM mc_blocks ORDER BY mc_seqno DESC LIMIT 1) - 100000) AS scan_from,
    (SELECT groupArray((mc_seqno, block_utime)) FROM (
        SELECT mc_seqno, any(block_utime) AS block_utime FROM mc_blocks
        WHERE mc_seqno >= scan_from AND mc_seqno <= scan_from + 100000
        GROUP BY mc_seqno
        ORDER BY mc_seqno)) AS run,
    arrayFirstIndex((b, i) -> b.1 != run[1].1 + i - 1, run, arrayEnumerate(run)) AS gap,
    if(gap = 0, run[-1], run[gap - 1]) AS head
SELECT 'live', toUInt32(head.1), toDateTime(head.2)
WHERE length(run) > 0 AND toUInt32(head.1) > saved

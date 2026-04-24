WITH context AS (
    SELECT bm_and(
               (SELECT bm_or_agg(bm) FROM li_bitmap_returnflag WHERE value IN ('R', 'N')),
               (SELECT bm FROM li_bitmap_linestatus WHERE value = 'O')
           ) AS bm
),
counts AS (
    SELECT
        value AS l_shipmode,
        bm_count_and(bm, (SELECT bm FROM context)) AS active_count
    FROM li_bitmap_shipmode
)
SELECT l_shipmode
FROM counts
WHERE active_count > 0
ORDER BY 1

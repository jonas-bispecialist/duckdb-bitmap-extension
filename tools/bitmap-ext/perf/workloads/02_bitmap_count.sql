WITH context AS (
    SELECT bm_and(
               (SELECT bm_or_agg(bm) FROM li_bitmap_returnflag WHERE value IN ('R', 'N')),
               (SELECT bm FROM li_bitmap_linestatus WHERE value = 'O')
           ) AS bm
)
SELECT bm_count_and(
    (SELECT bm FROM context),
    (SELECT bm_or_agg(bm) FROM li_bitmap_shipmode WHERE value IN ('AIR', 'RAIL'))
) AS active_row_count

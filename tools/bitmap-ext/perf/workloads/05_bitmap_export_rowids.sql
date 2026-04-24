WITH context AS (
    SELECT bm_and(
               bm_and(
                   (SELECT bm_or_agg(bm) FROM li_bitmap_returnflag WHERE value IN ('R', 'N')),
                   (SELECT bm FROM li_bitmap_linestatus WHERE value = 'O')
               ),
               (SELECT bm_or_agg(bm) FROM li_bitmap_shipmode WHERE value IN ('AIR', 'RAIL'))
           ) AS bm
)
SELECT COUNT(*) AS exported_row_ids
FROM (
    SELECT unnest(bm_to_rows((SELECT bm FROM context))) AS rid
) rows

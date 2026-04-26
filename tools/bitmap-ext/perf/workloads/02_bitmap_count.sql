WITH field_bitmaps AS (
    SELECT bm_or_agg(bm) AS bm
    FROM li_bitmap_returnflag
    WHERE value IN ('R', 'N')
    UNION ALL
    SELECT bm
    FROM li_bitmap_linestatus
    WHERE value = 'O'
    UNION ALL
    SELECT bm_or_agg(bm) AS bm
    FROM li_bitmap_shipmode
    WHERE value IN ('AIR', 'RAIL')
)
SELECT bm_count_and_agg(bm) AS active_row_count
FROM field_bitmaps

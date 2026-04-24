WITH context AS (
    SELECT bm_or_agg(bm) AS bm
    FROM li_bitmap_shipmode
    WHERE value IN ('AIR', 'RAIL', 'TRUCK')
)
SELECT bm_count((SELECT bm FROM context)) AS active_row_count

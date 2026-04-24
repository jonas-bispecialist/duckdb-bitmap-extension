WITH context AS (
    SELECT bm_or(
               bm_or(
                   (SELECT bm FROM li_bitmap_shipmode WHERE value = 'AIR'),
                   (SELECT bm FROM li_bitmap_shipmode WHERE value = 'RAIL')
               ),
               (SELECT bm FROM li_bitmap_shipmode WHERE value = 'TRUCK')
           ) AS bm
)
SELECT bm_count((SELECT bm FROM context)) AS active_row_count

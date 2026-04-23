WITH context AS (
    SELECT bm_and(
               bm_and(
                   bm_or(
                       (SELECT bm FROM li_bitmap_returnflag WHERE value = 'R'),
                       (SELECT bm FROM li_bitmap_returnflag WHERE value = 'N')
                   ),
                   (SELECT bm FROM li_bitmap_linestatus WHERE value = 'O')
               ),
               bm_or(
                   (SELECT bm FROM li_bitmap_shipmode WHERE value = 'AIR'),
                   (SELECT bm FROM li_bitmap_shipmode WHERE value = 'RAIL')
               )
           ) AS bm
)
SELECT bm_count((SELECT bm FROM context)) AS active_row_count
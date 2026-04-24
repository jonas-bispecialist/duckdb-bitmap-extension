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
),
selected_rows AS (
    SELECT unnest(bm_to_rows((SELECT bm FROM context))) AS rid
)
SELECT
    li.rid,
    li.l_returnflag,
    li.l_linestatus,
    li.l_shipmode,
    li.l_shipdate
FROM selected_rows
JOIN li USING (rid)
ORDER BY li.rid
LIMIT 10000

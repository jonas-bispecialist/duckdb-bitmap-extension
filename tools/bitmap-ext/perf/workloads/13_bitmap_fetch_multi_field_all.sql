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
),
context AS (
    SELECT bm_and_agg(bm) AS bm
    FROM field_bitmaps
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

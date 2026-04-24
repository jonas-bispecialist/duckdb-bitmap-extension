WITH context AS (
    SELECT bm_or_agg(bm) AS bm
    FROM li_bitmap_shipmode
    WHERE value IN ('AIR', 'RAIL', 'TRUCK')
),
selected_rows AS (
    SELECT unnest(bm_to_rows((SELECT bm FROM context), 10000::UBIGINT, -1::BIGINT)) AS rid
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

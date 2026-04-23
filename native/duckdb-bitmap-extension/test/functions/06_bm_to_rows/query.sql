WITH input AS (
    SELECT from_hex('4650424D010100000C000000010000000500000009000000') AS bitmap
)
SELECT
    bm_to_rows(bitmap) AS rows_list,
    (
        SELECT string_agg(CAST(x AS VARCHAR), ',' ORDER BY x)
        FROM (
            SELECT unnest(bm_to_rows(bitmap)) AS x
            FROM input
        ) rows
    ) AS unnested_csv,
    bm_to_rows(NULL::BLOB) IS NULL AS null_list
FROM input
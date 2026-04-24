WITH combined AS (
    SELECT bm_or_agg(bm) AS bm
    FROM (
        SELECT bm FROM postings
        UNION ALL
        SELECT NULL::BLOB AS bm
    ) inputs
)
SELECT
    bm_count(bm) AS cardinality,
    (
        SELECT string_agg(CAST(x AS VARCHAR), ',' ORDER BY x)
        FROM (
            SELECT unnest(bm_to_rows(bm)) AS x
            FROM combined
        ) rows
    ) AS rows_csv
FROM combined

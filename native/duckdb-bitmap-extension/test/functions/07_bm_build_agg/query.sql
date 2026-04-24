WITH built AS (
    SELECT bm_build_agg(id) AS bm
    FROM build_input
)
SELECT
    bm_count(bm) AS cardinality,
    bm_contains(bm, 65536) AS contains_65536,
    bm_contains(bm, 4294967295) AS contains_max_u32,
    (
        SELECT string_agg(CAST(x AS VARCHAR), ',' ORDER BY x)
        FROM (
            SELECT unnest(bm_to_rows(bm)) AS x
            FROM built
        ) rows
    ) AS rows_csv
FROM built

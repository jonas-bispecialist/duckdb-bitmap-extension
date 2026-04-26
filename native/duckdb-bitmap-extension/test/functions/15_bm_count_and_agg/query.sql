SELECT
    bm_count_and_agg(bm) AS intersection_count,
    (
        SELECT bm_count_and_agg(bm)
        FROM (
            SELECT bm FROM count_and_agg_postings WHERE name IN ('a', 'b')
            UNION ALL
            SELECT NULL::BLOB AS bm
        ) two_inputs
    ) AS ignores_nulls_count,
    (
        SELECT bm_count_and_agg(NULL::BLOB)
    ) AS all_null_count
FROM count_and_agg_postings

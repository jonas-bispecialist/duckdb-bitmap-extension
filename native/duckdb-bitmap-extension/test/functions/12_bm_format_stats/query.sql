WITH input AS (
    SELECT bm_build([1::UBIGINT, 5::UBIGINT, 9::UBIGINT]) AS bm
)
SELECT
    bm_format(bm) AS format_name,
    bm_stats(bm) AS stats,
    bm_format(NULL::BLOB) IS NULL AS null_format,
    bm_stats(NULL::BLOB) IS NULL AS null_stats
FROM input

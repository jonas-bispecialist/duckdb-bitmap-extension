WITH bitmaps AS (
    SELECT
        bm_build([1::UBIGINT, 3::UBIGINT, 5::UBIGINT, 7::UBIGINT]) AS a,
        bm_build([3::UBIGINT, 4::UBIGINT, 5::UBIGINT, 8::UBIGINT]) AS b
)
SELECT
    bm_count_and(a, b) AS and_count,
    bm_count_or(a, b) AS or_count,
    bm_count_andnot(a, b) AS andnot_count,
    bm_count_and(NULL::BLOB, b) IS NULL AS null_left
FROM bitmaps

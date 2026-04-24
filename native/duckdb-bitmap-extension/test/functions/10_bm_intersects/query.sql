WITH bitmaps AS (
    SELECT
        bm_build([1::UBIGINT, 3::UBIGINT, 5::UBIGINT]) AS a,
        bm_build([5::UBIGINT, 8::UBIGINT]) AS b,
        bm_build([2::UBIGINT, 4::UBIGINT]) AS c
)
SELECT
    bm_intersects(a, b) AS has_overlap,
    bm_intersects(a, c) AS no_overlap,
    bm_intersects(NULL::BLOB, b) IS NULL AS null_left
FROM bitmaps

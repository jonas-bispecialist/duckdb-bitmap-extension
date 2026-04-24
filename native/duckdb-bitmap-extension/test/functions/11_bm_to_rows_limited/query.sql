WITH input AS (
    SELECT bm_build([
        0::UBIGINT,
        1::UBIGINT,
        2::UBIGINT,
        3::UBIGINT,
        5::UBIGINT,
        8::UBIGINT,
        13::UBIGINT,
        65535::UBIGINT,
        65536::UBIGINT
    ]) AS bm
)
SELECT
    bm_to_rows(bm, 4::UBIGINT, -1::BIGINT) AS first_page,
    bm_to_rows(bm, 3::UBIGINT, 3::BIGINT) AS after_three,
    bm_to_rows(bm, 10::UBIGINT, 65535::BIGINT) AS after_65535,
    bm_to_rows(NULL::BLOB, 1::UBIGINT, -1::BIGINT) IS NULL AS null_bitmap
FROM input

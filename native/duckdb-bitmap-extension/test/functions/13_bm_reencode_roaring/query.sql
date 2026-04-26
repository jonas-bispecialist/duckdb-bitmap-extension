WITH inputs AS (
    SELECT
        from_hex('4650424D010100000C000000010000000500000009000000') AS sorted_bm,
        bm_build([1::UBIGINT, 5::UBIGINT, 9::UBIGINT]) AS roaring_bm
)
SELECT
    bm_format(bm_reencode_roaring(sorted_bm)) AS sorted_reencoded_format,
    bm_count(bm_reencode_roaring(sorted_bm)) AS sorted_reencoded_count,
    bm_contains(bm_reencode_roaring(sorted_bm), 5::UBIGINT) AS sorted_contains_5,
    bm_contains(bm_reencode_roaring(sorted_bm), 7::UBIGINT) AS sorted_contains_7,
    hex(bm_reencode_roaring(roaring_bm)) = hex(roaring_bm) AS roaring_passthrough,
    bm_reencode_roaring(NULL::BLOB) IS NULL AS null_reencode
FROM inputs

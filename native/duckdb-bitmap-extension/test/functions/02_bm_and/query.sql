WITH inputs AS (
    SELECT
        from_hex('4650424D010100000C000000010000000500000009000000') AS lhs,
        from_hex('4650424D01010000080000000500000007000000') AS rhs
)
SELECT
    hex(bm_and(lhs, rhs)) AS result_hex,
    bm_and(NULL::BLOB, rhs) IS NULL AS null_left,
    bm_and(lhs, NULL::BLOB) IS NULL AS null_right
FROM inputs
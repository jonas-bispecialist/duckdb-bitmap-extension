SELECT
    bm_count(from_hex('4650424D010100000C000000010000000500000009000000')) AS bitmap_count,
    bm_count(NULL::BLOB) IS NULL AS null_count
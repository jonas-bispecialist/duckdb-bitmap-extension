DROP TABLE IF EXISTS li_bitmap_returnflag;
DROP TABLE IF EXISTS li_bitmap_linestatus;
DROP TABLE IF EXISTS li_bitmap_shipmode;

CREATE TABLE li_bitmap_returnflag AS
SELECT
    l_returnflag AS value,
    bm_build_agg(CAST(rid AS UBIGINT)) AS bm
FROM li
GROUP BY 1;

CREATE TABLE li_bitmap_linestatus AS
SELECT
    l_linestatus AS value,
    bm_build_agg(CAST(rid AS UBIGINT)) AS bm
FROM li
GROUP BY 1;

CREATE TABLE li_bitmap_shipmode AS
SELECT
    l_shipmode AS value,
    bm_build_agg(CAST(rid AS UBIGINT)) AS bm
FROM li
GROUP BY 1;

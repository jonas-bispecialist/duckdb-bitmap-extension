INSTALL tpch;
LOAD tpch;
DROP TABLE IF EXISTS customer;
DROP TABLE IF EXISTS lineitem;
DROP TABLE IF EXISTS nation;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS part;
DROP TABLE IF EXISTS partsupp;
DROP TABLE IF EXISTS region;
DROP TABLE IF EXISTS supplier;
DROP TABLE IF EXISTS li;
DROP TABLE IF EXISTS li_bitmap_returnflag;
DROP TABLE IF EXISTS li_bitmap_linestatus;
DROP TABLE IF EXISTS li_bitmap_shipmode;
CALL dbgen(sf = {{SCALE_FACTOR}});

CREATE TABLE li AS
SELECT
    row_number() OVER () - 1 AS rid,
    l_returnflag,
    l_linestatus,
    l_shipmode,
    l_shipdate
FROM lineitem;

CREATE TABLE li_bitmap_returnflag AS
SELECT
    l_returnflag AS value,
    bm_build(list(CAST(rid AS UBIGINT) ORDER BY rid)) AS bm
FROM li
GROUP BY 1;

CREATE TABLE li_bitmap_linestatus AS
SELECT
    l_linestatus AS value,
    bm_build(list(CAST(rid AS UBIGINT) ORDER BY rid)) AS bm
FROM li
GROUP BY 1;

CREATE TABLE li_bitmap_shipmode AS
SELECT
    l_shipmode AS value,
    bm_build(list(CAST(rid AS UBIGINT) ORDER BY rid)) AS bm
FROM li
GROUP BY 1;
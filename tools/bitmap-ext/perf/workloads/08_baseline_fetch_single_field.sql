SELECT
    rid,
    l_returnflag,
    l_linestatus,
    l_shipmode,
    l_shipdate
FROM li
WHERE l_shipmode IN ('AIR', 'RAIL', 'TRUCK')
ORDER BY rid
LIMIT 10000

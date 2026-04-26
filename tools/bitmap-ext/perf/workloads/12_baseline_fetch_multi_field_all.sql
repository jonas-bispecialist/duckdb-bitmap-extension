SELECT
    rid,
    l_returnflag,
    l_linestatus,
    l_shipmode,
    l_shipdate
FROM li
WHERE l_returnflag IN ('R', 'N')
  AND l_linestatus = 'O'
  AND l_shipmode IN ('AIR', 'RAIL')
ORDER BY rid

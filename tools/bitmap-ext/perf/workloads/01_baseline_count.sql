SELECT COUNT(*) AS active_row_count
FROM li
WHERE l_returnflag IN ('R', 'N')
  AND l_linestatus = 'O'
  AND l_shipmode IN ('AIR', 'RAIL')
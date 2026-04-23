SELECT DISTINCT l_shipmode
FROM li
WHERE l_returnflag IN ('R', 'N')
  AND l_linestatus = 'O'
ORDER BY 1
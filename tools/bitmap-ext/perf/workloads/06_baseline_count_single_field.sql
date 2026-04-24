SELECT COUNT(*) AS active_row_count
FROM li
WHERE l_shipmode IN ('AIR', 'RAIL', 'TRUCK')

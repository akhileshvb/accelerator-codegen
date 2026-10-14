SELECT
  l_shipmode,
  SUM(CASE WHEN is_high = 1 THEN line_count ELSE 0 END) AS high_line_count,
  SUM(CASE WHEN is_high = 0 THEN line_count ELSE 0 END) AS low_line_count
FROM accel_tpch_q12_matview
WHERE l_shipmode IN ('REG AIR', 'SHIP')
  AND l_receiptdate >= DATE '1996-01-01'
  AND l_receiptdate < DATE '1996-01-01' + INTERVAL '1' year
GROUP BY l_shipmode
ORDER BY l_shipmode

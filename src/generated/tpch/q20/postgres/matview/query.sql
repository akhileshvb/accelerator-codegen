SELECT s_name, s_address
FROM accel_tpch_q20_matview
WHERE color = 'gainsboro'
  AND year = 1997
  AND nation = 'RUSSIA'
ORDER BY s_name

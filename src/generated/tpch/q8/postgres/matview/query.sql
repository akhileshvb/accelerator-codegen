SELECT
  o_year,
  SUM(CASE WHEN nation = 'INDIA' THEN volume ELSE 0 END) / SUM(volume) AS mkt_share
FROM accel_tpch_q8_matview
WHERE r_name = 'ASIA'
  AND p_type = 'BRASS'
GROUP BY o_year
ORDER BY o_year

SELECT SUM(sum_price) / 7.0 AS avg_yearly
FROM accel_tpch_q17_matview
WHERE p_brand = 'Brand#34'
  AND p_container = 'LARGE BRUSHED'

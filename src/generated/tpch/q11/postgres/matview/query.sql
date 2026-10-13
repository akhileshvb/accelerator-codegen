SELECT ps_partkey, value
FROM accel_tpch_q11_matview
WHERE n_name = 'ROMANIA'
  AND value > nation_total * 0.0001
ORDER BY value DESC

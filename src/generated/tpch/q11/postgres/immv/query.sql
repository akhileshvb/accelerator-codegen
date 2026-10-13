SELECT ps_partkey, value
FROM accel_tpch_q11_immv m
WHERE n_name = 'ROMANIA'
  AND value > (
    SELECT SUM(value) * 0.0001 FROM accel_tpch_q11_immv WHERE n_name = 'ROMANIA'
  )
ORDER BY value DESC

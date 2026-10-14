SELECT s_name, numwait
FROM accel_tpch_q21_matview
WHERE n_name = 'ALGERIA'
ORDER BY numwait DESC, s_name

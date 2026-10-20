SELECT s_suppkey, s_name, s_address, s_phone, total_revenue
FROM accel_tpch_q15_matview
WHERE start_date = DATE '1994-11-01'
ORDER BY s_suppkey

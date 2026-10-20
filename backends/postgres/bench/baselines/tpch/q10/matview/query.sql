SELECT
  c_custkey, c_name, revenue, c_acctbal, n_name,
  c_address, c_phone, c_comment
FROM accel_tpch_q10_matview
WHERE start_date = DATE '1993-06-01'
ORDER BY revenue DESC

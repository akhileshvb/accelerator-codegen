SELECT l_orderkey, revenue, o_orderdate, o_shippriority
FROM accel_tpch_q3_matview
WHERE c_mktsegment = 'FURNITURE'
  AND cutoff_date = DATE '1995-03-10'
ORDER BY revenue DESC, o_orderdate

SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, sum_qty
FROM accel_tpch_q18_matview
WHERE quantity_threshold = 312
ORDER BY o_totalprice DESC, o_orderdate

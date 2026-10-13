SELECT l_orderkey, SUM(revenue) AS revenue, o_orderdate, o_shippriority
FROM accel_tpch_q3_immv
WHERE c_mktsegment = 'FURNITURE'
  AND o_orderdate < DATE '1995-03-10'
  AND l_shipdate > DATE '1995-03-10'
GROUP BY l_orderkey, o_orderdate, o_shippriority
ORDER BY revenue DESC, o_orderdate
LIMIT 10

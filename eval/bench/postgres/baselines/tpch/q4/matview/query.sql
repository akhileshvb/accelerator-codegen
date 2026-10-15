SELECT o_orderpriority, SUM(order_count)::bigint AS order_count
FROM accel_tpch_q4_matview
WHERE o_orderdate >= DATE '1996-11-01'
  AND o_orderdate < DATE '1996-11-01' + INTERVAL '3' month
GROUP BY o_orderpriority
ORDER BY o_orderpriority

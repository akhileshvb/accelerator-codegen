SELECT
  c_custkey, c_name, SUM(revenue) AS revenue, c_acctbal, n_name,
  c_address, c_phone, c_comment
FROM accel_tpch_q10_immv
WHERE o_orderdate >= DATE '1993-06-01'
  AND o_orderdate < DATE '1993-06-01' + INTERVAL '3' month
GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment
ORDER BY revenue DESC
LIMIT 20

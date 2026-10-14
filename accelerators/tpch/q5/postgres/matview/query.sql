SELECT n_name, SUM(revenue) AS revenue
FROM accel_tpch_q5_matview
WHERE r_name = 'ASIA'
  AND o_orderdate >= DATE '1996-01-01'
  AND o_orderdate < DATE '1996-01-01' + INTERVAL '1' year
GROUP BY n_name
ORDER BY revenue DESC

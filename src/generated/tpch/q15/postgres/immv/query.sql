WITH revenue AS (
  SELECT supplier_no, SUM(total_revenue) AS total_revenue
  FROM accel_tpch_q15_immv
  WHERE l_shipdate >= DATE '1994-11-01'
    AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' month
  GROUP BY supplier_no
)
SELECT s.s_suppkey, s.s_name, s.s_address, s.s_phone, r.total_revenue
FROM supplier s
JOIN revenue r ON s.s_suppkey = r.supplier_no
WHERE r.total_revenue = (SELECT MAX(total_revenue) FROM revenue)
ORDER BY s.s_suppkey

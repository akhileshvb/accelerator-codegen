-- Revenue by region/nation/orderdate (year window applied in residual).
CREATE MATERIALIZED VIEW accel_tpch_q5_matview AS
SELECT
  r.r_name,
  n.n_name,
  o.o_orderdate,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS revenue
FROM customer c
JOIN orders o ON c.c_custkey = o.o_custkey
JOIN lineitem l ON l.l_orderkey = o.o_orderkey
JOIN supplier s ON l.l_suppkey = s.s_suppkey
JOIN nation n ON s.s_nationkey = n.n_nationkey
JOIN region r ON n.n_regionkey = r.r_regionkey
WHERE c.c_nationkey = s.s_nationkey
GROUP BY r.r_name, n.n_name, o.o_orderdate;

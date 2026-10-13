-- Incrementally maintainable matview (pg_ivm) for TPC-H Q7.
-- Same residual grain as matview where possible.
SELECT pgivm.create_immv(
  'accel_tpch_q7_immv',
  $immv$
SELECT
  n1.n_name AS supp_nation,
  n2.n_name AS cust_nation,
  EXTRACT(year FROM l.l_shipdate)::integer AS l_year,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS revenue
FROM supplier s
JOIN lineitem l ON s.s_suppkey = l.l_suppkey
JOIN orders o ON o.o_orderkey = l.l_orderkey
JOIN customer c ON c.c_custkey = o.o_custkey
JOIN nation n1 ON s.s_nationkey = n1.n_nationkey
JOIN nation n2 ON c.c_nationkey = n2.n_nationkey
WHERE l.l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
GROUP BY n1.n_name, n2.n_name, EXTRACT(year FROM l.l_shipdate)::integer
$immv$
);

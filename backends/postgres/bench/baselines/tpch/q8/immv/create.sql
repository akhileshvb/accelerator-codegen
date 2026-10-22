-- Incrementally maintainable matview (pg_ivm) for TPC-H Q8.
-- Same residual grain as matview where possible.
SELECT pgivm.create_immv(
  'accel_tpch_q8_immv',
  $immv$
SELECT
  r.r_name,
  p.p_type,
  EXTRACT(year FROM o.o_orderdate)::integer AS o_year,
  n2.n_name AS nation,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS volume
FROM part p
JOIN lineitem l ON p.p_partkey = l.l_partkey
JOIN supplier s ON s.s_suppkey = l.l_suppkey
JOIN orders o ON l.l_orderkey = o.o_orderkey
JOIN customer c ON o.o_custkey = c.c_custkey
JOIN nation n1 ON c.c_nationkey = n1.n_nationkey
JOIN nation n2 ON s.s_nationkey = n2.n_nationkey
JOIN region r ON n1.n_regionkey = r.r_regionkey
WHERE o.o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
GROUP BY r.r_name, p.p_type, EXTRACT(year FROM o.o_orderdate)::integer, n2.n_name
$immv$
);

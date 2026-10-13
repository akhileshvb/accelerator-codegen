-- Incrementally maintainable matview (pg_ivm) for TPC-H Q3.
-- Same residual grain as matview where possible.
SELECT pgivm.create_immv(
  'accel_tpch_q3_immv',
  $immv$
SELECT
  c.c_mktsegment,
  o.o_orderdate,
  l.l_shipdate,
  l.l_orderkey,
  o.o_shippriority,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS revenue
FROM customer c
JOIN orders o ON c.c_custkey = o.o_custkey
JOIN lineitem l ON l.l_orderkey = o.o_orderkey
GROUP BY c.c_mktsegment, o.o_orderdate, l.l_shipdate, l.l_orderkey, o.o_shippriority
$immv$
);

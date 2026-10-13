-- Incrementally maintainable matview (pg_ivm) for TPC-H Q15.
-- Same residual grain as matview where possible.
SELECT pgivm.create_immv(
  'accel_tpch_q15_immv',
  $immv$
SELECT
  l.l_suppkey AS supplier_no,
  l.l_shipdate,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS total_revenue
FROM lineitem l
GROUP BY l.l_suppkey, l.l_shipdate
$immv$
);

-- Incrementally maintainable matview (pg_ivm) for TPC-H Q6.
-- Same residual grain as matview where possible.
SELECT pgivm.create_immv(
  'accel_tpch_q6_immv',
  $immv$
SELECT
  l_shipdate,
  l_discount,
  l_quantity,
  SUM(l_extendedprice * l_discount) AS revenue
FROM lineitem
GROUP BY l_shipdate, l_discount, l_quantity
$immv$
);

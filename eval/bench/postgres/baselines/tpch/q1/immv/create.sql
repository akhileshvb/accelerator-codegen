-- Incrementally maintainable matview (pg_ivm) for TPC-H Q1.
-- Same residual grain as matview where possible.
SELECT pgivm.create_immv(
  'accel_tpch_q1_immv',
  $immv$
SELECT
  l_shipdate,
  l_returnflag,
  l_linestatus,
  SUM(l_quantity) AS sum_qty,
  SUM(l_extendedprice) AS sum_base_price,
  SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
  SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
  SUM(l_discount) AS sum_discount,
  COUNT(*) AS count_order
FROM lineitem
GROUP BY l_shipdate, l_returnflag, l_linestatus
$immv$
);

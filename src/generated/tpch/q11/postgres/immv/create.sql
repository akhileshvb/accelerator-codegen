-- Incrementally maintainable matview (pg_ivm) for TPC-H Q11.
-- Same residual grain as matview where possible.
SELECT pgivm.create_immv(
  'accel_tpch_q11_immv',
  $immv$
SELECT
  n.n_name,
  ps.ps_partkey,
  SUM(ps.ps_supplycost * ps.ps_availqty) AS value
FROM partsupp ps
JOIN supplier s ON ps.ps_suppkey = s.s_suppkey
JOIN nation n ON s.s_nationkey = n.n_nationkey
GROUP BY n.n_name, ps.ps_partkey
$immv$
);

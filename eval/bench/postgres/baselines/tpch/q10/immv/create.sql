-- Incrementally maintainable matview (pg_ivm) for TPC-H Q10.
-- Same residual grain as matview where possible.
SELECT pgivm.create_immv(
  'accel_tpch_q10_immv',
  $immv$
SELECT
  c.c_custkey,
  c.c_name,
  c.c_acctbal,
  c.c_phone,
  c.c_address,
  c.c_comment,
  n.n_name,
  o.o_orderdate,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS revenue
FROM customer c
JOIN orders o ON c.c_custkey = o.o_custkey
JOIN lineitem l ON l.l_orderkey = o.o_orderkey
JOIN nation n ON c.c_nationkey = n.n_nationkey
WHERE l.l_returnflag = 'R'
GROUP BY c.c_custkey, c.c_name, c.c_acctbal, c.c_phone, c.c_address, c.c_comment,
         n.n_name, o.o_orderdate
$immv$
);

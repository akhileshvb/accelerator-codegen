-- Incrementally maintainable matview (pg_ivm) for TPC-H Q4.
-- Same residual grain as matview where possible.
SELECT pgivm.create_immv(
  'accel_tpch_q4_immv',
  $immv$
SELECT
  o.o_orderkey,
  o.o_orderdate,
  o.o_orderpriority,
  COUNT(*) AS order_count
FROM orders o
WHERE EXISTS (
  SELECT 1 FROM lineitem l
  WHERE l.l_orderkey = o.o_orderkey
    AND l.l_commitdate < l.l_receiptdate
)
GROUP BY o.o_orderkey, o.o_orderdate, o.o_orderpriority
$immv$
);

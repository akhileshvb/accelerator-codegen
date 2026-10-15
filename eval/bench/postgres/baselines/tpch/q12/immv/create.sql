-- Incrementally maintainable matview (pg_ivm) for TPC-H Q12.
-- Same residual grain as matview where possible.
SELECT pgivm.create_immv(
  'accel_tpch_q12_immv',
  $immv$
SELECT
  l.l_shipmode,
  l.l_receiptdate,
  CASE
    WHEN o.o_orderpriority IN ('1-URGENT', '2-HIGH') THEN 1
    ELSE 0
  END AS is_high,
  COUNT(*) AS line_count
FROM orders o
JOIN lineitem l ON o.o_orderkey = l.l_orderkey
WHERE l.l_commitdate < l.l_receiptdate
  AND l.l_shipdate < l.l_commitdate
GROUP BY l.l_shipmode, l.l_receiptdate,
         CASE WHEN o.o_orderpriority IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END
$immv$
);

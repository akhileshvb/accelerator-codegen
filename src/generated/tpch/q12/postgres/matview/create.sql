-- Qualified lineitems with shipmode, receiptdate, priority class.
CREATE MATERIALIZED VIEW accel_tpch_q12_matview AS
SELECT
  l.l_shipmode,
  l.l_receiptdate,
  CASE
    WHEN o.o_orderpriority IN ('1-URGENT', '2-HIGH') THEN 1
    ELSE 0
  END AS is_high,
  COUNT(*)::bigint AS line_count
FROM orders o
JOIN lineitem l ON o.o_orderkey = l.l_orderkey
WHERE l.l_commitdate < l.l_receiptdate
  AND l.l_shipdate < l.l_commitdate
GROUP BY l.l_shipmode, l.l_receiptdate,
         CASE WHEN o.o_orderpriority IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END;

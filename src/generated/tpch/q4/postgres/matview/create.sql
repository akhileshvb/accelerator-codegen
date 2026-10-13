-- Daily late-order counts by priority (tiny MV; residual sums a date window).
CREATE MATERIALIZED VIEW accel_tpch_q4_matview AS
SELECT o.o_orderdate, o.o_orderpriority, COUNT(*)::bigint AS order_count
FROM orders o
WHERE EXISTS (
  SELECT 1 FROM lineitem l
  WHERE l.l_orderkey = o.o_orderkey
    AND l.l_commitdate < l.l_receiptdate
)
GROUP BY o.o_orderdate, o.o_orderpriority;

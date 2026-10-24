UPDATE orders SET o_orderpriority =
  CASE WHEN o_orderpriority IN ('1-URGENT', '2-HIGH') THEN '3-MEDIUM' ELSE '1-URGENT' END
WHERE o_orderkey IN (
  SELECT DISTINCT o_orderkey FROM orders JOIN lineitem ON o_orderkey = l_orderkey
  WHERE l_shipdate < l_commitdate AND l_commitdate < l_receiptdate
    AND l_receiptdate >= DATE '1995-01-01' AND l_receiptdate < DATE '1996-01-01'
    AND o_comment NOT LIKE 'q12_%'
  ORDER BY o_orderkey LIMIT 12);

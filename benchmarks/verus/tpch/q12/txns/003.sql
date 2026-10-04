UPDATE orders SET o_orderpriority = '5-LOW', o_comment = 'q12_priority_cycle'
WHERE o_orderkey = (
  SELECT o_orderkey FROM orders JOIN lineitem ON o_orderkey = l_orderkey
  WHERE o_orderpriority IN ('1-URGENT', '2-HIGH')
    AND l_shipdate < l_commitdate AND l_commitdate < l_receiptdate
    AND l_receiptdate >= DATE '1995-01-01' AND l_receiptdate < DATE '1996-01-01'
    AND o_comment NOT LIKE 'q12_%'
  ORDER BY o_orderkey LIMIT 1);

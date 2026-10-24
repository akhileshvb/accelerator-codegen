DELETE FROM orders
WHERE o_orderkey = (
  SELECT o_orderkey
  FROM orders
  WHERE o_orderstatus = 'F'
    AND EXISTS (
      SELECT 1 FROM lineitem l1
      WHERE l1.l_orderkey = orders.o_orderkey
        AND l1.l_receiptdate > l1.l_commitdate
        AND EXISTS (SELECT 1 FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey)
        AND NOT EXISTS (SELECT 1 FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate)
        AND EXISTS (SELECT 1 FROM supplier q JOIN nation n ON n.n_nationkey = q.s_nationkey WHERE q.s_suppkey = l1.l_suppkey AND n.n_name = 'CaseTrap')
    )
  ORDER BY o_orderkey
  LIMIT 1
);

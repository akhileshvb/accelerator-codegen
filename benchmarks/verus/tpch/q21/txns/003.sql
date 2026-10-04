UPDATE lineitem
SET l_commitdate = l_receiptdate
WHERE l_orderkey = (
  SELECT l_orderkey
  FROM lineitem l1
  WHERE l1.l_receiptdate > l1.l_commitdate
    AND EXISTS (SELECT 1 FROM orders o WHERE o.o_orderkey = l1.l_orderkey AND o.o_orderstatus = 'F')
    AND EXISTS (SELECT 1 FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey)
    AND NOT EXISTS (SELECT 1 FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate)
    AND EXISTS (SELECT 1 FROM supplier s JOIN nation n ON n.n_nationkey = s.s_nationkey WHERE s.s_suppkey = l1.l_suppkey AND n.n_name = 'LARGE ANSWER NATION')
  ORDER BY l_orderkey DESC
  LIMIT 1
)
  AND l_receiptdate > l_commitdate;

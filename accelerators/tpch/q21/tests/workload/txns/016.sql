UPDATE lineitem
SET l_orderkey = l_orderkey + 2000000000, l_suppkey = l_suppkey + 1
WHERE l_orderkey = (
  SELECT l_orderkey
  FROM lineitem l
  WHERE l.l_receiptdate > l.l_commitdate
    AND EXISTS (SELECT 1 FROM orders o WHERE o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F')
    AND EXISTS (SELECT 1 FROM lineitem p WHERE p.l_orderkey = l.l_orderkey AND p.l_suppkey <> l.l_suppkey)
    AND NOT EXISTS (SELECT 1 FROM lineitem x WHERE x.l_orderkey = l.l_orderkey AND x.l_suppkey <> l.l_suppkey AND x.l_receiptdate > x.l_commitdate)
    AND EXISTS (SELECT 1 FROM supplier s JOIN nation n ON n.n_nationkey = s.s_nationkey WHERE s.s_suppkey = l.l_suppkey AND n.n_name = 'CaseTrap')
  ORDER BY l.l_orderkey
  LIMIT 1
);

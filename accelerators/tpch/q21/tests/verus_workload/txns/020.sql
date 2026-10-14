DELETE FROM nation
WHERE n_nationkey = (
  SELECT n.n_nationkey
  FROM nation n
  JOIN supplier s ON s.s_nationkey = n.n_nationkey
  JOIN lineitem l ON l.l_suppkey = s.s_suppkey
  JOIN orders o ON o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F'
  WHERE l.l_receiptdate > l.l_commitdate
    AND EXISTS (SELECT 1 FROM lineitem p WHERE p.l_orderkey = l.l_orderkey AND p.l_suppkey <> l.l_suppkey)
    AND NOT EXISTS (SELECT 1 FROM lineitem x WHERE x.l_orderkey = l.l_orderkey AND x.l_suppkey <> l.l_suppkey AND x.l_receiptdate > x.l_commitdate)
    AND n.n_name = 'SAUDI ARABIA'
  ORDER BY n.n_nationkey
  LIMIT 1
);

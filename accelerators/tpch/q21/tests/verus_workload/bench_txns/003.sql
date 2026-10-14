UPDATE supplier
SET s_name = 'benchmark supplier name'
WHERE s_suppkey = (
  SELECT s_suppkey FROM supplier
  WHERE EXISTS (
    SELECT 1 FROM lineitem l
    JOIN orders o ON o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F'
    JOIN nation n ON n.n_nationkey = supplier.s_nationkey
    WHERE l.l_suppkey = supplier.s_suppkey
      AND n.n_name = 'SAUDI ARABIA'
      AND l.l_receiptdate > l.l_commitdate
      AND EXISTS (SELECT 1 FROM lineitem p WHERE p.l_orderkey = l.l_orderkey AND p.l_suppkey <> l.l_suppkey)
      AND NOT EXISTS (SELECT 1 FROM lineitem x WHERE x.l_orderkey = l.l_orderkey AND x.l_suppkey <> l.l_suppkey AND x.l_receiptdate > x.l_commitdate)
  )
  ORDER BY s_suppkey DESC
  LIMIT 1
);

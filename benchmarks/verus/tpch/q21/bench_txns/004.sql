UPDATE nation
SET n_name = 'CaseTrap'
WHERE n_nationkey = (
  SELECT n_nationkey FROM nation
  WHERE n_name = 'SAUDI ARABIA'
    AND EXISTS (
      SELECT 1
      FROM supplier s
      JOIN lineitem l ON l.l_suppkey = s.s_suppkey
      JOIN orders o ON o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F'
      WHERE s.s_nationkey = nation.n_nationkey
        AND l.l_receiptdate > l.l_commitdate
        AND EXISTS (SELECT 1 FROM lineitem p WHERE p.l_orderkey = l.l_orderkey AND p.l_suppkey <> l.l_suppkey)
        AND NOT EXISTS (SELECT 1 FROM lineitem x WHERE x.l_orderkey = l.l_orderkey AND x.l_suppkey <> l.l_suppkey AND x.l_receiptdate > x.l_commitdate)
    )
  ORDER BY n_nationkey
  LIMIT 1
);

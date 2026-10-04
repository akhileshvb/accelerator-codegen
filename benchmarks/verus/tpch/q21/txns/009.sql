UPDATE supplier
SET s_name = 'duplicate name', s_nationkey = (
    SELECT n_nationkey FROM nation WHERE n_name = 'CaseTrap' ORDER BY n_nationkey LIMIT 1
  )
WHERE s_suppkey = (
  SELECT s.s_suppkey
  FROM supplier s
  JOIN lineitem l ON l.l_suppkey = s.s_suppkey
  JOIN orders o ON o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F'
  WHERE l.l_receiptdate > l.l_commitdate
    AND EXISTS (SELECT 1 FROM lineitem p WHERE p.l_orderkey = l.l_orderkey AND p.l_suppkey <> l.l_suppkey)
    AND NOT EXISTS (SELECT 1 FROM lineitem x WHERE x.l_orderkey = l.l_orderkey AND x.l_suppkey <> l.l_suppkey AND x.l_receiptdate > x.l_commitdate)
    AND s.s_name <> 'duplicate name'
    AND EXISTS (SELECT 1 FROM nation n WHERE n.n_nationkey = s.s_nationkey AND n.n_name = 'LARGE ANSWER NATION')
  ORDER BY s.s_suppkey DESC
  LIMIT 1
);

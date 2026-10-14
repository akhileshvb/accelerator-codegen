INSERT INTO supplier (s_suppkey, s_name, s_address, s_nationkey, s_phone, s_acctbal, s_comment)
SELECT s.s_suppkey, 'duplicate key answer name', 'new duplicate', s.s_nationkey, '000', 0.00, ''
FROM supplier s
JOIN lineitem l ON l.l_suppkey = s.s_suppkey
JOIN orders o ON o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F'
WHERE l.l_receiptdate > l.l_commitdate
  AND EXISTS (SELECT 1 FROM lineitem p WHERE p.l_orderkey = l.l_orderkey AND p.l_suppkey <> l.l_suppkey)
  AND NOT EXISTS (SELECT 1 FROM lineitem x WHERE x.l_orderkey = l.l_orderkey AND x.l_suppkey <> l.l_suppkey AND x.l_receiptdate > x.l_commitdate)
  AND EXISTS (SELECT 1 FROM nation n WHERE n.n_nationkey = s.s_nationkey AND n.n_name = 'CaseTrap')
ORDER BY s.s_suppkey
LIMIT 1;

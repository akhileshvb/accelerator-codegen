INSERT INTO lineitem (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment)
SELECT l_orderkey, 999999, l_suppkey + 1, 999, 0.00, 0.00, 0.00, 0.00, 'A', 'F', l_shipdate, l_commitdate, l_commitdate, '', '', 'late peer enters answer'
FROM lineitem l1
WHERE l1.l_orderkey = (
  SELECT l_orderkey
  FROM lineitem l
  WHERE l.l_receiptdate > l.l_commitdate
    AND EXISTS (SELECT 1 FROM orders o WHERE o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F')
    AND NOT EXISTS (SELECT 1 FROM lineitem x WHERE x.l_orderkey = l.l_orderkey AND x.l_suppkey <> l.l_suppkey)
    AND EXISTS (SELECT 1 FROM supplier s JOIN nation n ON n.n_nationkey = s.s_nationkey WHERE s.s_suppkey = l.l_suppkey AND n.n_name = 'CASETRAP')
  ORDER BY l.l_orderkey DESC
  LIMIT 1
)
  AND l1.l_receiptdate > l1.l_commitdate
  AND EXISTS (SELECT 1 FROM supplier s JOIN nation n ON n.n_nationkey = s.s_nationkey WHERE s.s_suppkey = l1.l_suppkey AND n.n_name = 'CASETRAP')
LIMIT 1;

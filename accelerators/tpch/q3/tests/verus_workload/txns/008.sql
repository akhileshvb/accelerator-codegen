INSERT INTO orders
  (o_orderkey, o_custkey, o_orderstatus, o_totalprice, o_orderdate, o_orderpriority, o_clerk, o_shippriority, o_comment)
SELECT (SELECT MAX(o_orderkey) FROM orders) + v.key_step,
       (SELECT c.c_custkey FROM customer c WHERE c.c_mktsegment = 'BUILDING' ORDER BY c.c_custkey LIMIT 1),
       'O', v.o_totalprice, DATE '1994-06-01', '3-MEDIUM', 'Clerk#000008008', 0, v.o_comment
FROM (VALUES
  (1, 90000.00, 'First order in an application batch'),
  (2, 120000.00, 'Second order in an application batch')
) v(key_step, o_totalprice, o_comment);

INSERT INTO lineitem
  (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax,
   l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment)
SELECT CASE WHEN v.batch_order = 1 THEN k.minkey ELSE k.maxkey END,
       p.p_partkey, s.s_suppkey, v.l_linenumber, v.l_quantity, v.l_extendedprice,
       v.l_discount, v.l_tax, v.l_returnflag, v.l_linestatus, v.l_shipdate, v.l_commitdate,
       v.l_receiptdate, v.l_shipinstruct, v.l_shipmode, v.l_comment
FROM (VALUES
  (1, 1, 12.00, 30000.00, 0.02, 0.01, 'N', 'O', DATE '1995-05-01', DATE '1994-08-01', DATE '1995-05-20', 'NONE', 'AIR', 'batch line one'),
  (1, 2, 14.00, 36000.00, 0.03, 0.02, 'N', 'O', DATE '1995-05-02', DATE '1994-08-01', DATE '1995-05-21', 'NONE', 'AIR', 'batch line two'),
  (2, 1, 16.00, 44000.00, 0.04, 0.03, 'R', 'F', DATE '1995-05-03', DATE '1994-08-01', DATE '1995-05-22', 'COLLECT COD', 'SHIP', 'batch line three'),
  (2, 2, 18.00, 52000.00, 0.05, 0.04, 'A', 'F', DATE '1995-05-04', DATE '1994-08-01', DATE '1995-05-23', 'COLLECT COD', 'SHIP', 'batch line four')
) v(batch_order, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment)
CROSS JOIN (
  SELECT MIN(o_orderkey) AS minkey, MAX(o_orderkey) AS maxkey
  FROM orders
  WHERE o_clerk = 'Clerk#000008008' AND o_orderpriority = '3-MEDIUM'
) k
CROSS JOIN (SELECT MIN(p_partkey) AS p_partkey FROM part) p
CROSS JOIN (SELECT MIN(s_suppkey) AS s_suppkey FROM supplier) s;

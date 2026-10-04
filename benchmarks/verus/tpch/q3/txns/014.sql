INSERT INTO orders
  (o_orderkey, o_custkey, o_orderstatus, o_totalprice, o_orderdate, o_orderpriority, o_clerk, o_shippriority, o_comment)
SELECT (SELECT MAX(o_orderkey) FROM orders) + v.key_step,
       (SELECT c.c_custkey FROM customer c WHERE c.c_mktsegment = 'BUILDING' ORDER BY c.c_custkey LIMIT 1),
       'O', 525000.00, DATE '1995-02-01', '2-HIGH', 'Clerk#000014014', 2, 'Tied ordering test order'
FROM (VALUES (1), (2)) v(key_step);

INSERT INTO lineitem
  (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax,
   l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment)
SELECT o.o_orderkey, p.p_partkey, s.s_suppkey, 1, 50.00, 500000.00, 0.05, 0.08,
       'N', 'O', DATE '1995-06-01', DATE '1995-03-01', DATE '1995-06-20', 'DELIVER IN PERSON', 'AIR', 'Tied ordering test line'
FROM orders o
CROSS JOIN (SELECT MIN(p_partkey) AS p_partkey FROM part) p
CROSS JOIN (SELECT MIN(s_suppkey) AS s_suppkey FROM supplier) s
WHERE o.o_clerk = 'Clerk#000014014';

-- RF1-shaped bulk insert: 200 orders of the answer window cloned under fresh keys (4294967296 and above), each with its lines
INSERT INTO orders
SELECT o_orderkey + 4294967296, o_custkey, o_orderstatus, o_totalprice, o_orderdate, o_orderpriority, o_clerk,
       o_shippriority, 'BULK-Q3-022'
FROM orders
WHERE o_orderdate < DATE '1995-03-15' AND o_orderdate > DATE '1995-02-01'
ORDER BY o_orderkey
LIMIT 200;
INSERT INTO lineitem
SELECT l.l_orderkey + 4294967296, l.l_partkey, l.l_suppkey, l.l_linenumber, l.l_quantity, l.l_extendedprice * 3,
       l.l_discount, l.l_tax, l.l_returnflag, l.l_linestatus, l.l_shipdate + 40, l.l_commitdate, l.l_receiptdate,
       l.l_shipinstruct, l.l_shipmode, l.l_comment
FROM lineitem l
WHERE l.l_orderkey IN (SELECT o_orderkey - 4294967296 FROM orders WHERE o_comment = 'BULK-Q3-022');

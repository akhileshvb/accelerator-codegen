INSERT INTO customer
  (c_custkey, c_name, c_address, c_nationkey, c_phone, c_acctbal, c_mktsegment, c_comment)
VALUES
  ((SELECT MAX(c_custkey) + 1 FROM customer), 'Customer#test000002', 'One Test Plaza', 15,
   '25-000-000-0002', 1234.56, 'MACHINERY', 'Inserted by the Q3 application');

INSERT INTO orders
  (o_orderkey, o_custkey, o_orderstatus, o_totalprice, o_orderdate, o_orderpriority, o_clerk, o_shippriority, o_comment)
VALUES
  ((SELECT MAX(o_orderkey) + 1000 FROM orders), (SELECT MAX(c_custkey) FROM customer), 'O',
   570000.00, DATE '1995-01-01', '1-URGENT', 'Clerk#000002002', 0, 'New priority machinery order');

INSERT INTO lineitem
  (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax,
   l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment)
SELECT o.o_orderkey, p.p_partkey, s.s_suppkey, v.l_linenumber, v.l_quantity, v.l_extendedprice,
       v.l_discount, v.l_tax, v.l_returnflag, v.l_linestatus, v.l_shipdate, v.l_commitdate,
       v.l_receiptdate, v.l_shipinstruct, v.l_shipmode, v.l_comment
FROM (VALUES
  (1, 25.00, 200000.00, 0.10, 0.08, 'N', 'O', DATE '1996-01-01', DATE '1995-02-01', DATE '1996-01-20', 'DELIVER IN PERSON', 'AIR', 'priority line one'),
  (2, 30.00, 200000.00, 0.10, 0.08, 'N', 'O', DATE '1996-01-01', DATE '1995-02-01', DATE '1996-01-20', 'DELIVER IN PERSON', 'AIR', 'priority line two'),
  (3, 35.00, 200000.00, 0.10, 0.08, 'N', 'O', DATE '1996-01-01', DATE '1995-02-01', DATE '1996-01-20', 'DELIVER IN PERSON', 'AIR', 'priority line three')
) v(l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment)
CROSS JOIN (SELECT MAX(o_orderkey) AS o_orderkey FROM orders) o
CROSS JOIN (SELECT MIN(p_partkey) AS p_partkey FROM part) p
CROSS JOIN (SELECT MIN(s_suppkey) AS s_suppkey FROM supplier) s;

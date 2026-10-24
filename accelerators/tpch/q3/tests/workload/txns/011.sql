INSERT INTO customer
  (c_custkey, c_name, c_address, c_nationkey, c_phone, c_acctbal, c_mktsegment, c_comment)
SELECT c.c_custkey, c.c_name, c.c_address, c.c_nationkey, c.c_phone, c.c_acctbal,
       c.c_mktsegment, c.c_comment
FROM customer c
WHERE c.c_custkey = (
  SELECT o.o_custkey
  FROM customer x
  JOIN orders o ON o.o_custkey = x.c_custkey
  JOIN lineitem l ON l.l_orderkey = o.o_orderkey
  WHERE x.c_mktsegment = 'BUILDING'
    AND o.o_orderdate < DATE '1995-03-15'
    AND l.l_shipdate > DATE '1995-03-15'
  GROUP BY o.o_custkey
  ORDER BY SUM(l.l_extendedprice * (1 - l.l_discount)) DESC, o.o_custkey
  LIMIT 1
);

INSERT INTO orders
  (o_orderkey, o_custkey, o_orderstatus, o_totalprice, o_orderdate, o_orderpriority, o_clerk, o_shippriority, o_comment)
SELECT o.o_orderkey, o.o_custkey, o.o_orderstatus, o.o_totalprice, o.o_orderdate,
       o.o_orderpriority, o.o_clerk, o.o_shippriority, o.o_comment
FROM orders o
WHERE o.o_orderkey = (
  SELECT x.o_orderkey
  FROM customer c
  JOIN orders x ON x.o_custkey = c.c_custkey
  JOIN lineitem l ON l.l_orderkey = x.o_orderkey
  WHERE c.c_mktsegment = 'BUILDING'
    AND x.o_orderdate < DATE '1995-03-15'
    AND l.l_shipdate > DATE '1995-03-15'
  GROUP BY x.o_orderkey
  ORDER BY SUM(l.l_extendedprice * (1 - l.l_discount)) DESC, x.o_orderkey
  LIMIT 1
);

INSERT INTO lineitem
  (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax,
   l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment)
SELECT l.l_orderkey, l.l_partkey, l.l_suppkey, l.l_linenumber, l.l_quantity, l.l_extendedprice,
       l.l_discount, l.l_tax, l.l_returnflag, l.l_linestatus, l.l_shipdate, l.l_commitdate,
       l.l_receiptdate, l.l_shipinstruct, l.l_shipmode, l.l_comment
FROM lineitem l
WHERE l.l_orderkey = (
  SELECT x.o_orderkey
  FROM customer c
  JOIN orders x ON x.o_custkey = c.c_custkey
  JOIN lineitem y ON y.l_orderkey = x.o_orderkey
  WHERE c.c_mktsegment = 'BUILDING'
    AND x.o_orderdate < DATE '1995-03-15'
    AND y.l_shipdate > DATE '1995-03-15'
  GROUP BY x.o_orderkey
  ORDER BY SUM(y.l_extendedprice * (1 - y.l_discount)) DESC, x.o_orderkey
  LIMIT 1
)
AND l.l_linenumber = (
  SELECT MIN(y.l_linenumber)
  FROM lineitem y
  WHERE y.l_orderkey = l.l_orderkey
);

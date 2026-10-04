INSERT INTO lineitem
  (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax,
   l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment)
SELECT o.o_orderkey, p.p_partkey, s.s_suppkey, v.l_linenumber, v.l_quantity, v.l_extendedprice,
       v.l_discount, v.l_tax, v.l_returnflag, v.l_linestatus, v.l_shipdate, v.l_commitdate,
       v.l_receiptdate, v.l_shipinstruct, v.l_shipmode, v.l_comment
FROM (VALUES
  (7, 20.00, 35000.00, 0.02, 0.01, 'A', 'F', DATE '1995-06-20', DATE '1995-04-15', DATE '1995-07-05', 'TAKE BACK RETURN', 'RAIL', 'household added line seven'),
  (8, 18.00, 42500.00, 0.03, 0.02, 'R', 'F', DATE '1995-07-01', DATE '1995-04-15', DATE '1995-07-18', 'NONE', 'SHIP', 'household added line eight'),
  (9, 16.00, 51000.00, 0.04, 0.03, 'N', 'O', DATE '1995-07-15', DATE '1995-04-15', DATE '1995-08-01', 'COLLECT COD', 'TRUCK', 'household added line nine')
) v(l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment)
CROSS JOIN (
  SELECT o.o_orderkey
  FROM customer c
  JOIN orders o ON o.o_custkey = c.c_custkey
  JOIN lineitem l ON l.l_orderkey = o.o_orderkey
  WHERE c.c_mktsegment = 'HOUSEHOLD'
    AND o.o_orderdate < DATE '1995-03-15'
    AND l.l_shipdate > DATE '1995-03-15'
  GROUP BY o.o_orderkey
  ORDER BY SUM(l.l_extendedprice * (1 - l.l_discount)) DESC, o.o_orderkey
  LIMIT 1
) o
CROSS JOIN (SELECT MIN(p_partkey) AS p_partkey FROM part) p
CROSS JOIN (SELECT MIN(s_suppkey) AS s_suppkey FROM supplier) s;

UPDATE orders
SET o_orderdate = DATE '1994-01-01',
    o_shippriority = 1
WHERE o_orderkey = (
  SELECT o.o_orderkey
  FROM customer c
  JOIN orders o ON o.o_custkey = c.c_custkey
  JOIN lineitem l ON l.l_orderkey = o.o_orderkey
  WHERE c.c_mktsegment = 'FURNITURE'
    AND o.o_orderdate < DATE '1995-03-15'
    AND l.l_shipdate > DATE '1995-03-15'
  GROUP BY o.o_orderkey
  ORDER BY SUM(l.l_extendedprice * (1 - l.l_discount)) DESC, o.o_orderkey
  LIMIT 1
);

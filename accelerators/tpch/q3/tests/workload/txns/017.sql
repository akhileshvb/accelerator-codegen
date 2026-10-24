UPDATE orders
SET o_comment = 'MULTI TABLE 20261004'
WHERE o_orderkey = (
  SELECT o.o_orderkey
  FROM customer c
  JOIN orders o ON o.o_custkey = c.c_custkey
  JOIN lineitem l ON l.l_orderkey = o.o_orderkey
  WHERE c.c_mktsegment = 'BUILDING'
    AND o.o_orderdate < DATE '1995-03-15'
    AND l.l_shipdate > DATE '1995-03-15'
  GROUP BY o.o_orderkey
  ORDER BY SUM(l.l_extendedprice * (1 - l.l_discount)) DESC, o.o_orderkey
  LIMIT 1
);

UPDATE lineitem
SET l_extendedprice = l_extendedprice * 1.25,
    l_shipdate = DATE '1996-01-01'
WHERE l_orderkey = (SELECT MAX(o_orderkey) FROM orders WHERE o_comment = 'MULTI TABLE 20261004');

UPDATE orders
SET o_orderdate = DATE '1994-01-01',
    o_shippriority = 2
WHERE o_comment = 'MULTI TABLE 20261004';

UPDATE customer
SET c_mktsegment = 'MACHINERY'
WHERE c_custkey = (
  SELECT MAX(o_custkey) FROM orders WHERE o_comment = 'MULTI TABLE 20261004'
);

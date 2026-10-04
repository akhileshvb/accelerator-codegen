UPDATE orders
SET o_comment = 'KEY MOVE 20261004'
WHERE o_orderkey = (
  SELECT o.o_orderkey
  FROM customer c
  JOIN orders o ON o.o_custkey = c.c_custkey
  JOIN lineitem l ON l.l_orderkey = o.o_orderkey
  WHERE c.c_mktsegment = 'MACHINERY'
    AND o.o_orderdate < DATE '1995-03-15'
    AND l.l_shipdate > DATE '1995-03-15'
  GROUP BY o.o_orderkey
  ORDER BY SUM(l.l_extendedprice * (1 - l.l_discount)) DESC, o.o_orderkey
  LIMIT 1
);

UPDATE lineitem
SET l_orderkey = (SELECT MAX(o_orderkey) + 1000000 FROM orders)
WHERE l_orderkey = (SELECT MAX(o_orderkey) FROM orders WHERE o_comment = 'KEY MOVE 20261004');

UPDATE orders
SET o_orderkey = (SELECT MAX(o_orderkey) + 1000000 FROM orders),
    o_comment = 'KEY MOVE 20261004 MOVED'
WHERE o_comment = 'KEY MOVE 20261004';

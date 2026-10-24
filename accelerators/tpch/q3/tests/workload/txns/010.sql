UPDATE lineitem
SET l_extendedprice = ROUND(l_extendedprice * 1.05, 2),
    l_discount = CASE WHEN l_discount >= 0.09 THEN 0.08 ELSE l_discount + 0.01 END,
    l_shipdate = DATE '1995-04-01'
WHERE l_orderkey IN (
  SELECT o.o_orderkey
  FROM customer c
  JOIN orders o ON o.o_custkey = c.c_custkey
  JOIN lineitem l ON l.l_orderkey = o.o_orderkey
  WHERE c.c_mktsegment = 'BUILDING'
    AND o.o_orderdate < DATE '1995-03-15'
    AND l.l_shipdate > DATE '1995-03-15'
  GROUP BY o.o_orderkey
  ORDER BY SUM(l.l_extendedprice * (1 - l.l_discount)) DESC, o.o_orderkey
  LIMIT 8
);

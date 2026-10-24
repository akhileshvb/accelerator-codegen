UPDATE customer
SET c_mktsegment = 'HOUSEHOLD'
WHERE c_custkey = (
  SELECT o.o_custkey
  FROM orders o
  JOIN lineitem l ON l.l_orderkey = o.o_orderkey
  WHERE o.o_orderdate < DATE '1995-03-15'
    AND l.l_shipdate > DATE '1995-03-15'
    AND EXISTS (
      SELECT 1 FROM customer c
      WHERE c.c_custkey = o.o_custkey
        AND c.c_mktsegment <> 'HOUSEHOLD'
    )
  GROUP BY o.o_custkey
  ORDER BY SUM(l.l_extendedprice * (1 - l.l_discount)) DESC, o.o_custkey
  LIMIT 1
)
AND c_mktsegment <> 'HOUSEHOLD';

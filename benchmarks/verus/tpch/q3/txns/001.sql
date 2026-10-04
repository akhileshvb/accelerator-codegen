UPDATE customer
SET c_mktsegment = CASE WHEN c_mktsegment = 'BUILDING' THEN 'FURNITURE' ELSE 'BUILDING' END
WHERE c_custkey = (
  SELECT c.c_custkey
  FROM customer c
  JOIN orders o ON o.o_custkey = c.c_custkey
  JOIN lineitem l ON l.l_orderkey = o.o_orderkey
  WHERE c.c_mktsegment = 'BUILDING'
    AND o.o_orderdate < DATE '1995-03-15'
    AND l.l_shipdate > DATE '1995-03-15'
  GROUP BY c.c_custkey
  ORDER BY SUM(l.l_extendedprice * (1 - l.l_discount)) DESC, c.c_custkey
  LIMIT 1
)
AND c_mktsegment = 'BUILDING';

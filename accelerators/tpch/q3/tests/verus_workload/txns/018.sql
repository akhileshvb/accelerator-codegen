UPDATE lineitem
SET l_shipdate = DATE '1995-03-15',
    l_comment = 'SHIPDATE EDGE 20261004'
WHERE l_orderkey = (
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
)
AND l_linenumber = (
  SELECT MIN(x.l_linenumber)
  FROM lineitem x
  WHERE x.l_orderkey = (
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
  )
);

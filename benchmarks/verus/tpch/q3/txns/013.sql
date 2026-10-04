UPDATE orders
SET o_comment = CASE
  WHEN o_orderkey = (
    SELECT x.o_orderkey
    FROM customer c JOIN orders x ON x.o_custkey = c.c_custkey
    JOIN lineitem l ON l.l_orderkey = x.o_orderkey
    WHERE c.c_mktsegment = 'BUILDING' AND x.o_orderdate < DATE '1995-03-15'
      AND l.l_shipdate > DATE '1995-03-15'
    GROUP BY x.o_orderkey
    ORDER BY SUM(l.l_extendedprice * (1 - l.l_discount)) DESC, x.o_orderkey
    LIMIT 1
  ) THEN 'LINE MOVE SOURCE'
  WHEN o_orderkey = (
    SELECT x.o_orderkey
    FROM customer c JOIN orders x ON x.o_custkey = c.c_custkey
    JOIN lineitem l ON l.l_orderkey = x.o_orderkey
    WHERE c.c_mktsegment = 'BUILDING' AND x.o_orderdate < DATE '1995-03-15'
      AND l.l_shipdate > DATE '1995-03-15'
    GROUP BY x.o_orderkey
    ORDER BY SUM(l.l_extendedprice * (1 - l.l_discount)) DESC, x.o_orderkey
    LIMIT 1 OFFSET 1
  ) THEN 'LINE MOVE DEST'
  ELSE o_comment
END
WHERE o_comment IN ('LINE MOVE SOURCE', 'LINE MOVE DEST')
   OR o_orderkey IN (
    SELECT x.o_orderkey
    FROM customer c JOIN orders x ON x.o_custkey = c.c_custkey
    JOIN lineitem l ON l.l_orderkey = x.o_orderkey
    WHERE c.c_mktsegment = 'BUILDING' AND x.o_orderdate < DATE '1995-03-15'
      AND l.l_shipdate > DATE '1995-03-15'
    GROUP BY x.o_orderkey
    ORDER BY SUM(l.l_extendedprice * (1 - l.l_discount)) DESC, x.o_orderkey
    LIMIT 2
  );

UPDATE lineitem
SET l_orderkey = (SELECT MAX(o_orderkey) FROM orders WHERE o_comment = 'LINE MOVE DEST')
WHERE l_orderkey = (SELECT MAX(o_orderkey) FROM orders WHERE o_comment = 'LINE MOVE SOURCE')
  AND l_linenumber = (
    SELECT MIN(x.l_linenumber)
    FROM lineitem x
    WHERE x.l_orderkey = (SELECT MAX(o_orderkey) FROM orders WHERE o_comment = 'LINE MOVE SOURCE')
  );

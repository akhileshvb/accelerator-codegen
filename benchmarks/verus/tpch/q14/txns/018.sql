INSERT INTO lineitem
SELECT * REPLACE ('v14-batch-line' AS l_comment,
                  l_extendedprice + 42.17 AS l_extendedprice,
                  l_shipdate + INTERVAL '1 day' AS l_shipdate,
                  l_linenumber + 20 AS l_linenumber)
FROM lineitem
WHERE l_comment NOT LIKE 'v14-%'
  AND l_shipdate >= DATE '1995-03-01' AND l_shipdate < DATE '1995-10-01'
  AND EXISTS (SELECT 1 FROM part WHERE p_partkey = l_partkey)
ORDER BY l_shipdate, l_orderkey, l_linenumber LIMIT 24;

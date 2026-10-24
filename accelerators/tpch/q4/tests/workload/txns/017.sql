INSERT INTO lineitem
SELECT template.* REPLACE (
    seed.o_orderkey AS l_orderkey, 1 AS l_linenumber,
    seed.o_orderdate + 1 AS l_shipdate,
    seed.o_orderdate + 3 AS l_commitdate, seed.o_orderdate + 4 AS l_receiptdate,
    seed.o_comment AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (SELECT * FROM orders WHERE o_comment IN ('q4:seed:09', 'q4:seed:10')) AS seed;

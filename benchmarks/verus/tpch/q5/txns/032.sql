INSERT INTO orders
SELECT seed.* REPLACE (
    ((SELECT max(o_orderkey) FROM orders) + row_number() OVER (ORDER BY seed.o_orderkey))::INTEGER AS o_orderkey,
    DATE '1995-07-01' AS o_orderdate, 'q5 replacement order' AS o_comment
)
FROM orders seed WHERE o_comment='q5 fixture order' AND o_orderdate=DATE '1994-01-01';
INSERT INTO lineitem
SELECT seed.* REPLACE (
    o_orderkey AS l_orderkey, s_suppkey AS l_suppkey,
    line_number AS l_linenumber, price AS l_extendedprice,
    0.05 AS l_discount, 'q5 replacement order line' AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) seed
JOIN orders ON o_comment = 'q5 replacement order'
JOIN customer ON c_custkey = o_custkey
JOIN supplier ON s_nationkey = c_nationkey AND s_comment IN ('q5 fixture supplier', 'q5 new supplier')
CROSS JOIN (VALUES (1, 800.00), (2, 1700.00)) amounts(line_number, price);
DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment='q5 redo order');
DELETE FROM orders WHERE o_comment='q5 redo order';

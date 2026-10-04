INSERT INTO orders
SELECT seed.* REPLACE (
    ((SELECT max(o_orderkey) FROM orders)+1) AS o_orderkey,
    DATE '1994-08-15' AS o_orderdate, 'q5 benchmark order' AS o_comment
)
FROM orders seed WHERE o_comment='q5 fixture order' ORDER BY o_orderkey LIMIT 1;
INSERT INTO lineitem
SELECT seed.* REPLACE (
    (SELECT o_orderkey FROM orders WHERE o_comment='q5 benchmark order') AS l_orderkey,
    (SELECT s_suppkey FROM supplier JOIN customer ON s_nationkey=c_nationkey
        JOIN orders ON o_custkey=c_custkey
        WHERE o_comment='q5 benchmark order' AND s_comment='q5 fixture supplier') AS l_suppkey,
    1 AS l_linenumber, 1799.99 AS l_extendedprice, 0.05 AS l_discount,
    'q5 benchmark line' AS l_comment
)
FROM lineitem seed ORDER BY l_orderkey, l_linenumber LIMIT 1;

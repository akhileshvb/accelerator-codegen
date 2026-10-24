INSERT INTO lineitem
SELECT seed.* REPLACE (
    ((SELECT max(o_orderkey) FROM orders)+1) AS l_orderkey,
    (SELECT s_suppkey FROM supplier WHERE s_comment='q5 fixture supplier' AND s_nationkey=(SELECT c_nationkey FROM customer WHERE c_custkey=(SELECT o_custkey FROM orders WHERE o_comment='q5 fixture order' ORDER BY o_orderkey LIMIT 1))) AS l_suppkey,
    1 AS l_linenumber, 2500.00 AS l_extendedprice, 0.04 AS l_discount, 'q5 redo line' AS l_comment
) FROM lineitem seed ORDER BY l_orderkey, l_linenumber LIMIT 1;
INSERT INTO orders
SELECT seed.* REPLACE (
    ((SELECT max(o_orderkey) FROM orders) + row_number() OVER (ORDER BY seed.o_orderkey))::INTEGER AS o_orderkey,
    DATE '1994-06-15' AS o_orderdate, 'q5 redo order' AS o_comment
)
FROM orders seed WHERE o_comment = 'q5 fixture order' ORDER BY seed.o_orderkey LIMIT 1;

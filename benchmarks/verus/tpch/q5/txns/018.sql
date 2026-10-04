INSERT INTO customer
SELECT seed.* REPLACE (((SELECT max(c_custkey) FROM customer)+1) AS c_custkey, 'Q5 new customer' AS c_name, 'q5 new customer' AS c_comment)
FROM customer seed WHERE c_comment='q5 fixture customer' ORDER BY c_custkey LIMIT 1;
INSERT INTO supplier
SELECT seed.* REPLACE (((SELECT max(s_suppkey) FROM supplier)+1) AS s_suppkey, 'Q5 new supplier' AS s_name, 'q5 new supplier' AS s_comment)
FROM supplier seed WHERE s_comment='q5 fixture supplier' AND s_nationkey=(SELECT c_nationkey FROM customer WHERE c_comment='q5 new customer') ORDER BY s_suppkey LIMIT 1;
INSERT INTO orders
SELECT seed.* REPLACE (((SELECT max(o_orderkey) FROM orders)+1) AS o_orderkey, (SELECT c_custkey FROM customer WHERE c_comment='q5 new customer') AS o_custkey, DATE '1994-09-01' AS o_orderdate, 'q5 new order' AS o_comment)
FROM orders seed ORDER BY o_orderkey LIMIT 1;
INSERT INTO lineitem
SELECT seed.* REPLACE ((SELECT o_orderkey FROM orders WHERE o_comment='q5 new order') AS l_orderkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q5 new supplier') AS l_suppkey, 1 AS l_linenumber, 4999.99 AS l_extendedprice, 0.06 AS l_discount, 'q5 new line' AS l_comment)
FROM lineitem seed ORDER BY l_orderkey, l_linenumber LIMIT 1;

INSERT INTO orders
SELECT * REPLACE (
    -2147483648 AS o_orderkey, DATE '0001-01-01' AS o_orderdate,
    '1-URGENT' AS o_orderpriority, 'q4:extreme:min' AS o_comment
) FROM orders ORDER BY o_orderkey LIMIT 1;
INSERT INTO orders
SELECT * REPLACE (
    2147483647 AS o_orderkey, DATE '9999-12-31' AS o_orderdate,
    '5-LOW' AS o_orderpriority, 'q4:extreme:max' AS o_comment
) FROM orders ORDER BY o_orderkey LIMIT 1;
INSERT INTO lineitem
SELECT template.* REPLACE (
    extreme.o_orderkey AS l_orderkey, 1 AS l_linenumber,
    DATE '0001-01-01' AS l_shipdate, DATE '0001-01-01' AS l_commitdate,
    DATE '9999-12-31' AS l_receiptdate, extreme.o_comment AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (SELECT * FROM orders WHERE o_comment LIKE 'q4:extreme:%') AS extreme;
UPDATE orders SET o_orderpriority = '1-urgent'
WHERE o_comment = 'q4:text:1';
UPDATE orders SET o_orderpriority = '1-URGENT'
WHERE o_comment = 'q4:text:2';

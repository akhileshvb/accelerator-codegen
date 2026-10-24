INSERT INTO orders
SELECT * REPLACE (
    (SELECT MAX(o_orderkey) + 1 FROM orders) AS o_orderkey,
    DATE '1993-07-15' AS o_orderdate, '2-HIGH' AS o_orderpriority,
    'q4:point' AS o_comment
) FROM orders ORDER BY o_orderkey LIMIT 1;
INSERT INTO lineitem
SELECT * REPLACE (
    (SELECT o_orderkey FROM orders WHERE o_comment = 'q4:point') AS l_orderkey,
    1 AS l_linenumber, DATE '1993-07-16' AS l_shipdate,
    DATE '1993-07-18' AS l_commitdate, DATE '1993-07-20' AS l_receiptdate,
    'q4:point' AS l_comment
) FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1;

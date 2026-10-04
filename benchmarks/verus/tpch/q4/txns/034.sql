INSERT INTO orders
SELECT template.* REPLACE (
    (SELECT MAX(o_orderkey) FROM orders) + text_id::INTEGER AS o_orderkey,
    DATE '1993-07-01' AS o_orderdate,
    CASE text_id
        WHEN 1 THEN '1-URGENT' WHEN 2 THEN '1-urgent' WHEN 3 THEN '1-URGENT '
        WHEN 4 THEN ' 1-URGENT' WHEN 5 THEN '' WHEN 6 THEN ' '
        WHEN 7 THEN 'Å-URGENT' WHEN 8 THEN 'é-HIGH'
        WHEN 9 THEN 'Z-HIGH' ELSE 'z-HIGH' END AS o_orderpriority,
    'q4:text:' || text_id::VARCHAR AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS template
CROSS JOIN range(1, 11) AS variants(text_id);
INSERT INTO lineitem
SELECT template.* REPLACE (
    variant.o_orderkey AS l_orderkey, 1 AS l_linenumber,
    DATE '1993-07-01' AS l_commitdate, DATE '1993-07-02' AS l_receiptdate,
    variant.o_comment AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (SELECT * FROM orders WHERE o_comment LIKE 'q4:text:%') AS variant;

INSERT INTO lineitem
SELECT template.* REPLACE (
    (SELECT MAX(o_orderkey) FROM orders) + batch_id::INTEGER AS l_orderkey,
    1 AS l_linenumber, DATE '1993-09-02' AS l_shipdate,
    DATE '1993-09-04' AS l_commitdate, DATE '1993-09-06' AS l_receiptdate,
    'q4:linefirst:' || batch_id::VARCHAR AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN range(1, 7) AS batch(batch_id);
INSERT INTO orders
SELECT template.* REPLACE (
    pending.l_orderkey AS o_orderkey, DATE '1993-09-01' AS o_orderdate,
    '3-MEDIUM' AS o_orderpriority, pending.l_comment AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS template
CROSS JOIN (SELECT * FROM lineitem WHERE l_comment LIKE 'q4:linefirst:%') AS pending;

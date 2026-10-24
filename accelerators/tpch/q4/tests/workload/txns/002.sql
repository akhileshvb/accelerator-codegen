INSERT INTO orders
SELECT template.* REPLACE (
    (SELECT MAX(o_orderkey) FROM orders) + batch_id::INTEGER AS o_orderkey,
    DATE '1993-08-01' + batch_id::INTEGER AS o_orderdate,
    CASE WHEN batch_id % 2 = 0 THEN '1-URGENT' ELSE '5-LOW' END AS o_orderpriority,
    'q4:batch:' || batch_id::VARCHAR AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS template
CROSS JOIN range(1, 9) AS batch(batch_id);
INSERT INTO lineitem
SELECT template.* REPLACE (
    pending.o_orderkey AS l_orderkey, line_number::INTEGER AS l_linenumber,
    pending.o_orderdate + 1 AS l_shipdate,
    pending.o_orderdate + 2 AS l_commitdate,
    pending.o_orderdate + 5 AS l_receiptdate,
    pending.o_comment AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (SELECT * FROM orders WHERE o_comment LIKE 'q4:batch:%') AS pending
CROSS JOIN range(1, 4) AS lines(line_number);

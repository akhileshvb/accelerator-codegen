INSERT INTO orders
SELECT template.* REPLACE (
    (SELECT MAX(o_orderkey) FROM orders) + batch_id::INTEGER AS o_orderkey,
    DATE '1995-01-01' + batch_id::INTEGER AS o_orderdate,
    CASE batch_id % 3 WHEN 0 THEN '1-URGENT' WHEN 1 THEN '2-HIGH' ELSE '3-MEDIUM' END AS o_orderpriority,
    'q4:replacement:' || batch_id::VARCHAR AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS template
CROSS JOIN range(1, 25) AS batch(batch_id);
INSERT INTO lineitem
SELECT template.* REPLACE (
    pending.o_orderkey AS l_orderkey, line_number::INTEGER AS l_linenumber,
    pending.o_orderdate + 1 AS l_shipdate,
    pending.o_orderdate + 3 AS l_commitdate,
    pending.o_orderdate + CASE WHEN line_number = 1 THEN 6 ELSE 3 END AS l_receiptdate,
    pending.o_comment AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (SELECT * FROM orders WHERE o_comment LIKE 'q4:replacement:%') AS pending
CROSS JOIN range(1, 4) AS lines(line_number);

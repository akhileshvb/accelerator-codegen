INSERT INTO orders
SELECT template.* REPLACE (
    (SELECT MAX(o_orderkey) FROM orders) + fixture_id::INTEGER AS o_orderkey,
    DATE '1993-07-01' + ((fixture_id - 1) * 4)::INTEGER AS o_orderdate,
    CASE (fixture_id - 1) % 5
        WHEN 0 THEN '1-URGENT' WHEN 1 THEN '2-HIGH' WHEN 2 THEN '3-MEDIUM'
        WHEN 3 THEN '4-NOT SPECIFIED' ELSE '5-LOW' END AS o_orderpriority,
    'q4:seed:' || lpad(fixture_id::VARCHAR, 2, '0') AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS template
CROSS JOIN range(1, 21) AS fixtures(fixture_id);

INSERT INTO lineitem
SELECT template.* REPLACE (
    seed.o_orderkey AS l_orderkey,
    line_number::INTEGER AS l_linenumber,
    seed.o_orderdate + 1 AS l_shipdate,
    seed.o_orderdate + 3 AS l_commitdate,
    seed.o_orderdate + CASE WHEN line_number = 1 THEN 4 ELSE 3 END AS l_receiptdate,
    seed.o_comment AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (SELECT * FROM orders WHERE o_comment LIKE 'q4:seed:%') AS seed
CROSS JOIN range(1, 3) AS lines(line_number);

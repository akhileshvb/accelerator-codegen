INSERT INTO orders
SELECT template.* REPLACE (
    (SELECT GREATEST((SELECT MAX(o_orderkey) FROM orders), (SELECT MAX(l_orderkey) FROM lineitem)) + 1) AS o_orderkey,
    'O' AS o_orderstatus,
    21000.00 AS o_totalprice,
    DATE '1998-07-01' AS o_orderdate,
    'q1-new-seven' AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS template;

INSERT INTO lineitem
SELECT template.* REPLACE (
    (SELECT o_orderkey FROM orders WHERE o_comment = 'q1-new-seven') AS l_orderkey,
    CAST(fixture.sequence AS INTEGER) AS l_linenumber,
    CAST(10 + fixture.sequence AS DECIMAL(15,2)) AS l_quantity,
    CAST(2500 + fixture.sequence * 100.01 AS DECIMAL(15,2)) AS l_extendedprice,
    0.02 AS l_discount,
    0.05 AS l_tax,
    'N' AS l_returnflag,
    'O' AS l_linestatus,
    DATE '1998-07-20' + CAST(fixture.sequence AS INTEGER) AS l_shipdate,
    DATE '1998-07-20' + CAST(fixture.sequence AS INTEGER) AS l_commitdate,
    DATE '1998-08-01' AS l_receiptdate,
    'q1-new-seven' AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_shipdate, l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN range(1, 8) AS fixture(sequence);

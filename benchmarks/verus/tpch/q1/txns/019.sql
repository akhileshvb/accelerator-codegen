INSERT INTO orders
SELECT template.* REPLACE (
    CAST((SELECT GREATEST((SELECT MAX(o_orderkey) FROM orders), (SELECT MAX(l_orderkey) FROM lineitem))) + fixture.sequence AS INTEGER) AS o_orderkey,
    'O' AS o_orderstatus,
    12000.00 AS o_totalprice,
    DATE '1998-06-01' AS o_orderdate,
    'q1-new-bulk' AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS template
CROSS JOIN range(1, 7) AS fixture(sequence);

INSERT INTO lineitem
SELECT template.* REPLACE (
    new_order.o_orderkey AS l_orderkey,
    CAST(fixture.sequence AS INTEGER) AS l_linenumber,
    CAST(5 + fixture.sequence AS DECIMAL(15,2)) AS l_quantity,
    CAST(2000 + fixture.sequence * 250.05 AS DECIMAL(15,2)) AS l_extendedprice,
    0.04 AS l_discount,
    0.03 AS l_tax,
    'N' AS l_returnflag,
    'O' AS l_linestatus,
    DATE '1998-06-20' + CAST(fixture.sequence AS INTEGER) AS l_shipdate,
    DATE '1998-06-20' + CAST(fixture.sequence AS INTEGER) AS l_commitdate,
    DATE '1998-07-01' AS l_receiptdate,
    'q1-new-bulk' AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_shipdate, l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (SELECT o_orderkey FROM orders WHERE o_comment = 'q1-new-bulk') AS new_order
CROSS JOIN range(1, 5) AS fixture(sequence);

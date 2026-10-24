INSERT INTO orders
SELECT source.* REPLACE (
    (SELECT GREATEST(MAX(o_orderkey), (SELECT MAX(l_orderkey) FROM lineitem)) FROM orders) + fixture.number AS o_orderkey,
    DATE '1995-02-01' AS o_orderdate,
    'O' AS o_orderstatus,
    18000.00 AS o_totalprice,
    'q6-batch-order-' || CAST(fixture.number AS VARCHAR) AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS source
CROSS JOIN range(1, 9) AS fixture(number);
INSERT INTO lineitem
SELECT source.* REPLACE (
    new_order.o_orderkey AS l_orderkey,
    fixture.number AS l_linenumber,
    CAST(8 + fixture.number AS DECIMAL(15,2)) AS l_quantity,
    CAST(3000 + fixture.number * 100 AS DECIMAL(15,2)) AS l_extendedprice,
    CAST(0.03 + fixture.number * 0.01 AS DECIMAL(15,2)) AS l_discount,
    DATE '1995-03-01' + CAST(fixture.number AS INTEGER) AS l_shipdate,
    DATE '1995-02-25' AS l_commitdate,
    DATE '1995-03-10' AS l_receiptdate,
    new_order.o_comment AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source
CROSS JOIN (SELECT * FROM orders WHERE o_comment LIKE 'q6-batch-order-%') AS new_order
CROSS JOIN range(1, 5) AS fixture(number);

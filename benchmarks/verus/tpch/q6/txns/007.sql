INSERT INTO orders
SELECT source.* REPLACE (
    (SELECT GREATEST(MAX(o_orderkey), (SELECT MAX(l_orderkey) FROM lineitem)) + 1 FROM orders) AS o_orderkey,
    DATE '1994-05-01' AS o_orderdate,
    'O' AS o_orderstatus,
    40000.00 AS o_totalprice,
    'q6-new-order-a' AS o_comment
)
FROM orders AS source ORDER BY o_orderkey LIMIT 1;
INSERT INTO lineitem
SELECT source.* REPLACE (
    (SELECT o_orderkey FROM orders WHERE o_comment = 'q6-new-order-a') AS l_orderkey,
    fixture.number AS l_linenumber,
    fixture.quantity AS l_quantity,
    fixture.price AS l_extendedprice,
    fixture.discount AS l_discount,
    DATE '1994-06-01' AS l_shipdate,
    DATE '1994-05-25' AS l_commitdate,
    DATE '1994-06-05' AS l_receiptdate,
    'q6-new-order-a' AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source
CROSS JOIN (VALUES (1, 12.00, 14000.00, 0.06), (2, 23.00, 22000.00, 0.07), (3, 30.00, 4000.00, 0.04))
AS fixture(number, quantity, price, discount);

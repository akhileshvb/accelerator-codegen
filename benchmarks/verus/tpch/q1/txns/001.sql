INSERT INTO orders
SELECT template.* REPLACE (
    (SELECT GREATEST((SELECT MAX(o_orderkey) FROM orders), (SELECT MAX(l_orderkey) FROM lineitem)) + 1) AS o_orderkey,
    'O' AS o_orderstatus,
    4800.00 AS o_totalprice,
    DATE '1998-08-20' AS o_orderdate,
    'q1-new-small' AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS template;

INSERT INTO lineitem
SELECT template.* REPLACE (
    (SELECT o_orderkey FROM orders WHERE o_comment = 'q1-new-small') AS l_orderkey,
    fixture.linenumber AS l_linenumber,
    fixture.quantity AS l_quantity,
    fixture.price AS l_extendedprice,
    0.05 AS l_discount,
    0.02 AS l_tax,
    'N' AS l_returnflag,
    'O' AS l_linestatus,
    fixture.shipdate AS l_shipdate,
    fixture.shipdate AS l_commitdate,
    fixture.shipdate + 3 AS l_receiptdate,
    'q1-new-small' AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_shipdate, l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (VALUES
    (1, 10.00, 1000.00, DATE '1998-08-25'),
    (2, 15.00, 1800.00, DATE '1998-09-02'),
    (3, 20.00, 2000.00, DATE '1998-10-02')
) AS fixture(linenumber, quantity, price, shipdate);

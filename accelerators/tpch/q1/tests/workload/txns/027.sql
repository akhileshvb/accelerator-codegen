UPDATE lineitem
SET l_orderkey = (SELECT MAX(l_orderkey) + 1 FROM lineitem),
    l_linenumber = l_linenumber + 1000,
    l_returnflag = 'r', l_linestatus = ' ',
    l_quantity = 0.01, l_extendedprice = 0.01,
    l_discount = 0.99, l_tax = 0.01
WHERE l_comment = 'q1-exact-duplicate';

INSERT INTO lineitem
SELECT template.* REPLACE (
    fixture.linenumber AS l_linenumber,
    fixture.quantity AS l_quantity,
    fixture.price AS l_extendedprice,
    fixture.discount AS l_discount,
    fixture.tax AS l_tax,
    fixture.returnflag AS l_returnflag,
    fixture.linestatus AS l_linestatus,
    DATE '1998-09-02' AS l_shipdate,
    DATE '1998-09-02' AS l_commitdate,
    DATE '1998-09-05' AS l_receiptdate,
    'q1-extreme-text' AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_shipdate, l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (VALUES
    (201, 'Z', 'F', 9999999999999.99, 9999999999999.99, 0.00, 0.08),
    (202, 'Z', 'F', -9999999999999.99, -9999999999999.99, 0.00, 0.08),
    (203, 'z', 'F', 0.00, 0.00, 1.00, 0.00),
    (204, ' ', ' ', 0.01, 0.01, 0.99, 0.01),
    (205, '', '', -0.01, 0.01, -0.01, -0.01),
    (206, 'é', 'O', 1.01, 123.45, 1.01, 1.00),
    (207, 'N', 'o', 3.33, 9999999999999.99, 0.03, 0.08)
) AS fixture(linenumber, returnflag, linestatus, quantity, price, discount, tax);

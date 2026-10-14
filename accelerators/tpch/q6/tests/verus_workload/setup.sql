INSERT INTO lineitem
SELECT source.* REPLACE (
    (SELECT MAX(l_orderkey) + 1 FROM lineitem) AS l_orderkey,
    fixture.number AS l_linenumber,
    fixture.quantity AS l_quantity,
    12345.67 AS l_extendedprice,
    fixture.discount AS l_discount,
    fixture.shipdate AS l_shipdate,
    fixture.shipdate AS l_commitdate,
    fixture.shipdate + 1 AS l_receiptdate,
    'q6-boundary-' || CAST(fixture.number AS VARCHAR) AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source
CROSS JOIN (VALUES
    (1, DATE '1993-12-31', 0.06, 23.99),
    (2, DATE '1994-01-01', 0.06, 23.99),
    (3, DATE '1994-12-31', 0.06, 23.99),
    (4, DATE '1995-01-01', 0.06, 23.99),
    (5, DATE '1994-06-15', 0.04, 23.99),
    (6, DATE '1994-06-15', 0.05, 23.99),
    (7, DATE '1994-06-15', 0.06, 23.99),
    (8, DATE '1994-06-15', 0.07, 23.99),
    (9, DATE '1994-06-15', 0.08, 23.99),
    (10, DATE '1994-06-15', 0.06, 24.00),
    (11, DATE '1994-06-15', 0.06, 24.01),
    (12, DATE '1994-06-15', 0.06, 1.00),
    (13, DATE '1994-06-15', 0.06, 50.00)
) AS fixture(number, shipdate, discount, quantity);

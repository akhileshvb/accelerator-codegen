DELETE FROM lineitem WHERE l_comment = 'q6-isolated-1';
INSERT INTO lineitem
SELECT source.* REPLACE (
    (SELECT MAX(l_orderkey) + 1 FROM lineitem) AS l_orderkey,
    fixture.number AS l_linenumber,
    fixture.quantity AS l_quantity,
    fixture.price AS l_extendedprice,
    fixture.discount AS l_discount,
    'q6-extreme-' || CAST(fixture.number AS VARCHAR) AS l_comment
)
FROM (SELECT * FROM lineitem WHERE l_comment = 'q6-isolated-2') AS source
CROSS JOIN (VALUES
    (1, -0.01, 123.45, -0.06),
    (2, 9999999999999.98, 9999999999999.99, 9999999999999.99),
    (3, 9999999999999.99, 9999999999999.99, 9999999999999.99),
    (4, 12.00, 9999999999999.99, 0.06)
) AS fixture(number, quantity, price, discount);

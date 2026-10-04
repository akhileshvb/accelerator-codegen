INSERT INTO lineitem
SELECT source.* REPLACE (
    (SELECT MAX(l_orderkey) + 1 FROM lineitem) AS l_orderkey,
    fixture.number AS l_linenumber,
    12.00 AS l_quantity,
    fixture.price AS l_extendedprice,
    fixture.discount AS l_discount,
    DATE '2000-02-29' AS l_shipdate,
    DATE '2000-02-28' AS l_commitdate,
    DATE '2000-03-01' AS l_receiptdate,
    'q6-isolated-' || CAST(fixture.number AS VARCHAR) AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source
CROSS JOIN (VALUES (1, 0.00, 0.06), (2, 0.00, 0.06), (3, 100.00, 0.00))
AS fixture(number, price, discount);

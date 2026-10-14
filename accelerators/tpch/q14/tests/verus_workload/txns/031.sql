INSERT INTO lineitem
SELECT source.* REPLACE (
    CASE WHEN fixture.slot % 2 = 1
        THEN (SELECT p_partkey FROM part WHERE p_comment = 'v14-promo')
        ELSE (SELECT p_partkey FROM part WHERE p_comment = 'v14-regular') END AS l_partkey,
    fixture.price AS l_extendedprice,
    fixture.discount AS l_discount,
    DATE '1995-09-01' AS l_shipdate,
    'v14-extreme-' || CAST(fixture.slot AS VARCHAR) AS l_comment
)
FROM (SELECT * FROM lineitem WHERE l_comment = 'v14-base-2') source
CROSS JOIN (VALUES
    (1, CAST(9999999999999.99 AS DECIMAL(15,2)), CAST(0.01 AS DECIMAL(15,2))),
    (2, CAST(-4999999999999.99 AS DECIMAL(15,2)), CAST(0.00 AS DECIMAL(15,2))),
    (3, CAST(0.01 AS DECIMAL(15,2)), CAST(0.99 AS DECIMAL(15,2))),
    (4, CAST(0.00 AS DECIMAL(15,2)), CAST(0.00 AS DECIMAL(15,2))),
    (5, CAST(100.00 AS DECIMAL(15,2)), CAST(1.00 AS DECIMAL(15,2)))
) fixture(slot, price, discount);

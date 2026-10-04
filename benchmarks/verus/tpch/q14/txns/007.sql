INSERT INTO orders
SELECT * REPLACE (
    (SELECT max(o_orderkey) + 1 FROM orders) AS o_orderkey,
    'O' AS o_orderstatus,
    26340.50 AS o_totalprice,
    DATE '1995-08-25' AS o_orderdate,
    'v14-new-order' AS o_comment
)
FROM orders ORDER BY o_orderkey LIMIT 1;

INSERT INTO lineitem
SELECT source.* REPLACE (
    (SELECT o_orderkey FROM orders WHERE o_comment = 'v14-new-order') AS l_orderkey,
    fixture.slot AS l_linenumber,
    CASE WHEN fixture.slot = 2
        THEN (SELECT p_partkey FROM part WHERE p_comment = 'v14-regular')
        ELSE (SELECT p_partkey FROM part WHERE p_comment = 'v14-promo')
    END AS l_partkey,
    CAST(8000 + fixture.slot * 390.25 AS DECIMAL(15,2)) AS l_extendedprice,
    0.05 AS l_discount,
    fixture.shipdate AS l_shipdate,
    fixture.shipdate - INTERVAL '1 day' AS l_commitdate,
    fixture.shipdate + INTERVAL '2 days' AS l_receiptdate,
    'v14-new-order-line' AS l_comment
)
FROM (SELECT * FROM lineitem WHERE l_comment = 'v14-base-1') source
CROSS JOIN (VALUES (1, DATE '1995-08-31'), (2, DATE '1995-09-01'),
                   (3, DATE '1995-09-30')) fixture(slot, shipdate);

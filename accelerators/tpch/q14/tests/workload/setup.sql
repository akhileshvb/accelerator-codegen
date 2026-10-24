INSERT INTO part
SELECT * REPLACE (
    (SELECT max(p_partkey) + 1 FROM part) AS p_partkey,
    'PROMO POLISHED STEEL' AS p_type,
    'v14-promo' AS p_comment
)
FROM part
WHERE p_type LIKE 'PROMO%'
ORDER BY p_partkey LIMIT 1;

INSERT INTO part
SELECT * REPLACE (
    (SELECT max(p_partkey) + 1 FROM part) AS p_partkey,
    'STANDARD POLISHED STEEL' AS p_type,
    'v14-regular' AS p_comment
)
FROM part
WHERE p_type NOT LIKE 'PROMO%'
ORDER BY p_partkey LIMIT 1;

INSERT INTO lineitem
SELECT source.* REPLACE (
    CASE WHEN fixture.slot % 2 = 1
        THEN (SELECT p_partkey FROM part WHERE p_comment = 'v14-promo')
        ELSE (SELECT p_partkey FROM part WHERE p_comment = 'v14-regular')
    END AS l_partkey,
    source.l_linenumber + fixture.slot AS l_linenumber,
    CAST(10000 + fixture.slot * 321.07 AS DECIMAL(15,2)) AS l_extendedprice,
    CAST(0.01 * fixture.slot AS DECIMAL(15,2)) AS l_discount,
    fixture.shipdate AS l_shipdate,
    fixture.shipdate - INTERVAL '2 days' AS l_commitdate,
    fixture.shipdate + INTERVAL '3 days' AS l_receiptdate,
    'v14-base-' || CAST(fixture.slot AS VARCHAR) AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) source
CROSS JOIN (VALUES
    (1, DATE '1995-08-31'),
    (2, DATE '1995-09-01'),
    (3, DATE '1995-09-15'),
    (4, DATE '1995-09-30'),
    (5, DATE '1995-10-01'),
    (6, DATE '1995-10-15'),
    (7, DATE '1996-02-29'),
    (8, DATE '1998-11-20')
) fixture(slot, shipdate);

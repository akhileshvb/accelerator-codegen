INSERT INTO part
SELECT * REPLACE (
    (SELECT max(p_partkey) + 1 FROM part) AS p_partkey,
    'STANDARD BRUSHED COPPER' AS p_type,
    'v14-new' AS p_comment
)
FROM part WHERE p_comment = 'v14-regular';

INSERT INTO lineitem
SELECT source.* REPLACE (
    (SELECT p_partkey FROM part WHERE p_comment = 'v14-new') AS l_partkey,
    source.l_linenumber + fixture.slot AS l_linenumber,
    CAST(14000 + fixture.slot * 21.19 AS DECIMAL(15,2)) AS l_extendedprice,
    fixture.shipdate AS l_shipdate,
    'v14-new-line' AS l_comment
)
FROM (SELECT * FROM lineitem WHERE l_comment = 'v14-base-2') source
CROSS JOIN (VALUES (1, DATE '1994-09-10'), (2, DATE '1995-06-15'),
                   (3, DATE '1995-09-20')) fixture(slot, shipdate);

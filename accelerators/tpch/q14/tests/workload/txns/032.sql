DELETE FROM lineitem WHERE l_comment LIKE 'v14-extreme-%';
UPDATE lineitem SET l_extendedprice = 0.00
WHERE l_comment = 'v14-orphan';
INSERT INTO lineitem
SELECT * REPLACE (
    (SELECT p_partkey FROM part WHERE p_comment = 'v14-regular') AS l_partkey,
    100.00 AS l_extendedprice, 1.00 AS l_discount,
    'v14-zero-denominator' AS l_comment
)
FROM lineitem WHERE l_comment = 'v14-orphan';

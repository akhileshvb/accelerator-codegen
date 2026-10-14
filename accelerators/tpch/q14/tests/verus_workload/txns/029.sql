DELETE FROM part WHERE p_partkey IN
    (SELECT p_partkey FROM part WHERE p_comment = 'v14-promo');
UPDATE lineitem SET l_extendedprice = l_extendedprice + 333.33,
                    l_discount = 0.02
WHERE l_comment = 'v14-base-3';
INSERT INTO lineitem
SELECT * REPLACE (DATE '2001-01-15' AS l_shipdate,
                  'v14-orphan' AS l_comment,
                  123.45 AS l_extendedprice, 0.01 AS l_discount)
FROM lineitem WHERE l_comment = 'v14-base-3'
ORDER BY l_orderkey, l_linenumber LIMIT 1;

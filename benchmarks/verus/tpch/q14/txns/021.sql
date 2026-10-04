INSERT INTO part
SELECT * REPLACE ((SELECT max(p_partkey) + 1 FROM part) AS p_partkey,
                  'PROMO BURNISHED NICKEL' AS p_type,
                  'v14-new' AS p_comment)
FROM part WHERE p_comment = 'v14-promo';
INSERT INTO lineitem
SELECT * REPLACE ((SELECT p_partkey FROM part WHERE p_comment = 'v14-new') AS l_partkey,
                  'v14-new-line' AS l_comment,
                  l_extendedprice + 230.00 AS l_extendedprice)
FROM lineitem WHERE l_comment IN ('v14-base-2', 'v14-base-4');

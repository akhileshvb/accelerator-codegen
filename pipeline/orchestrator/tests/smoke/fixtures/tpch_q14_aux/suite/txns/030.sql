INSERT INTO lineitem
SELECT * REPLACE (DATE '1995-09-01' AS l_shipdate,
                  'v14-rejoin-line' AS l_comment, 23456.78 AS l_extendedprice)
FROM lineitem WHERE l_comment = 'v14-orphan';
INSERT INTO part
SELECT * REPLACE (
    (SELECT l_partkey FROM lineitem WHERE l_comment = 'v14-orphan') AS p_partkey,
    'PROMO BRUSHED STEEL' AS p_type,
    'v14-promo' AS p_comment
)
FROM part WHERE p_comment = 'v14-regular';
UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01,
                    l_shipdate = DATE '1995-09-01'
WHERE l_comment = 'v14-base-3';
UPDATE part SET p_type = 'PROMO POLISHED STEEL'
WHERE p_comment = 'v14-promo';

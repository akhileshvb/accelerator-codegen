INSERT INTO part
SELECT template.* REPLACE (
    (SELECT max(p_partkey) + 1 FROM part) AS p_partkey,
    'q19-restock' AS p_name, 'Brand#55' AS p_brand,
    'LG BOX' AS p_container, 13 AS p_size, 'q19 restock' AS p_comment
) FROM part AS template WHERE p_name = 'q19-stock-12';
UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.75 WHERE l_comment = 'q19-line-12';

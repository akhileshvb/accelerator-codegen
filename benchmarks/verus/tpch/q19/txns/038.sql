INSERT INTO part
SELECT template.* REPLACE (
    (SELECT max(p_partkey) + 1 FROM part) AS p_partkey,
    'q19-zero' AS p_name, 'Zero#19' AS p_brand, 'SM BOX' AS p_container,
    1 AS p_size, 'q19 zero' AS p_comment
) FROM part AS template WHERE p_name = 'q19-stock-01';
INSERT INTO lineitem
SELECT template.* REPLACE (
    (SELECT p_partkey FROM part WHERE p_name = 'q19-zero') AS l_partkey,
    5.00 AS l_quantity, 0.00 AS l_extendedprice, 0.00 AS l_discount,
    'q19-zero-line' AS l_comment
) FROM lineitem AS template WHERE l_comment = 'q19-line-01' AND l_linenumber = 1;

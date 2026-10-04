UPDATE part SET p_size = 0 WHERE p_name = 'q19-stock-01';
INSERT INTO lineitem
SELECT template.* REPLACE (6 AS l_linenumber, 6.00 AS l_quantity, 752.22 AS l_extendedprice)
FROM lineitem AS template WHERE l_comment = 'q19-line-01' AND l_linenumber = 1;

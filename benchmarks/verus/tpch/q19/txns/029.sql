INSERT INTO lineitem
SELECT template.* REPLACE (
    2 AS l_linenumber, l_quantity + 5 AS l_quantity,
    l_extendedprice + 626.85 AS l_extendedprice, 'AIR REG' AS l_shipmode
) FROM lineitem AS template WHERE l_comment LIKE 'q19-line-%' AND l_linenumber = 1;

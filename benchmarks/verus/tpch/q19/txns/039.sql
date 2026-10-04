UPDATE lineitem SET l_quantity = 9999999999999.99,
    l_extendedprice = 9999999999999.99, l_discount = 0.99
WHERE l_comment = 'q19-zero-line';
INSERT INTO lineitem
SELECT template.* REPLACE (
    7 AS l_linenumber, 5.00 AS l_quantity, 123.45 AS l_extendedprice, 1.00 AS l_discount
) FROM lineitem AS template WHERE l_comment = 'q19-zero-line';

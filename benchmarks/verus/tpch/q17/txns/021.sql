UPDATE lineitem SET l_quantity = 50.00 WHERE l_comment = 'q17-line-first' AND l_linenumber = 5;
UPDATE part SET p_brand = 'Brand#35', p_container = 'LG BOX' WHERE p_name = 'q17-line-first';
UPDATE lineitem SET l_extendedprice = l_extendedprice + 28.00 WHERE l_comment = 'q17-line-first' AND l_linenumber <= 2;

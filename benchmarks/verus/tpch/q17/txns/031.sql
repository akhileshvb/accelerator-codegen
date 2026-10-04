UPDATE lineitem SET l_quantity = -9999999999999.99, l_extendedprice = 9999999999999.99 WHERE l_comment = 'q17-boundary' AND l_linenumber = 1;
UPDATE lineitem SET l_quantity = 9999999999999.99 WHERE l_comment = 'q17-boundary' AND l_linenumber = 2;
UPDATE lineitem SET l_quantity = -9999999999999.99, l_extendedprice = -9999999999999.98 WHERE l_comment = 'q17-boundary' AND l_linenumber = 3;

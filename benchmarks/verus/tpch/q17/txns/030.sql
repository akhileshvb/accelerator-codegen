UPDATE lineitem SET l_quantity = -1.00, l_extendedprice = -7.00 WHERE l_comment = 'q17-boundary' AND l_linenumber = 1;
INSERT INTO lineitem SELECT * REPLACE (3 AS l_linenumber, 7.00 AS l_extendedprice) FROM lineitem WHERE l_comment = 'q17-boundary' AND l_linenumber = 1;

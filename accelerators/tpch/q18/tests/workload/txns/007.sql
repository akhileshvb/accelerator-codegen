UPDATE lineitem SET l_quantity = l_quantity + 42.00
WHERE l_comment = 'q18:new:line' AND l_linenumber = 1;

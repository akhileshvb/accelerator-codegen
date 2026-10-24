UPDATE orders SET o_orderkey = (SELECT MAX(o_orderkey) + 1 FROM orders)
WHERE o_comment = 'q4:seed:14';
UPDATE lineitem SET l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'q4:seed:14')
WHERE l_comment = 'q4:seed:15' AND l_linenumber = 1;

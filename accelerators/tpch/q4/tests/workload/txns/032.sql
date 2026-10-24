UPDATE lineitem SET l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'q4:seed:16')
WHERE l_comment = 'q4:seed:15' AND l_linenumber = 1;
UPDATE orders SET o_orderkey = (SELECT l_orderkey FROM lineitem WHERE l_comment = 'q4:seed:14' AND l_linenumber = 1)
WHERE o_comment = 'q4:seed:14';
UPDATE lineitem SET l_receiptdate = l_commitdate
WHERE l_comment = 'q4:seed:17' AND l_linenumber = 1;

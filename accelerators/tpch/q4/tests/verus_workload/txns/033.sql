UPDATE lineitem SET l_commitdate = l_receiptdate
WHERE l_comment = 'q4:seed:18' AND l_linenumber = 1;
UPDATE lineitem SET l_commitdate = l_receiptdate - 1
WHERE l_comment = 'q4:seed:18' AND l_linenumber = 2;
UPDATE orders SET o_orderdate = DATE '1993-07-01', o_orderpriority = '1-URGENT'
WHERE o_comment = 'q4:seed:18';
UPDATE lineitem SET l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'q4:seed:17'),
    l_receiptdate = l_commitdate + 1
WHERE l_comment = 'q4:seed:17' AND l_linenumber = 2;

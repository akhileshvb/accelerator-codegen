UPDATE orders SET o_orderdate = DATE '1993-10-01', o_orderpriority = '1-URGENT'
WHERE o_comment IN ('q4:seed:11', 'q4:seed:12');
UPDATE lineitem SET l_commitdate = l_receiptdate
WHERE l_comment IN ('q4:seed:11', 'q4:seed:12') AND l_linenumber = 1;

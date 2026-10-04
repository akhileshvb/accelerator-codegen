UPDATE orders SET o_orderpriority = '2-HIGH', o_totalprice = o_totalprice + 10.00
WHERE o_comment = 'q12_new_a';
UPDATE lineitem SET l_shipmode = 'TRUCK'
WHERE l_comment = 'q12_new_a_line' AND l_linenumber = 1;

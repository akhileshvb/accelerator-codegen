UPDATE lineitem SET l_extendedprice = l_extendedprice + 50.00, l_discount = 0.05
WHERE l_comment = 'q6-new-order-a' AND l_linenumber = 1;
UPDATE orders SET o_totalprice = o_totalprice + 50.00, o_orderstatus = 'P'
WHERE o_comment = 'q6-new-order-a';

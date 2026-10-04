DELETE FROM lineitem WHERE l_comment = 'q6-added-line';
UPDATE orders SET o_totalprice = o_totalprice - 9876.54 WHERE o_comment = 'q6-batch-order-3';
UPDATE lineitem SET l_discount = 0.05, l_extendedprice = l_extendedprice + 200.00
WHERE l_comment = 'q6-batch-order-4' AND l_linenumber = 1;

UPDATE lineitem SET l_quantity = l_quantity - 15.00, l_extendedprice = l_extendedprice - 1000.00
WHERE l_comment LIKE 'q6-batch-order-%' AND l_linenumber <= 2;
UPDATE orders SET o_totalprice = o_totalprice - 2000.00 WHERE o_comment LIKE 'q6-batch-order-%';

UPDATE lineitem SET l_extendedprice = 2000.00, l_discount = 0.05 WHERE l_comment = 'q15-point-line';
UPDATE orders SET o_totalprice = 1900.00 WHERE o_comment = 'q15-point-order';

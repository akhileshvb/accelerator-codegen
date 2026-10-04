UPDATE orders SET o_orderdate = DATE '1995-04-12' WHERE o_comment = 'q8-order-01';
UPDATE lineitem SET l_extendedprice = 1000.01, l_discount = 0.03 WHERE l_comment = 'q8-line-01-1';

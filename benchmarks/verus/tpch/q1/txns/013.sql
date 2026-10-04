UPDATE orders SET o_totalprice = o_totalprice + 700.00
WHERE o_comment = 'q1-new-seven';

UPDATE lineitem
SET l_extendedprice = l_extendedprice + 100.00, l_discount = 0.06
WHERE l_comment = 'q1-new-seven';

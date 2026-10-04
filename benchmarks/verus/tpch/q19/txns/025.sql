UPDATE orders SET o_totalprice = o_totalprice + 100, o_orderstatus = 'F'
WHERE o_comment IN ('q19-order-a','q19-order-b');
UPDATE lineitem SET l_extendedprice = l_extendedprice + 15.99, l_discount = 0.09
WHERE l_comment IN ('q19-order-a-line','q19-order-b-line');

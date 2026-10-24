UPDATE lineitem
SET l_extendedprice = l_extendedprice + 250.00,
    l_discount = 0.08,
    l_shipdate = l_shipdate + INTERVAL '1 day'
WHERE l_comment = 'v14-new-order-line';
UPDATE orders SET o_totalprice = 27090.50
WHERE o_comment = 'v14-new-order';

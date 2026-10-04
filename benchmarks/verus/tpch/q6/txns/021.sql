UPDATE lineitem SET l_quantity = 22.00, l_extendedprice = 12000.01, l_discount = 0.08,
    l_shipdate = DATE '1996-01-01'
WHERE l_comment = 'q6-added-line';
UPDATE orders SET o_totalprice = o_totalprice + 2123.47 WHERE o_comment = 'q6-batch-order-3';

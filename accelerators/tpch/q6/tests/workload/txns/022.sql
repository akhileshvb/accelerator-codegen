UPDATE lineitem SET l_quantity = 18.00, l_extendedprice = 9876.54, l_discount = 0.06,
    l_shipdate = DATE '1995-08-15'
WHERE l_comment = 'q6-added-line';
UPDATE orders SET o_totalprice = o_totalprice - 2123.47 WHERE o_comment = 'q6-batch-order-3';

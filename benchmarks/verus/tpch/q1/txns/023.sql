UPDATE lineitem
SET l_quantity = 10.00, l_extendedprice = 1000.00,
    l_discount = 0.05, l_tax = 0.02,
    l_returnflag = 'N', l_linestatus = 'O'
WHERE l_comment = 'q1-new-small' AND l_linenumber = 1;

UPDATE orders SET o_orderstatus = 'O'
WHERE o_comment = 'q1-new-small';

UPDATE lineitem
SET l_quantity = l_quantity + 2.50,
    l_extendedprice = l_extendedprice + 300.03,
    l_discount = 0.09,
    l_tax = 0.07
WHERE l_comment = 'q1-new-bulk';

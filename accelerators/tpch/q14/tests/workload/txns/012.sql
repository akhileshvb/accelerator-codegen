UPDATE lineitem SET l_extendedprice = l_extendedprice + 100.01, l_discount = 0.03
WHERE l_comment = 'v14-new-line';
UPDATE part SET p_type = 'STANDARD BRUSHED COPPER'
WHERE p_comment = 'v14-new';

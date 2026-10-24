UPDATE lineitem SET l_extendedprice = l_extendedprice + 222.22,
                    l_discount = 0.09, l_shipdate = DATE '1995-09-01'
WHERE l_comment = 'v14-base-4';
UPDATE part SET p_partkey =
    (SELECT l_partkey FROM lineitem WHERE l_comment = 'v14-base-2')
WHERE p_comment = 'v14-regular';

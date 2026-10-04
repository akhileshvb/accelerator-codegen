UPDATE lineitem SET l_extendedprice = l_extendedprice + 125.55
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-01') AND l_returnflag = 'R';

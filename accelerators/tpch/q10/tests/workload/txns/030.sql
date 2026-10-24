UPDATE lineitem SET l_extendedprice = 300000.00
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-24') AND l_returnflag = 'R';

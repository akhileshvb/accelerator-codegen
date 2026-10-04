UPDATE lineitem SET l_returnflag = 'r'
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-02') AND l_returnflag = 'R';
UPDATE lineitem SET l_extendedprice = l_extendedprice + 1000.01
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-02') AND l_returnflag = 'r';

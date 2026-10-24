UPDATE lineitem SET l_discount = 0.07
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-02') AND l_returnflag = 'R';

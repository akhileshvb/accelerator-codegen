UPDATE lineitem SET l_returnflag = 'A'
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-03') AND l_linenumber = 1;

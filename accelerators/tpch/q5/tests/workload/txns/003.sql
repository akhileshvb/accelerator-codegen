UPDATE lineitem SET l_extendedprice=l_extendedprice+19.95 WHERE l_orderkey=(SELECT min(o_orderkey) FROM orders WHERE o_comment='q5 point order') AND l_linenumber=1;

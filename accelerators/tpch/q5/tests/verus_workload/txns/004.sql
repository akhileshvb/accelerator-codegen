UPDATE lineitem SET l_discount=0.08 WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment='q5 batch order');

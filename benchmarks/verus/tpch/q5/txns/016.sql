DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment='q5 batch order' AND o_orderkey % 2 = 0);
DELETE FROM orders WHERE o_comment='q5 batch order' AND o_orderkey % 2 = 0;

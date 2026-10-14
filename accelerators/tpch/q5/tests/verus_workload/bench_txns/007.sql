DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment='q5 benchmark order');
DELETE FROM orders WHERE o_comment='q5 benchmark order';

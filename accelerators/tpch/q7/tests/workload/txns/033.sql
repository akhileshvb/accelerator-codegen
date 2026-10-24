DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment='q7 live');

DELETE FROM orders WHERE o_comment='q7 live';

DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment = 'q15-point-order');
DELETE FROM orders WHERE o_comment = 'q15-point-order';

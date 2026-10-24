DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment='q5 new order');
DELETE FROM orders WHERE o_comment='q5 new order';
DELETE FROM supplier WHERE s_comment='q5 new supplier';
DELETE FROM customer WHERE c_comment='q5 new customer';

DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment = 'q8-order-09');
DELETE FROM orders WHERE o_comment = 'q8-order-09';
DELETE FROM customer WHERE c_comment = 'q8-customer-c';

DELETE FROM lineitem WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'q19-order-a');
DELETE FROM orders WHERE o_comment = 'q19-order-a';

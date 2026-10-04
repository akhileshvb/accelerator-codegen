DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment = 'q12_new_a');
DELETE FROM orders WHERE o_comment = 'q12_new_a';

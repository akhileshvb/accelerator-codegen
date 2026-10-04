DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment LIKE 'q7 batch %');

DELETE FROM orders WHERE o_comment LIKE 'q7 batch %';

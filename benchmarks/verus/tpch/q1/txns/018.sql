DELETE FROM lineitem
WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment = 'q1-new-seven');

DELETE FROM orders WHERE o_comment = 'q1-new-seven';

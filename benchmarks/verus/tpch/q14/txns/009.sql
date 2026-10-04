DELETE FROM lineitem WHERE l_orderkey IN
    (SELECT o_orderkey FROM orders WHERE o_comment = 'v14-new-order');
DELETE FROM orders WHERE o_comment = 'v14-new-order';

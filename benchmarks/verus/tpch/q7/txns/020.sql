DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment LIKE 'q7 batch %' ORDER BY o_orderkey LIMIT 4);

DELETE FROM orders WHERE o_comment IN ('q7 batch 0','q7 batch 1','q7 batch 2','q7 batch 3');

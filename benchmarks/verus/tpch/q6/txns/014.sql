DELETE FROM orders WHERE o_comment IN ('q6-batch-order-1', 'q6-batch-order-2');
DELETE FROM lineitem WHERE l_comment IN ('q6-batch-order-1', 'q6-batch-order-2');

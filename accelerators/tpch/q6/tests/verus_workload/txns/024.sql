DELETE FROM lineitem WHERE l_comment LIKE 'q6-batch-order-%';
DELETE FROM orders WHERE o_comment LIKE 'q6-batch-order-%';

DELETE FROM lineitem WHERE l_comment = 'q20-extra';
DELETE FROM orders WHERE o_comment LIKE 'q20-extra/%';

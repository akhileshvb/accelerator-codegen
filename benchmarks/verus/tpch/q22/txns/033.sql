DELETE FROM orders
WHERE rowid = (SELECT rowid FROM orders WHERE o_comment = 'q22-duplicate-order' ORDER BY rowid LIMIT 1);

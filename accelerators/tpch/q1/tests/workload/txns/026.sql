DELETE FROM lineitem
WHERE rowid = (SELECT MIN(rowid) FROM lineitem WHERE l_comment = 'q1-exact-duplicate');

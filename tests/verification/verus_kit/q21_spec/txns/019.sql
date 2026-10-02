DELETE FROM lineitem WHERE rowid = (SELECT rowid FROM lineitem ORDER BY hash(rowid + 19) LIMIT 1);

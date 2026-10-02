DELETE FROM lineitem WHERE rowid = (SELECT rowid FROM lineitem ORDER BY hash(rowid + 3) LIMIT 1);

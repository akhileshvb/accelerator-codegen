DELETE FROM lineitem WHERE rowid = (SELECT rowid FROM lineitem ORDER BY hash(rowid + 27) LIMIT 1);

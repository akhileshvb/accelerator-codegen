DELETE FROM lineitem WHERE rowid = (SELECT rowid FROM lineitem ORDER BY hash(rowid + 35) LIMIT 1);

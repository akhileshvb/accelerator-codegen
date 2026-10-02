UPDATE lineitem SET l_receiptdate = CASE WHEN l_receiptdate > l_commitdate THEN l_commitdate ELSE l_commitdate + 1 END WHERE rowid = (SELECT rowid FROM lineitem ORDER BY hash(rowid + 33) LIMIT 1);

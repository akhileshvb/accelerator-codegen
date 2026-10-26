DELETE FROM num WHERE rowid IN (SELECT rowid FROM num WHERE adsh = 't1' AND tag = 'Dup' LIMIT 1);

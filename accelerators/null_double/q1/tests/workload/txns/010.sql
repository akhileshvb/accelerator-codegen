UPDATE num SET value = -value, ddate = ddate + 1 WHERE qtrs = 4 AND value IS NOT NULL AND rowid % 3 = 0;

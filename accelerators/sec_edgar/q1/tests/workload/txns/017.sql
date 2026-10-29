-- rfile changes: new (stmt, rfile) groups split off an existing one.
UPDATE pre SET rfile = 'R' WHERE stmt = 'IS' AND report % 3 = 0;

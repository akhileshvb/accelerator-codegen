-- ... and back.
UPDATE pre SET stmt = 'UN' WHERE stmt IS NULL AND tag <> 'NullStmt';

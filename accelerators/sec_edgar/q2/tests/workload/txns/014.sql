-- A filing is refiled with corrected values: its num rows are deleted and the same rows inserted with the new values.
INSERT INTO num SELECT adsh, tag, version, ddate, qtrs, uom, coreg, value + 1000, footnote FROM num WHERE adsh = '0000019617-22-000272';
DELETE FROM num WHERE adsh = '0000019617-22-000272' AND rowid IN (SELECT rowid FROM num WHERE adsh = '0000019617-22-000272' ORDER BY rowid LIMIT 1343);

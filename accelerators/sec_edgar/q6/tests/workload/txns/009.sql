-- the batch is withdrawn again, rows with NULL keys come and go
DELETE FROM num WHERE adsh LIKE 'bb-%';
DELETE FROM pre WHERE adsh LIKE 'bb-%';
DELETE FROM sub WHERE adsh LIKE 'bb-%';
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES (NULL, 'Pow2', 'v', 20231231, 4, 'USD', 1.0);
INSERT INTO pre (adsh, report, line, stmt, tag, version, plabel) VALUES (NULL, 1, 1, 'IS', 'Pow2', 'v', 'Orphan');

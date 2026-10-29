-- One transaction mixing inserts, updates and deletes over several groups.
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating)
SELECT '0009990003-24-000001', 100, i, 'CF', 0, 'H', 'Mix' || i, 'v', 'm', 0 FROM range(1, 30) t(i);
UPDATE pre SET stmt = 'BS' WHERE stmt = 'CF' AND report = 100 AND line % 2 = 0;
DELETE FROM pre WHERE stmt = 'CF' AND report = 100 AND line % 3 = 0;
UPDATE pre SET line = line * 2 WHERE stmt = 'CI';

-- An amendment is a new filing; it marks the one it amends.
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES ('B1-1A', 7100001, 'Batch Co 1', 4011, 2024, 0);
UPDATE sub SET prevrpt = 1 WHERE adsh = 'B1-1';
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES ('B1-1A', 'BatchTag', 'vB1', 20240101, 4, 'USD', 1000100.0);
INSERT INTO pre (adsh, report, line, stmt, tag, version) VALUES ('B1-1A', 1, 1, 'EQ', 'BatchTag', 'vB1');

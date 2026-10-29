-- The same filing is filed again after its withdrawal (a key reused).
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES ('B1-3', 7100003, 'Batch Co 3', 4011, 2024, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES ('B1-3', 'BatchTag', 'vB1', 20240101, 4, 'USD', 126.0);
INSERT INTO pre (adsh, report, line, stmt, tag, version) VALUES ('B1-3', 1, 1, 'EQ', 'BatchTag', 'vB1');

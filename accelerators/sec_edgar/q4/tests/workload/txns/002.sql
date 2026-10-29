-- An amendment is a new filing; it marks the one it amends.
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt)
    SELECT 'B1-1A', 7100001, 'Batch Co 1', 4011, 2024, 0 WHERE NOT EXISTS (SELECT 1 FROM sub x WHERE x.adsh = 'B1-1A');
UPDATE sub SET prevrpt = 1 WHERE adsh = 'B1-1';
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
    SELECT 'B1-1A', 'BatchTag', 'vB1', 20240101, 4, 'USD', 1000100.0 WHERE NOT EXISTS (SELECT 1 FROM num x WHERE x.adsh = 'B1-1A');
INSERT INTO pre (adsh, report, line, stmt, tag, version)
    SELECT 'B1-1A', 1, 1, 'EQ', 'BatchTag', 'vB1' WHERE NOT EXISTS (SELECT 1 FROM pre x WHERE x.adsh = 'B1-1A');

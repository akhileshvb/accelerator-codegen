-- Groups tied on cnt (five rows each) with different filing counts.
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating)
SELECT '0009990001-24-000001', 95, i, 'TI', 0, 'A', 'TTag', 'v', 't', 0 FROM range(1, 6) t(i);
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating)
SELECT '000999000' || (1 + (i % 3)) || '-24-000001', 95, i, 'TI', 0, 'B', 'TTag', 'v', 't', 0 FROM range(1, 6) t(i);
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating)
SELECT '000999000' || (1 + (i % 2)) || '-24-000001', 95, NULL, 'TI', 0, 'C', 'TTag', 'v', 't', 0 FROM range(1, 6) t(i);
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating)
SELECT '0009990001-24-000001', 95, i, 'TI', 0, NULL, 'TTag', 'v', 't', 0 FROM range(1, 6) t(i);

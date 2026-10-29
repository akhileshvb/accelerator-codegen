-- ... and a new EQ group is built from other rows.
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating)
SELECT adsh, report, line, 'EQ', inpth, rfile, tag, version, plabel, negating FROM pre WHERE stmt = 'BS' AND line % 3 = 0;

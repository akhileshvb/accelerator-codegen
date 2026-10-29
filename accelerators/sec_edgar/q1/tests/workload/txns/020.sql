-- ... and returns.
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating)
SELECT '0009990001-24-000001', 1, i, 'IS', 0, 'H', 'Back' || i, 'v', 'b', 0 FROM range(1, 4) t(i);

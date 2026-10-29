-- Extreme line values: sums far outside 32 bits.
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating)
SELECT '0009990001-24-000001', 98, 2147483647, 'EX', 0, 'H', 'Big', 'v', 'e', 0 FROM range(0, 1000);
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating)
SELECT '0009990002-24-000001', 98, -2147483648, 'EX', 0, 'H', 'Small', 'v', 'e', 0 FROM range(0, 1000);
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating) VALUES
  ('0009990002-24-000001', 98, 2147483647, 'EX', 0, 'H', 'Big', 'v', 'e', 0);

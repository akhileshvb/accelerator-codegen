-- a group appears: exactly 11 rows from a new filing, and a group reaches 10 from below (one short)
INSERT INTO sub (adsh, cik, name, fy, form) VALUES ('x-4', 94, 'EDGE FOUR', 2033, '10-K');
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-4', 'Appear', 'v', 20330630, 4, 'EDGE', 1.0 / 3 FROM range(11) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-4', 'Short', 'v', 20330630, 4, 'EDGE', 1.0 / 7 FROM range(10) t(i);

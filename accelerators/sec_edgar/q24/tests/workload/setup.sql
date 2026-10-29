-- Edge groups of the anti-join: HAVING boundary (10 rows, 11 rows), pre rows that match some of a group's rows,
-- NULL tag / version / adsh groups, date boundaries of the year, more than 100 groups tied at LIMIT,
-- a group whose sum cancels, huge and tiny values, negative zero.
INSERT INTO sub (adsh, cik, name, fy, form) VALUES
  ('x-1', 91, 'EDGE ONE', 2030, '10-K'), ('x-2', 92, 'EDGE TWO', 2030, '10-K'), ('x-3', 93, 'EDGE THREE', 2031, '10-K');
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-1', 'Ten', 'v', 20300630, 4, 'EDGE', i + 0.5 FROM range(10) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-1', 'Eleven', 'v', 20300630, 4, 'EDGE', i * 0.1 FROM range(11) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-2', 'Eleven', 'v', 20300101, 4, 'EDGE', 0.1 FROM range(3) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-1', 'Matched', 'v', 20300630, 4, 'EDGE', 1.5 FROM range(12) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-2', 'Matched', 'v', 20300630, 4, 'EDGE', 2.5 FROM range(12) t(i);
INSERT INTO pre (adsh, report, line, stmt, tag, version, plabel) VALUES
  ('x-1', 1, 1, 'IS', 'Matched', 'v', 'M'), ('x-1', 1, 2, 'IS', 'Matched', 'v', 'M2'), ('x-3', 1, 1, 'IS', 'Ten', 'v', 'T'), ('x-2', 1, 1, 'IS', 'Eleven', 'w', 'E');
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-1', NULL, 'v', 20300630, 4, 'EDGE', 1.25 FROM range(11) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-1', 'NoVersion', NULL, 20300630, 4, 'EDGE', 1.25 FROM range(12) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT NULL, 'NullAdsh', 'v', 20300630, 4, 'EDGE', 2.0 FROM range(11) t(i);
-- date boundaries: 0101 and 1231 count, 0100 and 1232 and the neighbour years do not
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-1', 'Dates', 'v', d, 4, 'EDGE', 1.0 FROM (VALUES (20300101), (20301231), (20300100), (20301232), (20291231), (20310101), (NULL)) t(d), range(3) u(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-1', 'Dates', 'v', 20300615, 4, 'EDGE', 1.0 FROM range(5) t(i);
-- values: cancelling, huge, tiny, negative zero, NULL value
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-2', 'Cancel', 'v', 20300630, 4, 'MIX', CASE WHEN i % 2 = 0 THEN 0.1 ELSE -0.1 END FROM range(12) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-2', 'Huge', 'v', 20320630, 4, 'MIX', 1.0e300 FROM range(11) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-2', 'Tiny', 'v', 20320630, 4, 'MIX', 4.9406564584124654e-324 FROM range(12) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-2', 'Zero', 'v', 20320630, 4, 'MIX', -0.0::DOUBLE FROM range(11) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-2', 'NullValue', 'v', 20320630, 4, 'MIX', CASE WHEN i < 11 THEN NULL ELSE 1.0 END FROM range(14) t(i);
-- 105 groups tied at 11 rows: the LIMIT cuts inside a tie, in any order
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-3', 'T' || lpad(g::VARCHAR, 3, '0'), 'v', 20310630, 4, 'TIE', g + i * 0.25 FROM range(105) a(g), range(11) b(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
  SELECT 'x-3', 'Big', 'v', 20310630, 4, 'TIE', 1.0 FROM range(20) b(i);

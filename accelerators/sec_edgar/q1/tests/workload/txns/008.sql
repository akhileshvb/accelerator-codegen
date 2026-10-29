-- NULLs: NULL stmt (never qualifies), NULL line, NULL adsh, NULL rfile inside a real group.
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating) VALUES
  ('0009990001-24-000001', 96, 1, NULL, 0, 'H', 'NullStmt', 'v', 'n', 0),
  ('0009990001-24-000001', 96, 2, NULL, 0, NULL, 'NullStmt', 'v', 'n', 0),
  ('0009990001-24-000001', 96, NULL, 'BS', 0, 'H', 'NullLine', 'v', 'n', 0),
  ('0009990001-24-000001', 96, NULL, 'BS', 0, NULL, 'NullBoth', 'v', 'n', 0),
  (NULL, 96, 7, 'BS', 0, 'H', 'NullAdsh', 'v', 'n', 0),
  (NULL, 96, 8, 'BS', 0, NULL, 'NullAdsh', 'v', 'n', 0),
  (NULL, 96, NULL, 'IS', 0, 'H', 'NullAdsh', 'v', 'n', 0),
  (NULL, 96, NULL, 'NL', 0, NULL, 'NullAdsh', 'v', 'n', 0),
  ('0009990002-24-000001', 96, NULL, 'NL', 0, NULL, 'NullLine', 'v', 'n', 0);

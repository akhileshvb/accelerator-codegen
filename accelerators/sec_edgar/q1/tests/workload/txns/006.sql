-- New groups appear: an unseen stmt with several rfile values, one of them NULL.
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating) VALUES
  ('0009990001-24-000001', 90, 1, 'XX', 0, 'H', 'XTag', 'v', 'x', 0),
  ('0009990001-24-000001', 90, 2, 'XX', 0, 'H', 'XTag', 'v', 'x', 0),
  ('0009990002-24-000001', 90, 3, 'XX', 0, 'H', 'XTag', 'v', 'x', 0),
  ('0009990002-24-000001', 91, 1, 'XX', 0, 'R', 'XTag', 'v', 'x', 0),
  ('0009990003-24-000001', 91, 2, 'XX', 0, NULL, 'XTag', 'v', 'x', 0),
  ('0009990003-24-000001', 91, 3, 'XX', 0, NULL, 'XTag', 'v', 'x', 0),
  ('0009990003-24-000001', 91, 4, 'XX', 0, NULL, 'XTag', 'v', 'x', 0);

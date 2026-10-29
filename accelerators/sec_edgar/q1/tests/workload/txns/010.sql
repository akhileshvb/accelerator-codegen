-- Rows inserted and deleted within one transaction (cancelling changes), next to a real insert.
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating) VALUES
  ('0009990003-24-000001', 97, 1, 'BS', 0, 'H', 'Ghost', 'v', 'g', 0),
  ('0009990003-24-000001', 97, 2, 'GH', 0, 'H', 'Ghost', 'v', 'g', 0),
  ('0009990003-24-000001', 97, 3, 'BS', 0, 'H', 'Real', 'v', 'r', 0);
DELETE FROM pre WHERE tag = 'Ghost';

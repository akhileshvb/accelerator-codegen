-- Zeros of both signs in one group (equal under comparison), and a positive row above them.
INSERT INTO sub (adsh, cik, name, sic, form, period, fy, fp, filed, prevrpt) VALUES
  ('0009990007-24-000001', 999007, 'Zero Filer', 1000, '10-K', 20231231, 2024, 'FY', 20240301, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
  ('0009990007-24-000001', 'Z1', 'v', 20231231, 4, 'ZERO', 0.0),
  ('0009990007-24-000001', 'Z1', 'v', 20231231, 4, 'ZERO', -(0.0::DOUBLE)),
  ('0009990007-24-000001', 'Z2', 'v', 20231231, 4, 'ZERO', -(0.0::DOUBLE)),
  ('0009990007-24-000001', 'Z3', 'v', 20231231, 4, 'ZERO', 0.0),
  ('0009990007-24-000001', 'Z3', 'v', 20231231, 4, 'ZERO', -1.0),
  ('0009990007-24-000001', 'Z4', 'v', 20231231, 4, 'ZERO', -(0.0::DOUBLE)),
  ('0009990007-24-000001', 'Z4', 'v', 20231231, 4, 'ZERO', -2.5);

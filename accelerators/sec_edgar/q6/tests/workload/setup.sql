-- Edge groups: one-tuple group with a power-of-two value, duplicate pre rows (stmt/plabel fan-out), NULL plabel and stmt,
-- NULL name, NULL fy, cancelling values, a huge pair, subnormal and negative zero.
INSERT INTO sub (adsh, cik, name, fy, form) VALUES
  ('e-1', 1, 'EDGE ONE', 2023, '10-K'), ('e-2', 2, 'EDGE TWO', 2023, '10-K'),
  ('e-3', 3, NULL, 2023, '10-K'), ('e-4', 4, 'NO FY', NULL, '10-K'), ('e-5', 5, 'EDGE ONE', 2023, '10-Q');
INSERT INTO pre (adsh, report, line, stmt, tag, version, plabel) VALUES
  ('e-1', 1, 1, 'IS', 'Pow2', 'v', 'Power'), ('e-1', 1, 2, 'IS', 'Dup', 'v', 'A'), ('e-1', 1, 3, 'IS', 'Dup', 'v', 'B'),
  ('e-1', 1, 4, 'IS', 'Dup', 'v', 'A'), ('e-1', 1, 5, 'IS', 'Nl', 'v', NULL), ('e-1', 1, 6, NULL, 'Nl', 'v', 'X'),
  ('e-2', 1, 1, 'IS', 'Pow2', 'v', 'Power'), ('e-2', 1, 2, 'IS', 'Big', 'v', 'Huge'), ('e-2', 1, 3, 'IS', 'Cancel', 'v', 'C'),
  ('e-2', 1, 4, 'IS', 'Tiny', 'v', 'T'), ('e-3', 1, 1, 'IS', 'Pow2', 'v', 'Power'), ('e-3', 1, 2, 'IS', 'Dup', 'v', 'A'),
  ('e-4', 1, 1, 'IS', 'Pow2', 'v', 'Power'), ('e-5', 1, 1, 'IS', 'Pow2', 'v', 'Power'), ('e-5', 1, 2, 'IS', 'Nl', 'v', NULL);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
  ('e-1', 'Pow2', 'v', 20231231, 4, 'USD', 1125899906842624.0), ('e-1', 'Dup', 'v', 20231231, 4, 'USD', 10.5),
  ('e-1', 'Nl', 'v', 20231231, 4, 'USD', 7.25), ('e-1', 'Dup', 'v', 20231231, 4, 'edge', 3.0),
  ('e-2', 'Pow2', 'v', 20231231, 4, 'USD', 1125899906842624.0), ('e-2', 'Big', 'v', 20231231, 4, 'USD', 1.0e300),
  ('e-2', 'Big', 'v', 20231231, 4, 'USD', 1.0e300), ('e-2', 'Cancel', 'v', 20231231, 4, 'USD', 0.1),
  ('e-2', 'Cancel', 'v', 20231231, 4, 'USD', -0.1), ('e-2', 'Tiny', 'v', 20231231, 4, 'USD', 4.9406564584124654e-324),
  ('e-3', 'Pow2', 'v', 20231231, 4, 'USD', 3.0), ('e-3', 'Dup', 'v', 20231231, 4, 'USD', -0.0::DOUBLE),
  ('e-4', 'Pow2', 'v', 20231231, 4, 'USD', 5.0), ('e-5', 'Pow2', 'v', 20231231, 4, 'USD', 0.3),
  ('e-5', 'Nl', 'v', 20231231, 4, 'USD', 0.1), ('e-5', 'Nl', 'v', 20231231, 4, 'USD', 0.2);

-- NULLs: value, tag, adsh, uom of num rows; name and fy of sub rows.
INSERT INTO sub (adsh, cik, name, sic, form, period, fy, fp, filed, prevrpt) VALUES
  ('0009990005-24-000001', 999005, NULL, 1000, '10-K', 20231231, 2023, 'FY', 20240301, 0),
  ('0009990006-24-000001', 999006, 'No Fiscal Year', 1000, '10-K', 20231231, NULL, 'FY', 20240301, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
  ('0009990005-24-000001', 'NullName', 'v', 20231231, 4, 'USD', 1e12),
  ('0009990005-24-000001', 'NullName', 'v', 20231231, 4, 'USD', NULL),
  ('0009990005-24-000001', NULL, 'v', 20231231, 4, 'USD', 5e11),
  ('0009990005-24-000001', NULL, 'v', 20231231, 4, 'USD', 5e11),
  ('0009990005-24-000001', 'NullUom', 'v', 20231231, 4, NULL, 4e11),
  (NULL, 'NullAdsh', 'v', 20231231, 4, 'USD', 3e11),
  ('0009990006-24-000001', 'NoFy', 'v', 20231231, 4, 'USD', 2e11),
  ('0009990006-24-000001', 'OnlyNull', 'v', 20231231, 4, 'USD', NULL);

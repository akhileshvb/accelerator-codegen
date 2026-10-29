-- Filing batch: three new filings, each one sub row with its pre and num rows (same adsh).
INSERT INTO sub (adsh, cik, name, sic, form, period, fy, fp, filed, prevrpt) VALUES
  ('0009990001-24-000001', 999001, 'New Filer One', 1000, '10-K', 20231231, 2023, 'FY', 20240301, 0),
  ('0009990002-24-000001', 999002, 'New Filer Two', 2000, '10-Q', 20240331, 2024, 'Q1', 20240510, 0),
  ('0009990003-24-000001', 999003, 'New Filer Three', 3000, '10-K', 20231231, 2023, 'FY', 20240302, 0);
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating)
SELECT a, 1 + (i // 40), 1 + (i % 40), ['BS', 'IS', 'CF', 'EQ', 'CI'][1 + (i // 40) % 5], 0, 'H', 'NewTag' || i, 'us-gaap/2024', 'Label ' || i, 0
FROM range(0, 200) t(i), (VALUES ('0009990001-24-000001'), ('0009990002-24-000001'), ('0009990003-24-000001')) s(a);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
SELECT a, 'NewTag' || i, 'us-gaap/2024', 20231231, 4, 'USD', i * 1000.5
FROM range(0, 20) t(i), (VALUES ('0009990001-24-000001'), ('0009990002-24-000001'), ('0009990003-24-000001')) s(a);

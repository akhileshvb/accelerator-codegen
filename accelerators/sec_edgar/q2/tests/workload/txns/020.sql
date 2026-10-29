-- Filing batch above the current top: new filings of fiscal year 2022 with USD values larger than any stored, then a second batch landing in the middle.
INSERT INTO sub (adsh, cik, name, sic, form, period, fy, fp, filed, prevrpt) VALUES
  ('0009990010-24-000001', 999010, 'Giant Filer A', 1000, '10-K', 20221231, 2022, 'FY', 20230301, 0),
  ('0009990011-24-000001', 999011, 'Giant Filer B', 2000, '10-K', 20221231, 2022, 'FY', 20230302, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
SELECT a, 'GiantTag' || (i % 40), 'us-gaap/2023', 20221231, 4, 'USD', 1.0e30 + i * 1.0e27
FROM range(0, 120) t(i), (VALUES ('0009990010-24-000001'), ('0009990011-24-000001')) s(a);
INSERT INTO sub (adsh, cik, name, sic, form, period, fy, fp, filed, prevrpt) VALUES
  ('0009990012-24-000001', 999012, 'Middle Filer', 3000, '10-K', 20221231, 2022, 'FY', 20230303, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
SELECT '0009990012-24-000001', 'MidTag' || (i % 60), 'us-gaap/2023', 20221231, 4, 'USD', (SELECT median(value) FROM num WHERE uom = 'USD' AND value > 0) * (1 + i / 1000.0)
FROM range(0, 300) t(i);

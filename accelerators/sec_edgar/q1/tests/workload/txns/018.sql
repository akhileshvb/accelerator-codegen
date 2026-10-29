-- Big batch: 400 filings with 50 rows each.
INSERT INTO sub (adsh, cik, name, sic, form, period, fy, fp, filed, prevrpt)
SELECT '00098' || printf('%05d', i) || '-24-000004', 980000 + i, 'Bulk Filer ' || i, 5000, '10-K', 20231231, 2023, 'FY', 20240401, 0
FROM range(0, 400) t(i);
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating)
SELECT '00098' || printf('%05d', f) || '-24-000004', 1 + (i // 10), 1 + (i % 10), ['BS', 'IS', 'CF', 'EQ', 'CI'][1 + (i // 10)], 0, 'H', 'BulkTag' || i, 'us-gaap/2024', 'Bulk', 0
FROM range(0, 400) g(f), range(0, 50) t(i);

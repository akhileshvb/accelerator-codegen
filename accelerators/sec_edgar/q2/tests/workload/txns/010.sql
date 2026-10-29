-- Big batch: 300 filings with 40 num rows each.
INSERT INTO sub (adsh, cik, name, sic, form, period, fy, fp, filed, prevrpt)
SELECT '00098' || printf('%05d', i) || '-24-000004', 980000 + i, 'Bulk Filer ' || i, 5000, '10-K', 20231231, 2022 + i % 3, 'FY', 20240401, 0
FROM range(0, 300) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
SELECT '00098' || printf('%05d', f) || '-24-000004', 'BulkTag' || (i % 20), 'us-gaap/2024', 20231231, 4, ['USD', 'pure'][1 + i % 2], ((f * 31 + i * 17) % 997) * 3.25
FROM range(0, 300) g(f), range(0, 40) t(i);

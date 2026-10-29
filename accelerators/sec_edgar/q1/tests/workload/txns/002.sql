-- A day of filings: ten small filings in one transaction.
INSERT INTO sub (adsh, cik, name, sic, form, period, fy, fp, filed, prevrpt)
SELECT '00099' || printf('%05d', i) || '-24-000002', 990000 + i, 'Batch Filer ' || i, 4000 + i, '8-K', 20240630, 2024, 'Q2', 20240710, 0
FROM range(0, 10) t(i);
INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating)
SELECT '00099' || printf('%05d', f) || '-24-000002', 1 + (i // 12), 1 + (i % 12), ['BS', 'IS', 'CF'][1 + (i // 12) % 3], 0, 'H', 'BatchTag' || i, 'us-gaap/2024', 'Batch label', 0
FROM range(0, 10) g(f), range(0, 36) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
SELECT '00099' || printf('%05d', f) || '-24-000002', 'BatchTag' || i, 'us-gaap/2024', 20240630, 1, 'USD', f * 100 + i
FROM range(0, 10) g(f), range(0, 6) t(i);

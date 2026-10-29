-- Ties at the LIMIT: 150 rows with the same value, names and tags that order them.
INSERT INTO sub (adsh, cik, name, sic, form, period, fy, fp, filed, prevrpt)
SELECT '00097' || printf('%05d', i) || '-24-000005', 970000 + i, 'Tie Filer ' || printf('%03d', (i * 37) % 150), 1000, '10-K', 20231231, 2023, 'FY', 20240301, 0
FROM range(0, 150) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
SELECT '00097' || printf('%05d', i) || '-24-000005', 'Tie' || printf('%02d', i % 7), 'v', 20231231, 4, 'TIE', 123456789.0 FROM range(0, 150) t(i);
